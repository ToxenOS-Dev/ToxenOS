#ifndef EXEC64_H
#define EXEC64_H

#include <stdint.h>
#include "paging64.h"

// Milestone 6/7/8: loads a NEX64 (primary) or ELF64 (fallback) binary by
// path from TxFS64 into the CALLER-OWNED address space `as` (Milestone 8:
// kernel/paging64.c, not a shared global page table) -- parses the file,
// maps its segments and a user stack page via paging64_map_user_page.
// Does NOT activate the address space (no CR3 write) or enter ring3
// itself -- that's kernel/userproc64.c's job, which owns CR3 activation
// and the process's kernel stack/RSP0. On success, writes the program's
// entry point, computed user stack top, and a heap_start hint (highest
// loaded vaddr, page-rounded -- no pages mapped there, just plumbing for
// a future brk-style syscall) to the three out-params and returns 0.
// Returns -1 on any load failure (missing file, unrecognized format /
// wrong architecture, oversized file, malformed segment, out-of-memory)
// -- the caller just reports and falls through.
int exec64_load(paging64_as_t* as, const char* path, uint64_t* entry_out,
                 uint64_t* stack_top_out, uint64_t* heap_start_out);

#endif // EXEC64_H
