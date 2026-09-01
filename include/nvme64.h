#ifndef NVME64_H
#define NVME64_H

#include <stdint.h>

// Milestone 28: NVMe driver for the 64-bit kernel. Ported from
// kernel/nvme.c's design (admin/IO queue setup, doorbell stride from
// CAP, identify-then-create-queues sequence) but re-targeted at this
// kernel's own facilities: BAR0 is mapped via physmem64_map_mmio()
// (cache-disabled MMIO), every queue/DMA buffer comes from
// physmem64_alloc_pages() (physically contiguous, real addresses --
// not the 32-bit driver's identity-mapped-BSS-minus-KERNEL_VIRT_BASE
// shortcut), and every namespace registers itself as its own
// kernel/blockdev64.c device.
//
// Per-command transfers are capped at 8 sectors (4096 bytes = exactly
// one page) so PRP1 alone ever describes a transfer's DMA buffer --
// deliberately avoiding PRP-list support, since kernel/txfs64.c never
// asks for more than 8 sectors (its own block size) in one call anyway
// (see kernel/blockdev64.h's sector-size contract); a caller requesting
// more loops this driver's read/write over multiple 8-sector commands
// rather than needing a PRP list built. Documented as a scope choice,
// not a missing feature nothing here currently needs.
//
// Completion queues are POLLED (phase-tag spin, bounded by an
// iteration-count timeout) -- no interrupt/MSI-X registration. See
// include/ahci64.h's header comment for why this is this milestone's
// deliberate, documented choice for every storage driver added.
//
// Scans every NVMe controller PCI enumeration found, initializes its
// admin queue, creates one I/O queue pair per controller, identifies
// the controller (to learn its namespace count) and each active
// namespace (to learn its real LBA size/count -- see
// include/blockdev64.h if a namespace's native block size isn't 512),
// and registers each as "nvmeCnN" (controller C, namespace N). Returns
// the number of namespaces registered (0 if no NVMe controller, or none
// usable, was found -- not an error).
int nvme64_init(void);

#endif // NVME64_H
