#include <stdint.h>

typedef uint64_t efi_status;
typedef void* efi_handle;

// minimal reset types
typedef enum {
    EfiResetCold,
    EfiResetWarm,
    EfiResetShutdown
} EFI_RESET_TYPE;

struct efi_system_table;

// the runtime services struct contains the ResetSystem function
typedef struct {
    char             hdr[24]; 
    void             *GetTime;
    void             *SetTime;
    void             *GetWakeupTime;
    void             *SetWakeupTime;
    void             *SetVirtualAddressMap;
    void             *ConvertPointer;
    void             *GetVariable;
    void             *GetNextVariableName;
    void             *SetVariable;
    void             *GetNextHighMonotonicCount;
    // ResetSystem is at offset 80
    void (*ResetSystem)(EFI_RESET_TYPE, efi_status, uint64_t, uint16_t*);
} efi_runtime_services;

typedef struct {
    char                  hdr[24];
    uint16_t              *firmware_vendor;
    uint32_t              firmware_revision;
    efi_handle            con_in_handle;
    void                  *con_in;
    efi_handle            con_out_handle;
    void                  *con_out;
    efi_handle            std_err_handle;
    void                  *std_err;
    efi_runtime_services  *runtime_services; // offset 88
} efi_system_table;

efi_status efi_main(efi_handle image, efi_system_table *st) {
    // if this runs, qemu will quit/reset immediately
    st->runtime_services->ResetSystem(EfiResetShutdown, 0, 0, 0);
    return 0;
}
