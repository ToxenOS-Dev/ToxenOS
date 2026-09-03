// ToxenOS/userlib/toxui/tox_heap.h — Milestone 33: a small general-
// purpose allocator for ToxUI's own use, backed by sys_brk (the
// existing Milestone 25 heap-growth syscall -- see include/uservm64.h).
// ToxenOS's userspace had no malloc/free equivalent before this
// milestone (every existing user64/*.c program either uses fixed
// stack/static buffers or calls sys_mmap directly for one big region --
// see user64/compositor64.c's backbuffer); PNG/TTF decoding needs many
// variably-sized, individually-freed allocations (scratch buffers
// during inflate, glyph bitmaps, the decoded image/font data itself),
// which is exactly what a real allocator is for.
//
// Design: a classic single-free-list, boundary-tag-free allocator
// (first-fit, address-ordered singly-linked block chain, splits on
// alloc, coalesces both directions on free) -- deliberately NOT
// kernel/heap64.c's segregated-size-class design (that one is
// justified there by needing to be fast/robust for the WHOLE kernel's
// allocation traffic; this one only ever serves one graphics/font
// library's own moderate allocation counts, where a simple design is
// the right tradeoff). No locking: ToxenOS has no userspace threads,
// only single-threaded processes.
#ifndef TOX_HEAP_H
#define TOX_HEAP_H
#include <stdint.h>

// Allocates at least `size` bytes, 16-byte aligned, uninitialized.
// Returns NULL on failure (sys_brk growth failed) -- callers must
// check. size == 0 returns NULL.
void* tox_alloc(uint64_t size);

// Frees a pointer previously returned by tox_alloc/tox_realloc. NULL
// is a safe no-op. Coalesces with physically-adjacent free blocks
// immediately (both directions) -- repeated alloc/free cycles do not
// fragment the heap into unusable slivers.
void tox_free(void* ptr);

// realloc(3)-style, but takes the OLD size explicitly (this allocator
// does not track per-block "originally requested size" separately from
// the block's actual payload capacity, and every caller in this
// codebase -- stb_image's STBI_REALLOC_SIZED -- already knows its own
// old size anyway). ptr == NULL behaves like tox_alloc(new_size);
// new_size == 0 behaves like tox_free(ptr) and returns NULL. On
// allocation failure, the original block is left untouched and NULL is
// returned (matches realloc(3)'s contract -- callers must not assume
// `ptr` was freed on failure).
void* tox_realloc(void* ptr, uint64_t old_size, uint64_t new_size);

// Diagnostics: total bytes currently allocated (payload, excluding
// headers) and free (available for reuse without growing the heap via
// sys_brk again). Used by ToxUI's own self-tests to confirm repeated
// load/free cycles return to the exact same baseline (no drift).
typedef struct {
    uint64_t used_bytes;
    uint64_t free_bytes;
    uint64_t block_count;
} tox_heap_stats_t;
void tox_heap_stats(tox_heap_stats_t* out);

#endif // TOX_HEAP_H
