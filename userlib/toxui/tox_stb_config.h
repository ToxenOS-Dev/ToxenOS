// ToxenOS/userlib/toxui/tox_stb_config.h — Milestone 33: every macro
// override userlib/toxui/third_party/stb_image.h and stb_truetype.h
// need to build in ToxenOS's freestanding, libc-less userspace,
// collected in ONE place so tox_stb_impl.c (which instantiates both
// libraries' implementations) and tox_font.c (which only needs
// stb_truetype's declarations/struct layout to call its API) see the
// EXACT same configuration -- these macros affect the third-party
// header's DECLARATIONS too (not just the implementation block), so
// any file including that header at all must include this first.
//
// See tox_stb_impl.c's own header comment for the vendored-library
// provenance/version/license and the format-scope rationale (PNG
// only -- JPEG/BMP/etc left off, a one-line change to re-enable
// later); see tox_math.h's header comment for which STBTT_ math
// macros back genuinely-used code vs. only stb_truetype's unused SDF
// path.
#ifndef TOX_STB_CONFIG_H
#define TOX_STB_CONFIG_H
#include "tox_heap.h"
#include "tox_math.h"
#include "tox_libc_shim.h"

#define STBI_NO_STDIO      // ToxenOS has its own VFS -- see tox_image.c's read_whole_file
// Milestone 33 bug found via a real #GP/page-fault crash: stb_image.h
// auto-detects `__thread`/`_Thread_local` support from the host
// compiler (see its own STBI_THREAD_LOCAL block) and, being built with
// plain GCC, unconditionally decides TLS is available -- but ToxenOS
// userspace has no concept of threads and never sets up a valid FS
// segment base for ANY process, so any real `__thread` access reads
// through an uninitialized/zero FS.base, faulting immediately.
// STBI_NO_THREAD_LOCALS makes it fall back to plain (non-thread-local)
// static globals, which is exactly correct for ToxenOS's single-
// threaded-per-process model.
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_JPEG
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_LINEAR     // no float linear<->sRGB gamma conversion -- avoids needing a real libm pow()
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_ASSERT(x)                ((void)0)
#define STBI_MALLOC(sz)               tox_alloc(sz)
#define STBI_REALLOC_SIZED(p,o,n)     tox_realloc(p,o,n)
#define STBI_FREE(p)                  tox_free(p)

#define STBTT_ifloor(x)   ((int)tox_floor(x))
#define STBTT_iceil(x)    ((int)tox_ceil(x))
#define STBTT_sqrt(x)     tox_sqrt(x)
#define STBTT_pow(x,y)    tox_pow(x,y)
#define STBTT_fmod(x,y)   tox_fmod(x,y)
#define STBTT_cos(x)      tox_cos(x)
#define STBTT_acos(x)     tox_acos(x)
#define STBTT_fabs(x)     tox_fabs(x)
#define STBTT_malloc(x,u) ((void)(u), tox_alloc(x))
#define STBTT_free(x,u)   ((void)(u), tox_free(x))
#define STBTT_assert(x)   ((void)0)
#define STBTT_strlen(x)   tox_strlen(x)
#define STBTT_memcpy      memcpy
#define STBTT_memset      memset

#endif // TOX_STB_CONFIG_H
