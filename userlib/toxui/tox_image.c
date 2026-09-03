// ToxenOS/userlib/toxui/tox_image.c — see tox_image.h's header comment.
// PNG decoding itself lives in the vendored userlib/toxui/third_party/
// stb_image.h (see tox_stb_impl.c for the ONE translation unit that
// actually instantiates its implementation + all macro overrides) --
// this file is just the ToxenOS-side glue: reading a file off the VFS
// into memory, handing it to stb_image, and wrapping the result in the
// canonical tox_image_t.
#include <stdint.h>
#include "tox_image.h"
#include "tox_heap.h"
#include "tox64.h"

// stb_image's own entry points -- declared here (not by including the
// third_party header, which is only ever #included by tox_stb_impl.c)
// so this file doesn't need to know or care about any of stb_image's
// OTHER declarations, matching this codebase's general precedent of
// exposing third-party internals through the smallest possible seam.
extern unsigned char* stbi_load_from_memory(unsigned char const* buffer, int len, int* x, int* y, int* channels_in_file, int desired_channels);
extern int stbi_info_from_memory(unsigned char const* buffer, int len, int* x, int* y, int* comp);

// Reads the ENTIRE contents of `path` into a tox_alloc'd buffer --
// sys_handle_read (SYS64_HANDLE_READ, see include/syscall64.h) caps
// each call at SYS64_READ_MAX (1024) bytes, so a real asset file (the
// wallpaper especially) needs many calls in a loop, exactly like
// user64/init64.c's own put_buf() already does for OUTPUT in the
// opposite direction.
static int read_whole_file(const char* path, uint8_t** out_data, uint64_t* out_len) {
    uint64_t size = 0;
    if (sys_stat(path, &size, 0) < 0) return -1;
    if (size == 0 || size > 0x10000000ULL) return -1; // 256MB sanity cap -- see TOX_IMAGE_MAX_DIM's own reasoning

    int64_t fd = sys_open(path);
    if (fd < 0) return -1;

    uint8_t* buf = (uint8_t*)tox_alloc(size);
    if (!buf) { sys_close((int)fd); return -1; }

    uint64_t got = 0;
    while (got < size) {
        int64_t n = sys_read((int)fd, (char*)(buf + got), size - got);
        if (n <= 0) { sys_close((int)fd); tox_free(buf); return -1; } // truncated/error mid-read
        got += (uint64_t)n;
    }
    sys_close((int)fd);

    *out_data = buf;
    *out_len = size;
    return 0;
}

int tox_image_load_memory(const uint8_t* data, uint64_t len, tox_image_t* out) {
    if (!data || len == 0 || len > 0x7FFFFFFFULL) return -1; // stb_image's own `int len` parameter bounds this anyway

    // Milestone 33 bug found during self-testing: a PNG declaring
    // absurd dimensions (e.g. 65535x65535) could make stb_image's OWN
    // internal allocation-size arithmetic overflow ITS 32-bit `int`
    // computation before ever reaching STBI_MALLOC, wrapping around to
    // a small allocation that the subsequent decode then wrote well
    // past the end of -- a real heap-corruption / page-fault crash,
    // reproduced via this milestone's own oversized-dimensions self-
    // test. stbi_info_from_memory parses just the header (cheap, no
    // pixel-buffer allocation at all) so dimensions can be validated
    // and rejected BEFORE stbi_load_from_memory ever computes an
    // allocation size from them -- this closes the overflow window
    // entirely rather than only checking (too late) after the fact.
    int info_w = 0, info_h = 0, info_comp = 0;
    if (!stbi_info_from_memory(data, (int)len, &info_w, &info_h, &info_comp)) return -1; // unparseable header
    if (info_w <= 0 || info_h <= 0 || info_w > TOX_IMAGE_MAX_DIM || info_h > TOX_IMAGE_MAX_DIM) return -1;

    int w = 0, h = 0, channels = 0;
    // desired_channels = 4 forces RGBA8888 output regardless of the
    // source PNG's actual color type -- stb_image transparently
    // expands RGB (no alpha channel) to RGBA with alpha=255 for every
    // pixel, so tox_image_t's canonical format never varies by source
    // file, exactly the "generic image representation, not exposing
    // PNG-specific details" the Milestone 33 summary documents.
    unsigned char* pixels = stbi_load_from_memory(data, (int)len, &w, &h, &channels, 4);
    if (!pixels) return -1; // malformed/truncated/unsupported -- stb_image already validated and cleaned up internally

    if (w <= 0 || h <= 0 || w > TOX_IMAGE_MAX_DIM || h > TOX_IMAGE_MAX_DIM || w != info_w || h != info_h) {
        // Defense in depth: re-check post-decode too (and cross-check
        // against the pre-validated header values) rather than trusting
        // either pass alone.
        tox_free(pixels); // STBI_FREE routes here -- see tox_stb_impl.c
        return -1;
    }

    out->width = (uint32_t)w;
    out->height = (uint32_t)h;
    out->stride = (uint32_t)w * 4u;
    out->format = TOX_IMAGE_RGBA8888;
    out->pixels = (uint8_t*)pixels; // already tox_alloc'd (STBI_MALLOC), no copy needed -- see tox_image_free
    return 0;
}

int tox_image_load(const char* path, tox_image_t* out) {
    uint8_t* data = 0;
    uint64_t len = 0;
    if (read_whole_file(path, &data, &len) < 0) return -1;

    int r = tox_image_load_memory(data, len, out);
    tox_free(data); // the FILE buffer is never needed again once stb_image has parsed it -- only the DECODED pixels are kept
    return r;
}

void tox_image_free(tox_image_t* img) {
    if (!img->pixels) return;
    tox_free(img->pixels);
    img->pixels = 0;
    img->width = 0;
    img->height = 0;
    img->stride = 0;
}
