# ToxenOS visual assets

Real files, installed into the TxFS64 filesystem image by the
`populate` Makefile target under
`/system_manager/system_data/display_interface/` — genuinely read at
runtime through the VFS (`sys_open`/`sys_read`) by `userlib/toxui`,
never baked into any C source. See the Milestone 33 technical summary
for the full filesystem layout rationale.

## `fonts/DejaVuSans.ttf`

**DejaVu Sans**, used as Milestone 33's scalable-font test asset.
Licensed under the **Bitstream Vera License** (see
`fonts/DejaVuSans-LICENSE.txt` for the complete, unmodified license
text) — a permissive license explicitly permitting redistribution,
modification, and use in both free and commercial software, with no
copyleft obligation on software using the font. DejaVu is one of the
most widely redistributed open fonts in existence (bundled by
essentially every Linux distribution) specifically because of this
license. Obtained from this machine's locally-installed DejaVu Fonts
package (version corresponding to DejaVu Fonts 2.37).

This is a **placeholder for testing scalable-font architecture only**
— the final ToxenOS design font will be selected later, per the
Milestone 33 instructions.

## `backgrounds/toxenos-default.png`

The **real** ToxenOS default wallpaper, supplied directly by the
project owner (originally `Light-TX-Background.png`, 1536x1024,
8-bit RGB) and copied into this repo byte-for-byte (verified via
`sha256sum`) — never redrawn, recolored, regenerated, or approximated.
This is the genuine Milestone 33 wallpaper acceptance-test asset,
loaded at runtime through the VFS by `user64/wallpaper_demo64.c`, not
hardcoded into any source.

## `icons/placeholder-logo.png`

**Placeholder**, synthetically generated (a simple flat-color icon
shape) purely to exercise and test the PNG-loading/alpha-blending
pipeline end to end — no real transparent ToxenOS logo PNG has been
supplied yet (the ToxenOS SVG logo is a separate master design asset;
per the Milestone 33 instructions, runtime rendering uses rasterized
PNG icon sizes, not the SVG itself). Replace this file with the real
transparent logo PNG (same filename, same path) and re-run `make
populate` when it's ready — no source code or kernel rebuild is
required, by design.
