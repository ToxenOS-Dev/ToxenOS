// kernel/physmem64.c — Milestone 23: real physical memory manager.
//
// Replaces Milestone 8's fixed 64-page/256KB static pool with an
// allocator built from the Multiboot2 memory map: every byte range GRUB
// reports as type=1 (available), with our own reservations (kernel
// image, the Multiboot2 info block itself, the framebuffer LFB) carved
// back out, becomes one or more "regions" -- physically contiguous,
// independently bitmap-tracked spans of allocatable RAM. Regions are
// NOT assumed to be contiguous with each other or to start at any
// particular address; a machine with a RAM hole (or, on real hardware,
// a split below/above the 4GB MMIO window) just ends up with more than
// one region.
//
// ── Direct-map bootstrap problem ────────────────────────────────────
// Two chicken-and-egg problems have to be solved before this allocator
// can hand out its first page:
//
// 1. Every region's own bitmap has to live SOMEWHERE, and that
//    somewhere has to scale with detected RAM, not a guess sized for
//    today's test machine. Solution: each region self-hosts its own
//    bitmap in its own leading pages (pre-marked allocated at region
//    creation, never freed) -- no separate fixed-size bookkeeping
//    arena is needed at all, and the bitmap naturally grows with the
//    region it describes.
//
// 2. To WRITE that bitmap (or any other page this allocator will ever
//    hand out) the kernel needs a dereferenceable pointer for its
//    physical address. boot64.asm's identity map only covers the first
//    64MB, and the existing high-half mapping (memmap64.h's
//    phys_to_ptr()/phys_of()) is reserved for the kernel IMAGE's own
//    load address relationship -- kernel/ring3_test64.c depends on that
//    exact formula and is never modified, so it cannot be repurposed.
//    Solution: a brand new, dedicated direct-map window, built into
//    pdpt_high[0..509] (slot 510 is the kernel image, already spoken
//    for -- see boot64.asm/paging64.c). This reuses the pdpt_high page
//    boot64.asm already allocated (just fills in more of its otherwise
//    all-zero entries) and gives up to 510GB of linear PA->VA space,
//    far more headroom than this OS will need for the foreseeable
//    future, without requiring a new top-level PML4 entry.
//
//    Building THAT mapping needs fresh physical pages for its own PD
//    tables, and the general allocator obviously can't supply those
//    before it exists -- so a tiny dedicated bootstrap pool (BSS, part
//    of the kernel image, always reachable via the existing
//    kernel-image-relative phys_to_ptr()) supplies PD-table pages
//    during physmem64_init() only. One PD table is needed per 1GB of
//    RAM beyond the first (RAM within the first 1GB needs none -- it
//    piggybacks on the one PD table always allocated for pdpt_high[0]);
//    the pool is sized generously (see PHYSMEM64_BOOTSTRAP_PAGES) and
//    any pages left over afterward are simply never reclaimed (a few
//    hundred KB, permanently, in the pathological case of very little
//    RAM beyond 1GB -- negligible and documented, not worth the extra
//    complexity of folding leftovers back into a region).
//
// ── Cross-process visibility of the direct map ──────────────────────
// kernel/exec64.c's copy_segment_data() calls physmem64_to_virt() on a
// freshly allocated user page WHILE STILL RUNNING UNDER THE CALLER's
// CR3 (userproc64_run() only switches to the new process's own address
// space after exec64_load() returns -- see kernel/userproc64.c). For a
// nested sys_spawn, "the caller" is another process's own page tables,
// which by default would NOT include this new direct-map window --
// kernel/paging64.c's paging64_create_as() builds every process's
// pdpt_high from scratch and previously only populated slot 510 (the
// kernel image). It now also copies pdpt_high[0..509] (this direct map)
// into every process's own table, by value, exactly like it already
// does for the flat kernel PD entries -- so physmem64_to_virt() resolves
// correctly no matter whose CR3 happens to be active. See
// kernel/paging64.c for that half of this milestone's change.
#include <stdint.h>
#include "../include/physmem64.h"
#include "../include/memmap64.h"
#include "../include/klog.h"

// ── boot64.asm's static, identity-mapped (VA==PA) page tables ───────
extern uint64_t pdpt_high[512];

// Kernel image end, from linker64.ld -- a genuine kernel-image symbol,
// so memmap64.h's phys_of() (kernel-image-relative, NOT this file's own
// direct map) is the correct tool to find its physical address.
extern uint8_t kernel64_end;

#define PT_PRESENT  0x001ULL
#define PT_WRITABLE 0x002ULL
#define PT_HUGE_2M  0x080ULL
// Milestone 28: same bit lapic64.c's own one-off MMIO mapping already
// uses for the LAPIC's page (PAGE_CD, bit 4) -- disables caching for a
// PD-level 2MB huge-page entry. Never set for the RAM-backed direct-map
// entries ensure_directmap() populates during physmem64_init().
#define PT_CACHE_DISABLE 0x010ULL

// pdpt_high[510] is the kernel image (boot64.asm/paging64.c); slots
// 0..509 are free for this file to claim as the physical direct map.
#define DIRECTMAP_PDPT_LIMIT 510ULL
// VA base of pdpt_high[0] within PML4[511] -- i.e. KERNEL_VIRT_BASE64
// (== pdpt_high[510]'s own base) minus 510 * 1GB. A physical address
// `p` is reachable at PHYS_DIRECTMAP_BASE + p once its 1GB-aligned
// chunk has a PD table wired into the matching pdpt_high[] slot.
#define PHYS_DIRECTMAP_BASE (KERNEL_VIRT_BASE64 - DIRECTMAP_PDPT_LIMIT * 0x40000000ULL)

// ── Bootstrap-only page source (see header comment above) ───────────
#define PHYSMEM64_BOOTSTRAP_PAGES 96 // headroom for ~96GB of RAM beyond the first 1GB
static uint8_t bootstrap_pool[PHYSMEM64_BOOTSTRAP_PAGES][4096] __attribute__((aligned(4096)));
static int bootstrap_next = 0;

static uint64_t bootstrap_alloc_page(void) {
    if (bootstrap_next >= PHYSMEM64_BOOTSTRAP_PAGES) return 0;
    uint8_t* page = bootstrap_pool[bootstrap_next++];
    for (int i = 0; i < 4096; i++) page[i] = 0;
    return phys_of(page);
}

// Ensures every 2MB-aligned chunk covering [phys_start, phys_end) has a
// present PD entry in the direct-map window, allocating new PD tables
// (from the bootstrap pool) only for 1GB chunks not already mapped.
// Idempotent -- safe to call again for a range that overlaps one
// already mapped (an already-present entry's cache attribute is left
// as it was, see physmem64_map_mmio's doc comment). Returns 0 on
// success, -1 if the direct map's 510GB budget or the bootstrap pool is
// exhausted. `cache_disable` selects PT_CACHE_DISABLE on newly-created
// entries only -- physmem64_init's own RAM-mapping calls always pass 0;
// Milestone 28's physmem64_map_mmio passes 1.
static int ensure_directmap_ex(uint64_t phys_start, uint64_t phys_end, int cache_disable) {
    uint64_t start = phys_start & ~0x1FFFFFULL;
    uint64_t end   = (phys_end + 0x1FFFFFULL) & ~0x1FFFFFULL;
    int touched = 0;

    for (uint64_t addr = start; addr < end; addr += 0x200000ULL) {
        uint64_t gb_idx = addr >> 30;
        uint32_t pd_idx = (uint32_t)((addr >> 21) & 0x1FF);

        if (gb_idx >= DIRECTMAP_PDPT_LIMIT) {
            klog("physmem64: direct map exhausted -- range extends beyond the supported window\n");
            return -1;
        }

        if (!(pdpt_high[gb_idx] & PT_PRESENT)) {
            uint64_t pd_phys = bootstrap_alloc_page();
            if (!pd_phys) {
                klog("physmem64: bootstrap pool exhausted while extending the direct map\n");
                return -1;
            }
            pdpt_high[gb_idx] = pd_phys | PT_PRESENT | PT_WRITABLE;
        }

        uint64_t* pd_ptr = (uint64_t*)phys_to_ptr(pdpt_high[gb_idx] & ~0xFFFULL);
        if (!(pd_ptr[pd_idx] & PT_PRESENT)) {
            uint64_t flags = PT_PRESENT | PT_WRITABLE | PT_HUGE_2M;
            if (cache_disable) flags |= PT_CACHE_DISABLE;
            pd_ptr[pd_idx] = addr | flags;
            touched = 1;
        }
    }

    if (touched) {
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    }
    return 0;
}

static int ensure_directmap(uint64_t phys_start, uint64_t phys_end) {
    return ensure_directmap_ex(phys_start, phys_end, 0);
}

void* physmem64_to_virt(uint64_t phys) {
    return (void*)(phys + PHYS_DIRECTMAP_BASE);
}

uint64_t physmem64_to_phys(const void* virt) {
    return (uint64_t)(uintptr_t)virt - PHYS_DIRECTMAP_BASE;
}

void* physmem64_map_mmio(uint64_t phys, uint64_t size) {
    if (size == 0) return 0;
    if (ensure_directmap_ex(phys, phys + size, 1) < 0) return 0;
    return physmem64_to_virt(phys);
}

// ── Regions ──────────────────────────────────────────────────────────
#define PHYSMEM64_MAX_REGIONS 32

typedef struct {
    uint64_t base;         // physical, page-aligned
    uint64_t page_count;   // total pages, INCLUDING the leading bitmap_pages
    uint64_t bitmap_pages; // leading pages permanently consumed by this region's own bitmap
    uint64_t free_count;   // currently free pages (excludes bitmap_pages)
    uint64_t alloc_hint;   // next page index to start scanning from
    uint8_t* bitmap;       // 1 bit/page: 1 = allocated (or permanently reserved), 0 = free
} physmem64_region_t;

static physmem64_region_t regions[PHYSMEM64_MAX_REGIONS];
static int num_regions = 0;

static uint64_t stat_usable_ram_bytes = 0;
static uint64_t stat_kernel_bytes = 0;   // == kernel/low-memory reservation end (base is always 0)
static uint64_t stat_mbinfo_base = 0;
static uint64_t stat_mbinfo_bytes = 0;
static uint64_t stat_fb_base = 0;
static uint64_t stat_fb_bytes = 0;

// Milestone 24: allocations/frees must not be interrupted and re-entered
// by another scheduled context now that the timer can genuinely preempt
// into a DIFFERENT process (or the kernel-task equivalent) mid-call.
// Every current call site already runs with IF=0 regardless (int 0x80
// and IRQ gates are interrupt gates, which hardware-clear IF on entry,
// and this kernel is single-core, so there is nothing to protect
// against yet in practice) -- this is explicit, load-bearing defense in
// depth rather than a currently-observable bug, and is what a future
// change that ever re-enables interrupts mid-syscall would actually
// need to keep this allocator safe.
static inline uint64_t physmem64_lock(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void physmem64_unlock(uint64_t flags) {
    if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
}

static inline int bit_is_set(const uint8_t* bm, uint64_t i) { return (bm[i >> 3] >> (i & 7)) & 1; }
static inline void bit_set(uint8_t* bm, uint64_t i)   { bm[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static inline void bit_clear(uint8_t* bm, uint64_t i) { bm[i >> 3] &= (uint8_t)~(1u << (i & 7)); }

static physmem64_region_t* find_region(uint64_t phys) {
    for (int i = 0; i < num_regions; i++) {
        if (phys >= regions[i].base && phys < regions[i].base + regions[i].page_count * 4096ULL)
            return &regions[i];
    }
    return 0;
}

// Finds `count` consecutive clear bits in [from, to). Returns the
// starting page index, or -1.
static int64_t scan_range(const physmem64_region_t* r, uint64_t from, uint64_t to, uint64_t count) {
    uint64_t run_start = from, run_len = 0;
    for (uint64_t i = from; i < to; i++) {
        if (bit_is_set(r->bitmap, i)) {
            run_len = 0;
            run_start = i + 1;
            continue;
        }
        run_len++;
        if (run_len == count) return (int64_t)run_start;
    }
    return -1;
}

static int64_t region_find_run(const physmem64_region_t* r, uint64_t count) {
    int64_t found = scan_range(r, r->alloc_hint, r->page_count, count);
    if (found >= 0) return found;
    return scan_range(r, 0, r->alloc_hint, count);
}

static void add_region(uint64_t base, uint64_t len) {
    if (num_regions >= PHYSMEM64_MAX_REGIONS) return;

    uint64_t end = base + len;
    base = (base + 0xFFFULL) & ~0xFFFULL;
    end  = end & ~0xFFFULL;
    if (end <= base) return; // nothing left after page-alignment

    uint64_t page_count   = (end - base) / 4096ULL;
    uint64_t bitmap_bytes = (page_count + 7) / 8;
    uint64_t bitmap_pages = (bitmap_bytes + 4095) / 4096;
    if (bitmap_pages >= page_count) return; // too small to even host its own bitmap

    if (ensure_directmap(base, end) < 0) {
        klog("physmem64: dropping a region that could not be direct-mapped\n");
        return;
    }

    uint8_t* bitmap = (uint8_t*)physmem64_to_virt(base);
    for (uint64_t i = 0; i < bitmap_bytes; i++) bitmap[i] = 0;
    for (uint64_t i = 0; i < bitmap_pages; i++) bit_set(bitmap, i);

    physmem64_region_t* r = &regions[num_regions++];
    r->base         = base;
    r->page_count   = page_count;
    r->bitmap_pages = bitmap_pages;
    r->bitmap       = bitmap;
    r->free_count   = page_count - bitmap_pages;
    r->alloc_hint   = bitmap_pages;
}

// ── Multiboot2 memory map parsing ────────────────────────────────────
typedef struct { uint64_t base, len; } range_t;

#define PHYSMEM64_MAX_RAW_RANGES  16
#define PHYSMEM64_MAX_WORK_RANGES 64

static int mb2_find_mmap(uint64_t mb_info_addr, range_t* out, int max_out) {
    if (!mb_info_addr) return 0;

    uint32_t total = *(uint32_t*)(uintptr_t)mb_info_addr;
    uint8_t* p   = (uint8_t*)(uintptr_t)(mb_info_addr + 8);
    uint8_t* end = (uint8_t*)(uintptr_t)(mb_info_addr + (uint64_t)total);
    int count = 0;

    while (p + 8 <= end) {
        uint32_t type = *(uint32_t*)p;
        uint32_t size = *(uint32_t*)(p + 4);
        if (type == 0) break; // end tag

        if (type == 6) {
            uint32_t entry_size = *(uint32_t*)(p + 8);
            uint8_t* e     = p + 16;
            uint8_t* e_end = p + size;
            while (entry_size >= 24 && e + entry_size <= e_end) {
                uint64_t e_base = *(uint64_t*)e;
                uint64_t e_len  = *(uint64_t*)(e + 8);
                uint32_t e_type = *(uint32_t*)(e + 16);
                if (e_type == 1 && e_len > 0 && count < max_out) {
                    out[count].base = e_base;
                    out[count].len  = e_len;
                    count++;
                }
                e += entry_size;
            }
            break; // only one memory map tag exists per the spec
        }

        uint32_t skip = (size + 7u) & ~7u;
        if (!skip) break;
        p += skip;
    }
    return count;
}

// Subtracts `cut` from `r`, writing 0-2 resulting sub-ranges into `out`.
static int subtract_range(range_t r, range_t cut, range_t* out) {
    if (r.len == 0) return 0;
    uint64_t r_end = r.base + r.len;
    uint64_t c_end = cut.base + cut.len;
    if (cut.len == 0 || c_end <= r.base || cut.base >= r_end) {
        out[0] = r;
        return 1;
    }
    int n = 0;
    if (cut.base > r.base) { out[n].base = r.base; out[n].len = cut.base - r.base; n++; }
    if (c_end < r_end)     { out[n].base = c_end;  out[n].len = r_end - c_end;      n++; }
    return n;
}

static void klog_u64dec(const char* label, uint64_t v) {
    char buf[24];
    int i = 23;
    buf[i--] = 0;
    if (v == 0) buf[i--] = '0';
    while (v && i >= 0) { buf[i--] = (char)('0' + (v % 10)); v /= 10; }
    klog(label);
    klog(&buf[i + 1]);
    klog("\n");
}

void physmem64_init(uint64_t mb_info_addr, uint64_t fb_addr, uint64_t fb_size) {
    num_regions     = 0;
    bootstrap_next  = 0;
    stat_usable_ram_bytes = 0;
    stat_kernel_bytes = 0;
    stat_mbinfo_base = 0;
    stat_mbinfo_bytes = 0;
    stat_fb_base = 0;
    stat_fb_bytes = 0;

    range_t raw[PHYSMEM64_MAX_RAW_RANGES];
    int raw_count = mb2_find_mmap(mb_info_addr, raw, PHYSMEM64_MAX_RAW_RANGES);
    if (raw_count == 0) {
        klog("physmem64: no usable Multiboot2 memory map found -- allocator will have 0 pages\n");
    }
    for (int i = 0; i < raw_count; i++) stat_usable_ram_bytes += raw[i].len;

    // Reservations: low memory + kernel image as one combined range
    // (the kernel loads at 1MB, so this also covers the conventional
    // "always reserve below 1MB" rule for free), the Multiboot2 info
    // block itself, and the framebuffer LFB if one was found.
    range_t reservations[3];
    int res_count = 0;

    stat_kernel_bytes = phys_of(&kernel64_end);
    reservations[res_count].base = 0;
    reservations[res_count].len  = stat_kernel_bytes;
    res_count++;

    if (mb_info_addr) {
        stat_mbinfo_base  = mb_info_addr;
        stat_mbinfo_bytes = *(uint32_t*)(uintptr_t)mb_info_addr;
        reservations[res_count].base = mb_info_addr;
        reservations[res_count].len  = stat_mbinfo_bytes;
        res_count++;
    }
    if (fb_addr) {
        stat_fb_base  = fb_addr;
        stat_fb_bytes = fb_size;
        reservations[res_count].base = fb_addr;
        reservations[res_count].len  = fb_size;
        res_count++;
    }

    range_t work[PHYSMEM64_MAX_WORK_RANGES];
    int work_count = 0;
    for (int i = 0; i < raw_count && work_count < PHYSMEM64_MAX_WORK_RANGES; i++) work[work_count++] = raw[i];

    for (int ri = 0; ri < res_count; ri++) {
        range_t next[PHYSMEM64_MAX_WORK_RANGES];
        int next_count = 0;
        for (int wi = 0; wi < work_count; wi++) {
            range_t pieces[2];
            int n = subtract_range(work[wi], reservations[ri], pieces);
            for (int k = 0; k < n && next_count < PHYSMEM64_MAX_WORK_RANGES; k++) next[next_count++] = pieces[k];
        }
        for (int wi = 0; wi < next_count; wi++) work[wi] = next[wi];
        work_count = next_count;
    }

    for (int i = 0; i < work_count; i++) add_region(work[i].base, work[i].len);

    uint64_t managed_bytes = 0;
    for (int i = 0; i < num_regions; i++) managed_bytes += regions[i].page_count * 4096ULL;

    klog("physmem64: init complete\n");
    klog_u64dec("  usable RAM bytes:   ", stat_usable_ram_bytes);
    klog_u64dec("  managed bytes:      ", managed_bytes);
    klog_u64dec("  region count:       ", (uint64_t)num_regions);
}

uint64_t physmem64_alloc_pages(uint64_t count) {
    if (count == 0) return 0;
    uint64_t flags = physmem64_lock();

    for (int i = 0; i < num_regions; i++) {
        physmem64_region_t* r = &regions[i];
        if (r->free_count < count) continue;

        int64_t start = region_find_run(r, count);
        if (start < 0) continue;

        uint8_t* base_ptr = (uint8_t*)physmem64_to_virt(r->base + (uint64_t)start * 4096ULL);
        for (uint64_t k = 0; k < count; k++) {
            bit_set(r->bitmap, (uint64_t)start + k);
            uint8_t* page = base_ptr + k * 4096ULL;
            for (uint64_t b = 0; b < 4096; b++) page[b] = 0;
        }
        r->free_count -= count;
        r->alloc_hint = (uint64_t)start + count;
        if (r->alloc_hint >= r->page_count) r->alloc_hint = 0;

        uint64_t result = r->base + (uint64_t)start * 4096ULL;
        physmem64_unlock(flags);
        return result;
    }

    physmem64_unlock(flags);
    return 0;
}

uint64_t physmem64_alloc_page(void) {
    return physmem64_alloc_pages(1);
}

void physmem64_free_pages(uint64_t phys, uint64_t count) {
    if (count == 0) return;
    uint64_t flags = physmem64_lock();

    if ((phys & 0xFFFULL) != 0) {
        klog("physmem64: free: address not page-aligned -- ignoring\n");
        physmem64_unlock(flags);
        return;
    }

    physmem64_region_t* r = find_region(phys);
    if (!r) {
        klog("physmem64: free: address outside any managed region -- ignoring\n");
        physmem64_unlock(flags);
        return;
    }

    uint64_t start = (phys - r->base) / 4096ULL;
    if (start + count > r->page_count) {
        klog("physmem64: free: range runs past the end of its region -- ignoring\n");
        physmem64_unlock(flags);
        return;
    }
    if (start < r->bitmap_pages) {
        klog("physmem64: free: attempted to free allocator-internal memory -- ignoring\n");
        physmem64_unlock(flags);
        return;
    }
    for (uint64_t k = 0; k < count; k++) {
        if (!bit_is_set(r->bitmap, start + k)) {
            klog("physmem64: free: double free (or partially-unallocated range) -- ignoring the whole call\n");
            physmem64_unlock(flags);
            return;
        }
    }

    for (uint64_t k = 0; k < count; k++) bit_clear(r->bitmap, start + k);
    r->free_count += count;
    if (start < r->alloc_hint) r->alloc_hint = start;
    physmem64_unlock(flags);
}

void physmem64_free_page(uint64_t phys) {
    physmem64_free_pages(phys, 1);
}

int physmem64_range_is_allocatable(uint64_t phys, uint64_t len) {
    if (len == 0) return 0;
    physmem64_region_t* r = find_region(phys);
    if (!r) return 0;
    if (phys + len > r->base + r->page_count * 4096ULL) return 0;
    uint64_t start_page = (phys - r->base) / 4096ULL;
    if (start_page < r->bitmap_pages) return 0;
    return 1;
}

void physmem64_stats(physmem64_stats_t* out) {
    physmem64_stats_t s;
    s.usable_ram_bytes   = stat_usable_ram_bytes;
    s.kernel_image_bytes = stat_kernel_bytes;
    s.mb_info_bytes      = stat_mbinfo_bytes;
    s.framebuffer_bytes  = stat_fb_bytes;
    s.region_count       = (uint32_t)num_regions;
    s.managed_pages      = 0;
    s.free_pages         = 0;
    s.largest_region_pages   = 0;
    s.largest_free_run_pages = 0;

    for (int i = 0; i < num_regions; i++) {
        physmem64_region_t* r = &regions[i];
        s.managed_pages += r->page_count;
        s.free_pages    += r->free_count;
        if (r->page_count > s.largest_region_pages) s.largest_region_pages = r->page_count;

        uint64_t run = 0, best = 0;
        for (uint64_t p = 0; p < r->page_count; p++) {
            if (!bit_is_set(r->bitmap, p)) { run++; if (run > best) best = run; }
            else run = 0;
        }
        if (best > s.largest_free_run_pages) s.largest_free_run_pages = best;
    }

    s.used_pages = s.managed_pages - s.free_pages;
    uint64_t managed_bytes = s.managed_pages * 4096ULL;
    s.reserved_bytes = (s.usable_ram_bytes > managed_bytes) ? s.usable_ram_bytes - managed_bytes : 0;

    *out = s;
}

void physmem64_dump(void) {
    klog("physmem64: dump ---\n");
    klog_u64dec("  kernel/low-mem reserved end=", stat_kernel_bytes);
    if (stat_mbinfo_bytes) {
        klog_u64dec("  mb_info base=", stat_mbinfo_base);
        klog_u64dec("           len=", stat_mbinfo_bytes);
    }
    if (stat_fb_bytes) {
        klog_u64dec("  framebuffer base=", stat_fb_base);
        klog_u64dec("               len=", stat_fb_bytes);
    }
    for (int i = 0; i < num_regions; i++) {
        physmem64_region_t* r = &regions[i];
        klog_u64dec("  region base=", r->base);
        klog_u64dec("       pages=", r->page_count);
        klog_u64dec("       free=", r->free_count);
        klog_u64dec("       bitmap_pages=", r->bitmap_pages);
    }
    klog("physmem64: dump end ---\n");
}

// ── Self-test suite ──────────────────────────────────────────────────
static int test_single_alloc_free(void) {
    uint64_t a = physmem64_alloc_page();
    if (!a) return 0;
    uint8_t* p = (uint8_t*)physmem64_to_virt(a);
    for (int i = 0; i < 4096; i++) p[i] = (uint8_t)i;
    for (int i = 0; i < 4096; i++) if (p[i] != (uint8_t)i) return 0;
    physmem64_free_page(a);
    return 1;
}

static int test_reuse(void) {
    uint64_t a = physmem64_alloc_page();
    if (!a) return 0;
    physmem64_free_page(a);
    uint64_t b = physmem64_alloc_page();
    int ok = (b == a);
    if (b) physmem64_free_page(b);
    return ok;
}

static int test_contiguous_alloc_free(void) {
    const uint64_t n = 16;
    uint64_t base = physmem64_alloc_pages(n);
    if (!base) return 0;

    for (uint64_t i = 0; i < n; i++) {
        uint8_t* p = (uint8_t*)physmem64_to_virt(base + i * 4096ULL);
        p[0] = (uint8_t)i;
    }
    int ok = 1;
    for (uint64_t i = 0; i < n; i++) {
        uint8_t* p = (uint8_t*)physmem64_to_virt(base + i * 4096ULL);
        if (p[0] != (uint8_t)i) { ok = 0; break; }
    }

    physmem64_free_pages(base, n);

    uint64_t base2 = physmem64_alloc_pages(n);
    int reused = (base2 == base);
    if (base2) physmem64_free_pages(base2, n);

    return ok && reused;
}

static int test_free_partial_run(void) {
    const uint64_t n = 8;
    uint64_t base = physmem64_alloc_pages(n);
    if (!base) return 0;

    // Free the whole run one page at a time (not via free_pages) and
    // confirm every page independently becomes reallocatable.
    for (uint64_t i = 0; i < n; i++) physmem64_free_page(base + i * 4096ULL);

    uint64_t base2 = physmem64_alloc_pages(n);
    int ok = (base2 == base);
    if (base2) physmem64_free_pages(base2, n);
    return ok;
}

static int test_reservation_correctness(void) {
    // Address 0 (low memory) must never be reported allocatable.
    if (physmem64_range_is_allocatable(0, 0x1000)) return 0;
    // Any point strictly inside the kernel-image reservation.
    if (stat_kernel_bytes > 0x2000 && physmem64_range_is_allocatable(0x1000, 0x1000)) return 0;
    // The Multiboot2 info block, if one was reserved.
    if (stat_mbinfo_bytes > 0 && physmem64_range_is_allocatable(stat_mbinfo_base, 1)) return 0;
    // The framebuffer LFB, if one was reserved.
    if (stat_fb_bytes > 0 && physmem64_range_is_allocatable(stat_fb_base, 1)) return 0;
    // Every region's own self-hosted bitmap pages.
    for (int i = 0; i < num_regions; i++) {
        physmem64_region_t* r = &regions[i];
        if (r->bitmap_pages > 0 &&
            physmem64_range_is_allocatable(r->base, r->bitmap_pages * 4096ULL)) {
            return 0;
        }
    }
    return 1;
}

static int test_exhaustion_and_recovery(void) {
    if (num_regions == 0) return 0;

    // Pick the region with the fewest free pages so this stays cheap
    // regardless of how much RAM the machine actually has.
    int idx = 0;
    for (int i = 1; i < num_regions; i++) {
        if (regions[i].free_count < regions[idx].free_count) idx = i;
    }
    physmem64_region_t* r = &regions[idx];
    uint64_t original_free = r->free_count;
    if (original_free == 0) return 0;

    // Drain the ENTIRE region as one contiguous allocation rather than
    // one page at a time -- this both exercises the "ask for everything
    // currently free" boundary and avoids needing to track a possibly
    // huge number of individual page addresses just to free them back.
    // Every earlier test case fully restores the region it touches, so
    // its free space is a single contiguous run by the time this runs.
    uint64_t whole = physmem64_alloc_pages(original_free);
    if (!whole) return 0;
    int exhausted_cleanly = (r->free_count == 0);

    uint64_t extra = physmem64_alloc_pages(1);
    int extra_did_not_come_from_exhausted_region = (!extra) || find_region(extra) != r;
    if (extra) physmem64_free_page(extra);

    physmem64_free_pages(whole, original_free);
    int recovered = (r->free_count == original_free);

    return exhausted_cleanly && extra_did_not_come_from_exhausted_region && recovered;
}

static int test_bookkeeping(void) {
    physmem64_stats_t before, after;
    physmem64_stats(&before);

    uint64_t a = physmem64_alloc_page();
    uint64_t b = physmem64_alloc_pages(4);
    if (!a || !b) return 0;

    physmem64_stats(&after);
    int used_grew = (after.used_pages == before.used_pages + 5) &&
                    (after.free_pages == before.free_pages - 5) &&
                    (after.managed_pages == before.managed_pages);

    physmem64_free_page(a);
    physmem64_free_pages(b, 4);

    physmem64_stats_t restored;
    physmem64_stats(&restored);
    int fully_restored = (restored.used_pages == before.used_pages) &&
                         (restored.free_pages == before.free_pages);

    return used_grew && fully_restored;
}

#define PHYSMEM64_TEST(name, expr) do {          \
    int _r = (expr);                             \
    klog("physmem64_selftest: " name " ");       \
    klog(_r ? "PASS\n" : "FAIL\n");              \
    if (_r) pass++; else fail++;                 \
} while (0)

int physmem64_selftest(void) {
    int pass = 0, fail = 0;
    klog("physmem64_selftest: starting\n");

    PHYSMEM64_TEST("single-page alloc/free", test_single_alloc_free());
    PHYSMEM64_TEST("reuse after free", test_reuse());
    PHYSMEM64_TEST("contiguous multi-page alloc/free", test_contiguous_alloc_free());
    PHYSMEM64_TEST("free contiguous run page-by-page", test_free_partial_run());
    PHYSMEM64_TEST("reservation correctness", test_reservation_correctness());
    PHYSMEM64_TEST("safe exhaustion and recovery", test_exhaustion_and_recovery());
    PHYSMEM64_TEST("bookkeeping consistency", test_bookkeeping());

    klog_u64dec("physmem64_selftest: pass=", (uint64_t)pass);
    klog_u64dec("physmem64_selftest: fail=", (uint64_t)fail);
    return fail == 0;
}
