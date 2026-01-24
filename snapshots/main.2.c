#include <stdint.h>

typedef uint64_t efi_status;
typedef void* efi_handle;

// gop guid: 9042a9de-23dc-4a38-96fb-7adde0d08051
typedef struct {
    uint32_t data1; uint16_t data2; uint16_t data3; uint8_t data4[8];
} efi_guid;

typedef struct {
    uint32_t max_mode; uint32_t mode; void *info; uint64_t size_of_info;
    uint64_t frame_buffer_base;
    uint64_t frame_buffer_size;
} efi_graphics_output_protocol_mode;

typedef struct {
    void *query_mode; void *set_mode; void *blt;
    efi_graphics_output_protocol_mode *mode;
} efi_graphics_output_protocol;

typedef struct {
    char hdr[24];
    void *tpl[3]; void *mem[3]; void *handle[2]; void *event[2]; void *free[3]; void *tpl_ext[2];
    // locate_protocol is at offset 320
    efi_status (*locate_protocol)(efi_guid *protocol, void *registration, void **interface);
} efi_boot_services;

typedef struct {
    char hdr[24];
    uint16_t *vendor; uint32_t revision;
    efi_handle con_in_handle; void *con_in;
    efi_handle con_out_handle; void *con_out;
    efi_handle std_err_handle; void *std_err;
    void *runtime_services;
    efi_boot_services *boot_services;
} efi_system_table;

void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

void print_serial(const char *s) {
    while (*s) { outb(0x3F8, *s++); }
}

efi_status efi_main(efi_handle image, efi_system_table *st) {
    print_serial("Locating GOP...\r\n");

    efi_guid gop_guid = {0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xdd, 0xe0, 0xd0, 0x80, 0x51}};
    efi_graphics_output_protocol *gop = 0;

    efi_status status = st->boot_services->locate_protocol(&gop_guid, 0, (void**)&gop);

    if (status != 0 || !gop) {
        print_serial("GOP NOT FOUND!\r\n");
    } else {
        print_serial("GOP FOUND! Plotting...\r\n");
        uint32_t *fb = (uint32_t*)gop->mode->frame_buffer_base;
        // fill screen with white to be unmistakable
        for (uint64_t i = 0; i < 500000; i++) fb[i] = 0xFFFFFFFF;
        print_serial("Done.\r\n");
    }

    while (1) { __asm__("hlt"); }
    return 0;
}
