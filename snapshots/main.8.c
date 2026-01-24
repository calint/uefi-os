#include <efi.h>

// --- SERIAL DEBUG ---
void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

void print_serial(const char* s) {
    while (*s) outb(0x3F8, *s++);
}

void print_hex(UINT64 val) {
    char hex_chars[] = "0123456789ABCDEF";
    print_serial("0x");
    for (int i = 60; i >= 0; i -= 4) {
        outb(0x3F8, hex_chars[(val >> i) & 0xF]);
    }
}

// --- ENTRY POINT ---
// EFIAPI ensures the compiler uses the correct calling convention for UEFI
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    print_serial("\r\n--- KERNEL START (GOP PROBE) ---\r\n");

    EFI_BOOT_SERVICES *bs = SystemTable->BootServices;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

    // Direct call to LocateProtocol using the BootServices table
    // No uefi_call_wrapper needed on modern Clang/GCC
    EFI_STATUS status = bs->LocateProtocol(&gop_guid, NULL, (VOID**)&gop);

    print_serial("LocateProtocol Status: ");
    print_hex(status);
    print_serial("\r\n");

    if (status == EFI_SUCCESS && gop != NULL) {
        UINT64 fb_base = gop->Mode->FrameBufferBase;
        UINT32 stride = gop->Mode->Info->PixelsPerScanLine;
        UINT32 width = gop->Mode->Info->HorizontalResolution;
        UINT32 height = gop->Mode->Info->VerticalResolution;

        print_serial("GOP Found! Base: ");
        print_hex(fb_base);
        print_serial("\r\n");

        if (fb_base != 0) {
            UINT32 *fb = (UINT32*)(UINTN)fb_base;
            // Fill screen with White to prove we have control
            for (UINT32 i = 0; i < (stride * height); i++) {
                fb[i] = 0xFFFFFFFF;
            }
            print_serial("Screen should be WHITE now.\r\n");
        }
    } else {
        print_serial("GOP not found. Status: ");
        print_hex(status);
        print_serial("\r\n");
    }

    while (1) { __asm__("hlt"); }
    return EFI_SUCCESS;
}
