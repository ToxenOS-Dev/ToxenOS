// kernel/exec64.c — Milestone 6: load a real NEX64 (primary) or ELF64
// (fallback) binary from TxFS64 and run it in ring3.
//
// Reuses Milestone 3B's carved-PD-entry technique (kernel/ring3_test64.c)
// exactly, just at a different PD index (PD_EXEC_IDX, not PD_RING3_IDX
// -- the two test modes are mutually exclusive but both must be safe to
// build) and with a real, validated segment table instead of 2
// hardcoded pages. ring3_enter64 (kernel/ring3_test64.asm) is reused
// completely unchanged.
#include <stdint.h>
#include "../include/exec64.h"
#include "../include/nex64.h"
#include "../include/elf64.h"
#include "../include/txfs64.h"
#include "../include/memmap64.h"
#include "../include/klog.h"

#define PAGE_PRESENT  0x001ULL
#define PAGE_WRITABLE 0x002ULL
#define PAGE_USER     0x004ULL

#define PML4_HIGH_IDX 511
#define PDPT_HIGH_IDX 510

#define EXEC64_FILE_MAX  (64u * 1024u)
#define EXEC64_MAX_PAGES 16
#define EXEC64_STACK_SLOT 511  // fixed -- segment slots must stay below this

extern uint64_t pml4[512];
extern uint64_t pdpt_high[512];
extern uint64_t pd[512];
extern void ring3_enter64(uint64_t user_rip, uint64_t user_rsp);

static uint8_t  file_buf[EXEC64_FILE_MAX];
static uint64_t exec64_pt[512] __attribute__((aligned(4096)));
static uint8_t  exec64_pages[EXEC64_MAX_PAGES][4096] __attribute__((aligned(4096)));
static uint8_t  exec64_stack_page[4096] __attribute__((aligned(4096)));
static int      next_page = 0;

static inline uint64_t phys_of(const void* high_half_ptr) {
    return (uint64_t)high_half_ptr - KERNEL_VIRT_BASE64;
}

static inline uint8_t* phys_to_ptr(uint64_t phys) {
    return (uint8_t*)(phys + KERNEL_VIRT_BASE64);
}

// Generic segment fields, normalized from either nex64_seg_t or
// elf64_phdr_t before the shared mapping/copy logic below runs.
typedef struct {
    uint64_t vaddr, offset, filesz, memsz, flags;
} seg_t;

// Ensures every 4KB page covering [vaddr, vaddr+memsz) is mapped in
// exec64_pt, allocating + zeroing a fresh static page on first touch
// and reusing an already-mapped one otherwise -- segments can share a
// page (e.g. .rodata immediately following .text), so mapping must be
// per-page, not per-segment, or a later segment would re-zero and
// destroy an earlier one's data.
static int ensure_mapped(uint64_t vaddr, uint64_t memsz, uint64_t flags) {
    uint64_t start = vaddr & ~0xFFFULL;
    uint64_t end   = (vaddr + memsz + 0xFFF) & ~0xFFFULL;

    for (uint64_t page_va = start; page_va < end; page_va += 0x1000) {
        uint64_t slot = (page_va - USER64_ELF_BASE) / 0x1000;
        if (slot >= EXEC64_STACK_SLOT) {
            klog("exec64: segment runs outside the user region (would clobber the stack slot)\n");
            return -1;
        }

        if (!(exec64_pt[slot] & PAGE_PRESENT)) {
            if (next_page >= EXEC64_MAX_PAGES) {
                klog("exec64: out of static pages -- binary too large/fragmented\n");
                return -1;
            }
            uint8_t* page = exec64_pages[next_page++];
            for (int i = 0; i < 4096; i++) page[i] = 0;

            uint64_t pflags = PAGE_PRESENT | PAGE_USER;
            if (flags & NEX64_PF_W) pflags |= PAGE_WRITABLE;
            exec64_pt[slot] = phys_of(page) | pflags;
        } else if (flags & NEX64_PF_W) {
            // A later overlapping segment needs write access to a page
            // an earlier one mapped read-only -- widen it.
            exec64_pt[slot] |= PAGE_WRITABLE;
        }
    }
    return 0;
}

static void copy_segment_data(const seg_t* seg, const uint8_t* file_data) {
    for (uint64_t i = 0; i < seg->filesz; i++) {
        uint64_t va   = seg->vaddr + i;
        uint64_t slot = (va - USER64_ELF_BASE) / 0x1000;
        uint8_t* phys_page = phys_to_ptr(exec64_pt[slot] & ~0xFFFULL);
        phys_page[va & 0xFFF] = file_data[seg->offset + i];
    }
}

static int load_segment(const seg_t* seg, const uint8_t* file_buf_base, uint64_t file_size) {
    if (seg->memsz == 0) return 0;
    if (seg->vaddr < USER64_ELF_BASE || seg->vaddr + seg->memsz > USER64_ELF_BASE + USER64_ELF_MAX_SIZE) {
        klog("exec64: segment vaddr outside the user region -- refusing to load\n");
        return -1;
    }
    if (seg->filesz > seg->memsz) {
        klog("exec64: segment filesz > memsz -- malformed binary\n");
        return -1;
    }
    if (seg->offset + seg->filesz > file_size) {
        klog("exec64: segment data runs past end of file -- malformed binary\n");
        return -1;
    }

    if (ensure_mapped(seg->vaddr, seg->memsz, seg->flags) < 0) return -1;
    copy_segment_data(seg, file_buf_base);
    return 0;
}

static int finish_mapping_and_enter(uint64_t entry) {
    // Stack: always the fixed last slot, independent of how many
    // segment pages were used -- segments are bounds-checked to stay
    // below EXEC64_STACK_SLOT specifically so they can never collide.
    exec64_pt[EXEC64_STACK_SLOT] = phys_of(exec64_stack_page) | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;

    pd[PD_EXEC_IDX] = phys_of(exec64_pt) | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    // Set unconditionally rather than assumed from ring3_test64.c --
    // the two test modes are mutually exclusive, so that file's init
    // may never have run this boot.
    pdpt_high[PDPT_HIGH_IDX] |= PAGE_USER;
    pml4[PML4_HIGH_IDX]      |= PAGE_USER;

    __asm__ volatile ("mov %0, %%cr3" : : "r"((uint64_t)pml4) : "memory");

    uint64_t stack_top = USER64_ELF_BASE + (uint64_t)(EXEC64_STACK_SLOT + 1) * 0x1000;
    ring3_enter64(entry, stack_top);
    return 0;  // unreachable -- ring3_enter64 ends in iretq
}

static int load_nex64(uint32_t file_size) {
    nex64_header_t* nh = (nex64_header_t*)file_buf;
    if (file_size < sizeof(nex64_header_t)) {
        klog("exec64: file too small to be a NEX64 header\n");
        return -1;
    }
    if (nh->version != 1) {
        klog("exec64: unsupported NEX64 version\n");
        return -1;
    }
    uint64_t seg_table_end = sizeof(nex64_header_t) + (uint64_t)nh->seg_count * sizeof(nex64_seg_t);
    if (seg_table_end > file_size) {
        klog("exec64: NEX64 segment table runs past end of file\n");
        return -1;
    }

    nex64_seg_t* segs = (nex64_seg_t*)(file_buf + sizeof(nex64_header_t));
    for (uint32_t i = 0; i < nh->seg_count; i++) {
        seg_t seg = { segs[i].vaddr, segs[i].offset, segs[i].filesz, segs[i].memsz, segs[i].flags };
        if (load_segment(&seg, file_buf, file_size) < 0) return -1;
    }

    klog("exec64: NEX64 loaded\n");
    return finish_mapping_and_enter(nh->entry);
}

static int load_elf64(uint32_t file_size) {
    elf64_header_t* eh = (elf64_header_t*)file_buf;
    if (file_size < sizeof(elf64_header_t)) {
        klog("exec64: file too small to be an ELF64 header\n");
        return -1;
    }
    if ((uint64_t)eh->phoff + (uint64_t)eh->phnum * eh->phentsize > file_size) {
        klog("exec64: ELF64 program header table runs past end of file\n");
        return -1;
    }

    for (uint16_t i = 0; i < eh->phnum; i++) {
        elf64_phdr_t* ph = (elf64_phdr_t*)(file_buf + eh->phoff + (uint64_t)i * eh->phentsize);
        if (ph->type != PT_LOAD64) continue;
        if (ph->memsz == 0) continue;
        if (ph->vaddr + ph->memsz <= USER64_ELF_BASE) continue;  // linker metadata below the user region

        seg_t seg = { ph->vaddr, ph->offset, ph->filesz, ph->memsz, ph->flags };
        if (load_segment(&seg, file_buf, file_size) < 0) return -1;
    }

    klog("exec64: ELF64 loaded (fallback path)\n");
    return finish_mapping_and_enter(eh->entry);
}

int exec64_load_and_run(const char* path) {
    uint64_t size;
    if (txfs64_stat(path, &size) < 0 || size == 0 || size > EXEC64_FILE_MAX) {
        klog("exec64: file missing, empty, or too large for the static buffer: ");
        klog(path);
        klog("\n");
        return -1;
    }

    int fd = txfs64_open(path);
    if (fd < 0) {
        klog("exec64: open failed: ");
        klog(path);
        klog("\n");
        return -1;
    }

    int n = txfs64_read(fd, file_buf, (uint32_t)size);
    txfs64_close(fd);
    if (n < 0 || (uint64_t)n != size) {
        klog("exec64: short read\n");
        return -1;
    }

    if (size < 4) {
        klog("exec64: file too small to identify\n");
        return -1;
    }

    uint32_t magic = *(uint32_t*)file_buf;
    if (magic == NEX64_MAGIC) return load_nex64((uint32_t)size);

    if (magic == ELF64_MAGIC && size >= sizeof(elf64_header_t)) {
        elf64_header_t* eh = (elf64_header_t*)file_buf;
        if (eh->bits == ELFCLASS64 && eh->endian == ELFDATA2LSB && eh->machine == EM_X86_64)
            return load_elf64((uint32_t)size);
    }

    klog("exec64: unrecognized format / wrong architecture -- refusing to load\n");
    return -1;
}
