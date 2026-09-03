// ToxenOS/userlib/toxui/tox_image.h — Milestone 33: the canonical
// decoded-image representation every ToxUI image loader produces and
// every drawing primitive consumes, independent of which file format
// it came from (PNG today -- see tox_stb_impl.c; the point of this
// indirection is that a future JPEG loader could produce the exact
// same tox_image_t with zero change to any drawing/application code).
//
// Canonical pixel format: RGBA8888, 4 bytes per pixel, byte order
// R,G,B,A in MEMORY (pixels[i*4+0]==R, +1==G, +2==B, +3==A) regardless
// of host endianness -- deliberately NOT "a uint32_t 0xRRGGBBAA value"
// (whose BYTE layout would depend on host endianness) or "whatever
// stb_image happens to hand back" (which conveniently already matches
// this exact byte order for its 4-channel output, verified against
// stb_image.h's own documented "R8G8B8A8" output convention -- but
// tox_image_t's definition stands on its own regardless of that detail
// staying true). Real alpha is preserved end to end -- see tox_draw.h's
// blending for how it composites onto a surface (whose OWN format,
// WM_FORMAT_XRGB8888, has no destination alpha channel at all).
//
// Ownership: tox_image_load/tox_image_load_memory allocate `pixels`
// via tox_alloc (userlib/toxui/tox_heap.h) on success; tox_image_free
// releases it and zeroes the struct. A failed load leaves `out`
// entirely untouched (zero it yourself first if you need a defined
// "not loaded" state) and leaks nothing -- every partial allocation
// made during a failed decode is unwound before returning (see
// tox_stb_impl.c's use of stb_image's own STBI_FREE-on-failure
// discipline, which routes through tox_free identically).
#ifndef TOX_IMAGE_H
#define TOX_IMAGE_H
#include <stdint.h>

typedef enum {
    TOX_IMAGE_RGBA8888 = 1,
} tox_image_fmt_t;

typedef struct {
    uint32_t width, height;
    uint32_t stride;         // bytes per row, always width*4 for this format (tightly packed)
    tox_image_fmt_t format;
    uint8_t* pixels;         // tox_alloc'd, owned by this struct -- see tox_image_free
} tox_image_t;

// Sanity cap on decoded image dimensions -- rejects a hostile/corrupt
// file claiming absurd dimensions BEFORE any allocation is attempted
// (defense in depth on top of stb_image's own internal overflow
// checks; see the Milestone 33 summary's PNG validation section).
// Comfortably larger than any real ToxenOS asset (the 1920x1080
// wallpaper included) while still bounding worst-case memory use to a
// sane amount (8192*8192*4 == 256MB, an explicit, deliberate ceiling).
#define TOX_IMAGE_MAX_DIM 8192

// Loads and decodes an image file from the ToxenOS filesystem (via the
// existing VFS/sys_open+sys_handle_read syscalls -- see tox_image.c).
// Returns 0 with `out` filled on success, or -1 (file not found/
// unreadable, or decode failure -- corrupt/truncated/unsupported-mode
// data, oversized dimensions, or allocation failure) with `out` left
// completely untouched.
int tox_image_load(const char* path, tox_image_t* out);

// Decodes an already-in-memory image buffer (e.g. one obtained some
// other way, or used directly by tests that want to exercise malformed
// input without needing a real file on disk). Same contract as
// tox_image_load otherwise.
int tox_image_load_memory(const uint8_t* data, uint64_t len, tox_image_t* out);

// Releases `img`'s pixel data and zeroes the struct (safe to call on an
// already-zeroed/never-loaded tox_image_t -- a no-op in that case).
void tox_image_free(tox_image_t* img);

#endif // TOX_IMAGE_H
