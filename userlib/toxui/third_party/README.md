# Vendored third-party code

Both files in this directory are vendored **verbatim** (byte-for-byte,
no ToxenOS-specific edits) from the [stb](https://github.com/nothings/stb)
project by Sean Barrett.

| File               | Version | Commit                                    | Date       |
|---------------------|---------|--------------------------------------------|------------|
| `stb_image.h`       | v2.30   | `f0569113c93ad095470c54bf34a17b36646bbbb5` | 2024-05-26 |
| `stb_truetype.h`    | v1.26   | `f0569113c93ad095470c54bf34a17b36646bbbb5` | 2024-05-26 |

Source: `https://raw.githubusercontent.com/nothings/stb/<commit>/<file>`

## License

Both files are dual-licensed (reader's choice) under the MIT License or
public domain (Unlicense) — the full license text is reproduced,
unmodified, at the bottom of each file exactly as shipped upstream. No
separate `LICENSE` file is needed here since each vendored file already
carries its own complete license text.

## ToxenOS-specific customization

**No line of either file has been edited.** All ToxenOS-specific
configuration (routing memory allocation through `tox_heap.h`, math
through `tox_math.h`, disabling image formats other than PNG, etc.)
happens entirely through the standard `#define` override mechanism
these libraries are designed to be configured through — see
`../tox_stb_config.h` for the complete list of overrides, and
`../tox_stb_impl.c` for the one translation unit that actually
instantiates `STB_IMAGE_IMPLEMENTATION` / `STB_TRUETYPE_IMPLEMENTATION`.

## Why these libraries

See the Milestone 33 technical summary for the full assessment; in
short: both are small, single-file, dependency-free (beyond the
overridable malloc/memcpy/math macros), extremely widely used and
audited, and explicitly designed to be embedded in exactly this kind
of freestanding/embedded environment — a much better fit than pulling
in libpng+zlib or FreeType's larger dependency chains for a kernel
project with no libc.
