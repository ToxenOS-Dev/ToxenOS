#ifndef NEX64_H
#define NEX64_H

#include <stdint.h>

// Milestone 6: 64-bit-native NEX format. Deliberately a SEPARATE magic
// and header from include/nex.h's 32-bit format, not a shared/extended
// struct -- this keeps the 32-bit nex.h/tools/elf2nex.c/kernel/process.c
// loader completely untouched, and "wrong arch" rejection falls out for
// free: a loader that only recognizes NEX64_MAGIC can't misinterpret a
// 32-bit NEX file, and vice versa.
#define NEX64_MAGIC 0x0258454Eu  // 'N' 'E' 'X' 0x02 -- note 0x02, distinct from nex.h's NEX_MAGIC (0x01)

#define NEX64_PF_X 0x1
#define NEX64_PF_W 0x2
#define NEX64_PF_R 0x4

typedef struct {
    uint32_t magic;      // NEX64_MAGIC
    uint32_t version;    // format version, currently 1
    uint32_t seg_count;  // number of nex64_seg_t entries following the header
    uint32_t reserved;   // padding -- keeps `entry` 8-byte aligned
    uint64_t entry;      // entry point (virtual address)
} __attribute__((packed)) nex64_header_t;

typedef struct {
    uint64_t vaddr;
    uint64_t offset;     // offset into file of segment data
    uint64_t filesz;
    uint64_t memsz;
    uint64_t flags;      // NEX64_PF_*
} __attribute__((packed)) nex64_seg_t;

// Milestone 6/8 user address layout: NEX64 segment vaddrs must land in
// the executable-image region. Milestone 25 generalized this from one
// fixed 2MB carve-out into a real per-process VA layout -- see
// include/uservm64.h (USER_IMAGE_BASE/USER_IMAGE_MAX_SIZE) for the
// actual bounds, which kernel/exec64.c and tools/elf2nex64.c both use
// directly now instead of a nex64.h-local alias.

#endif // NEX64_H
