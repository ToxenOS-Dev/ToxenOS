#ifndef MADT64_H
#define MADT64_H

// M+11A: MADT ("APIC" table) parser. Pure, bounds-checked, no hardware.
// Everything read from firmware is untrusted: the table checksum/length
// are validated, every entry's length is validated against BOTH the
// table end and the minimum size for its type, unknown entry types are
// skipped by their own length, and any structural problem fails the
// whole parse (the caller then falls back to PIC) rather than acting on
// partially-trusted data.
#include <stdint.h>

#define MADT64_MAX_LAPIC   64
#define MADT64_MAX_IOAPIC  8
#define MADT64_MAX_ISO     32
#define MADT64_MAX_NMI     16

#define MADT_ERR_NULL        (-1)
#define MADT_ERR_TOO_SHORT   (-2)
#define MADT_ERR_SIGNATURE   (-3)
#define MADT_ERR_LENGTH      (-4)
#define MADT_ERR_CHECKSUM    (-5)
#define MADT_ERR_ENTRY_LEN   (-6)   // entry length < 2 or runs past table end
#define MADT_ERR_ENTRY_SHORT (-7)   // entry shorter than its type's minimum
#define MADT_ERR_ISO_FLAGS   (-8)   // reserved polarity/trigger encoding

#define MADT_LAPIC_ENABLED         0x1u
#define MADT_LAPIC_ONLINE_CAPABLE  0x2u

typedef struct { uint8_t acpi_id, apic_id; uint32_t flags; } madt_lapic_t;
typedef struct { uint8_t id; uint32_t addr; uint32_t gsi_base; } madt_ioapic_t;
typedef struct { uint8_t bus, source; uint32_t gsi; uint16_t flags; } madt_iso_t;
typedef struct { uint8_t acpi_id; uint16_t flags; uint8_t lint; } madt_nmi_t;

typedef struct {
    uint64_t lapic_phys;       // 32-bit header address, replaced by a type-5 override
    uint32_t flags;            // bit0 PCAT_COMPAT (dual 8259 present)
    int      lapic_addr_overridden;
    int n_lapic, n_ioapic, n_iso, n_nmi;
    int truncated;             // entries dropped because a fixed array was full
    int unknown_entries;       // entry types skipped (e.g. x2APIC, GICC)
    madt_lapic_t  lapic[MADT64_MAX_LAPIC];
    madt_ioapic_t ioapic[MADT64_MAX_IOAPIC];
    madt_iso_t    iso[MADT64_MAX_ISO];
    madt_nmi_t    nmi[MADT64_MAX_NMI];
} madt64_info_t;

// Parses `avail` bytes at `table`. 0 on success, MADT_ERR_* otherwise.
int madt64_parse(const void* table, uint64_t avail, madt64_info_t* out);

// MPS/ACPI INTI flag decode. Polarity: 0 = active high, 1 = active low.
// Trigger: 0 = edge, 1 = level. `bus_isa` selects the "conforms to bus"
// (00) default: ISA = edge, active high. Returns 0, or -1 for the
// reserved encoding 10.
int madt64_decode_flags(uint16_t flags, int* polarity, int* trigger);

// ISA IRQ (0-15) -> GSI + polarity/trigger, applying any bus-0 Interrupt
// Source Override; identity/edge/high when none. Returns 0 (always
// succeeds for isa_irq 0-15) or -1 for a bad irq.
int madt64_resolve_isa(const madt64_info_t* m, int isa_irq,
                       uint32_t* gsi, int* polarity, int* trigger);

// LAPIC entry (enabled or online-capable) for an APIC id, or NULL.
const madt_lapic_t* madt64_find_lapic(const madt64_info_t* m, uint32_t apic_id);

#endif // MADT64_H
