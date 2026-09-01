#ifndef EXEC64_H
#define EXEC64_H

#include <stdint.h>
#include "paging64.h"

// Milestone 6/7/8: loads a NEX64 (primary) or ELF64 (fallback) binary by
// path from TxFS64 into the CALLER-OWNED address space `as` -- parses
// the file and maps its segments (Milestone 25: into
// include/uservm64.h's USER_IMAGE_BASE region, honoring each segment's
// read/write/execute flags via the generalized kernel/paging64.c API)
// plus one user stack page (USER_STACK_TOP - 4KB). Does NOT activate
// the address space (no CR3 write) or enter ring3 itself -- that's
// kernel/process64.c's job. On success, writes the program's entry
// point and computed user stack top to the two out-params and returns
// 0. Milestone 25: no longer returns a heap_start hint -- the heap now
// has its own fixed region (include/uservm64.h's USER_HEAP_BASE),
// independent of wherever the loaded image happens to end, so
// kernel/process64.c just calls uservm64_init() directly instead.
// Returns -1 on any load failure (missing file, unrecognized format /
// wrong architecture, oversized file, malformed segment, out-of-memory)
// -- the caller just reports and falls through.
int exec64_load(paging64_as_t* as, const char* path, uint64_t* entry_out,
                 uint64_t* stack_top_out);

#endif // EXEC64_H
