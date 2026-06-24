#ifndef EXEC64_H
#define EXEC64_H

// Milestone 6: loads a NEX64 (primary) or ELF64 (fallback) binary by
// path from TxFS64 and runs it in ring3. On success this never returns
// -- the loaded program's own sys64_exit (Milestone 5) is what
// eventually halts, exactly like Milestone 3B/5's hardcoded stub did.
// Returns -1 on any load failure (missing file, unrecognized format /
// wrong architecture, oversized file, malformed segment) -- the caller
// just reports and falls through.
int exec64_load_and_run(const char* path);

#endif // EXEC64_H
