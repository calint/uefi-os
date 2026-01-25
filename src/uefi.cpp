#include <efi.h>

#include "kernel.hpp"

FrameBuffer frame_buffer;

extern "C" EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle,
                                      EFI_SYSTEM_TABLE* SystemTable) {
    serial_print("efi_main\r\n");

    EFI_BOOT_SERVICES* bs = SystemTable->BootServices;
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = NULL;
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

    serial_print("get frame buffer\r\n");
    EFI_STATUS status = bs->LocateProtocol(&gop_guid, NULL, (VOID**)&gop);

    if (status == EFI_SUCCESS && gop != NULL) {
        frame_buffer.pixels = (UINT32*)(UINTN)gop->Mode->FrameBufferBase;
        frame_buffer.stride = gop->Mode->Info->PixelsPerScanLine;
        frame_buffer.height = gop->Mode->Info->VerticalResolution;
        frame_buffer.width = gop->Mode->Info->HorizontalResolution;

        UINTN map_size = 0;
        EFI_MEMORY_DESCRIPTOR* map = NULL;
        UINTN map_key;
        UINTN descriptor_size;
        UINT32 descriptor_version;

        bs->GetMemoryMap(&map_size, NULL, &map_key, &descriptor_size,
                         &descriptor_version);
        map_size += (2 * descriptor_size);
        bs->AllocatePool(EfiLoaderData, map_size, (VOID**)&map);
        bs->GetMemoryMap(&map_size, map, &map_key, &descriptor_size,
                         &descriptor_version);

        serial_print("exit boot service\r\n");
        status = bs->ExitBootServices(ImageHandle, map_key);

        if (status == EFI_SUCCESS) {
            extern void kernel_init();
            kernel_init();
        } else {
            serial_print("failed to exit boot service\r\n");
        }
    }

    return EFI_SUCCESS;
}
