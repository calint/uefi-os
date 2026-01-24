#include <stdint.h>

// basic uefi types
typedef uint64_t efi_status;
typedef void* efi_handle;

// reset types for the poison pill
typedef enum {
    EfiResetCold,
    EfiResetWarm,
    EfiResetShutdown
} efi_reset_type;

// runtime services (contains ResetSystem)
typedef struct {
    char hdr[24];
    void *GetTime, *SetTime, *GetWakeupTime, *SetWakeupTime;
    void *SetVirtualAddressMap, *ConvertPointer;
    void *GetVariable, *GetNextVariableName, *SetVariable;
    void *GetNextHighMonotonicCount;
    void (*ResetSystem)(efi_reset_type, efi_status, uint64_t, uint16_t*);
} efi_runtime_services;

// system table
typedef struct {
    char hdr[24];
    uint16_t *vendor;
    uint32_t revision;
    efi_handle con_in_handle;
    void *con_in;
    efi_handle con_out_handle;
    void *con_out;
    efi_handle std_err_handle;
    void *std_err;
    efi_runtime_services *runtime_services;
} efi_system_table;

// write one byte to serial port
void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

// write a string to serial port
void print_serial(const char *s) {
    while (*s) {
        outb(0x3F8, *s++);
    }
}

efi_status efi_main(efi_handle image, efi_system_table *st) {
    // this will appear in your terminal if using -nographic
    print_serial("EFI EXECUTION CONFIRMED\r\n");

    // poison pill: exit qemu
    st->runtime_services->ResetSystem(EfiResetShutdown, 0, 0, 0);

    return 0;
}
