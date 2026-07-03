// ToxenOS/user64/toxpath64.h — Milestone 12: visible ToxenOS path
// identity. TxFS64 itself still only understands flat, lowercase,
// underscore-separated slash paths (e.g.
// /system_manager/system_tools/command_tools/shw.nex64) -- that stays
// the internal/canonical form, unchanged, exactly per the milestone's
// "do not rewrite the filesystem" constraint. This header is a small,
// pure-userland translation layer so the SHELL and COMMANDS can speak
// ToxenOS's real visible path style instead:
//
//   C:\System Manager\System Tools\Command Tools\shw.nex
//
// Translation rules (both directions are exact inverses of each other):
//   - "C:" drive prefix <-> the existing flat TxFS64 root ("/").
//   - Backslash or forward slash both work as separators going in.
//   - Each directory component: lowercase + spaces become underscores
//     going in ("System Manager" -> "system_manager"); underscores
//     become spaces + each word capitalized coming back out.
//   - The filename's extension: visible .nex/.elf <-> internal
//     .nex64/.elf64 (the visible extension hides the 64-bit detail --
//     the file's actual magic/header still says NEX64). The filename
//     stem itself is left alone coming back out (no capitalization),
//     unlike directory components.
//   - A path that's already internal-style (starts with '/') passes
//     through unchanged either way -- legacy slash input keeps working
//     everywhere, per the milestone's explicit requirement.
//   - One matching pair of surrounding double quotes is stripped before
//     translation, so a caller can hand toxpath64_to_internal a raw
//     shell token like `"C:\System Manager\...\shw.nex"` (quoting is
//     how a space-containing visible path survives shell64's
//     whitespace-based tokenizer as a single command token).
#ifndef TOXPATH64_H
#define TOXPATH64_H

#define TOXPATH64_CMDTOOLS_INTERNAL "/system_manager/system_tools/command_tools/"
#define TOXPATH64_CMDTOOLS_VISIBLE  "C:\\System Manager\\System Tools\\Command Tools\\"

__attribute__((unused))
static int toxpath64_strlen(const char* s) {
    int i = 0;
    while (s[i]) i++;
    return i;
}

// Strips one matching leading/trailing '"' pair, in place. No-op if `s`
// isn't quoted that way.
__attribute__((unused))
static void toxpath64_strip_quotes(char* s) {
    int len = toxpath64_strlen(s);
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') {
        for (int i = 0; i < len - 2; i++) s[i] = s[i + 1];
        s[len - 2] = 0;
    }
}

// 1 if `s` (already quote-stripped) looks like a path rather than a
// bare command name -- a leading '/' (internal-style) or a "C:"/"c:"
// drive prefix (visible-style).
__attribute__((unused))
static int toxpath64_looks_like_path(const char* s) {
    if (s[0] == '/') return 1;
    if ((s[0] == 'C' || s[0] == 'c') && s[1] == ':') return 1;
    return 0;
}

__attribute__((unused))
static void toxpath64_componentize(char* s) {
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (c == ' ') s[i] = '_';
        else if (c >= 'A' && c <= 'Z') s[i] = (char)(c - 'A' + 'a');
    }
}

// Translates a visible ToxenOS path (or an already-internal slash path,
// passed through unchanged) into the internal TxFS64 path. Returns 0 on
// success. The only failure case is a legacy slash path too long to fit
// `out` -- every other input is translated best-effort (severely
// oversized visible-path components are silently truncated to fit `max`,
// matching this codebase's existing "truncate safely" convention rather
// than failing loudly on it).
__attribute__((unused))
static int toxpath64_to_internal(const char* visible_in, char* out, int max) {
    char tmp[256];
    int n = 0;
    while (visible_in[n] && n < (int)sizeof(tmp) - 1) { tmp[n] = visible_in[n]; n++; }
    tmp[n] = 0;
    toxpath64_strip_quotes(tmp);

    const char* p = tmp;

    if (p[0] == '/') {
        int i = 0;
        while (p[i] && i < max - 1) { out[i] = p[i]; i++; }
        out[i] = 0;
        return (p[i] == 0) ? 0 : -1;
    }

    if ((p[0] == 'C' || p[0] == 'c') && p[1] == ':') p += 2;

    int oi = 0;
    if (oi < max - 1) out[oi++] = '/';

    char comp[64];
    int ci = 0;
    while (*p == '\\' || *p == '/') p++;

    while (*p) {
        if (*p == '\\' || *p == '/') {
            comp[ci] = 0;
            toxpath64_componentize(comp);
            for (int i = 0; comp[i] && oi < max - 1; i++) out[oi++] = comp[i];
            if (oi < max - 1) out[oi++] = '/';
            ci = 0;
            while (*p == '\\' || *p == '/') p++;
            continue;
        }
        if (ci < (int)sizeof(comp) - 1) comp[ci++] = *p;
        p++;
    }

    comp[ci] = 0;
    toxpath64_componentize(comp);
    int clen = toxpath64_strlen(comp);
    const char* suffix = "";
    if (clen >= 4 && comp[clen-4] == '.' && comp[clen-3] == 'n' && comp[clen-2] == 'e' && comp[clen-1] == 'x') suffix = "64";
    else if (clen >= 4 && comp[clen-4] == '.' && comp[clen-3] == 'e' && comp[clen-2] == 'l' && comp[clen-1] == 'f') suffix = "64";
    for (int i = 0; comp[i] && oi < max - 1; i++) out[oi++] = comp[i];
    for (int i = 0; suffix[i] && oi < max - 1; i++) out[oi++] = suffix[i];
    out[oi] = 0;

    return 0;
}

// Milestone 15: the single-component half of the transform
// toxpath64_to_visible applies per path segment, extracted so it can
// also be reused by the `ls` command on a single directory-entry name
// (not a full path) -- there, the right transform is keyed off "is
// this entry a file" rather than "is this the path's last component",
// a different but compatible selection of the same two operations.
// Operates on comp in place.
//   is_file = 1: strip a trailing .nex64/.elf64 "64" suffix (the
//     filename stem itself is left untouched, no capitalization).
//   is_file = 0: underscore -> space, then Title-Case each word
//     (matches every existing directory-component display, e.g.
//     "system_manager" -> "System Manager").
__attribute__((unused))
static void toxpath64_format_component(char* comp, int is_file) {
    if (is_file) {
        int clen = toxpath64_strlen(comp);
        if (clen >= 6 && comp[clen-2] == '6' && comp[clen-1] == '4' && comp[clen-6] == '.' &&
            ((comp[clen-5]=='n' && comp[clen-4]=='e' && comp[clen-3]=='x') ||
             (comp[clen-5]=='e' && comp[clen-4]=='l' && comp[clen-3]=='f'))) {
            comp[clen - 2] = 0;  // drop the trailing "64"
        }
    } else {
        int start_of_word = 1;
        for (int i = 0; comp[i]; i++) {
            if (comp[i] == '_') { comp[i] = ' '; start_of_word = 1; continue; }
            if (start_of_word && comp[i] >= 'a' && comp[i] <= 'z') comp[i] = (char)(comp[i] - 'a' + 'A');
            start_of_word = 0;
        }
    }
}

// Milestone 17: applies one path component to the accumulator `acc`
// (an internal slash path being built up, capacity acc_max) in place --
// "." is a no-op, ".." pops the last component (clamped at root, never
// underflows past "/"), anything else is componentized (lowercase +
// spaces->underscores) and appended. Shared by toxpath64_resolve_cwd's
// loop and its final (no-trailing-separator) component.
__attribute__((unused))
static void toxpath64_apply_component(char* acc, int acc_max, const char* comp) {
    if (comp[0] == '.' && comp[1] == 0) {
        return;
    }
    if (comp[0] == '.' && comp[1] == '.' && comp[2] == 0) {
        int alen = toxpath64_strlen(acc);
        if (alen > 1) {
            int j = alen - 1;
            while (j > 0 && acc[j] != '/') j--;
            if (j == 0) { acc[0] = '/'; acc[1] = 0; }
            else acc[j] = 0;
        }
        return;
    }

    char tmp[64];
    int i = 0;
    while (comp[i] && i < (int)sizeof(tmp) - 1) { tmp[i] = comp[i]; i++; }
    tmp[i] = 0;
    toxpath64_componentize(tmp);

    int alen = toxpath64_strlen(acc);
    if (alen == 0 || acc[alen - 1] != '/') {
        if (alen < acc_max - 1) { acc[alen] = '/'; acc[alen + 1] = 0; alen++; }
    }
    int k = 0;
    while (tmp[k] && alen < acc_max - 1) { acc[alen++] = tmp[k]; k++; }
    acc[alen] = 0;
}

// Milestone 17: resolves `input_in` (quoted/relative/absolute, visible
// or legacy-internal) against `cwd_internal` (always an internal slash
// path, e.g. "/" at the drive root or "/system_manager") into an
// internal absolute path in `out` -- drives shell64's `cd`/`ls`
// cwd-aware navigation. An absolute input -- leading "/" legacy-internal
// (used verbatim, no componentization, the lower-level M11/M12
// convention), "C:"/"c:" visible drive prefix, or a bare leading "\"
// (ToxenOS-rooted without a drive letter, e.g. `cd \` or
// `\System Manager` -- this is a single-drive system today, so it's
// root just like "C:\") -- ignores cwd_internal entirely; anything else
// is resolved component-by-component starting from cwd_internal, so
// "System Manager\System Tools" while sitting at the root lands on the
// same place as "C:\System Manager\System Tools". Best-effort, always
// "succeeds" (silently truncates on a pathological oversized input,
// matching every other function in this header).
__attribute__((unused))
static void toxpath64_resolve_cwd(const char* cwd_internal, const char* input_in, char* out, int max) {
    char tmp[256];
    int n = 0;
    while (input_in[n] && n < (int)sizeof(tmp) - 1) { tmp[n] = input_in[n]; n++; }
    tmp[n] = 0;
    toxpath64_strip_quotes(tmp);

    char acc[256];
    const char* p = tmp;

    if (p[0] == '/') {
        int i = 0;
        while (p[i] && i < (int)sizeof(acc) - 1) { acc[i] = p[i]; i++; }
        acc[i] = 0;
        p = "";
    } else if ((p[0] == 'C' || p[0] == 'c') && p[1] == ':') {
        acc[0] = '/'; acc[1] = 0;
        p += 2;
    } else if (p[0] == '\\') {
        acc[0] = '/'; acc[1] = 0;
        p += 1;
    } else {
        int i = 0;
        while (cwd_internal[i] && i < (int)sizeof(acc) - 1) { acc[i] = cwd_internal[i]; i++; }
        acc[i] = 0;
    }

    while (*p == '\\' || *p == '/') p++;

    char comp[64];
    int ci = 0;
    while (*p) {
        if (*p == '\\' || *p == '/') {
            comp[ci] = 0;
            if (ci > 0) toxpath64_apply_component(acc, (int)sizeof(acc), comp);
            ci = 0;
            while (*p == '\\' || *p == '/') p++;
            continue;
        }
        if (ci < (int)sizeof(comp) - 1) comp[ci++] = *p;
        p++;
    }
    if (ci > 0) {
        comp[ci] = 0;
        toxpath64_apply_component(acc, (int)sizeof(acc), comp);
    }

    int i = 0;
    while (acc[i] && i < max - 1) { out[i] = acc[i]; i++; }
    out[i] = 0;
}

// Milestone 18: directory-specific display variant -- all path
// components (including the final one) are formatted as directory names
// (Title-Case), not as the last/file component. Used when displaying
// the current working directory (prompt, `pwd`) where the final
// component is always a folder, never a file. toxpath64_to_visible is
// still used for file paths like `where` output (e.g. "shw.nex").
__attribute__((unused))
static void toxpath64_to_visible_dir(const char* internal, char* out, int max) {
    int oi = 0;
    if (oi < max - 1) out[oi++] = 'C';
    if (oi < max - 1) out[oi++] = ':';

    const char* p = internal;
    if (*p == '/') p++;

    char comp[64];
    int ci = 0;
    for (;;) {
        char c = *p;
        if (c == '/' || c == 0) {
            comp[ci] = 0;
            toxpath64_format_component(comp, 0);  // always dir/Title-Case
            if (oi < max - 1) out[oi++] = '\\';
            for (int i = 0; comp[i] && oi < max - 1; i++) out[oi++] = comp[i];
            ci = 0;
            if (c == 0) break;
            p++;
            continue;
        }
        if (ci < (int)sizeof(comp) - 1) comp[ci++] = c;
        p++;
    }
    out[oi] = 0;
}

// Reverse of toxpath64_to_internal -- formats an internal TxFS64 slash
// path for display in ToxenOS's visible C:\ style. Best-effort, always
// "succeeds" (silently truncates to fit `max` on a pathological input).
__attribute__((unused))
static void toxpath64_to_visible(const char* internal, char* out, int max) {
    int oi = 0;
    if (oi < max - 1) out[oi++] = 'C';
    if (oi < max - 1) out[oi++] = ':';

    const char* p = internal;
    if (*p == '/') p++;

    char comp[64];
    int ci = 0;
    for (;;) {
        char c = *p;
        if (c == '/' || c == 0) {
            comp[ci] = 0;
            int is_last = (c == 0);

            toxpath64_format_component(comp, is_last);

            if (oi < max - 1) out[oi++] = '\\';
            for (int i = 0; comp[i] && oi < max - 1; i++) out[oi++] = comp[i];
            ci = 0;
            if (c == 0) break;
            p++;
            continue;
        }
        if (ci < (int)sizeof(comp) - 1) comp[ci++] = c;
        p++;
    }
    out[oi] = 0;
}

#endif // TOXPATH64_H
