#include <efi.h>

#include "efierr.h"
#include "kernel.hpp"

namespace {
// helper functions

auto guids_equal(EFI_GUID const* g1, EFI_GUID const* g2) -> bool {
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

auto acpi_checksum(void const* ptr, u32 length) -> bool {
    auto p = static_cast<u8 const*>(ptr);
    auto sum = u8(0);
    for (auto i = 0u; i < length; ++i) {
        sum += p[i];
    }
    return sum == 0;
}

auto init_serial() -> void {
    outb(0x3f8 + 1, 0x00); // disable interrupts
    outb(0x3f8 + 3, 0x80); // enable dlab
    outb(0x3f8 + 0, 0x03); // divisor low (38400 baud)
    outb(0x3f8 + 1, 0x00); // divisor high
    outb(0x3f8 + 3, 0x03); // 8n1, disable dlab
    outb(0x3f8 + 2, 0xc7); // enable fifo
    outb(0x3f8 + 4, 0x0b); // irqs enabled, rts/dsr set
}

template <typename T = u8> auto ptr_offset(void* ptr, u64 bytes) -> T* {
    return reinterpret_cast<T*>(reinterpret_cast<uptr>(ptr) + bytes);
}

} // namespace

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

    // get root system description pointer (rsdp)
    //  the "entry point" found via uefi
    struct [[gnu::packed]] RSDP {
        char signature[8];
        u8 checksum;
        char oem_id[6];
        u8 revision;
        u32 rsdt_address;
        u32 length;
        u64 xsdt_address; // 64-bit pointer to the XSDT
        u8 extended_checksum;
        u8 reserved[3];
    };
    auto rsdp = static_cast<RSDP*>(nullptr);
    auto acpi_20_guid = EFI_GUID(ACPI_20_TABLE_GUID);
    for (auto i = 0u; i < sys->NumberOfTableEntries; ++i) {
        if (!guids_equal(&sys->ConfigurationTable[i].VendorGuid,
                         &acpi_20_guid)) {
            continue;
        }
        rsdp = static_cast<RSDP*>(sys->ConfigurationTable[i].VendorTable);
        break;
    }
    if (rsdp == nullptr || !acpi_checksum(rsdp, rsdp->length) ||
        rsdp->revision < 2) {
        serial_print("abort: malformed ACPI 2.0+ RSDP\n");
        return EFI_ABORTED;
    }

    // get extended system description table (xsdt)
    //  a list of pointers to all other acpi tables
    struct [[gnu::packed]] SDTHeader {
        char signature[4];
        u32 length;
        u8 revision;
        u8 checksum;
        char oem_id[6];
        char oem_table_id[8];
        u32 oem_revision;
        u32 creator_id;
        u32 creator_revision;
    };
    auto xsdt = reinterpret_cast<SDTHeader*>(rsdp->xsdt_address);
    if (xsdt == nullptr || !acpi_checksum(xsdt, xsdt->length) ||
        xsdt->length < sizeof(SDTHeader) ||
        ((xsdt->length - sizeof(SDTHeader)) & 7) != 0) {
        serial_print("abort: malformed XSDT\n");
        return EFI_ABORTED;
    }

    // calculate number of pointers in xsdt
    auto entries = (xsdt->length - sizeof(SDTHeader)) / 8;
    auto table_ptrs = ptr_offset<u64>(xsdt, sizeof(SDTHeader));

    // retrieve i/o apics in the system
    // find keyboard and get gsi and flags

    // default system configuration
    keyboard_config = {.gsi = 1u, .flags = 0u};
    apic = {.io = reinterpret_cast<u32 volatile*>(0xfec00000),
            .local = reinterpret_cast<u32 volatile*>(0xfee00000)};

    // i/o apics found in the system (most systems < 8)
    struct [[gnu::packed]] MADT_IOAPIC {
        u8 type;
        u8 len;
        u8 id;
        u8 res;
        u32 address;
        u32 gsi_base;
    };
    MADT_IOAPIC io_apics[8];
    auto io_apic_count = 0u;

    // find apic values and keyboard configuration
    for (auto i = 0u; i < entries; ++i) {
        auto header = reinterpret_cast<SDTHeader*>(table_ptrs[i]);
        if (header == nullptr || !acpi_checksum(header, header->length)) {
            return EFI_ABORTED;
        }

        if (header->signature[0] == 'A' && header->signature[1] == 'P' &&
            header->signature[2] == 'I' && header->signature[3] == 'C') {

            // get multiple apic description table (madt)
            //  defines how interrupts are routed to CPUs
            struct [[gnu::packed]] MADT {
                SDTHeader header;
                u32 lapic_address;
                u32 flags;
                u8 entries[]; // Start of variable-length structures
            };
            auto madt = reinterpret_cast<MADT*>(header);
            if (!acpi_checksum(madt, madt->header.length)) {
                serial_print("abort: invalid MADT checksum\n");
                return EFI_ABORTED;
            }

            apic.local = reinterpret_cast<u32 volatile*>(madt->lapic_address);

            auto p = madt->entries;
            auto end = ptr_offset<u8>(madt, madt->header.length);
            while (p < end) {
                struct [[gnu::packed]] MADT_EntryHeader {
                    u8 type;
                    u8 length;
                };
                auto entry = reinterpret_cast<MADT_EntryHeader*>(p);
                if (entry->length < sizeof(MADT_EntryHeader) ||
                    (p + entry->length > end) || entry->length < 2) {
                    serial_print("abort: malformed MADT entry\n");
                    return EFI_ABORTED;
                }

                switch (entry->type) {

                // I/O APIC: physical MMIO address and GSI range for an external
                // interrupt controller
                case 1: {
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
                    break;
                }

                // Multiple APIC Description Table: Interrupt Source Override
                case 2: {
                    struct [[gnu::packed]] MADT_ISO {
                        u8 type;   // 2
                        u8 length; // 10
                        u8 bus;    // 0 (ISA)
                        u8 source; // The IRQ number (1 for keyboard)
                        u32 gsi;   // The Global System Interrupt (IO APIC pin)
                        u16 flags; // Polarity and Trigger Mode
                    };
                    if (entry->length < sizeof(MADT_ISO)) {
                        serial_print("abort: malformed MADT ISO entry\n");
                        return EFI_ABORTED;
                    }
                    auto iso = reinterpret_cast<MADT_ISO*>(p);
                    if (iso->source == 1) {
                        serial_print("info: found keyboard config\n");
                        keyboard_config.gsi = iso->gsi;
                        keyboard_config.flags = 0;
                        // polarity: 3 = active low
                        if ((iso->flags & 0x3) == 0x3) {
                            keyboard_config.flags |= (1 << 13);
                        }
                        // trigger: 3 = level
                        if (((iso->flags >> 2) & 0x3) == 0x3) {
                            keyboard_config.flags |= (1 << 15);
                        }
                    }
                    break;
                }

                // APIC Address Override
                case 5: {
                    struct [[gnu::packed]] MADT_LAPIC_Override {
                        u8 type;
                        u8 len;
                        u16 res;
                        u64 address;
                    };
                    if (entry->length < sizeof(MADT_LAPIC_Override)) {
                        serial_print(
                            "abort: malformed MADT LAPIC Override entry\n");
                        return EFI_ABORTED;
                    }
                    apic.local = reinterpret_cast<u32 volatile*>(
                        reinterpret_cast<MADT_LAPIC_Override*>(p)->address);
                    break;
                }

                default:
                    break;
                }

                p += entry->length;
                if (p > end) {
                    serial_print("abort: MADT entry overruns table\n");
                    return EFI_ABORTED;
                }
            }
            // done with apic configuration
            break;
        }
    }

    // select the apic connected to the keyboard gsi
    for (auto i = 0u; i < io_apic_count; ++i) {
        if (keyboard_config.gsi >= io_apics[i].gsi_base) {
            apic.io = reinterpret_cast<u32 volatile*>(io_apics[i].address);
            // note: in a true multi-apic system, check (gsi_base +
            //       max_interrupts)
            break;
        }
    }

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
