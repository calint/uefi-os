#include <efi.h>

#include "kernel.hpp"

namespace {
// helper functions

auto inline guids_equal(EFI_GUID const* g1, EFI_GUID const* g2) -> bool {
    // note: byte compare avoids alignment, padding, and aliasing issues
    auto p1 = reinterpret_cast<u8 const*>(g1);
    auto p2 = reinterpret_cast<u8 const*>(g2);
    for (auto i = 0u; i < sizeof(EFI_GUID); ++i) {
        if (p1[i] != p2[i]) {
            return false;
        }
    }
    return true;
}

auto inline init_serial() -> void {
    outb(0x3f8 + 1, 0x00); // disable interrupts
    outb(0x3f8 + 3, 0x80); // enable dlab
    outb(0x3f8 + 0, 0x03); // divisor low (38400 baud)
    outb(0x3f8 + 1, 0x00); // divisor high
    outb(0x3f8 + 3, 0x03); // 8n1, disable dlab
    outb(0x3f8 + 2, 0xc7); // enable fifo
    outb(0x3f8 + 4, 0x0b); // irqs enabled, rts/dsr set
}

template <typename T> auto ptr_offset(void const* ptr, u64 bytes) -> T* {
    return reinterpret_cast<T*>(reinterpret_cast<uptr>(ptr) + bytes);
}

} // namespace

// assumptions:
// - ACPI 2.0+ firmware present and correct
// - XSDT entries are valid physical pointers
// - firmware is part of the trusted computing base
// - failure == abort, no recovery paths

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
    RSDP* rsdp = nullptr;
    auto acpi_20_guid = EFI_GUID(ACPI_20_TABLE_GUID);
    for (auto i = 0u; i < sys->NumberOfTableEntries; ++i) {
        if (!guids_equal(&sys->ConfigurationTable[i].VendorGuid,
                         &acpi_20_guid)) {
            continue;
        }
        rsdp = static_cast<RSDP*>(sys->ConfigurationTable[i].VendorTable);
        break;
    }
    if (!rsdp) {
        serial_print("abort: no acpi 2.0 rsdp\n");
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

    // calculate number of pointers in xsdt
    auto entries = (xsdt->length - sizeof(SDTHeader)) / sizeof(u64);
    auto ptrs = ptr_offset<u64>(xsdt, sizeof(SDTHeader));

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
        auto header = reinterpret_cast<SDTHeader*>(ptrs[i]);
        auto constexpr APIC_SIGNATURE = u32(0x43495041); // 'APIC' little-endian
        // signature compare (x86 little-endian)
        if (*reinterpret_cast<u32*>(header->signature) == APIC_SIGNATURE) {
            // get multiple apic description table (madt)
            //  defines how interrupts are routed to CPUs
            struct [[gnu::packed]] MADT {
                SDTHeader header;
                u32 lapic_address;
                u32 flags;
                u8 entries[]; // Start of variable-length structures
            };
            auto madt = reinterpret_cast<MADT*>(header);

            apic.local = reinterpret_cast<u32 volatile*>(madt->lapic_address);

            auto curr = madt->entries;
            auto end = ptr_offset<u8>(madt, madt->header.length);
            while (curr < end) {
                struct [[gnu::packed]] MADT_EntryHeader {
                    u8 type;
                    u8 length;
                };
                auto entry = reinterpret_cast<MADT_EntryHeader*>(curr);

                switch (entry->type) {

                // I/O APIC: physical MMIO address and GSI range for an external
                // interrupt controller
                case 1: {
                    if (io_apic_count >= 8) {
                        serial_print("warning: >8 IOAPICs, ignoring extras\n");
                    } else {
                        io_apics[io_apic_count] =
                            *reinterpret_cast<MADT_IOAPIC*>(curr);
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
                    auto iso = reinterpret_cast<MADT_ISO*>(curr);
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
                    auto lapic = reinterpret_cast<MADT_LAPIC_Override*>(curr);
                    apic.local =
                        reinterpret_cast<u32 volatile*>(lapic->address);
                    break;
                }

                default:
                    break;
                }
                if (entry->length == 0) {
                    // just in case
                    break;
                }
                curr += entry->length;
            }
            // done with apic configuration
            break;
        }
    }

    // select ioapic with highest gsi_base <= keyboard gsi
    for (auto i = 0u; i < io_apic_count; ++i) {
        if (keyboard_config.gsi >= io_apics[i].gsi_base) {
            apic.io = reinterpret_cast<u32 volatile*>(io_apics[i].address);
            // note: in a true multi-apic system, check (gsi_base +
            //       max_interrupts)
        }
    }

    //
    // get memory map, exit boot services and start kernel
    //

    auto size = UINTN(0);
    auto key = UINTN(0);
    auto descriptor_size = UINTN(0);
    auto descriptor_ver = UINT32(0);

    bs->GetMemoryMap(&size, nullptr, &key, &descriptor_size, &descriptor_ver);

    EFI_MEMORY_DESCRIPTOR* map = nullptr;
    if (bs->AllocatePages(
            AllocateAnyPages, EfiLoaderData, EFI_SIZE_TO_PAGES(size + 4096),
            reinterpret_cast<EFI_PHYSICAL_ADDRESS*>(&map)) != EFI_SUCCESS) {
        // note: +4096 add a full page of padding for fragmented maps
        serial_print("abort: could not allocate pages\n");
        return EFI_ABORTED;
    }

    // multiple attempts because interrupts etc may change the memory map
    // between GetMemoryMap and ExitBootServices
    for (auto i = 0u; i < 16; ++i) {
        if (bs->GetMemoryMap(&size, map, &key, &descriptor_size,
                             &descriptor_ver) == EFI_SUCCESS) {

            if (bs->ExitBootServices(img, key) == EFI_SUCCESS) {
                memory_map = {.buffer = static_cast<void*>(map),
                              .size = size,
                              .descriptor_size = descriptor_size,
                              .descriptor_version = descriptor_ver};

                kernel_start();
                __builtin_unreachable();
            }
        }
    }

    serial_print("abort: 16 attempts of clean exit failed");
    return EFI_ABORTED;
}
