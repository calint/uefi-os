#include <stdint.h>

void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

void print_serial(const char *s) {
    while (*s) { outb(0x3F8, *s++); }
}

uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t addr = (uint32_t)((1UL << 31) | (bus << 16) | (slot << 11) | (func << 8) | (offset & 0xfc));
    outl(0xCF8, addr);
    return inl(0xCFC);
}

void efi_main(void *img, void *st) {
    print_serial("Scanning PCI for VGA BAR...\r\n");

    uint32_t *fb = 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t id = pci_read(bus, slot, 0, 0);
            if (id == 0xFFFFFFFF) continue;

            uint32_t class_info = pci_read(bus, slot, 0, 0x08);
            uint8_t base_class = (class_info >> 24) & 0xFF;

            if (base_class == 0x03) { // display controller
                print_serial("Found VGA Controller! Reading BAR0...\r\n");
                // bar 0 is at offset 0x10
                fb = (uint32_t*)(uint64_t)(pci_read(bus, slot, 0, 0x10) & 0xFFFFFFF0);
                break;
            }
        }
        if (fb) break;
    }

    if (fb) {
        print_serial("Hardware FB found. Plotting Gradient...\r\n");
        for (uint32_t y = 0; y < 600; y++) {
            for (uint32_t x = 0; x < 800; x++) {
                // simple gradient using x and y
                fb[y * 1024 + x] = (x & 0xFF) | ((y & 0xFF) << 8) | (0xAA << 16);
            }
        }
        print_serial("Gradient complete.\r\n");
    }

    while (1) { __asm__("hlt"); }
}
