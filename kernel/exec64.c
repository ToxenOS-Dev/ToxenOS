// kernel/exec64.c — Milestone 6: load a real NEX64 (primary) or ELF64
// (fallback) binary from TxFS64 into the carved PD_EXEC_IDX region.
//
// Milestone 8: maps segments into a process's OWN address space
// (paging64_as_t), passed in by the caller, instead of one shared global
// page table -- kernel/paging64.c owns all the actual page-table
// manipulation (allocating frames, setting PRESENT/USER/WRITABLE,
// widening an already-mapped page); this file is now just "parse the
// file format, hand paging64 one page at a time."
//
// Milestone 7: stops at "parsed, mapped" -- entering ring3 is
// kernel/userproc64.c's job (it owns CR3 activation and the process's
// kernel stack/RSP0).
#include <stdint.h>
#include "../include/exec64.h"
#include "../include/nex64.h"
#include "../include/elf64.h"
#include "../include/txfs64.h"
#include "../include/memmap64.h"
#include "../include/paging64.h"
#include "../include/klog.h"

#define EXEC64_FILE_MAX  (64u * 1024u)
#define EXEC64_STACK_SLOT 511  // fixed -- segment slots must stay below this

// Single shared parse scratch buffer. Safe only because process loading
// is still strictly serialized (one process is created, loaded, run, and
// exited/faulted before the next is ever started -- see
// kernel/userproc64.c's userproc64_run). The day this kernel gains
// concurrent process creation, this must become per-call (stack or
// pool-allocated) -- it is NOT part of any process's live memory, just
// transient parse state, but two overlapping exec64_load calls would
// corrupt each other's parse here.
static uint8_t file_buf[EXEC64_FILE_MAX];

// Generic segment fields, normalized from either nex64_seg_t or
// elf64_phdr_t before the shared mapping/copy logic below runs.
typedef struct {
    uint64_t vaddr, offset, filesz, memsz, flags;
} seg_t;

static uint64_t g_max_end = 0;  // highest vaddr+memsz seen, for heap_start_out

// Ensures every 4KB page covering [vaddr, vaddr+memsz) is mapped in the
// process's own address space via paging64_map_user_page -- segments can
// share a page (e.g. .rodata immediately following .text), so mapping
// must be per-page, not per-segment; paging64 already handles "already
// mapped, widen to writable" so this loop is just a thin caller.
static int ensure_mapped(paging64_as_t* as, uint64_t vaddr, uint64_t memsz, uint64_t flags) {
    uint64_t start = vaddr & ~0xFFFULL;
    uint64_t end   = (vaddr + memsz + 0xFFF) & ~0xFFFULL;

    for (uint64_t page_va = start; page_va < end; page_va += 0x1000) {
        uint64_t slot = (page_va - USER64_ELF_BASE) / 0x1000;
        if (slot >= EXEC64_STACK_SLOT) {
            klog("exec64: segment runs outside the user region (would clobber the stack slot)\n");
            return -1;
        }

        int writable = (flags & NEX64_PF_W) ? 1 : 0;
        if (paging64_map_user_page(as, page_va, writable) == 0) {
            klog("exec64: out of pages -- binary too large/fragmented\n");
            return -1;
        }
    }
    return 0;
}

static void copy_segment_data(paging64_as_t* as, const seg_t* seg, const uint8_t* file_data) {
    for (uint64_t i = 0; i < seg->filesz; i++) {
        uint64_t va   = seg->vaddr + i;
        uint64_t page_va = va & ~0xFFFULL;
        // Re-resolve via paging64_map_user_page rather than caching the
        // page's phys from ensure_mapped's loop -- it's already mapped at
        // this point so this is just a cheap lookup (no new allocation),
        // and keeps all PT-slot knowledge inside paging64.c.
        uint64_t phys = paging64_map_user_page(as, page_va, (seg->flags & NEX64_PF_W) ? 1 : 0);
        uint8_t* phys_page = phys_to_ptr(phys & ~0xFFFULL);
        phys_page[va & 0xFFF] = file_data[seg->offset + i];
    }
}

static int load_segment(paging64_as_t* as, const seg_t* seg, const uint8_t* file_buf_base, uint64_t file_size) {
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

    if (ensure_mapped(as, seg->vaddr, seg->memsz, seg->flags) < 0) return -1;
    copy_segment_data(as, seg, file_buf_base);

    uint64_t end = (seg->vaddr + seg->memsz + 0xFFFULL) & ~0xFFFULL;
    if (end > g_max_end) g_max_end = end;
    return 0;
}

static int finish_mapping(paging64_as_t* as, uint64_t entry, uint64_t* entry_out,
                           uint64_t* stack_top_out, uint64_t* heap_start_out) {
    uint64_t stack_vaddr = USER64_ELF_BASE + (uint64_t)EXEC64_STACK_SLOT * 0x1000;
    if (paging64_map_user_page(as, stack_vaddr, 1) == 0) {
        klog("exec64: failed to map the user stack page\n");
        return -1;
    }

    *entry_out      = entry;
    *stack_top_out  = USER64_ELF_BASE + (uint64_t)(EXEC64_STACK_SLOT + 1) * 0x1000;
    // Heap plumbing only -- establishes where a future brk/sys_heap would
    // start; no pages are mapped here in Milestone 8.
    *heap_start_out = g_max_end;
    return 0;
}

static int load_nex64(paging64_as_t* as, uint32_t file_size, uint64_t* entry_out,
                       uint64_t* stack_top_out, uint64_t* heap_start_out) {
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
        if (load_segment(as, &seg, file_buf, file_size) < 0) return -1;
    }

    klog("exec64: NEX64 loaded\n");
    return finish_mapping(as, nh->entry, entry_out, stack_top_out, heap_start_out);
}

static int load_elf64(paging64_as_t* as, uint32_t file_size, uint64_t* entry_out,
                       uint64_t* stack_top_out, uint64_t* heap_start_out) {
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
        if (load_segment(as, &seg, file_buf, file_size) < 0) return -1;
    }

    klog("exec64: ELF64 loaded (fallback path)\n");
    return finish_mapping(as, eh->entry, entry_out, stack_top_out, heap_start_out);
}

int exec64_load(paging64_as_t* as, const char* path, uint64_t* entry_out,
                 uint64_t* stack_top_out, uint64_t* heap_start_out) {
    g_max_end = 0;

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
    if (magic == NEX64_MAGIC) return load_nex64(as, (uint32_t)size, entry_out, stack_top_out, heap_start_out);

    if (magic == ELF64_MAGIC && size >= sizeof(elf64_header_t)) {
        elf64_header_t* eh = (elf64_header_t*)file_buf;
        if (eh->bits == ELFCLASS64 && eh->endian == ELFDATA2LSB && eh->machine == EM_X86_64)
            return load_elf64(as, (uint32_t)size, entry_out, stack_top_out, heap_start_out);
    }

    klog("exec64: unrecognized format / wrong architecture -- refusing to load\n");
    return -1;
}
