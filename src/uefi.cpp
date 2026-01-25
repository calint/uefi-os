#include <efi.h>

#include "efiprot.h"
#include "kernel.hpp"

extern "C" auto EFIAPI efi_main(EFI_HANDLE img, EFI_SYSTEM_TABLE* sys)
    -> EFI_STATUS {

    serial_print("efi_main\r\n");

    auto bs = sys->BootServices;

    EFI_GUID guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = nullptr;

    if (bs->LocateProtocol(&guid, nullptr, reinterpret_cast<void**>(&gop)) !=
        EFI_SUCCESS) {
        serial_print("failed to get frame buffer\r\n");
        return EFI_ABORTED;
    }

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

    if (bs->ExitBootServices(img, key) != EFI_SUCCESS) {
        serial_print("failed to exit boot service\r\n");
        return EFI_ABORTED;
    }

    kernel_init({.pixels = reinterpret_cast<u32*>(gop->Mode->FrameBufferBase),
                 .width = gop->Mode->Info->HorizontalResolution,
                 .height = gop->Mode->Info->VerticalResolution,
                 .stride = gop->Mode->Info->PixelsPerScanLine},
                {.buffer = reinterpret_cast<void*>(map),
                 .size = size,
                 .descriptor_size = d_size,
                 .descriptor_version = d_ver});

    return EFI_SUCCESS;
}
