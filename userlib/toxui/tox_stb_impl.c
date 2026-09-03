// ToxenOS/userlib/toxui/tox_stb_impl.c — Milestone 33: the ONE
// translation unit that instantiates stb_image's and stb_truetype's
// implementations (STB_IMAGE_IMPLEMENTATION / STB_TRUETYPE_IMPLEMENTATION).
// All macro overrides live in tox_stb_config.h (shared with tox_font.c,
// which also needs stb_truetype's declarations/struct layout).
//
// Third-party code: userlib/toxui/third_party/stb_image.h (v2.30) and
// stb_truetype.h (v1.26), vendored VERBATIM from
// https://github.com/nothings/stb, commit f0569113c (2024-05-26), dual
// MIT/public-domain licensed (see the license block at the bottom of
// each file -- reproduced unmodified, not re-typed). No line of either
// file has been edited; every ToxenOS-specific adjustment happens
// ENTIRELY through tox_stb_config.h's override macros, exactly the
// customization seam those libraries are designed to be vendored
// through. See userlib/toxui/third_party/README.md for the exact
// provenance record.
//
// Format scope: only PNG is enabled (tox_stb_config.h's STBI_NO_*
// macros disable every other image format stb_image can decode) --
// JPEG support is explicitly optional for Milestone 33 and left off to
// keep this milestone's compiled/tested surface exactly what it
// claims; re-enabling it later is a one-line change (delete
// STBI_NO_JPEG) with zero impact on tox_image_t or any drawing code,
// by design.
#include <stdint.h>
#include "tox_stb_config.h"

#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"
