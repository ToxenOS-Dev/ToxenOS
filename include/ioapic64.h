#ifndef IOAPIC64_H
#define IOAPIC64_H

// M+11A: IOAPIC driver. Register access is IOREGSEL (+0x00) / IOWIN
// (+0x10); the select+data pair is done with interrupts off so an IRQ
// handler touching another route can't clobber IOREGSEL mid-sequence.
// RTE writes program the HIGH dword (destination) first and the LOW
// dword (vector/mask) last, so a route is never live half-configured.
//
// The encode/decode and GSI-range mapping are pure functions (no MMIO)
// and are exercised by the boot self-tests.
#include <stdint.h>
#include "madt64.h"

#define IOAPIC64_MAX          MADT64_MAX_IOAPIC
#define IOAPIC64_REG_ID       0x00
#define IOAPIC64_REG_VER      0x01
#define IOAPIC64_REG_RTE(n)   (0x10 + 2 * (n))
#define IOAPIC64_MAX_ENTRIES  240

typedef struct {
    uint8_t  vector;
    uint8_t  delivery;   // 0 = fixed
    uint8_t  dest_mode;  // 0 = physical
    uint8_t  polarity;   // 0 = active high, 1 = active low
    uint8_t  trigger;    // 0 = edge, 1 = level
    uint8_t  masked;
    uint8_t  dest;       // APIC id (bits 56-63)
    uint8_t  remote_irr; // read-only status
    uint8_t  pending;    // delivery-status, read-only
} ioapic64_rte_t;

uint64_t ioapic64_rte_encode(const ioapic64_rte_t* r);
void     ioapic64_rte_decode(uint64_t raw, ioapic64_rte_t* r);

typedef struct {
    uint8_t  id;
    uint32_t phys;
    uint32_t gsi_base;
    uint32_t nr_entries;   // from VER bits 16-23, +1
    uint8_t  version;
    volatile uint32_t* virt;
} ioapic64_t;

// Pure: index of the IOAPIC whose [gsi_base, gsi_base+nr_entries) holds
// `gsi`, or -1; *pin gets gsi - gsi_base.
int  ioapic64_find(const ioapic64_t* arr, int n, uint32_t gsi, uint32_t* pin);
// Pure: ranges non-empty, in-bounds, and pairwise non-overlapping.
int  ioapic64_ranges_valid(const ioapic64_t* arr, int n);

// Hardware: map + probe every IOAPIC in the MADT and mask ALL of their
// redirection entries. 0 on success, -1 on any failure (unmapped, bogus
// version register, overlapping GSI ranges).
int  ioapic64_init_all_masked(const madt64_info_t* m);

int  ioapic64_route(uint32_t gsi, const ioapic64_rte_t* rte);
int  ioapic64_mask(uint32_t gsi);
int  ioapic64_unmask(uint32_t gsi);
int  ioapic64_read_rte(uint32_t gsi, uint64_t* raw);
extern volatile uint32_t ioapic64_op_count;   // M+11B: count of route/mask/unmask operations
int  ioapic64_count(void);
const ioapic64_t* ioapic64_get(int i);

#endif // IOAPIC64_H
