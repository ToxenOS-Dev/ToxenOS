#ifndef AHCI64_H
#define AHCI64_H

#include <stdint.h>

// Milestone 28: AHCI/SATA driver for the 64-bit kernel. Ported from
// kernel/ahci.c's design (command list/FIS/command table layout,
// port-enable sequence, polled command issue) but re-targeted at this
// kernel's own physical-memory/MMIO facilities: the ABAR is mapped via
// physmem64_map_mmio() (cache-disabled device MMIO, not the 32-bit
// driver's cacheable identity map), every DMA structure (command list,
// received-FIS area, command table, data buffer) comes from
// physmem64_alloc_pages() (guaranteed physically contiguous -- the
// 32-bit driver got this for free from its identity-mapped BSS statics,
// which doesn't exist here), and every usable port registers itself as
// its own kernel/blockdev64.c device instead of the 32-bit driver's
// single-drive-only design.
//
// Command completion is POLLED (checked every call, no interrupt
// registration) -- see the Milestone 28 summary for why this is an
// explicit, documented, temporary scope decision, not an oversight:
// each poll loop is bounded by an iteration-count timeout (matching
// kernel/ata64.c's own existing precedent) so a wedged or missing
// controller cannot hang the kernel forever, and polling never disables
// interrupts, so the scheduler/keyboard stay responsive while it spins
// (see kernel/txfs64.c's Milestone 28 kmutex64 rewrite for the other
// half of that story).
//
// Scans every AHCI controller PCI enumeration found and registers one
// blockdev64_t per USABLE port (device present, active SATA link, not
// an ATAPI/enclosure signature) as "ahciN.P" (controller N, port P).
// Returns the number of ports registered (0 if no AHCI controller or no
// usable port was found -- not an error, just nothing to do).
int ahci64_init(void);

#endif // AHCI64_H
