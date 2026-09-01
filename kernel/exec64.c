// kernel/exec64.c — Milestone 6: load a real NEX64 (primary) or ELF64
// (fallback) binary from TxFS64 into the process's image region.
//
// Milestone 8: maps segments into a process's OWN address space
// (paging64_as_t), passed in by the caller -- kernel/paging64.c owns
// all the actual page-table manipulation; this file is now just "parse
// the file format, hand paging64 one page at a time."
//
// Milestone 7: stops at "parsed, mapped" -- entering ring3 is
// kernel/process64.c's job (it owns CR3 activation and the process's
// kernel stack/RSP0).
//
// Milestone 24: the file parse buffer is kmalloc'd/kfree'd per call
// instead of one static file-scope array -- process creation is no
// longer strictly serialized, so two overlapping exec64_load calls
// would otherwise corrupt each other's parse. `max_end` (no longer
// actually needed for anything -- see below) was threaded through as
// an explicit parameter for the same reason.
//
// Milestone 25: uses the generalized paging64 map/unmap/translate API
// instead of the old fixed-2MB-window paging64_map_user_page, and maps
// segments into the new USER_IMAGE_BASE/USER_IMAGE_MAX_SIZE region
// (include/uservm64.h) instead of one shared carve-out that also held
// the heap and stack. The heap no longer starts wherever the image
// happens to end -- it has its own fixed region now, so this file no
// longer computes or returns a heap_start at all (kernel/process64.c
// just calls uservm64_init() with the fixed USER_HEAP_BASE directly).
// PT_LOAD/NEX64 segment flags now genuinely control the mapped page's
// permissions (writable and, via the NX bit, executable) instead of
// only ever tracking writability.
#include <stdint.h>
#include "../include/exec64.h"
#include "../include/nex64.h"
#include "../include/elf64.h"
#include "../include/txfs64.h"
#include "../include/memmap64.h"
#include "../include/paging64.h"
#include "../include/uservm64.h"
#include "../include/physmem64.h"
#include "../include/heap64.h"
#include "../include/klog.h"

#define EXEC64_FILE_MAX  (64u * 1024u)

// Generic segment fields, normalized from either nex64_seg_t or
// elf64_phdr_t before the shared mapping/copy logic below runs.
typedef struct {
    uint64_t vaddr, offset, filesz, memsz, flags;
} seg_t;

static uint32_t perm_flags(uint64_t seg_flags) {
    uint32_t f = 0;
    if (seg_flags & NEX64_PF_W) f |= PAGING64_WRITE;
    if (seg_flags & NEX64_PF_X) f |= PAGING64_EXEC;
    return f;
}

// Ensures every 4KB page covering [vaddr, vaddr+memsz) is mapped in the
// process's own address space -- segments can share a page (e.g.
// .rodata immediately followed by .data), so mapping must be per-page,
// not per-segment. A page already mapped by an EARLIER segment has its
// permissions widened (OR'd) rather than left alone or clobbered, so a
// shared page ends up with the union of whatever any segment covering
// it needs (e.g. writable, if .data's portion needs it, even though
// .rodata's own portion doesn't).
static int ensure_mapped(paging64_as_t* as, uint64_t vaddr, uint64_t memsz, uint64_t seg_flags) {
    uint64_t start = vaddr & ~0xFFFULL;
    uint64_t end   = (vaddr + memsz + 0xFFFULL) & ~0xFFFULL;
    uint32_t flags = perm_flags(seg_flags);

    for (uint64_t page_va = start; page_va < end; page_va += 0x1000ULL) {
        if (page_va < USER_IMAGE_BASE || page_va >= USER_IMAGE_END) {
            klog("exec64: segment runs outside the image region\n");
            return -1;
        }

        if (paging64_is_mapped(as, page_va)) {
            uint32_t existing = paging64_get_perms(as, page_va);
            if (paging64_set_perms(as, page_va, existing | flags) < 0) return -1;
        } else if (paging64_map_new(as, page_va, flags) < 0) {
            klog("exec64: out of pages -- binary too large/fragmented\n");
            return -1;
        }
    }
    return 0;
}

static void copy_segment_data(paging64_as_t* as, const seg_t* seg, const uint8_t* file_data) {
    for (uint64_t i = 0; i < seg->filesz; i++) {
        uint64_t va = seg->vaddr + i;
        // The physical page is already mapped by ensure_mapped -- this
        // is a lookup, not an allocation. Writing through
        // physmem64_to_virt (the kernel's OWN mapping of that physical
        // page) is independent of whatever permissions the USER-mode
        // PTE has (e.g. a read-only/executable .text page) -- the two
        // mappings share the same physical frame but carry entirely
        // separate permission bits.
        uint64_t phys = paging64_translate(as, va);
        uint8_t* kptr = (uint8_t*)physmem64_to_virt(phys);
        *kptr = file_data[seg->offset + i];
    }
}

static int load_segment(paging64_as_t* as, const seg_t* seg, const uint8_t* file_buf_base, uint64_t file_size) {
    if (seg->memsz == 0) return 0;
    if (seg->vaddr < USER_IMAGE_BASE || seg->vaddr + seg->memsz > USER_IMAGE_END) {
        klog("exec64: segment vaddr outside the image region -- refusing to load\n");
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
    return 0;
}

static int finish_mapping(paging64_as_t* as, uint64_t entry, uint64_t* entry_out, uint64_t* stack_top_out) {
    uint64_t stack_page_va = USER_STACK_TOP - 0x1000ULL;
    if (paging64_map_new(as, stack_page_va, PAGING64_WRITE) < 0) {
        klog("exec64: failed to map the user stack page\n");
        return -1;
    }

    *entry_out     = entry;
    *stack_top_out = USER_STACK_TOP;
    return 0;
}

static int load_nex64(paging64_as_t* as, const uint8_t* file_buf, uint32_t file_size,
                       uint64_t* entry_out, uint64_t* stack_top_out) {
    const nex64_header_t* nh = (const nex64_header_t*)file_buf;
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

    const nex64_seg_t* segs = (const nex64_seg_t*)(file_buf + sizeof(nex64_header_t));
    for (uint32_t i = 0; i < nh->seg_count; i++) {
        seg_t seg = { segs[i].vaddr, segs[i].offset, segs[i].filesz, segs[i].memsz, segs[i].flags };
        if (load_segment(as, &seg, file_buf, file_size) < 0) return -1;
    }

    klog("exec64: NEX64 loaded\n");
    return finish_mapping(as, nh->entry, entry_out, stack_top_out);
}

static int load_elf64(paging64_as_t* as, const uint8_t* file_buf, uint32_t file_size,
                       uint64_t* entry_out, uint64_t* stack_top_out) {
    const elf64_header_t* eh = (const elf64_header_t*)file_buf;
    if (file_size < sizeof(elf64_header_t)) {
        klog("exec64: file too small to be an ELF64 header\n");
        return -1;
    }
    if ((uint64_t)eh->phoff + (uint64_t)eh->phnum * eh->phentsize > file_size) {
        klog("exec64: ELF64 program header table runs past end of file\n");
        return -1;
    }

    for (uint16_t i = 0; i < eh->phnum; i++) {
        const elf64_phdr_t* ph = (const elf64_phdr_t*)(file_buf + eh->phoff + (uint64_t)i * eh->phentsize);
        if (ph->type != PT_LOAD64) continue;
        if (ph->memsz == 0) continue;
        if (ph->vaddr + ph->memsz <= USER_IMAGE_BASE) continue;  // linker metadata below the image region

        seg_t seg = { ph->vaddr, ph->offset, ph->filesz, ph->memsz, ph->flags };
        if (load_segment(as, &seg, file_buf, file_size) < 0) return -1;
    }

    klog("exec64: ELF64 loaded (fallback path)\n");
    return finish_mapping(as, eh->entry, entry_out, stack_top_out);
}

int exec64_load(paging64_as_t* as, const char* path, uint64_t* entry_out, uint64_t* stack_top_out) {
    uint64_t size;
    if (txfs64_stat(path, &size) < 0 || size == 0 || size > EXEC64_FILE_MAX) {
        klog("exec64: file missing, empty, or too large for the parse buffer: ");
        klog(path);
        klog("\n");
        return -1;
    }

    // Milestone 24: a fresh buffer per call -- see the file header
    // comment for why this can no longer be one shared static array.
    uint8_t* file_buf = (uint8_t*)kmalloc(EXEC64_FILE_MAX);
    if (!file_buf) {
        klog("exec64: out of memory for parse buffer\n");
        return -1;
    }

    int fd = txfs64_open(path);
    if (fd < 0) {
        klog("exec64: open failed: ");
        klog(path);
        klog("\n");
        kfree(file_buf);
        return -1;
    }

    int n = txfs64_read(fd, file_buf, (uint32_t)size);
    txfs64_close(fd);
    if (n < 0 || (uint64_t)n != size) {
        klog("exec64: short read\n");
        kfree(file_buf);
        return -1;
    }

    if (size < 4) {
        klog("exec64: file too small to identify\n");
        kfree(file_buf);
        return -1;
    }

    int result;
    uint32_t magic = *(uint32_t*)file_buf;
    if (magic == NEX64_MAGIC) {
        result = load_nex64(as, file_buf, (uint32_t)size, entry_out, stack_top_out);
    } else if (magic == ELF64_MAGIC && size >= sizeof(elf64_header_t) &&
               ((const elf64_header_t*)file_buf)->bits == ELFCLASS64 &&
               ((const elf64_header_t*)file_buf)->endian == ELFDATA2LSB &&
               ((const elf64_header_t*)file_buf)->machine == EM_X86_64) {
        result = load_elf64(as, file_buf, (uint32_t)size, entry_out, stack_top_out);
    } else {
        klog("exec64: unrecognized format / wrong architecture -- refusing to load\n");
        result = -1;
    }

    kfree(file_buf);
    return result;
}
