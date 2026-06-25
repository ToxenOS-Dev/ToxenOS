#ifndef NEX64_H
#define NEX64_H

#include <stdint.h>
#include "memmap64.h"

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

// Milestone 6/8 user address layout: every process gets its OWN carved-out
// 2MB PD entry at this same fixed index (kernel/paging64.c gives each
// process a private PD_EXEC_IDX page table, not a shared one -- see
// paging64_create_as). PD_EXEC_IDX must differ from kernel/ring3_test64.c's
// PD_RING3_IDX (31, Milestones 3B/5) so the two carve-outs never collide --
// they are mutually exclusive test modes, but both must be safe to build
// even if only one runs in a given boot.
#define PD_EXEC_IDX 30
#define USER64_ELF_BASE (KERNEL_VIRT_BASE64 + (uint64_t)PD_EXEC_IDX * 0x200000ULL)
// Top of the carved 2MB region, minus the last page (reserved for the
// stack at PT slot 511) -- segment vaddrs must stay below this. Must
// stay in sync with the Makefile's USER64_ELF_BASE literal (used for
// -Ttext=, since Make can't evaluate this expression) and with
// tools/elf2nex64.c's own copy of this same bound.
#define USER64_ELF_MAX_SIZE 0x1FF000ULL

#endif // NEX64_H
