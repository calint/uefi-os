#include <stdint.h>

void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

void print_serial(const char *s) {
    while (*s) { outb(0x3F8, *s++); }
}

void efi_main(void *img, void *st) {
    print_serial("Bypassing firmware. Searching for VGA memory...\r\n");

    // In QEMU -vga std, the framebuffer is almost always at 0xFD000000 
    // or 0x4000000000 (for 64-bit BARs).
    // Let's try the most common 32-bit BAR location first.
    uint32_t *fb = (uint32_t*)0xFD000000;

    // We'll try to 'blindly' paint. If we hit the wrong memory, 
    // QEMU might crash or just do nothing.
    print_serial("Testing 0xFD000000...\r\n");
    for (uint32_t i = 0; i < 1000000; i++) {
        fb[i] = 0x0000FF00; // Bright Green
    }

    // Fallback guess: 0x80000000
    print_serial("Testing 0x80000000...\r\n");
    uint32_t *fb2 = (uint32_t*)0x80000000;
    for (uint32_t i = 0; i < 1000000; i++) {
        fb2[i] = 0x000000FF; // Bright Blue
    }

    print_serial("Blind write complete. Check the QEMU window.\r\n");

    while (1) { __asm__("hlt"); }
}
