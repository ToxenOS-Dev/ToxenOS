// ToxenOS/userlib/toxui/tox_libc_shim.h — Milestone 33: the handful of
// libc-shaped symbols the vendored third_party/stb_image.h and
// stb_truetype.h need that aren't math (see tox_math.h for those) or
// allocation (see tox_heap.h). ToxenOS's userspace has no libc at all
// (same reasoning as Milestone 31's rust/toxenos_rs/src/mem.rs, which
// solved the identical problem for Rust) -- global (not static)
// definitions so they satisfy the linker wherever stb_image.h's own
// compiled code calls memcpy/memset directly by name.
#ifndef TOX_LIBC_SHIM_H
#define TOX_LIBC_SHIM_H
#include <stdint.h>

void* memcpy(void* dst, const void* src, uint64_t n);
void* memset(void* dst, int val, uint64_t n);
uint64_t tox_strlen(const char* s);

#endif // TOX_LIBC_SHIM_H
