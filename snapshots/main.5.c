#include <stdint.h>
#include <stddef.h>

// compatibility defines for C23 keywords
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#define nullptr ((void*)0)
#define bool _Bool
#define true 1
#define false 0
#endif

typedef uint64_t efi_status;

// VBE Dispi Interface Ports
#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA  0x01CF
#define VBE_DISPI_INDEX_XRES   0x1
#define VBE_DISPI_INDEX_YRES   0x2
#define VBE_DISPI_INDEX_BPP    0x3

// 8x8 font bitmaps
const uint8_t FONT_G[8] = { 0x3C, 0x66, 0xC0, 0xDE, 0xC6, 0x66, 0x3C, 0x00 };
const uint8_t FONT_O[8] = { 0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00 };
const uint8_t FONT_P[8] = { 0xFC, 0x66, 0x66, 0x7C, 0x60, 0x60, 0xF0, 0x00 };

// --- I/O HELPERS ---

void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// --- VBE HELPERS ---

void vbe_write(uint16_t index, uint16_t value) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, value);
}

uint16_t vbe_read(uint16_t index) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

// --- PCI & GRAPHICS ---

void print_serial(const char* s) {
    while (*s) { outb(0x3F8, *s++); }
}

uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t addr = (uint32_t)((1UL << 31) | (bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC));
    outl(0xCF8, addr);
    return inl(0xCFC);
}

void draw_char(uint32_t* fb, uint32_t x, uint32_t y, const uint8_t* bitmap, uint32_t color, uint32_t stride) {
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 8; ++j) {
            if (bitmap[i] & (1 << (7 - j))) {
                fb[(y + i) * stride + (x + j)] = color;
            }
        }
    }
}



efi_status efi_main(void* img, void* st) {
    print_serial("C23 Kernel: Probing Hardware...\r\n");

    // Get true hardware resolution
    uint32_t width  = vbe_read(VBE_DISPI_INDEX_XRES);
    uint32_t height = vbe_read(VBE_DISPI_INDEX_YRES);
    
    // Locate Framebuffer via PCI
    uint32_t* fb = nullptr;
    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            uint32_t class_info = pci_read((uint8_t)bus, slot, 0, 0x08);
            if (((class_info >> 24) & 0xFF) == 0x03) {
                fb = (uint32_t*)(uintptr_t)(pci_read((uint8_t)bus, slot, 0, 0x10) & 0xFFFFFFF0);
                break;
            }
        }
        if (fb != nullptr) break;
    }

    if (fb != nullptr) {
        // Stride logic: In QEMU -vga std, the stride is usually equal to width at 32bpp
        uint32_t stride = width; 

        // Clear screen
        for (uint32_t i = 0; i < (stride * height); ++i) {
            fb[i] = 0x00000000;
        }

        // Draw text with dynamic stride to fix skewing
        draw_char(fb, 100, 100, FONT_G, 0xFFFFFFFF, stride);
        draw_char(fb, 110, 100, FONT_O, 0xFFFFFFFF, stride);
        draw_char(fb, 120, 100, FONT_P, 0xFFFFFFFF, stride);
        
        print_serial("Resolution detected and graphics rendered.\r\n");
    }

    while (true) { __asm__("hlt"); }
    return 0;
}
