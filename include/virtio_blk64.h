#ifndef VIRTIO_BLK64_H
#define VIRTIO_BLK64_H

#include <stdint.h>

// Milestone 28: VirtIO block driver for the 64-bit kernel, using the
// LEGACY VirtIO PCI transport (I/O-BAR-based, fixed register layout) --
// a deliberate choice, not a default: QEMU's virtio-blk-pci device is
// "transitional" by default (device ID 0x1001, exposing BOTH the
// legacy I/O-BAR interface AND the modern capability-list interface
// simultaneously), so a legacy driver works against an UNMODIFIED
// `-device virtio-blk-pci` line with no extra flags, and the register
// layout is a small, fixed set of offsets rather than a PCI-capability
// walk to locate common/notify/ISR/device config structures in
// separate BARs. Modern-transport support (needed for a real
// `disable-legacy=on` device, i.e. most real hardware) is explicitly
// out of scope this milestone -- see the Milestone 28 summary.
//
// Feature negotiation accepts nothing (Guest Features = 0) -- the
// simplest possible legacy virtio-blk driver, sufficient for plain
// block reads/writes; no VIRTIO_BLK_F_* feature (multi-queue, disard,
// flush, etc.) is used or required for the queue layout/request format
// this driver expects.
//
// One request is ever outstanding at a time (submit, notify, poll the
// used ring for exactly one new entry, done) -- see
// include/ahci64.h's header comment for why polling is this
// milestone's deliberate, documented choice for every storage driver.
//
// Scans every legacy/transitional virtio-blk PCI device found and
// registers each as "vblkN". Returns the number registered (0 if none
// found or usable -- not an error).
int virtio_blk64_init(void);

#endif // VIRTIO_BLK64_H
