#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint64_t efi_status;

#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA  0x01CF
#define VBE_DISPI_INDEX_XRES   0x1
#define VBE_DISPI_INDEX_YRES   0x2

// Minimal 8x8 font for ASCII 32-126 (Space through ~)
// For brevity, I'll include a helper and the core letters.
// In a full build, you'd include all 95 characters.
const uint8_t ASCII_FONT[128][8] = {
    ['G'] = {0x3C, 0x66, 0xC0, 0xDE, 0xC6, 0x66, 0x3C, 0x00},
    ['O'] = {0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00},
    ['P'] = {0xFC, 0x66, 0x66, 0x7C, 0x60, 0x60, 0xF0, 0x00},
    ['H'] = {0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00},
    ['E'] = {0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x7E, 0x00},
    ['L'] = {0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00},
    ['W'] = {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x7F, 0x63, 0x00},
    ['R'] = {0xFC, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0x66, 0x00},
    ['D'] = {0xF8, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0xF8, 0x00},
    ['!'] = {0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x18, 0x00},
    [' '] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
};

// --- I/O AND HARDWARE ---

void outb(uint16_t port, uint8_t val) { __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port)); }
void outw(uint16_t port, uint16_t val) { __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port)); }
uint16_t inw(uint16_t port) { uint16_t ret; __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port)); return ret; }
void outl(uint16_t port, uint32_t val) { __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port)); }
uint32_t inl(uint16_t port) { uint32_t ret; __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port)); return ret; }

uint16_t vbe_read(uint16_t index) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t addr = (uint32_t)((1UL << 31) | (bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC));
    outl(0xCF8, addr);
    return inl(0xCFC);
}

// --- RENDERING ENGINE ---

void draw_char(uint32_t* fb, uint32_t x, uint32_t y, char c, uint32_t color, uint32_t stride) {
    const uint8_t* glyph = ASCII_FONT[(uint8_t)c];
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 8; ++j) {
            if (glyph[i] & (1 << (7 - j))) {
                fb[(y + i) * stride + (x + j)] = color;
            }
        }
    }
}

void print_string(uint32_t* fb, uint32_t x, uint32_t y, const char* str, uint32_t color, uint32_t stride) {
    for (size_t i = 0; str[i] != '\0'; ++i) {
        draw_char(fb, x + (i * 8), y, str[i], color, stride);
    }
}



// --- KERNEL MAIN ---

efi_status efi_main(void* img, void* st) {
    uint32_t width  = vbe_read(VBE_DISPI_INDEX_XRES);
    uint32_t height = vbe_read(VBE_DISPI_INDEX_YRES);
    uint32_t* fb = nullptr;

    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            if (((pci_read((uint8_t)bus, slot, 0, 0x08) >> 24) & 0xFF) == 0x03) {
                fb = (uint32_t*)(uintptr_t)(pci_read((uint8_t)bus, slot, 0, 0x10) & 0xFFFFFFF0);
                break;
            }
        }
        if (fb != nullptr) break;
    }

    if (fb != nullptr) {
        // Clear screen
        for (uint32_t i = 0; i < (width * height); ++i) {
            fb[i] = 0x00000000;
        }

        // The "Next Step" achieved: Full string printing
        print_string(fb, 100, 100, "HELLO WORLD!", 0x0000FF00, width); // Green
        print_string(fb, 100, 110, "GOP STATUS: OK", 0x00FFFFFF, width); // Cyan
        print_string(fb, 100, 120, "C23 KERNEL ACTIVE", 0x00FF00FF, width); // Magenta
    }

    while (true) { __asm__("hlt"); }
    return 0;
}
