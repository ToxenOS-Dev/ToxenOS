// ToxenOS/kernel/pmm.c — physical page frame allocator
//
// Uses a flat bitmap: one bit per 4KB frame.
// Bit = 1 means the frame is in use (reserved or allocated).
// Bit = 0 means the frame is free.
//
// Initialisation:
//   1. Mark all frames as used (conservative start).
//   2. Walk the multiboot2 memory map; for each usable region, mark
//      the frames within it as free.
//   3. Re-mark the kernel image + heap region as used so we never
//      hand out frames that the kernel itself occupies.
//
// After init, phys_alloc_page() scans for the first free bit, marks it
// used, and returns the physical address.  A "last allocated" hint keeps
// the scan O(1) amortised.

#include <stdint.h>
#include "../include/pmm.h"
#include "../include/klog.h"

// ── Bitmap storage ────────────────────────────────────────────────────────────
// 1M frames / 32 bits per uint32 = 32768 uint32 words = 128KB
#define BITMAP_WORDS  (PMM_MAX_FRAMES / 32)
static uint32_t bitmap[BITMAP_WORDS];

static uint32_t total_frames = 0;
static uint32_t free_frame_count = 0;
static uint32_t alloc_hint = 0;   // start next scan here (moves forward)

// ── Bitmap helpers ────────────────────────────────────────────────────────────
static inline void bitmap_set(uint32_t frame)
{
    bitmap[frame / 32] |= (1u << (frame % 32));
}

static inline void bitmap_clear(uint32_t frame)
{
    bitmap[frame / 32] &= ~(1u << (frame % 32));
}

static inline int bitmap_test(uint32_t frame)
{
    return (bitmap[frame / 32] >> (frame % 32)) & 1;
}

// Mark a physical range [start, start+size) as used (1).
static void pmm_mark_used(uint32_t start, uint32_t size)
{
    uint32_t frame_start = start / PMM_FRAME_SIZE;
    uint32_t frame_end   = (start + size + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    if (frame_end > PMM_MAX_FRAMES) frame_end = PMM_MAX_FRAMES;
    for (uint32_t f = frame_start; f < frame_end; f++)
        bitmap_set(f);
}

// Mark a physical range [start, start+size) as free (0).
// Only frees frames that are within total_frames.
static void pmm_mark_free(uint32_t start, uint32_t size)
{
    uint32_t frame_start = (start + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;  // round up
    uint32_t frame_end   = (start + size) / PMM_FRAME_SIZE;                // round down
    if (frame_end > total_frames) frame_end = total_frames;
    for (uint32_t f = frame_start; f < frame_end; f++) {
        if (bitmap_test(f)) {
            bitmap_clear(f);
            free_frame_count++;
        }
    }
}

// ── Multiboot2 memory map parsing ─────────────────────────────────────────────
// Tag type 6: memory map
// Each entry: base_addr(8) + length(8) + type(4) + reserved(4)
// type == 1 means "usable RAM"

#define MB2_TAG_MMAP      6
#define MB2_MMAP_USABLE   1

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
} __attribute__((packed)) mb2_mmap_entry_t;

// ── pmm_init ──────────────────────────────────────────────────────────────────
void pmm_init(uint32_t mb_info_addr)
{
    // Start with all frames marked used
    for (int i = 0; i < BITMAP_WORDS; i++) bitmap[i] = 0xFFFFFFFF;
    free_frame_count = 0;

    // Walk multiboot2 tags to find the memory map and determine total RAM
    uint32_t mb_total = *(uint32_t*)mb_info_addr;
    uint8_t* tag      = (uint8_t*)(mb_info_addr + 8);
    uint8_t* mb_end   = (uint8_t*)(mb_info_addr + mb_total);

    // First pass: find highest usable address to set total_frames
    uint64_t highest = 0;
    uint8_t* t = tag;
    while (t < mb_end) {
        uint32_t type = *(uint32_t*)t;
        uint32_t size = *(uint32_t*)(t + 4);
        if (type == MB2_TAG_MMAP) {
            uint32_t entry_size = *(uint32_t*)(t + 8);
            uint8_t* entry = t + 16;
            uint8_t* mmap_end = t + size;
            while (entry < mmap_end) {
                mb2_mmap_entry_t* e = (mb2_mmap_entry_t*)entry;
                if (e->type == MB2_MMAP_USABLE) {
                    uint64_t top = e->base + e->length;
                    if (top > highest) highest = top;
                }
                entry += entry_size;
            }
        }
        t += (size + 7) & ~7u;
    }

    // Cap at 4GB (we're 32-bit)
    if (highest > 0xFFFFFFFFULL) highest = 0xFFFFFFFFULL;
    total_frames = (uint32_t)(highest / PMM_FRAME_SIZE);
    if (total_frames > PMM_MAX_FRAMES) total_frames = PMM_MAX_FRAMES;

    // Second pass: mark all usable regions as free
    t = tag;
    while (t < mb_end) {
        uint32_t type = *(uint32_t*)t;
        uint32_t size = *(uint32_t*)(t + 4);
        if (type == MB2_TAG_MMAP) {
            uint32_t entry_size = *(uint32_t*)(t + 8);
            uint8_t* entry = t + 16;
            uint8_t* mmap_end = t + size;
            while (entry < mmap_end) {
                mb2_mmap_entry_t* e = (mb2_mmap_entry_t*)entry;
                if (e->type == MB2_MMAP_USABLE && e->base < 0x100000000ULL) {
                    uint32_t base = (uint32_t)e->base;
                    uint32_t len  = (e->base + e->length > 0x100000000ULL)
                                    ? (uint32_t)(0x100000000ULL - e->base)
                                    : (uint32_t)e->length;
                    pmm_mark_free(base, len);
                }
                entry += entry_size;
            }
        }
        t += (size + 7) & ~7u;
    }

    // Re-mark frames 0-255 (first 1MB) as used — BIOS, VGA, reserved
    pmm_mark_used(0, 0x100000);

    // Re-mark the multiboot info block itself as used (physical address)
    // mb_info_addr is now a virtual address — convert to physical
    uint32_t mb_info_phys = mb_info_addr - 0xC0000000u;
    pmm_mark_used(mb_info_phys, mb_total);

    // Re-mark the kernel image + heap region as used.
    // kernel_end is now a VIRTUAL address (0xC0xxxxxx) because the kernel
    // is linked at KERNEL_VIRT_BASE.  Subtract KERNEL_VIRT_BASE to get
    // the physical address.
    extern uint32_t kernel_end;
    uint32_t kernel_virt_end = (uint32_t)&kernel_end;
    uint32_t kernel_phys_end = kernel_virt_end - 0xC0000000u;
    // Cover kernel image + 18MB (image + 16MB heap + slack)
    pmm_mark_used(0x100000, kernel_phys_end - 0x100000 + (18u * 1024u * 1024u));

    klog("PMM: initialised\n");
}

// ── phys_alloc_page ───────────────────────────────────────────────────────────
uint32_t phys_alloc_page(void)
{
    // Search from alloc_hint forward, then wrap around once
    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t start = pass ? 0 : alloc_hint;
        uint32_t end   = pass ? alloc_hint : total_frames;

        for (uint32_t word = start / 32; word < (end + 31) / 32; word++) {
            if (bitmap[word] == 0xFFFFFFFF) continue;  // all used, skip word

            // Find the first free bit in this word
            uint32_t bits = ~bitmap[word];  // free bits are 1
            // ctz: count trailing zeros = index of first set bit
            uint32_t bit = 0;
            uint32_t tmp = bits;
            while (!(tmp & 1)) { tmp >>= 1; bit++; }

            uint32_t frame = word * 32 + bit;
            if (frame >= total_frames) continue;
            // Respect the pass boundaries
            if (pass == 0 && frame < start) continue;
            if (pass == 1 && frame >= end)  continue;

            bitmap_set(frame);
            free_frame_count--;
            alloc_hint = frame + 1;
            if (alloc_hint >= total_frames) alloc_hint = 0;

            // Zero the page via its kernel virtual address.
            // With higher-half kernel: physical P is at virtual P + 0xC0000000.
            uint32_t phys = frame * PMM_FRAME_SIZE;
            uint8_t* p = (uint8_t*)(phys + 0xC0000000u);
            for (int i = 0; i < (int)PMM_FRAME_SIZE; i++) p[i] = 0;
            return phys;
        }
    }

    klog("PMM: out of memory!\n");
    return 0;
}

// ── phys_free_page ────────────────────────────────────────────────────────────
void phys_free_page(uint32_t phys_addr)
{
    if (phys_addr == 0) return;
    uint32_t frame = phys_addr / PMM_FRAME_SIZE;
    if (frame >= total_frames) return;
    if (!bitmap_test(frame)) return;  // double-free guard

    bitmap_clear(frame);
    free_frame_count++;

    // Move hint back to encourage reuse of recently freed frames
    if (frame < alloc_hint) alloc_hint = frame;
}

// ── Diagnostics ───────────────────────────────────────────────────────────────
uint32_t pmm_total_frames(void) { return total_frames; }
uint32_t pmm_free_frames(void)  { return free_frame_count; }
