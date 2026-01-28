#include <efi.h>

#include "acpi.hpp"
#include "kernel.hpp"
#include "x86_64/efibind.h"

static auto guids_equal(const EFI_GUID* g1, const EFI_GUID* g2) -> bool {
    const u64* p1 = reinterpret_cast<const u64*>(g1);
    const u64* p2 = reinterpret_cast<const u64*>(g2);
    return (p1[0] == p2[0]) && (p1[1] == p2[1]);
}

extern "C" auto EFIAPI efi_main(EFI_HANDLE img, EFI_SYSTEM_TABLE* sys)
    -> EFI_STATUS {

    serial_print("efi_main\n");

    auto bs = sys->BootServices;

    //
    // get frame buffer config
    //
    auto graphics_guid = EFI_GUID(EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID);
    auto gop = static_cast<EFI_GRAPHICS_OUTPUT_PROTOCOL*>(nullptr);
    if (bs->LocateProtocol(&graphics_guid, nullptr,
                           reinterpret_cast<void**>(&gop)) != EFI_SUCCESS) {
        serial_print("failed to get frame buffer\n");
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

    auto xsdt = reinterpret_cast<SDTHeader*>(rsdp->xsdt_address);

    // calculate number of pointers in XSDT
    auto entries = (xsdt->length - sizeof(SDTHeader)) / 8;
    auto table_ptrs = reinterpret_cast<u64*>(u64(xsdt) + sizeof(SDTHeader));

    // default keyboard config
    auto kbd_gsi = 1u;   // default to Pin 1
    auto kbd_flags = 0u; // default active high, edge

    // default io_apic and apic config
    io_apic = reinterpret_cast<u32 volatile*>(0xfec00000);
    lapic = reinterpret_cast<u32 volatile*>(0xfee00000);

    // find override values
    for (auto i = 0u; i < entries; ++i) {
        auto header = reinterpret_cast<SDTHeader*>(table_ptrs[i]);
        if (header->signature[0] == 'A' && header->signature[1] == 'P' &&
            header->signature[2] == 'I' && header->signature[3] == 'C') {

            auto madt = reinterpret_cast<MADT*>(header);
            auto p = madt->entries;
            auto end = reinterpret_cast<u8*>(madt) + madt->header.length;

            while (p < end) {
                auto entry = reinterpret_cast<MADT_EntryHeader*>(p);
                if (entry->type == 1) {
                    io_apic = reinterpret_cast<u32 volatile*>(
                        reinterpret_cast<MADT_IOAPIC*>(p)->address);
                } else if (entry->type == 5) {
                    lapic = reinterpret_cast<u32 volatile*>(
                        reinterpret_cast<MADT_LAPIC_Override*>(p)->address);
                } else if (entry->type == 2) { // ISO
                    auto iso = reinterpret_cast<MADT_ISO*>(p);
                    if (iso->source == 1) { // keyboard
                        serial_print("uefi: found keyboard config");
                        kbd_gsi = iso->gsi;
                        // polarity: 3 = active low
                        if ((iso->flags & 0x3) == 0x3) {
                            kbd_flags |= (1 << 13);
                        }
                        // trigger: 3 = level
                        if (((iso->flags >> 2) & 0x3) == 0x3) {
                            kbd_flags |= (1 << 15);
                        }
                    }
                }
                p += entry->length;
            }
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
    auto map = static_cast<EFI_MEMORY_DESCRIPTOR*>(nullptr);

    bs->GetMemoryMap(&size, nullptr, &key, &d_size, &d_ver);
    size += 2 * d_size;

    if (bs->AllocatePool(EfiLoaderData, size, reinterpret_cast<void**>(&map)) !=
        EFI_SUCCESS) {
        serial_print("failed to allocate pool\n");
        return EFI_ABORTED;
    }

    while (bs->GetMemoryMap(&size, map, &key, &d_size, &d_ver) == EFI_SUCCESS) {
        memory_map = {.buffer = reinterpret_cast<void*>(map),
                      .size = size,
                      .descriptor_size = d_size,
                      .descriptor_version = d_ver};
        if (bs->ExitBootServices(img, key) == EFI_SUCCESS) {
            break;
        }
        // if failed then the key was stale due to something like an interrupt
        // that changed the memory map, retry
    }

    //
    // done with uefi information, start kenerl
    //
    kernel_start();
}
