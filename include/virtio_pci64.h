#ifndef VIRTIO_PCI64_H
#define VIRTIO_PCI64_H

#include <stdint.h>
#include "pci64.h"

// M+2: modern ("virtio 1.0+") VirtIO-over-PCI transport discovery. This
// file is the ONE place PCI capability bytes are ever interpreted as
// VirtIO structures -- see this header's own comment on
// virtio_pci64_probe() for why nothing else, C or Rust, duplicates this
// parsing (Revision 2's own instruction: one source of truth).
//
// Deliberately NOT here: device status/feature negotiation, virtqueue
// setup, or anything that pokes a register more than once. Those live in
// Rust (rust/toxenos_rs/src/virtio_pci.rs) on top of the already-mapped,
// already-validated regions this file hands back -- see that module's
// own header comment for why the split falls exactly there.
//
// Scope: standard virtio-pci capability types only (COMMON_CFG/
// NOTIFY_CFG/ISR_CFG/DEVICE_CFG/PCI_CFG, values 1-5 per the VirtIO PCI
// Transport specification's struct virtio_pci_cap). No MSI-X vector
// tables are read or configured (M+2 is polling-only -- see this
// milestone's own scope notes). No legacy I/O-BAR transport concepts
// appear here at all; that remains entirely kernel/virtio_blk64.c's own,
// completely separate code path.

// cfg_type values from the VirtIO PCI Transport spec's struct
// virtio_pci_cap (confirmed against Linux's uapi/linux/virtio_pci.h,
// the spec's own reference implementation).
#define VIRTIO_PCI64_CAP_COMMON_CFG 1
#define VIRTIO_PCI64_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI64_CAP_ISR_CFG    3
#define VIRTIO_PCI64_CAP_DEVICE_CFG 4
#define VIRTIO_PCI64_CAP_PCI_CFG    5

// M+2 test-only: the modern-only PCI device ID QEMU's virtio-gpu-pci
// exposes when started with disable-legacy=on (0x1040 + virtio device
// type 16 (GPU) = 0x1050, per the VirtIO spec's device-ID convention --
// NOT the transitional ID kernel/virtio_blk64.c already claims a
// different, unrelated device by). Used ONLY to find a real modern
// device to probe against under QEMU this milestone -- gpu64.c's device
// registry is completely untouched by M+2; nothing here claims this
// device as a GPU, sends it a single VirtIO-GPU command, or registers
// anything with kernel/gpu64.c.
#define PCI64_DEVICE_VIRTIO_GPU_MODERN 0x1050

// One discovered-and-validated capability region, already mapped:
// `mmio_base` is a real kernel virtual address from physmem64_map_mmio
// (never a physical address, never unmapped), valid for exactly
// `length` bytes from that base. `present` is 0 if this capability
// wasn't found (DEVICE_CFG and PCI_CFG are optional per spec) or failed
// validation (bad BAR index/type, offset+length overflow or doesn't fit
// the BAR, truncated cap_len) -- every other field is meaningless when
// `present == 0`.
typedef struct {
    uint32_t present;
    uint64_t mmio_base;
    uint32_t length;
    uint32_t notify_off_multiplier; // NOTIFY_CFG only; 0 for every other kind
} virtio_pci64_region_t;

typedef struct {
    uint32_t ok; // 1 iff common+notify+isr were all found, validated, and mapped -- device_cfg may legitimately be absent
    virtio_pci64_region_t common;
    virtio_pci64_region_t notify;
    virtio_pci64_region_t isr;
    virtio_pci64_region_t device;   // present may be 0 -- optional per spec (a device with no device-specific config)
    virtio_pci64_region_t pci_cfg;  // discovered only, per this milestone's own scope note -- never mapped or used as a config-space-window fallback
} virtio_pci64_transport_info_t;

// Walks `dev`'s PCI capability list (pci64_find_capability) for every
// PCI64_CAP_ID_VENDOR_SPECIFIC entry, reads and validates each one's
// virtio_pci_cap header (bar index in range and genuinely an MMIO BAR
// present on this device, offset+length arithmetic checked for overflow
// BEFORE it's used, offset+length fits entirely inside the BAR's own
// size, cap_len not truncated by the end of config space), keeps the
// FIRST occurrence of each cfg_type (real devices never advertise more
// than one of a given kind), and maps every validated region via
// physmem64_map_mmio (idempotent per 2MB chunk -- safe to call once per
// capability even when several share the same underlying BAR, as
// QEMU's virtio-gpu-pci does by default). An unrecognized cfg_type (any
// value outside 1-5) is logged and skipped, never treated as an error --
// a future spec revision or device may add one this code doesn't know
// about yet.
//
// Returns 0 with `*out` filled (check `out->ok`) if `dev` has a usable
// modern transport, or -1 if `dev` has no vendor-specific capabilities
// at all (a legacy-only or non-VirtIO device). A malformed/incomplete
// modern transport is NOT a -1 return -- it's a 0 return with
// `out->ok == 0`, so a caller can distinguish "not a VirtIO device"
// from "is one, but its modern transport is broken/absent" (e.g. a
// legacy-only transitional device with no capability list at all falls
// into the first case; a modern device missing NOTIFY_CFG falls into
// the second).
int virtio_pci64_probe(pci64_device_t* dev, virtio_pci64_transport_info_t* out);

// Logs every discovered/validated region via klog(). Development use only.
void virtio_pci64_dump(const virtio_pci64_transport_info_t* info);

// Runs this file's own self-test (the pure bounds-check arithmetic that
// can be exercised without a real device -- see kernel/virtio_pci64.c's
// own header comment on what this does and doesn't cover). Returns 1 if
// every case passed.
int virtio_pci64_selftest(void);

#endif // VIRTIO_PCI64_H
