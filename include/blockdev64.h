#ifndef BLOCKDEV64_H
#define BLOCKDEV64_H

#include <stdint.h>

// Milestone 28: a generic block-device abstraction so kernel/txfs64.c
// (and, in principle, any future filesystem) can read/write a disk
// without knowing whether it's PIO ATA, AHCI/SATA, NVMe, or VirtIO-blk
// underneath. Drivers register one blockdev64_t per usable disk/
// namespace/port; kernel/txfs64.c is rewritten this milestone to talk
// to whichever one was selected as the root device (see
// blockdev64_select_root()), never to a specific driver directly.
//
// ── Sector-size contract ─────────────────────────────────────────────
// TxFS64's on-disk layout addresses everything in 512-byte LBA units
// (kernel/txfs64.c's TXFS64_LBA_OFFSET and block-to-sector math are
// baked into the existing, unchanged-this-milestone on-disk format).
// This API therefore standardizes on 512-byte LBA units at the
// interface boundary -- exactly like TxFS64 already assumed, and
// exactly what every driver added this milestone (ATA, AHCI, NVMe,
// VirtIO-blk) exposes by default under QEMU without extra
// configuration. `logical_block_size` is reported for diagnostics and
// for a driver's OWN internal translation if its native block size
// differs (e.g. a real NVMe drive formatted with 4Kn LBAs) -- a driver
// whose native block size isn't a divisor of 512 that it can cleanly
// translate should refuse to register rather than silently corrupt
// reads/writes. No driver added this milestone needs that translation:
// QEMU's ide-hd/ich9-ahci+ide-hd/nvme/virtio-blk-pci all default to
// 512-byte logical blocks.
typedef struct blockdev64_s blockdev64_t;

typedef enum {
    BLOCKDEV64_TYPE_ATA = 1,
    BLOCKDEV64_TYPE_AHCI,
    BLOCKDEV64_TYPE_NVME,
    BLOCKDEV64_TYPE_VIRTIO,
} blockdev64_type_t;

typedef struct {
    // Reads/writes `count` 512-byte sectors starting at `lba` into/from
    // `buf`. Returns `count` on success, or -1. `dev->driver_data` is
    // whatever the registering driver stashed there (its own port/
    // namespace/queue state) -- these functions are the only thing
    // kernel/blockdev64.c or kernel/txfs64.c ever calls on it.
    int (*read)(blockdev64_t* dev, uint64_t lba, uint8_t* buf, uint32_t count);
    int (*write)(blockdev64_t* dev, uint64_t lba, const uint8_t* buf, uint32_t count);
} blockdev64_ops_t;

struct blockdev64_s {
    char name[16];                 // stable diagnostic identifier, e.g. "ata0", "ahci0.0", "nvme0n1", "vblk0"
    blockdev64_type_t type;
    uint32_t logical_block_size;   // native block size in bytes, as reported by the device (diagnostics only -- see header comment)
    uint64_t block_count;          // total capacity in 512-byte LBA units
    const blockdev64_ops_t* ops;
    void* driver_data;
    struct blockdev64_s* next;     // registry linkage -- kernel/blockdev64.c owns this field
};

// Registers `dev` (already fully filled in by the caller, including a
// unique `name`) in the global registry. `dev` must remain valid for
// the kernel's lifetime (drivers kmalloc it or use static storage --
// never a stack-local struct). Returns 0, or -1 (duplicate name, or
// registry allocation failure).
int blockdev64_register(blockdev64_t* dev);

// Looks up a previously registered device by name, or NULL.
blockdev64_t* blockdev64_find(const char* name);

// Registry iteration (diagnostics, root-device probing) -- pass NULL to
// start, then each device returned in turn, until NULL.
blockdev64_t* blockdev64_iter(blockdev64_t* prev);

int blockdev64_count(void);

static inline int blockdev64_read(blockdev64_t* dev, uint64_t lba, uint8_t* buf, uint32_t count) {
    return dev->ops->read(dev, lba, buf, count);
}
static inline int blockdev64_write(blockdev64_t* dev, uint64_t lba, const uint8_t* buf, uint32_t count) {
    return dev->ops->write(dev, lba, buf, count);
}

// Logs every registered block device (name, type, logical block size,
// capacity) via klog(). Development use only.
void blockdev64_dump(void);

#endif // BLOCKDEV64_H
