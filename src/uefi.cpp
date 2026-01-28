#include <efi.h>

#include "acpi.hpp"
#include "efierr.h"
#include "kernel.hpp"

auto static guids_equal(EFI_GUID const* g1, EFI_GUID const* g2) -> bool {
    // note: compare byte by byte because g1 and g2 not guaranteed to be aligned
    //       at 8 bytes. in c++ that is UB
    auto p1 = reinterpret_cast<u8 const*>(g1);
    auto p2 = reinterpret_cast<u8 const*>(g2);
    for (auto i = 0u; i < sizeof(EFI_GUID); ++i) {
        if (p1[i] != p2[i]) {
            return false;
        }
    }
    return true;
}

auto static acpi_checksum(void const* ptr, u32 length) -> bool {
    auto p = static_cast<u8 const*>(ptr);
    auto sum = u8(0);
    for (auto i = 0u; i < length; ++i) {
        sum += p[i];
    }
    return sum == 0;
}

auto static init_serial() -> void {
    outb(0x3f8 + 1, 0x00); // disable interrupts
    outb(0x3f8 + 3, 0x80); // enable dlab
    outb(0x3f8 + 0, 0x03); // divisor low (38400 baud)
    outb(0x3f8 + 1, 0x00); // divisor high
    outb(0x3f8 + 3, 0x03); // 8n1, disable dlab
    outb(0x3f8 + 2, 0xc7); // enable fifo
    outb(0x3f8 + 4, 0x0b); // irqs enabled, rts/dsr set
}

extern "C" auto EFIAPI efi_main(EFI_HANDLE img, EFI_SYSTEM_TABLE* sys)
    -> EFI_STATUS {

    init_serial();

    serial_print("efi_main\n");

    auto bs = sys->BootServices;

    //
    // get frame buffer config
    //

    auto graphics_guid = EFI_GUID(EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID);
    auto gop = static_cast<EFI_GRAPHICS_OUTPUT_PROTOCOL*>(nullptr);
    if (bs->LocateProtocol(&graphics_guid, nullptr,
                           reinterpret_cast<void**>(&gop)) != EFI_SUCCESS) {
        serial_print("abort: failed to get frame buffer\n");
        return EFI_ABORTED;
    }
    frame_buffer = {.pixels =
                        reinterpret_cast<u32*>(gop->Mode->FrameBufferBase),
                    .width = gop->Mode->Info->HorizontalResolution,
                    .height = gop->Mode->Info->VerticalResolution,
                    .stride = gop->Mode->Info->PixelsPerScanLine};

    //
    // get keyboard config, io_apic and lapic pointers
    //

    // get root system description pointer (rsdp): the "entry point" found via
    // uefi.
    auto rsdp = static_cast<RSDP*>(nullptr);
    auto acpi_20_guid = EFI_GUID(ACPI_20_TABLE_GUID);
    for (auto i = 0u; i < sys->NumberOfTableEntries; ++i) {
        if (guids_equal(&sys->ConfigurationTable[i].VendorGuid,
                        &acpi_20_guid)) {
            rsdp =
                reinterpret_cast<RSDP*>(sys->ConfigurationTable[i].VendorTable);
            break;
        }
    }

    if (!rsdp) {
        serial_print("abort: no ACPI RSDP found\n");
        return EFI_ABORTED;
    }
    if (!acpi_checksum(rsdp, rsdp->length)) {
        serial_print("abort: RSDP checksum failed\n");
        return EFI_ABORTED;
    }
    if (rsdp->revision < 2 || rsdp->xsdt_address == 0) {
        serial_print("abort: ACPI < 2.0 not supported\n");
        return EFI_ABORTED;
    }

    // get extended system description table: a list of pointers to all other
    // acpi tables.
    auto xsdt = reinterpret_cast<SDTHeader*>(rsdp->xsdt_address);
    if (xsdt->length < sizeof(SDTHeader) ||
        ((xsdt->length - sizeof(SDTHeader)) & 7) != 0) {
        serial_print("abort: invalid XSDT length\n");
        return EFI_ABORTED;
    }
    if (!acpi_checksum(xsdt, xsdt->length)) {
        serial_print("abort: XSDT checksum failed\n");
        return EFI_ABORTED;
    }

    // calculate number of pointers in xsdt
    auto entries = (xsdt->length - sizeof(SDTHeader)) / 8;
    auto table_ptrs = reinterpret_cast<u64*>(u64(xsdt) + sizeof(SDTHeader));

    // default keyboard config
    auto kbd_gsi = 1u;   // default to pin 1
    auto kbd_flags = 0u; // default active high, edge

    // default apic values
    apic.io = reinterpret_cast<u32 volatile*>(0xfec00000);
    apic.local = reinterpret_cast<u32 volatile*>(0xfee00000);

    // retrieve all apic in the system then find the keyboard and map it
    MADT_IOAPIC io_apics[8]; // most systems have < 8
    auto io_apic_count = 0u;

    // find apic values and keyboard configuration
    for (auto i = 0u; i < entries; ++i) {
        auto header = reinterpret_cast<SDTHeader*>(table_ptrs[i]);
        if (!acpi_checksum(header, header->length)) {
            serial_print("abort: SDTHeader checksum failed\n");
            return EFI_ABORTED;
        }
        if (header->signature[0] == 'A' && header->signature[1] == 'P' &&
            header->signature[2] == 'I' && header->signature[3] == 'C') {

            // get multiple apic description table: defines how interrupts are
            // routed to CPUs
            auto madt = reinterpret_cast<MADT*>(header);
            if (!acpi_checksum(madt, madt->header.length)) {
                serial_print("abort: invalid MADT checksum\n");
                return EFI_ABORTED;
            }
            auto p = madt->entries;
            auto end = reinterpret_cast<u8*>(madt) + madt->header.length;

            apic.local = reinterpret_cast<u32 volatile*>(madt->lapic_address);

            while (p < end) {
                auto entry = reinterpret_cast<MADT_EntryHeader*>(p);
                if (entry->length < sizeof(MADT_EntryHeader)) {
                    serial_print("abort: MADT entry length less than header\n");
                    return EFI_ABORTED;
                }
                if (p + entry->length > end) {
                    serial_print("abort: MADT entry overruns table\n");
                    return EFI_ABORTED;
                }

                if (entry->type == 1) {
                    // I/O APIC: physical MMIO address and GSI range for an
                    // external interrupt controller
                    if (entry->length < sizeof(MADT_IOAPIC)) {
                        serial_print("abort: short MADT IOAPIC entry\n");
                        return EFI_ABORTED;
                    }
                    if (io_apic_count >= 8) {
                        serial_print("warning: >8 IOAPICs, ignoring extras\n");
                    } else {
                        io_apics[io_apic_count] =
                            *reinterpret_cast<MADT_IOAPIC*>(p);
                        ++io_apic_count;
                    }

                } else if (entry->type == 2) {
                    // Multiple APIC Description Table: Interrupt Source
                    // Override
                    if (entry->length < sizeof(MADT_ISO)) {
                        serial_print("abort: short MADT ISO entry\n");
                        return EFI_ABORTED;
                    }
                    auto iso = reinterpret_cast<MADT_ISO*>(p);
                    if (iso->source == 1) { // keyboard
                        serial_print("uefi: found keyboard config\n");
                        kbd_gsi = iso->gsi;
                        kbd_flags = 0;
                        // polarity: 3 = active low
                        if ((iso->flags & 0x3) == 0x3) {
                            kbd_flags |= (1 << 13);
                        }
                        // trigger: 3 = level
                        if (((iso->flags >> 2) & 0x3) == 0x3) {
                            kbd_flags |= (1 << 15);
                        }
                    }

                } else if (entry->type == 5) {
                    // Local APIC Address Override
                    if (entry->length < sizeof(MADT_LAPIC_Override)) {
                        serial_print(
                            "abort: short MADT LAPIC Override entry\n");
                        return EFI_ABORTED;
                    }
                    apic.local = reinterpret_cast<u32 volatile*>(
                        reinterpret_cast<MADT_LAPIC_Override*>(p)->address);
                }

                p += entry->length;
                if (p > end) {
                    serial_print("abort: MADT entry overruns table\n");
                    return EFI_ABORTED;
                }
            }
        }
    }

    // select the apic connected to the keyboard gsi
    for (auto i = 0u; i < io_apic_count; ++i) {
        if (kbd_gsi >= io_apics[i].gsi_base) {
            apic.io = reinterpret_cast<u32 volatile*>(io_apics[i].address);
            // note: in a true multi-apic system, you'd check (gsi_base +
            //       max_interrupts)
            break;
        }
    }

    keyboard_config = {.gsi = kbd_gsi, .flags = kbd_flags};

    //
    // get memory map and exit boot services
    //

    auto size = UINTN(0);
    auto key = UINTN(0);
    auto d_size = UINTN(0);
    auto d_ver = UINT32(0);

    bs->GetMemoryMap(&size, nullptr, &key, &d_size, &d_ver);
    size += 2 * d_size;

    auto map_phys = EFI_PHYSICAL_ADDRESS(0);
    if (bs->AllocatePages(AllocateAnyPages, EfiLoaderData,
                          EFI_SIZE_TO_PAGES(size), &map_phys) != EFI_SUCCESS) {
        serial_print("abort: could not allocate pages\n");
        return EFI_ABORTED;
    }

    auto map = reinterpret_cast<EFI_MEMORY_DESCRIPTOR*>(map_phys);

    auto clean_exit = false;
    for (auto attempt = 0; attempt < 8; ++attempt) {
        if (bs->GetMemoryMap(&size, map, &key, &d_size, &d_ver) !=
            EFI_SUCCESS) {
            continue;
        }
        if (bs->ExitBootServices(img, key) == EFI_SUCCESS) {
            clean_exit = true;
            break;
        }
    }
    if (!clean_exit) {
        serial_print("abort: did not do clean exit\n");
        return EFI_ABORTED;
    }

    memory_map = {.buffer = static_cast<void*>(map),
                  .size = size,
                  .descriptor_size = d_size,
                  .descriptor_version = d_ver};

    //
    // done with uefi information, start kernel
    //

    kernel_start();
}
