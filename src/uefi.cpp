#include <efi.h>

#include "kernel.hpp"

FrameBuffer frame_buffer;

extern "C" auto EFIAPI efi_main(EFI_HANDLE img, EFI_SYSTEM_TABLE* sys)
    -> EFI_STATUS {
    serial_print("efi_main\r\n");

    auto* bs{sys->BootServices};
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = nullptr;
    EFI_GUID guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

    // locate gop or fail immediately
    if (bs->LocateProtocol(&guid, nullptr, (void**)&gop) != EFI_SUCCESS) {
        serial_print("failed to get frame buffer\r\n");
        return EFI_ABORTED;
    }

    // initialize global framebuffer using designated initializers
    frame_buffer = {.pixels = (UINT32*)(UINTN)gop->Mode->FrameBufferBase,
                    .width = gop->Mode->Info->HorizontalResolution,
                    .height = gop->Mode->Info->VerticalResolution,
                    .stride = gop->Mode->Info->PixelsPerScanLine};

    // prepare memory map variables
    UINTN size = 0;
    UINTN key = 0;
    UINTN d_size = 0;
    UINT32 d_ver = 0;
    EFI_MEMORY_DESCRIPTOR* map = nullptr;

    // get map size, allocate, and fetch actual map
    bs->GetMemoryMap(&size, nullptr, &key, &d_size, &d_ver);
    size += 2 * d_size;
    bs->AllocatePool(EfiLoaderData, size, (void**)&map);
    bs->GetMemoryMap(&size, map, &key, &d_size, &d_ver);

    serial_print("exit boot service\r\n");

    // exit boot services and jump to kernel if successful
    if (bs->ExitBootServices(img, key) == EFI_SUCCESS) {
        extern auto kernel_init() -> void;
        kernel_init();
    }

    serial_print("failed to exit boot service\r\n");
    return EFI_ABORTED;
}
