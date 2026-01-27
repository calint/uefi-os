#include <efi.h>

#include "acpi.hpp"
#include "kernel.hpp"

static auto guids_equal(const EFI_GUID* g1, const EFI_GUID* g2) -> bool {
    const u64* p1 = reinterpret_cast<const u64*>(g1);
    const u64* p2 = reinterpret_cast<const u64*>(g2);
    return (p1[0] == p2[0]) && (p1[1] == p2[1]);
}

extern "C" auto EFIAPI efi_main(EFI_HANDLE img, EFI_SYSTEM_TABLE* sys)
    -> EFI_STATUS {

    serial_print("efi_main\r\n");

    auto bs = sys->BootServices;

    EFI_GUID graphics_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = nullptr;

    if (bs->LocateProtocol(&graphics_guid, nullptr,
                           reinterpret_cast<void**>(&gop)) != EFI_SUCCESS) {
        serial_print("failed to get frame buffer\r\n");
        return EFI_ABORTED;
    }
    frame_buffer = {.pixels =
                        reinterpret_cast<u32*>(gop->Mode->FrameBufferBase),
                    .width = gop->Mode->Info->HorizontalResolution,
                    .height = gop->Mode->Info->VerticalResolution,
                    .stride = gop->Mode->Info->PixelsPerScanLine};

    // make keyboard config
    RSDP* rsdp = nullptr;
    EFI_GUID acpi_20_guid = ACPI_20_TABLE_GUID;
    for (UINTN i = 0; i < sys->NumberOfTableEntries; ++i) {
        if (guids_equal(&sys->ConfigurationTable[i].VendorGuid,
                        &acpi_20_guid)) {
            rsdp =
                reinterpret_cast<RSDP*>(sys->ConfigurationTable[i].VendorTable);
            break;
        }
    }

    serial_print("rsdp: ");
    serial_print_hex(u64(rsdp));
    serial_print("\r\n");

    auto xsdt = reinterpret_cast<SDTHeader*>(rsdp->xsdt_address);

    // calculate number of pointers in XSDT
    u32 entries = (xsdt->length - sizeof(SDTHeader)) / 8;
    u64* table_ptrs = reinterpret_cast<u64*>(u64(xsdt) + sizeof(SDTHeader));

    u32 kbd_gsi = 1;   // default to Pin 1
    u32 kbd_flags = 0; // default active high, edge

    for (u32 i = 0; i < entries; ++i) {
        auto header = reinterpret_cast<SDTHeader*>(table_ptrs[i]);
        if (header->signature[0] == 'A' && header->signature[1] == 'P' &&
            header->signature[2] == 'I' && header->signature[3] == 'C') {

            auto madt = reinterpret_cast<MADT*>(header);
            u8* p = madt->entries;
            u8* end = reinterpret_cast<u8*>(madt) + madt->header.length;

            while (p < end) {
                auto entry = reinterpret_cast<MADT_EntryHeader*>(p);
                if (entry->type == 2) { // ISO
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

    // make memory map
    UINTN size = 0;
    UINTN key = 0;
    UINTN d_size = 0;
    UINT32 d_ver = 0;
    EFI_MEMORY_DESCRIPTOR* map = nullptr;

    bs->GetMemoryMap(&size, nullptr, &key, &d_size, &d_ver);
    size += 2 * d_size;

    if (bs->AllocatePool(EfiLoaderData, size, reinterpret_cast<void**>(&map)) !=
        EFI_SUCCESS) {
        serial_print("failed to allocate pool\r\n");
        return EFI_ABORTED;
    }

    if (bs->GetMemoryMap(&size, map, &key, &d_size, &d_ver) != EFI_SUCCESS) {
        serial_print("failed to get memory map\r\n");
        return EFI_ABORTED;
    }
    memory_map = {.buffer = reinterpret_cast<void*>(map),
                  .size = size,
                  .descriptor_size = d_size,
                  .descriptor_version = d_ver};

    if (bs->ExitBootServices(img, key) != EFI_SUCCESS) {
        serial_print("failed to exit boot service\r\n");
        return EFI_ABORTED;
    }

    kernel_start();

    return EFI_SUCCESS;
}
