#pragma once

#include "kernel.hpp"

// The Root System Description Pointer (RSDP)
struct [[gnu::packed]] RSDP {
    char signature[8];
    u8 checksum;
    char oem_id[6];
    u8 revision;
    u32 rsdt_address;
    u32 length;
    u64 xsdt_address; // 64-bit pointer to the XSDT
    u8 extended_checksum;
    u8 reserved[3];
};

// Generic Header for all System Description Tables (SDTs)
struct [[gnu::packed]] SDTHeader {
    char signature[4];
    u32 length;
    u8 revision;
    u8 checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32 oem_revision;
    u32 creator_id;
    u32 creator_revision;
};

// Multiple APIC Description Table (MADT)
struct [[gnu::packed]] MADT {
    SDTHeader header;
    u32 lapic_address;
    u32 flags;
    u8 entries[]; // Start of variable-length structures
};

// Common header for MADT sub-structures
struct [[gnu::packed]] MADT_EntryHeader {
    u8 type;
    u8 length;
};

// Type 2: Interrupt Source Override
struct [[gnu::packed]] MADT_ISO {
    u8 type;   // 2
    u8 length; // 10
    u8 bus;    // 0 (ISA)
    u8 source; // The IRQ number (1 for keyboard)
    u32 gsi;   // The Global System Interrupt (IO APIC pin)
    u16 flags; // Polarity and Trigger Mode
};

struct [[gnu::packed]] MADT_IOAPIC {
    u8 type, len, id, res;
    u32 address;
    u32 gsi_base;
};

struct [[gnu::packed]] MADT_LAPIC_Override {
    u8 type, len;
    u16 res;
    u64 address;
};
