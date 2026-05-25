// ToxenOS/user/bin/edit.c — simple terminal text editor (nano-style)
// Keys: arrows=move, Enter=newline, Backspace=delete, Ctrl+S=save,
//       Ctrl+Q=quit, Ctrl+K=kill line, Ctrl+G=go to line
#include "../tox.h"

// ── helpers ────────────────────────────────────────────────────────────────────
static int  elen(const char* s)             { return tox_strlen(s); }
static void ecpy(char* d, const char* s)    { tox_strcpy(d, s); }
static void ecat(char* d, const char* s)    { tox_strcat(d, s); }
static int  eeq(const char* a, const char* b){ return !tox_strcmp(a, b); }

static void itoa2(int n, char* b) {
    if (n < 0) { b[0]='-'; itoa2(-n,b+1); return; }
    if (!n)    { b[0]='0'; b[1]=0; return; }
    char t[12]; int i=0;
    while (n) { t[i++]='0'+n%10; n/=10; }
    int j=0; while (i>0) b[j++]=t[--i]; b[j]=0;
}

// ── syntax highlighting ────────────────────────────────────────────────────────
#define SYN_DEF  0x07   // default: light gray
#define SYN_CMT  0x0A   // comment: bright green
#define SYN_STR  0x06   // string: brown/orange
#define SYN_KEY  0x09   // keyword: bright blue
#define SYN_FUN  0x0E   // function call: bright yellow
#define SYN_NUM  0x0B   // number: bright cyan
#define SYN_OP   0x03   // operator: dark cyan
#define SYN_PUN  0x08   // punctuation: dark gray

static const char* syn_kw[] = {
    "var","string","int","float","bool",
    "if","else","for","while","foreach","in","return","break","continue",
    "print","true","false", 0
};

static int syn_eqn(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int syn_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int syn_alnum(char c) {
    return syn_alpha(c) || (c >= '0' && c <= '9');
}

static void colorize(const char* ln, uint8_t* col, int len) {
    int i = 0;
    while (i < len) {
        if (ln[i] == '/' && i+1 < len && ln[i+1] == '/') {
            while (i < len) col[i++] = SYN_CMT;
            return;
        }
        if (ln[i] == '"' || ln[i] == '\'') {
            char q = ln[i];
            // Lookahead: only color if the closing quote exists on this line
            int end = i + 1;
            while (end < len && ln[end] != q) end++;
            if (end < len) {
                // Closed string — color everything including both quotes
                while (i <= end) col[i++] = SYN_STR;
            } else {
                // Unclosed string — leave opening quote as default, move on
                col[i++] = SYN_DEF;
            }
            continue;
        }
        if (ln[i] >= '0' && ln[i] <= '9') {
            while (i < len && ((ln[i] >= '0' && ln[i] <= '9') || ln[i] == '.'))
                col[i++] = SYN_NUM;
            continue;
        }
        if (syn_alpha(ln[i])) {
            int s = i;
            while (i < len && syn_alnum(ln[i])) i++;
            int wl = i - s;
            // Function call: identifier immediately followed by '('
            if (i < len && ln[i] == '(') {
                for (int j = s; j < i; j++) col[j] = SYN_FUN;
            } else {
                uint8_t c = SYN_DEF;
                for (int k = 0; syn_kw[k]; k++) {
                    int kl = elen(syn_kw[k]);
                    if (kl == wl && syn_eqn(ln+s, syn_kw[k], wl)) { c = SYN_KEY; break; }
                }
                for (int j = s; j < i; j++) col[j] = c;
            }
            continue;
        }
        if (ln[i] == '{' || ln[i] == '}' || ln[i] == '(' || ln[i] == ')' ||
            ln[i] == '[' || ln[i] == ']' || ln[i] == ';' || ln[i] == ',') {
            col[i++] = SYN_PUN;
            continue;
        }
        if (ln[i] == '=' || ln[i] == '+' || ln[i] == '-' || ln[i] == '*' ||
            ln[i] == '/' || ln[i] == '%' || ln[i] == '<' || ln[i] == '>' ||
            ln[i] == '!' || ln[i] == '&' || ln[i] == '|') {
            col[i++] = SYN_OP;
            continue;
        }
        col[i++] = SYN_DEF;
    }
}

// ── buffer ─────────────────────────────────────────────────────────────────────
#define MAX_LINES    256
#define MAX_LINE_LEN 200

static char  lines[MAX_LINES][MAX_LINE_LEN + 1];
static int   nlines   = 1;
static int   crow     = 0;  // cursor row
static int   ccol     = 0;  // cursor col
static int   vtop     = 0;  // viewport top row
static int   vleft    = 0;  // viewport left col
static int   modified = 0;
static char  filename[256] = "";

// Terminal dimensions (queried at startup)
static int COLS  = 128;
static int ROWS  = 48;
static int TROWS = 44; // text rows = ROWS - 4

static void query_term_size(void) {
    char buf[16];
    if (tox_sysctl("term.cols", buf, sizeof(buf)) >= 0) {
        int v = 0;
        for (int i = 0; buf[i] >= '0' && buf[i] <= '9'; i++) v = v*10 + (buf[i]-'0');
        if (v >= 40) COLS = v;
    }
    if (tox_sysctl("term.rows", buf, sizeof(buf)) >= 0) {
        int v = 0;
        for (int i = 0; buf[i] >= '0' && buf[i] <= '9'; i++) v = v*10 + (buf[i]-'0');
        if (v >= 10) ROWS = v;
    }
    TROWS = ROWS - 4;  // header + top-div + bottom-div + hints = 4 rows overhead
    if (TROWS < 4) TROWS = 4;
}

// ── file I/O ───────────────────────────────────────────────────────────────────
static void load_file(const char* path) {
    int sz = tox_stat(path);
    if (sz <= 0) { nlines = 1; lines[0][0] = 0; return; }
    static char fbuf[MAX_LINES * (MAX_LINE_LEN + 2)];
    int fd = tox_open(path, 1);
    if (fd < 0) { nlines = 1; lines[0][0] = 0; return; }
    int n = tox_read(fd, (uint8_t*)fbuf, (uint32_t)(sizeof(fbuf)-1));
    tox_close(fd);
    if (n < 0) n = 0; fbuf[n] = 0;

    nlines = 0; int i = 0;
    if (n == 0) { nlines = 1; lines[0][0] = 0; return; }
    lines[nlines++][0] = 0; // start first line
    int li = 0;
    while (i < n && nlines <= MAX_LINES) {
        char c = fbuf[i++];
        if (c == '\r') continue;
        if (c == '\n') {
            lines[nlines-1][li] = 0; li = 0;
            if (nlines < MAX_LINES) { lines[nlines][0] = 0; nlines++; }
        } else {
            if (li < MAX_LINE_LEN) lines[nlines-1][li++] = c;
        }
    }
    lines[nlines-1][li] = 0;
    if (nlines == 0) { nlines = 1; lines[0][0] = 0; }
}

static int save_file(const char* path) {
    int fd = tox_open(path, 0x16); // VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC
    if (fd < 0) return 0;
    for (int r = 0; r < nlines; r++) {
        int ll = elen(lines[r]);
        if (ll > 0) tox_write(fd, (const uint8_t*)lines[r], (uint32_t)ll);
        tox_write(fd, (const uint8_t*)"\n", 1);
    }
    tox_close(fd);
    modified = 0;
    return 1;
}

// ── rendering ──────────────────────────────────────────────────────────────────
static void print_repeat(char c, int n) {
    char buf[2] = {c, 0};
    for (int i = 0; i < n; i++) print(buf);
}

static void print_n(const char* s, int n) {
    char buf[2] = {0, 0};
    for (int i = 0; i < n && s[i]; i++) { buf[0] = s[i]; print(buf); }
}

static void print_padded(const char* s, int w) {
    int l = elen(s);
    print(s);
    if (l < w) print_repeat(' ', w - l);
}

static void redraw(void) {
    // Home the cursor to (0,0) without clearing — avoids the black-flash caused by
    // fbterm_clear() wiping the entire framebuffer before each redraw.
    print("\x0C");

    // ── header (row 0) — at most COLS-1 chars to avoid auto-wrap ──
    char lbuf[12], cbuf[12], nlbuf[12];
    itoa2(crow + 1, lbuf); itoa2(ccol + 1, cbuf); itoa2(nlines, nlbuf);
    char pos[32]; pos[0] = 0;
    ecat(pos, "L:"); ecat(pos, lbuf); ecat(pos, "/"); ecat(pos, nlbuf);
    ecat(pos, " C:"); ecat(pos, cbuf); ecat(pos, " ");

    char hbuf[256]; hbuf[0] = 0;
    ecat(hbuf, " ToxenOS Edit | ");
    ecat(hbuf, filename[0] ? filename : "[new file]");
    if (modified) ecat(hbuf, " *");

    int max_hdr = COLS - 1;
    int lw = elen(hbuf), rw = elen(pos);
    if (lw + rw > max_hdr) { hbuf[max_hdr - rw] = 0; lw = max_hdr - rw; }
    int pad = max_hdr - lw - rw;

    set_color(0x1F);
    print(hbuf);
    if (pad > 0) print_repeat(' ', pad);
    print(pos);
    set_color(0x07); print("\n");

    // ── top divider (row 1) ──
    set_color(0x08); print_repeat('-', COLS - 1); set_color(0x07); print("\n");

    // ── text area ── adjust viewport first
    if (crow < vtop) vtop = crow;
    if (crow >= vtop + TROWS) vtop = crow - TROWS + 1;
    if (ccol < vleft) vleft = ccol;
    if (ccol >= vleft + COLS - 1) vleft = ccol - COLS + 2;
    if (vleft < 0) vleft = 0;

    static uint8_t lc[MAX_LINE_LEN + 1];

    for (int r = vtop; r < vtop + TROWS; r++) {
        int chars_drawn = 0;
        if (r < nlines) {
            char* ln = lines[r];
            int ll   = elen(ln);
            int is_cur = (r == crow);
            int cursor_shown = 0;

            colorize(ln, lc, ll);

            for (int c = vleft; c < vleft + COLS - 1; c++) {
                if (is_cur && c == ccol) {
                    set_color(0x70);
                    char ch[2] = {(c < ll) ? ln[c] : ' ', 0};
                    print(ch); set_color(0x07);
                    chars_drawn++; cursor_shown = 1;
                } else if (c < ll) {
                    char ch[2] = {ln[c], 0};
                    set_color(lc[c]);
                    print(ch); set_color(0x07);
                    chars_drawn++;
                } else {
                    if (is_cur && !cursor_shown) {
                        set_color(0x70); print(" "); set_color(0x07);
                        chars_drawn++;
                    }
                    break;
                }
            }
        } else {
            set_color(0x08); print("~"); set_color(0x07);
            chars_drawn = 1;
        }
        // Pad to COLS-1 to overwrite any stale content from the previous frame
        if (chars_drawn < COLS - 1) {
            set_color(0x07);
            print_repeat(' ', COLS - 1 - chars_drawn);
        }
        print("\n");
    }

    // ── bottom divider ──
    set_color(0x08); print_repeat('-', COLS - 1); set_color(0x07); print("\n");

    // ── hints — pad to COLS-1 and no trailing \n to avoid last-row scroll ──
    const char* hints = "^S Save  ^Q Quit  ^K Kill Line  ^G Go To  Arrows Navigate";
    int hlen = elen(hints);
    set_color(0x08);
    print(hints);
    if (hlen < COLS - 1) print_repeat(' ', COLS - 1 - hlen);
    set_color(0x07);
}

// ── editing ops ────────────────────────────────────────────────────────────────
static void clamp_col(void) {
    int ll = elen(lines[crow]);
    if (ccol > ll) ccol = ll;
    if (ccol < 0)  ccol = 0;
}

static void insert_char(char c) {
    char* ln = lines[crow];
    int ll = elen(ln);
    if (ll >= MAX_LINE_LEN) return;
    // Shift right from ccol
    for (int i = ll; i >= ccol; i--) ln[i+1] = ln[i];
    ln[ccol] = c; ccol++;
    modified = 1;
}

static void delete_char_at_cursor(void) {
    char* ln = lines[crow];
    int ll = elen(ln);
    if (ccol >= ll) return;
    for (int i = ccol; i < ll; i++) ln[i] = ln[i+1];
    modified = 1;
}

static void backspace(void) {
    if (ccol > 0) {
        ccol--;
        delete_char_at_cursor();
    } else if (crow > 0) {
        // Merge current line with previous
        char* prev = lines[crow-1];
        char* cur  = lines[crow];
        int pl = elen(prev);
        int cl = elen(cur);
        if (pl + cl <= MAX_LINE_LEN) {
            ecat(prev, cur);
            // Shift lines up
            for (int r = crow; r < nlines - 1; r++)
                ecpy(lines[r], lines[r+1]);
            nlines--;
            crow--; ccol = pl;
        }
        modified = 1;
    }
}

static void insert_newline(void) {
    if (nlines >= MAX_LINES) return;
    char* ln = lines[crow];
    int ll = elen(ln);

    // Shift lines down
    for (int r = nlines; r > crow + 1; r--) ecpy(lines[r], lines[r-1]);
    nlines++;

    // Split current line at ccol
    ecpy(lines[crow+1], ln + ccol);
    ln[ccol] = 0;

    crow++; ccol = 0;
    modified = 1;
}

static void kill_line(void) {
    char* ln = lines[crow];
    if (elen(ln) > 0 && ccol < elen(ln)) {
        // Delete from cursor to end of line
        ln[ccol] = 0;
    } else if (nlines > 1) {
        // Line is empty or cursor at end — delete the newline (merge)
        for (int r = crow; r < nlines - 1; r++) ecpy(lines[r], lines[r+1]);
        nlines--;
        if (crow >= nlines) crow = nlines - 1;
        clamp_col();
    }
    modified = 1;
}

// ── go to line prompt ─────────────────────────────────────────────────────────
static void goto_line_prompt(void) {
    // Draw prompt as overlay over the top rows — no tox_clear, no black flash.
    // The editor content below row 2 stays visible.
    print("\x0C");  // cursor home without clearing

    // Row 0: header
    char hbuf[256]; hbuf[0] = 0;
    ecat(hbuf, " ToxenOS Edit | Go To Line");
    int hw = elen(hbuf);
    set_color(0x1F); print(hbuf);
    if (hw < COLS - 1) print_repeat(' ', COLS - 1 - hw);
    set_color(0x07); print("\n");

    // Row 1: divider
    set_color(0x08); print_repeat('-', COLS - 1); set_color(0x07); print("\n");

    // Row 2: prompt + input
    set_color(0x0E); print("  Line: "); set_color(0x07);
    print("\x10");  // show cursor during input

    char buf[16]; int bi = 0;
    while (1) {
        char c = tox_getchar();
        if (c == '\n' || c == '\r') break;
        if (c == 0x1B || c == 0x11) { bi = 0; break; }  // Esc or Ctrl+Q = cancel
        if ((c == 0x08 || c == 0x7F) && bi > 0) { bi--; tox_erase(); continue; }
        if (c >= '0' && c <= '9' && bi < 15) {
            buf[bi++] = c;
            char ch[2] = {c, 0}; print(ch);
        }
    }

    print("\x0F");  // hide cursor again

    buf[bi] = 0;
    if (bi > 0) {
        int target = 0;
        for (int i = 0; buf[i]; i++) target = target * 10 + (buf[i] - '0');
        target--;  // 1-based to 0-based
        if (target < 0) target = 0;
        if (target >= nlines) target = nlines - 1;
        crow = target; ccol = 0;
        vtop = crow - TROWS / 2;
        if (vtop < 0) vtop = 0;
    }
}

// ── entry point ────────────────────────────────────────────────────────────────
void _start() {
    static char argbuf[256];
    tox_get_args(argbuf);
    const char* arg = argbuf;
    while (*arg == ' ') arg++;

    query_term_size();

    if (arg && arg[0]) {
        // Resolve path
        char cwd[256] = "/C:";
        tox_getenv("CWD", cwd, sizeof(cwd));
        if (arg[0] == '/') ecpy(filename, arg);
        else { ecpy(filename, cwd); ecat(filename, "/"); ecat(filename, arg); }
        load_file(filename);
    } else {
        nlines = 1; lines[0][0] = 0;
    }

    crow = 0; ccol = 0; vtop = 0; vleft = 0;

    print("\x0F");  // hide fbterm cursor — editor uses its own inverted-cell cursor

    while (1) {
        redraw();
        char c = tox_getchar();

        // Ctrl+S = save
        if (c == 0x13) {
            if (!filename[0]) {
                // Prompt for filename — for now just show message
                set_color(0x0E); print("\nNo filename. Use: edit <filename>\n"); set_color(0x07);
                tox_getchar();
            } else {
                if (save_file(filename)) {
                    // Show saved message briefly (will redraw on next key)
                }
            }
            continue;
        }

        // Ctrl+Q = quit
        if (c == 0x11) {
            if (modified) {
                // Show confirm
                tox_clear();
                set_color(0x0C); print("\nUnsaved changes. Ctrl+S to save, any key to quit.\n");
                set_color(0x07);
                char cc = tox_getchar();
                if (cc == 0x13) { save_file(filename); }
            }
            break;
        }

        // Ctrl+K = kill line
        if (c == 0x0B) { kill_line(); continue; }

        // Ctrl+G = go to line
        if (c == 0x07) { goto_line_prompt(); continue; }

        // Arrow keys
        if (c == 0x01) { // Up
            if (crow > 0) { crow--; clamp_col(); }
            continue;
        }
        if (c == 0x02) { // Down
            if (crow < nlines - 1) { crow++; clamp_col(); }
            continue;
        }
        if (c == 0x03) { // Left
            if (ccol > 0) ccol--;
            else if (crow > 0) { crow--; ccol = elen(lines[crow]); }
            continue;
        }
        if (c == 0x04) { // Right
            int ll = elen(lines[crow]);
            if (ccol < ll) ccol++;
            else if (crow < nlines - 1) { crow++; ccol = 0; }
            continue;
        }

        // Backspace
        if (c == 8 || c == 0x7F) { backspace(); continue; }

        // Enter
        if (c == '\n' || c == '\r') { insert_newline(); continue; }

        // Tab → 4 spaces
        if (c == '\t') {
            for (int i = 0; i < 4; i++) insert_char(' ');
            continue;
        }

        // Printable characters
        if (c >= 32 && c < 127) { insert_char(c); continue; }
    }

    print("\x10");  // restore fbterm cursor for the shell
    tox_clear();
    tox_exit();
}
