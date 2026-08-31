// kernel/heap64.c — Milestone 22: 64-bit kernel heap (kmalloc/kfree).
//
// Design overview
// ----------------
// A segregated free-list, boundary-tag allocator (same family as the
// 32-bit kernel's kernel/mm.c) laid out over one or more "spans" --
// contiguous runs of 4KB pages obtained one at a time from
// physmem64_alloc_page() and given back via physmem64_free_page() once
// a whole span is completely free again. This is the key difference
// from mm.c, which owns one fixed static 16MB arena for the life of the
// kernel: this heap grows and shrinks its physical backing dynamically,
// per the Milestone 22 brief.
//
// A span is only ever grown by appending a page that physmem64 happens
// to hand back contiguous with the span's current physical end -- there
// is no "allocate N contiguous physical pages" primitive to ask for
// (physmem64_alloc_page() hands out one page at a time with no
// contiguity guarantee across separate calls). When a freshly obtained
// page is NOT contiguous with the span currently being grown, it starts
// a brand new, independent span instead of ever being spliced onto an
// unrelated one -- the boundary-tag pointer arithmetic below requires
// true address contiguity within a span, so two disjoint spans are
// never merged into one logical block run. A single allocation request
// larger than what fits within one span therefore fails even if the
// SUM of free space across multiple spans would be enough; see
// include/heap64.h and the Milestone 23 notes in the commit/summary for
// why this is acceptable for now and what a real physical memory
// manager should fix.
//
// Every page handed out by physmem64_alloc_page() lives inside the
// boot-time flat-mapped low-64MB window (see include/physmem64.h), so
// phys_to_ptr() (memmap64.h) gives a usable kernel VA for it with zero
// extra page-table work -- this heap never touches paging64.c itself.
//
// Physical layout of one block (identical in spirit to mm.c):
//
//   [ heap64_block_t header ][ payload ... ][ heap64_footer_t footer ]
//
// header.size / footer.size = payload bytes (excludes header+footer).
// The footer exists purely for O(1) backward (left) coalescing on free.
// Every block also carries a pointer back to the span.h it came from,
// used to bound coalescing/dump/check walks to that span's own
// [base, base+size) range -- never past it, even under a corrupted
// neighbor.
//
// Concurrency: this kernel is single-core with a real timer-preemptive
// kernel-task scheduler (kernel/process64.c) already running once
// interrupts are enabled, so kmalloc/kfree/the diagnostics below take a
// simple cli-based critical section (save/restore the actual RFLAGS.IF
// bit, not a blind sti) around their bodies. That is sufficient mutual
// exclusion on a single core; it would need to become a real spinlock
// before this heap could be used safely under SMP.
#include <stdint.h>
#include "../include/heap64.h"
#include "../include/physmem64.h"
#include "../include/memmap64.h"
#include "../include/klog.h"

// ── Tunables ─────────────────────────────────────────────────────────
#define HEAP64_PAGE_SIZE   4096ULL
#define HEAP64_MIN_BLOCK   16ULL   // minimum payload, and the alignment kmalloc() rounds up to
#define HEAP64_NUM_CLASSES 32      // size classes 0..31, powers of two starting at HEAP64_MIN_BLOCK

// Own internal capacity knob -- independent of physmem64's pool size on
// purpose (see include/heap64.h). Sized generously for today's 64-page
// physmem64 pool (worst case: every page ends up in its own span due to
// fragmentation); Milestone 23's real memory manager will have far more
// RAM to hand out and may need this raised.
#define HEAP64_MAX_SPANS 64

#define HEAP64_MAGIC_FREE 0xF4EEB10Cu
#define HEAP64_MAGIC_USED 0xA110CA7Eu

// ── Block/footer/span layout ────────────────────────────────────────
typedef struct heap64_span_s {
    uint64_t base;     // kernel VA, page-aligned; 0 size == unused slot
    uint64_t size;     // total bytes currently held for this span (multiple of 4096)
    uint64_t phys_end; // physical address just past the last page held -- for contiguity checks
} heap64_span_t;

typedef struct heap64_block_s {
    uint32_t magic;
    uint32_t size_class;
    uint64_t size;              // payload bytes
    heap64_span_t* span;        // owning span -- bounds every coalesce/walk
    struct heap64_block_s* next; // free-list link (valid only while free)
    struct heap64_block_s* prev;
} heap64_block_t;

typedef struct {
    uint64_t size;   // mirrors header size, for O(1) backward merge
    uint32_t magic;  // mirrors header magic, cross-checked on free
    uint32_t _pad;
} heap64_footer_t;

#define HDR_SIZE  ((uint64_t)sizeof(heap64_block_t))
#define FTR_SIZE  ((uint64_t)sizeof(heap64_footer_t))
#define OVERHEAD  (HDR_SIZE + FTR_SIZE)

// ── State ────────────────────────────────────────────────────────────
static heap64_block_t* free_lists[HEAP64_NUM_CLASSES];
static heap64_span_t   spans[HEAP64_MAX_SPANS];
static int             active_span_idx = -1; // spans[] index grow_by_one_page extends next, or -1

// ── Interrupt-safety ─────────────────────────────────────────────────
static inline uint64_t heap64_lock(void) {
    uint64_t flags;
    __asm__ volatile("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void heap64_unlock(uint64_t flags) {
    if (flags & (1ULL << 9)) __asm__ volatile("sti" ::: "memory");
}

// ── Tiny logging helpers (klog only takes strings) ──────────────────
static void klog_u64hex(const char* label, uint64_t val) {
    const char* h = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) { buf[2 + (15 - i)] = h[val & 0xF]; val >>= 4; }
    buf[18] = 0;
    klog(label); klog(buf); klog("\n");
}

static void klog_dec(uint32_t v) {
    char buf[12];
    int i = 11;
    buf[i--] = 0;
    if (v == 0) buf[i--] = '0';
    while (v && i >= 0) { buf[i--] = (char)('0' + (v % 10)); v /= 10; }
    klog(&buf[i + 1]);
}

// ── Size classes ─────────────────────────────────────────────────────
static inline int size_to_class(uint64_t size) {
    if (size < HEAP64_MIN_BLOCK) size = HEAP64_MIN_BLOCK;
    int cls = 0;
    uint64_t s = HEAP64_MIN_BLOCK;
    while (s < size && cls < HEAP64_NUM_CLASSES - 1) { s <<= 1; cls++; }
    return cls;
}

// ── Footer access ────────────────────────────────────────────────────
static inline heap64_footer_t* hdr_to_ftr(heap64_block_t* h) {
    return (heap64_footer_t*)((uint8_t*)h + HDR_SIZE + h->size);
}

static inline void write_footer(heap64_block_t* h) {
    heap64_footer_t* f = hdr_to_ftr(h);
    f->size = h->size;
    f->magic = h->magic;
}

// ── Free list operations ─────────────────────────────────────────────
static void fl_push(heap64_block_t* b) {
    int cls = size_to_class(b->size);
    b->size_class = (uint32_t)cls;
    b->next = free_lists[cls];
    b->prev = 0;
    if (free_lists[cls]) free_lists[cls]->prev = b;
    free_lists[cls] = b;
}

static void fl_remove(heap64_block_t* b) {
    int cls = (int)b->size_class;
    if (b->prev) b->prev->next = b->next;
    else         free_lists[cls] = b->next;
    if (b->next) b->next->prev = b->prev;
    b->next = b->prev = 0;
}

// Smallest-class-first, then scans the WHOLE list in that class rather
// than just its head (a block near the head of class C can legitimately
// be smaller than `size` while a later one in the same class still
// fits -- classes bucket by "smallest class that can hold this size",
// not by an exact size range boundary equal to the request).
static heap64_block_t* find_fit(uint64_t size) {
    int cls = size_to_class(size);
    for (int c = cls; c < HEAP64_NUM_CLASSES; c++) {
        for (heap64_block_t* b = free_lists[c]; b; b = b->next) {
            if (b->size >= size) return b;
        }
    }
    return 0;
}

static heap64_span_t* find_span_containing(uint64_t addr) {
    for (int i = 0; i < HEAP64_MAX_SPANS; i++) {
        if (spans[i].size == 0) continue;
        if (addr >= spans[i].base && addr < spans[i].base + spans[i].size) return &spans[i];
    }
    return 0;
}

static int alloc_span_slot(void) {
    for (int i = 0; i < HEAP64_MAX_SPANS; i++) if (spans[i].size == 0) return i;
    return -1;
}

static inline heap64_span_t* active_span(void) {
    return active_span_idx >= 0 ? &spans[active_span_idx] : 0;
}

// Formats [va, va+HEAP64_PAGE_SIZE) as one free block spanning exactly
// the new page, then left-coalesces it with whatever block currently
// sits immediately before it in `span` (there is nothing to its right
// yet -- this is only ever called to extend a span at its current
// tail, so the invariant "span is fully tiled, no gaps" guarantees the
// preceding block, if any, ends exactly at `va`).
static void absorb_new_page(heap64_span_t* span, uint64_t va) {
    heap64_block_t* b = (heap64_block_t*)va;
    b->magic = HEAP64_MAGIC_FREE;
    b->size  = HEAP64_PAGE_SIZE - OVERHEAD;
    b->span  = span;
    write_footer(b);

    if (va > span->base) {
        heap64_footer_t* left_ftr = (heap64_footer_t*)(va - FTR_SIZE);
        uint64_t left_addr = va - FTR_SIZE - left_ftr->size - HDR_SIZE;
        if (left_addr >= span->base && left_addr < va) {
            heap64_block_t* left = (heap64_block_t*)left_addr;
            if (left->magic == HEAP64_MAGIC_FREE) {
                fl_remove(left);
                left->size += OVERHEAD + b->size;
                write_footer(left);
                b = left;
            }
        }
    }
    fl_push(b);
}

// Gives every page in `span` back to physmem64 and frees its slot.
// Only called when `span` is already known to be exactly one free
// block spanning its entire [base, base+size) -- there is nothing left
// to unlink from any free list by the time this runs (the caller
// removed that final block before calling in).
static void release_span(heap64_span_t* span) {
    uint64_t phys_base = span->phys_end - span->size;
    uint64_t pages = span->size / HEAP64_PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        physmem64_free_page(phys_base + i * HEAP64_PAGE_SIZE);
    }
    int idx = (int)(span - spans);
    if (active_span_idx == idx) active_span_idx = -1;
    span->base = 0;
    span->size = 0;
    span->phys_end = 0;
}

// Obtains one more physical page and either extends the active span (if
// physmem64 happened to hand back a page contiguous with it) or starts
// a fresh, independent span. Returns 1 on success, 0 if physmem64 is
// exhausted or every span slot is in use.
static int grow_by_one_page(void) {
    uint64_t phys = physmem64_alloc_page();
    if (!phys) return 0;

    heap64_span_t* cur = active_span();
    if (cur && phys == cur->phys_end) {
        uint64_t va = cur->base + cur->size;
        cur->size     += HEAP64_PAGE_SIZE;
        cur->phys_end += HEAP64_PAGE_SIZE;
        absorb_new_page(cur, va);
        return 1;
    }

    int idx = alloc_span_slot();
    if (idx < 0) {
        physmem64_free_page(phys); // nowhere to track it -- give it straight back
        return 0;
    }
    heap64_span_t* ns = &spans[idx];
    ns->base     = (uint64_t)phys_to_ptr(phys);
    ns->size     = HEAP64_PAGE_SIZE;
    ns->phys_end = phys + HEAP64_PAGE_SIZE;
    active_span_idx = idx;
    absorb_new_page(ns, ns->base);
    return 1;
}

static void split_if_worthwhile(heap64_block_t* b, uint64_t size) {
    uint64_t remainder = b->size - size;
    if (remainder < OVERHEAD + HEAP64_MIN_BLOCK) return; // not worth carving off

    heap64_block_t* split = (heap64_block_t*)((uint8_t*)b + HDR_SIZE + size + FTR_SIZE);
    split->magic = HEAP64_MAGIC_FREE;
    split->size  = remainder - OVERHEAD;
    split->span  = b->span;
    write_footer(split);
    fl_push(split);

    b->size = size;
    write_footer(b);
}

// ── Public API ───────────────────────────────────────────────────────
void heap64_init(void) {
    for (int i = 0; i < HEAP64_NUM_CLASSES; i++) free_lists[i] = 0;
    for (int i = 0; i < HEAP64_MAX_SPANS; i++) {
        spans[i].base = 0;
        spans[i].size = 0;
        spans[i].phys_end = 0;
    }
    active_span_idx = -1;
}

void* kmalloc(uint64_t size) {
    if (size == 0) return 0;
    size = (size + HEAP64_MIN_BLOCK - 1) & ~(HEAP64_MIN_BLOCK - 1);

    uint64_t flags = heap64_lock();

    heap64_block_t* b = find_fit(size);
    while (!b) {
        if (!grow_by_one_page()) { heap64_unlock(flags); return 0; }
        b = find_fit(size);
    }

    fl_remove(b);
    split_if_worthwhile(b, size);
    b->magic = HEAP64_MAGIC_USED;
    write_footer(b);

    heap64_unlock(flags);
    return (void*)((uint8_t*)b + HDR_SIZE);
}

void kfree(void* ptr) {
    if (!ptr) return;
    uint64_t flags = heap64_lock();

    heap64_block_t* b = (heap64_block_t*)((uint8_t*)ptr - HDR_SIZE);
    heap64_span_t* span = find_span_containing((uint64_t)b);
    if (!span || (uint64_t)b + HDR_SIZE > span->base + span->size) {
        klog("heap64: kfree: pointer not in any heap span -- ignoring\n");
        heap64_unlock(flags);
        return;
    }

    heap64_footer_t* ftr = hdr_to_ftr(b);
    if ((uint64_t)ftr + FTR_SIZE > span->base + span->size) {
        klog("heap64: kfree: corrupt header size -- ignoring\n");
        heap64_unlock(flags);
        return;
    }
    if (b->magic == HEAP64_MAGIC_FREE) {
        klog("heap64: kfree: double free -- ignoring\n");
        heap64_unlock(flags);
        return;
    }
    if (b->magic != HEAP64_MAGIC_USED || b->span != span ||
        ftr->size != b->size || ftr->magic != HEAP64_MAGIC_USED) {
        klog("heap64: kfree: corrupt or invalid block -- ignoring\n");
        heap64_unlock(flags);
        return;
    }

    b->magic = HEAP64_MAGIC_FREE;

    // Right coalesce.
    uint64_t right_addr = (uint64_t)b + HDR_SIZE + b->size + FTR_SIZE;
    if (right_addr + HDR_SIZE <= span->base + span->size) {
        heap64_block_t* right = (heap64_block_t*)right_addr;
        if (right->magic == HEAP64_MAGIC_FREE) {
            fl_remove(right);
            b->size += OVERHEAD + right->size;
        }
    }
    // Left coalesce.
    if ((uint64_t)b > span->base) {
        heap64_footer_t* left_ftr = (heap64_footer_t*)((uint64_t)b - FTR_SIZE);
        uint64_t left_addr = (uint64_t)b - FTR_SIZE - left_ftr->size - HDR_SIZE;
        if (left_addr >= span->base && left_addr < (uint64_t)b) {
            heap64_block_t* left = (heap64_block_t*)left_addr;
            if (left->magic == HEAP64_MAGIC_FREE) {
                fl_remove(left);
                left->size += OVERHEAD + b->size;
                b = left;
            }
        }
    }

    if ((uint64_t)b == span->base && b->size == span->size - OVERHEAD) {
        // The whole span just became one free block -- hand it all back
        // to physmem64 rather than hoarding it (this is what makes the
        // heap shrink dynamically, not just grow).
        release_span(span);
    } else {
        b->span = span;
        write_footer(b);
        fl_push(b);
    }

    heap64_unlock(flags);
}

// Same technique as the 32-bit kernel's kmalloc_aligned/kfree_aligned
// (kernel/mm.c): over-allocate by align+sizeof(void*), carve the
// aligned address out of the middle, and stash the original raw
// pointer just before it so kfree_aligned can recover it.
void* kmalloc_aligned(uint64_t size, uint64_t align) {
    if (align == 0 || (align & (align - 1)) != 0) return 0;

    uint8_t* raw = (uint8_t*)kmalloc(size + align + sizeof(void*));
    if (!raw) return 0;

    uint64_t addr = (uint64_t)raw + sizeof(void*);
    if (addr % align != 0) addr = (addr + align - 1) & ~(align - 1);

    void** orig_slot = (void**)(addr - sizeof(void*));
    *orig_slot = raw;
    return (void*)addr;
}

void kfree_aligned(void* ptr) {
    if (!ptr) return;
    void** orig_slot = (void**)((uint8_t*)ptr - sizeof(void*));
    kfree(*orig_slot);
}

void heap64_stats(heap64_stats_t* out) {
    heap64_stats_t s = {0, 0, 0, 0, 0, 0, 0, 0};
    uint64_t flags = heap64_lock();

    for (int i = 0; i < HEAP64_MAX_SPANS; i++) {
        heap64_span_t* sp = &spans[i];
        if (sp->size == 0) continue;
        s.span_count++;
        s.total_bytes += sp->size;

        uint64_t addr = sp->base, end = sp->base + sp->size;
        while (addr < end) {
            heap64_block_t* b = (heap64_block_t*)addr;
            s.block_count++;
            s.overhead_bytes += OVERHEAD;
            if (b->magic == HEAP64_MAGIC_FREE) {
                s.free_block_count++;
                s.free_bytes += b->size;
                if (b->size > s.largest_free_block) s.largest_free_block = b->size;
            } else {
                s.used_bytes += b->size;
            }
            addr += HDR_SIZE + b->size + FTR_SIZE;
        }
    }

    heap64_unlock(flags);
    *out = s;
}

void heap64_dump(void) {
    uint64_t flags = heap64_lock();
    klog("heap64: dump ---\n");
    for (int i = 0; i < HEAP64_MAX_SPANS; i++) {
        heap64_span_t* sp = &spans[i];
        if (sp->size == 0) continue;
        klog_u64hex("  span base=", sp->base);
        klog_u64hex("       size=", sp->size);
        uint64_t addr = sp->base, end = sp->base + sp->size;
        while (addr < end) {
            heap64_block_t* b = (heap64_block_t*)addr;
            klog(b->magic == HEAP64_MAGIC_FREE ? "    FREE " : "    USED ");
            klog_u64hex("addr=", addr);
            klog_u64hex("      size=", b->size);
            addr += HDR_SIZE + b->size + FTR_SIZE;
        }
    }
    klog("heap64: dump end ---\n");
    heap64_unlock(flags);
}

uint64_t heap64_check(void) {
    uint64_t flags = heap64_lock();

    for (int i = 0; i < HEAP64_MAX_SPANS; i++) {
        heap64_span_t* sp = &spans[i];
        if (sp->size == 0) continue;

        uint64_t addr = sp->base, end = sp->base + sp->size;
        while (addr < end) {
            heap64_block_t* b = (heap64_block_t*)addr;

            if (b->magic != HEAP64_MAGIC_FREE && b->magic != HEAP64_MAGIC_USED) {
                klog("heap64_check: bad header magic\n");
                heap64_unlock(flags);
                return addr;
            }
            if (addr + HDR_SIZE + b->size + FTR_SIZE > end) {
                klog("heap64_check: block overruns its span\n");
                heap64_unlock(flags);
                return addr;
            }
            heap64_footer_t* f = hdr_to_ftr(b);
            if (f->size != b->size || f->magic != b->magic) {
                klog("heap64_check: header/footer mismatch\n");
                heap64_unlock(flags);
                return addr;
            }
            if (b->span != sp) {
                klog("heap64_check: block does not point back to its span\n");
                heap64_unlock(flags);
                return addr;
            }
            addr += HDR_SIZE + b->size + FTR_SIZE;
        }
        if (addr != end) {
            klog("heap64_check: span not exactly tiled by its blocks\n");
            heap64_unlock(flags);
            return sp->base;
        }
    }

    heap64_unlock(flags);
    return 0;
}

// ── Self-test suite ──────────────────────────────────────────────────
// Every case below assumes it starts on an idle heap (nothing else has
// called kmalloc yet) and leaves it fully released (span_count == 0)
// on success -- both so each case's assertions about span growth are
// deterministic, and so a full pass returns every borrowed physmem64
// page before the rest of boot needs them.

static int test_basic(void) {
    void* p = kmalloc(64);
    if (!p) return 0;
    uint8_t* b = (uint8_t*)p;
    for (int i = 0; i < 64; i++) b[i] = (uint8_t)i;
    for (int i = 0; i < 64; i++) if (b[i] != (uint8_t)i) return 0;
    kfree(p);

    heap64_stats_t s; heap64_stats(&s);
    return s.span_count == 0 && heap64_check() == 0;
}

static int test_reuse(void) {
    void* a = kmalloc(128);
    if (!a) return 0;
    kfree(a);
    void* b = kmalloc(128);
    int same_block = (b == a); // the just-freed block must be reused, not a fresh page
    if (b) kfree(b);

    heap64_stats_t s; heap64_stats(&s);
    return same_block && s.span_count == 0 && heap64_check() == 0;
}

static int test_splitting(void) {
    void* a = kmalloc(100); // first-ever alloc -- forces a page grow, then a split
    if (!a) return 0;

    heap64_stats_t mid; heap64_stats(&mid);
    int split_happened = (mid.free_block_count >= 1) && (mid.span_count == 1);

    void* b = kmalloc(50); // should carve out of the split remainder -- no new page
    heap64_stats_t after; heap64_stats(&after);
    int no_new_span = (after.span_count == 1);

    kfree(a); kfree(b);
    heap64_stats_t final; heap64_stats(&final);
    return split_happened && no_new_span && b != 0 &&
           final.span_count == 0 && heap64_check() == 0;
}

static int test_coalescing(void) {
    void* a = kmalloc(200);
    void* b = kmalloc(200);
    void* c = kmalloc(200);
    if (!a || !b || !c) return 0;

    kfree(b); // hole in the middle
    kfree(a); // right-coalesces with the hole
    kfree(c); // right-coalesces with the tail remainder, then left-coalesces with (a+b)

    heap64_stats_t s; heap64_stats(&s);
    return s.span_count == 0 && heap64_check() == 0; // whole page released
}

static int test_fragmentation(void) {
    enum { N = 20 };
    void* p[N];
    for (int i = 0; i < N; i++) {
        p[i] = kmalloc(64);
        if (!p[i]) return 0;
    }
    for (int i = 0; i < N; i += 2) kfree(p[i]); // checkerboard -- deliberately fragment
    if (heap64_check() != 0) return 0;

    for (int i = 1; i < N; i += 2) kfree(p[i]); // free the rest -- should fully recombine

    heap64_stats_t s; heap64_stats(&s);
    return s.span_count == 0 && heap64_check() == 0;
}

static int test_alignment(void) {
    uint64_t aligns[4] = {16, 64, 256, 4096};
    void* p[4];
    int ok = 1;
    for (int i = 0; i < 4; i++) {
        p[i] = kmalloc_aligned(32, aligns[i]);
        if (!p[i] || ((uint64_t)p[i] % aligns[i]) != 0) ok = 0;
    }
    for (int i = 0; i < 4; i++) if (p[i]) kfree_aligned(p[i]);

    heap64_stats_t s; heap64_stats(&s);
    return ok && s.span_count == 0 && heap64_check() == 0;
}

static int test_multi_page(void) {
    const uint64_t size = 10000; // exceeds one page's usable payload -- forces span growth
    uint8_t* p = (uint8_t*)kmalloc(size);
    if (!p) return 0;

    for (uint64_t i = 0; i < size; i++) p[i] = (uint8_t)(i & 0xFF);
    int data_ok = 1;
    for (uint64_t i = 0; i < size; i++) {
        if (p[i] != (uint8_t)(i & 0xFF)) { data_ok = 0; break; }
    }

    heap64_stats_t mid; heap64_stats(&mid);
    int spanned_pages = (mid.span_count == 1) && (mid.total_bytes >= size);

    kfree(p);
    heap64_stats_t final; heap64_stats(&final);
    return data_ok && spanned_pages && final.span_count == 0 && heap64_check() == 0;
}

static int test_repeated_cycles(void) {
    for (int cycle = 0; cycle < 50; cycle++) {
        void* a = kmalloc(32 + (uint64_t)(cycle % 7) * 16);
        void* b = kmalloc(500);
        if (!a || !b) return 0;
        kfree(a);
        void* c = kmalloc(64);
        if (!c) return 0;
        kfree(b);
        kfree(c);
        if (heap64_check() != 0) return 0;
    }
    heap64_stats_t s; heap64_stats(&s);
    return s.span_count == 0;
}

static int test_double_free_protection(void) {
    // `keep` holds the span open so freeing `a` lands on the "already
    // FREE, still within a live span" path rather than "span fully
    // released, pointer no longer in any span" -- both are safely
    // rejected, but this exercises the more common real-world case.
    void* keep = kmalloc(48);
    void* a = kmalloc(48);
    if (!keep || !a) return 0;

    kfree(a);
    kfree(a); // double free -- must be ignored, not corrupt the heap

    void* b = kmalloc(48);
    int reused_ok = (b == a); // the one legitimate free still worked
    if (b) kfree(b);
    kfree(keep);

    heap64_stats_t s; heap64_stats(&s);
    return reused_ok && s.span_count == 0 && heap64_check() == 0;
}

#define HEAP64_TEST(name, expr) do {                    \
    int _r = (expr);                                    \
    klog("heap64_selftest: " name " ");                 \
    klog(_r ? "PASS\n" : "FAIL\n");                      \
    if (_r) pass++; else fail++;                        \
} while (0)

int heap64_selftest(void) {
    int pass = 0, fail = 0;
    klog("heap64_selftest: starting\n");

    HEAP64_TEST("basic alloc/free", test_basic());
    HEAP64_TEST("reuse after free", test_reuse());
    HEAP64_TEST("splitting", test_splitting());
    HEAP64_TEST("coalescing + span release", test_coalescing());
    HEAP64_TEST("fragmentation + recovery", test_fragmentation());
    HEAP64_TEST("kmalloc_aligned", test_alignment());
    HEAP64_TEST("multi-page allocation", test_multi_page());
    HEAP64_TEST("repeated alloc/free cycles", test_repeated_cycles());
    HEAP64_TEST("double-free protection", test_double_free_protection());

    klog("heap64_selftest: pass=");
    klog_dec((uint32_t)pass);
    klog(" fail=");
    klog_dec((uint32_t)fail);
    klog("\n");
    return fail == 0;
}
