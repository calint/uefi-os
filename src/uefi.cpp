#include <efi.h>

#include "kernel.hpp"

namespace {

auto constexpr MAX_IO_APICS = 8u;
auto constexpr MAX_CORES = 256u;

// efi guid comparison
// performs a robust byte-by-byte comparison of two efi guids
auto inline guids_equal(EFI_GUID const* g1, EFI_GUID const* g2) -> bool {
    // casting to u8* (byte pointer) ensures to ignore structural padding
    // and avoids potential alignment faults on strict architectures
    auto const* p1 = ptr<u8 const>(g1);
    auto const* p2 = ptr<u8 const>(g2);

    for (auto i = 0u; i < sizeof(EFI_GUID); ++i) {
        if (p1[i] != p2[i]) {
            return false;
        }
    }

    return true;
}

// uefi console output
// high-level wrapper for the uefi boot-time text console
auto inline console_print(EFI_SYSTEM_TABLE const* sys, char16_t const* s)
    -> void {

    sys->ConOut->OutputString(sys->ConOut,
                              ptr<CHAR16>(const_cast<char16_t*>(s)));
}

} // namespace

// assumptions:
// - ACPI 2.0+ firmware present and correct
// - XSDT entries are valid physical pointers
// - firmware is part of the trusted computing base
// - failure == abort, no recovery paths

// the uefi entry point
extern "C" auto EFIAPI efi_main(EFI_HANDLE const img,
                                EFI_SYSTEM_TABLE const* sys) -> EFI_STATUS {

    sys->ConOut->ClearScreen(sys->ConOut);

    console_print(sys, u"efi_main\n");

    auto const* bs = sys->BootServices;

    //
    // get frame buffer config
    //

    // locate the gop (graphics output protocol) to get a linear frame buffer
    EFI_GUID graphics_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = nullptr;
    if (bs->LocateProtocol(&graphics_guid, nullptr, ptr<void*>(&gop)) !=
        EFI_SUCCESS) {
        console_print(sys, u"abort: failed to get frame buffer\n");
        return EFI_ABORTED;
    }

    // store dimensions and address for the kernel's future renderer
    kernel::frame_buffer = {.pixels = ptr<u32>(gop->Mode->FrameBufferBase),
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
        u64 xsdt_address;
        u8 extended_checksum;
        u8 reserved[3];
    };
    RSDP* rsdp = nullptr;
    EFI_GUID acpi_20_guid = ACPI_20_TABLE_GUID;
    for (auto i = 0u; i < sys->NumberOfTableEntries; ++i) {
        if (guids_equal(&sys->ConfigurationTable[i].VendorGuid,
                        &acpi_20_guid)) {
            rsdp = ptr<RSDP>(sys->ConfigurationTable[i].VendorTable);
            break;
        }
    }
    if (!rsdp) {
        console_print(sys, u"abort: no ACPI 2.0+ RSDP\n");
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
    auto const* xsdt = ptr<SDTHeader>(rsdp->xsdt_address);

    // calculate number of pointers in xsdt
    auto const entries = (xsdt->length - sizeof(SDTHeader)) / sizeof(u64);
    auto const* ptrs = ptr_offset<u64>(xsdt, sizeof(SDTHeader));

    // retrieve i/o apics in the system
    // find keyboard and get gsi and flags

    // default system configuration
    kernel::keyboard_config = {.gsi = 1u, .flags = 0u};
    kernel::apic = {.io = ptr<u32>(0xfec00000), .local = ptr<u32>(0xfee00000)};

    // i/o apics found in the system (most systems < 8)
    struct [[gnu::packed]] MADT_IOAPIC {
        u8 type; // 1
        u8 len;
        u8 id;
        u8 res;
        u32 address;
        u32 gsi_base;
    };
    MADT_IOAPIC io_apics[MAX_IO_APICS];
    auto io_apic_count = 0u;

    // find apic values and keyboard configuration
    // parse the madt (multiple apic description table) to route interrupts
    for (auto i = 0u; i < entries; ++i) {
        auto const* header = ptr<SDTHeader>(ptrs[i]);
        auto constexpr APIC_SIGNATURE = u32(0x43495041); // 'APIC' little-endian
        // signature compare (x86 little-endian)
        if (*ptr<u32>(header->signature) == APIC_SIGNATURE) {
            // get multiple apic description table (madt)
            //  defines how interrupts are routed to CPUs
            struct [[gnu::packed]] MADT {
                SDTHeader header;
                u32 lapic_address;
                u32 flags;
                u8 entries[];
            };
            auto const* madt = ptr<MADT>(header);

            kernel::apic.local = ptr<u32>(madt->lapic_address);

            auto const* curr = madt->entries;
            auto const* const end = ptr_offset<void>(madt, madt->header.length);
            while (curr < end) {
                struct [[gnu::packed]] MADT_EntryHeader {
                    u8 type;
                    u8 length;
                };
                auto const* entry = ptr<MADT_EntryHeader>(curr);

                switch (entry->type) {

                case 0: {
                    struct [[gnu::packed]] MADT_LAPIC {
                        u8 type;   // 0
                        u8 length; // 8
                        u8 processor_id;
                        u8 apic_id; // id used to target the core via ipi
                        u32 flags;  // bit 0: enabled, bit 1: online capable
                    };
                    auto const* core = ptr<MADT_LAPIC>(curr);
                    if (core->flags & 3) { // if enabled or online capable
                        kernel::cores[kernel::core_count] = {.apic_id =
                                                                 core->apic_id};
                        ++kernel::core_count;
                    }
                    break;
                }
                // i/o apic: physical mmio address and gsi range for an external
                // interrupt controller
                case 1: {
                    if (io_apic_count >= MAX_IO_APICS) {
                        console_print(sys,
                                      u"abort: more IOAPICs than configured");
                        return EFI_ABORTED;
                    } else {
                        io_apics[io_apic_count] = *ptr<MADT_IOAPIC>(curr);
                        ++io_apic_count;
                    }
                    break;
                }

                // multiple apic description table: interrupt source override
                case 2: {
                    struct [[gnu::packed]] MADT_ISO {
                        u8 type;   // 2
                        u8 length; // 10
                        u8 bus;    // 0 (isa)
                        u8 source; // the irq number (1 for keyboard)
                        u32 gsi;   // the global system interrupt (io apic pin)
                        u16 flags; // polarity and trigger mode
                    };
                    auto const* iso = ptr<MADT_ISO>(curr);

                    // check for keyboard irq
                    if (iso->source == 1) {
                        console_print(sys, u"info: found keyboard config\n");
                        kernel::keyboard_config.gsi = iso->gsi;
                        kernel::keyboard_config.flags = 0;
                        // polarity: 3 = active low
                        if ((iso->flags & 3) == 3) {
                            kernel::keyboard_config.flags |= (1 << 13);
                        }
                        // trigger: 3 = level
                        if (((iso->flags >> 2) & 3) == 3) {
                            kernel::keyboard_config.flags |= (1 << 15);
                        }
                    }
                    break;
                }

                // apic address override
                case 5: {
                    struct [[gnu::packed]] MADT_LAPIC_Override {
                        u8 type;
                        u8 len;
                        u16 res;
                        u64 address;
                    };
                    auto const* lapic = ptr<MADT_LAPIC_Override>(curr);
                    kernel::apic.local = ptr<u32>(lapic->address);
                    break;
                }

                default:
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
        if (kernel::keyboard_config.gsi >= io_apics[i].gsi_base) {
            kernel::apic.io = ptr<u32>(io_apics[i].address);
            // note: in a true multi-apic system, check (gsi_base +
            //       max_interrupts)
        }
    }

    //
    // get memory map, exit boot services and start kernel
    //

    UINTN size = 0;
    UINTN key = 0;
    UINTN descriptor_size = 0;
    UINT32 descriptor_ver = 0;

    // calculate how many bytes needed to store the system memory map
    bs->GetMemoryMap(&size, nullptr, &key, &descriptor_size, &descriptor_ver);

    // allocate an extra page in case memory map increases
    auto map_capacity = size + 4096;

    // allocate the memory
    EFI_MEMORY_DESCRIPTOR* map = nullptr;
    if (bs->AllocatePages(AllocateAnyPages, EfiLoaderData,
                          EFI_SIZE_TO_PAGES(map_capacity),
                          ptr<EFI_PHYSICAL_ADDRESS>(&map)) != EFI_SUCCESS) {
        console_print(sys, u"abort: could not allocate pages\n");
        return EFI_ABORTED;
    }

    // multiple attempts because interrupts etc may change the memory map
    // between GetMemoryMap and ExitBootServices
    for (auto i = 0u; i < 16; ++i) {
        size = map_capacity;
        if (bs->GetMemoryMap(&size, map, &key, &descriptor_size,
                             &descriptor_ver) == EFI_SUCCESS) {

            if (bs->ExitBootServices(img, key) == EFI_SUCCESS) {
                kernel::memory_map = {.buffer = ptr<void>(map),
                                      .size = size,
                                      .descriptor_size = descriptor_size,
                                      .descriptor_version = descriptor_ver};
                kernel::start();
            }
        }
    }

    console_print(sys, u"abort: 16 attempts of clean exit failed");

    // free the allocated pages
    bs->FreePages(EFI_PHYSICAL_ADDRESS(map), EFI_SIZE_TO_PAGES(map_capacity));

    return EFI_ABORTED;
}
