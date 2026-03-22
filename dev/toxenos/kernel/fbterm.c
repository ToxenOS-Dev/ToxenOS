// ToxenOS/kernel/fbterm.c
// Framebuffer terminal — replaces VGA text-mode rendering.
// Supports 4 virtual TTYs, each with its own pixel backbuffer and cursor.
// Color attributes are the same VGA byte the shell already uses (0x07, 0x0C …).

#include <stdint.h>
#include "../include/fbterm.h"
#include "../include/framebuffer.h"
#include "../include/font.h"
#include "../include/process.h"

// ── VGA color palette → 24-bit RGB ───────────────────────────────────────────
// Matches the classic 16-color CGA/VGA palette so existing shell colors are
// preserved exactly.  Index = VGA color number (0-15).
static const uint32_t vga_palette[16] = {
    0x000000,  // 0  black
    0x0000AA,  // 1  blue
    0x00AA00,  // 2  green
    0x00AAAA,  // 3  cyan
    0xAA0000,  // 4  red
    0xAA00AA,  // 5  magenta
    0xFF6600,  // 6  brown → ToxenOS orange (brand color)
    0xAAAAAA,  // 7  light grey
    0x555555,  // 8  dark grey
    0x5555FF,  // 9  bright blue
    0x55FF55,  // 10 bright green
    0x55FFFF,  // 11 bright cyan
    0xFF5555,  // 12 bright red
    0xFF55FF,  // 13 bright magenta
    0xFFFF55,  // 14 yellow
    0xFFFFFF,  // 15 white
};

// ── Framebuffer geometry (set by fb_init) ────────────────────────────────────
static uint32_t fb_w;
static uint32_t fb_h;
static uint32_t fb_stride;  // pixels per row  (pitch / 4)

static int term_cols;
static int term_rows;

// ── Per-TTY state ─────────────────────────────────────────────────────────────

// Pixel backbuffers.  Each TTY owns a complete copy of the screen.
// Allocated statically to avoid needing malloc at init time.
// Max supported resolution: 1024×768 32-bpp = 3 MB per TTY.
// We cap at 1024×768 so the static array stays manageable.
#define MAX_FB_PIXELS  (1024 * 768)
static uint32_t tty_pixels[FBTERM_TTY_COUNT][MAX_FB_PIXELS];

typedef struct {
    int  cx;        // cursor column  (0-based)
    int  cy;        // cursor row     (0-based)
    uint8_t fg;     // foreground VGA index (0-15)
    uint8_t bg;     // background VGA index (0-15)
} tty_state_t;

static tty_state_t tty_st[FBTERM_TTY_COUNT];
static int         active_tty = 0;

// Per-process TTY assignment (mirrors old tty_for_pid).
int fbterm_pid_tty[MAX_PROCESSES];

// ── TTY indicator badge colors (one per TTY) ─────────────────────────────────
static const uint8_t indicator_fg[FBTERM_TTY_COUNT] = { 6, 11, 10, 14 };
//                                                      orange cyan green yellow

// ── Internal helpers ──────────────────────────────────────────────────────────

// Flush a single cell from the active TTY's backbuffer to the real framebuffer.
static void flush_cell(int col, int row)
{
    uint32_t px = (uint32_t)col * FBTERM_CHAR_W;
    uint32_t py = (uint32_t)row * FBTERM_CHAR_H;
    if (px + FBTERM_CHAR_W > fb_w) return;
    if (py + FBTERM_CHAR_H > fb_h) return;

    for (uint32_t dy = 0; dy < FBTERM_CHAR_H; dy++)
    {
        uint32_t src = (py + dy) * fb_stride + px;
        for (uint32_t dx = 0; dx < FBTERM_CHAR_W; dx++)
            fb_put_pixel(px + dx, py + dy, tty_pixels[active_tty][src + dx]);
    }
}

// Draw one character glyph into the active TTY's backbuffer (not direct to FB).
static void draw_glyph(int col, int row, char c, uint8_t fg_idx, uint8_t bg_idx)
{
    uint32_t fg = vga_palette[fg_idx & 0x0F];
    uint32_t bg = vga_palette[bg_idx & 0x0F];

    if (c < 32 || c > 126) c = ' ';
    const uint8_t* glyph = font_get_glyph(c);

    uint32_t px = (uint32_t)col * FBTERM_CHAR_W;
    uint32_t py = (uint32_t)row * FBTERM_CHAR_H;

    for (int y = 0; y < FBTERM_CHAR_H; y++)
    {
        uint8_t bits = glyph[y];
        uint32_t base = (py + y) * fb_stride + px;
        for (int x = 0; x < FBTERM_CHAR_W; x++)
            tty_pixels[active_tty][base + x] = (bits & (0x80 >> x)) ? fg : bg;
    }
}

// Fill a rectangle in the active TTY's backbuffer with a solid color.
static void fill_rect_buf(int col, int row, int cols, int rows, uint32_t color)
{
    uint32_t px = (uint32_t)col * FBTERM_CHAR_W;
    uint32_t py = (uint32_t)row * FBTERM_CHAR_H;
    uint32_t pw = (uint32_t)cols * FBTERM_CHAR_W;
    uint32_t ph = (uint32_t)rows * FBTERM_CHAR_H;

    for (uint32_t dy = 0; dy < ph; dy++)
    {
        if (py + dy >= fb_h) break;
        uint32_t base = (py + dy) * fb_stride + px;
        for (uint32_t dx = 0; dx < pw; dx++)
        {
            if (px + dx >= fb_w) break;
            tty_pixels[active_tty][base + dx] = color;
        }
    }
}

// Blit entire active TTY backbuffer to the real framebuffer.
static void flush_all(void)
{
    uint32_t* fb = (uint32_t*)0;  // will use fb_put_pixel

    for (uint32_t y = 0; y < fb_h; y++)
    {
        uint32_t base = y * fb_stride;
        for (uint32_t x = 0; x < fb_w; x++)
            fb_put_pixel(x, y, tty_pixels[active_tty][base + x]);
    }
}

// Draw the text cursor block at the current cursor position.
static void draw_cursor(int tty)
{
    tty_state_t* s = &tty_st[tty];
    if (tty != active_tty) return;

    uint32_t fg = vga_palette[s->fg & 0x0F];

    // draw a 2-pixel underline at the bottom of the cell
    uint32_t px = (uint32_t)s->cx * FBTERM_CHAR_W;
    uint32_t py = (uint32_t)s->cy * FBTERM_CHAR_H + FBTERM_CHAR_H - 3;

    for (int dy = 0; dy < 2; dy++)
    {
        for (int dx = 0; dx < FBTERM_CHAR_W; dx++)
        {
            uint32_t off = (py + dy) * fb_stride + px + dx;
            tty_pixels[active_tty][off] = fg;
            fb_put_pixel(px + dx, py + dy, fg);
        }
    }
}

// Erase cursor (redraw the cell without cursor mark).
static void erase_cursor(int tty)
{
    // We don't store what character is under the cursor, so just redraw cell
    // as blank bg — cursor position is always a blank in practice (after typing
    // the char moves the cursor forward). This is fine for a simple terminal.
    tty_state_t* s = &tty_st[tty];
    if (tty != active_tty) return;

    uint32_t bg = vga_palette[s->bg & 0x0F];
    uint32_t px = (uint32_t)s->cx * FBTERM_CHAR_W;
    uint32_t py = (uint32_t)s->cy * FBTERM_CHAR_H + FBTERM_CHAR_H - 3;

    for (int dy = 0; dy < 2; dy++)
        for (int dx = 0; dx < FBTERM_CHAR_W; dx++)
        {
            uint32_t off = (py + dy) * fb_stride + px + dx;
            fb_put_pixel(px + dx, py + dy, tty_pixels[active_tty][off]);
        }
}

// Scroll the active TTY up by one row.
static void scroll_tty(int tty)
{
    uint32_t row_bytes = (uint32_t)FBTERM_CHAR_H * fb_stride;
    uint32_t total     = (uint32_t)fb_h * fb_stride;

    // move rows 1..N-1 up by one character row
    uint32_t copy_end = total - row_bytes;
    for (uint32_t i = 0; i < copy_end; i++)
        tty_pixels[tty][i] = tty_pixels[tty][i + row_bytes];

    // blank the last row with bg color
    uint32_t bg = vga_palette[tty_st[tty].bg & 0x0F];
    for (uint32_t i = copy_end; i < total; i++)
        tty_pixels[tty][i] = bg;

    if (tty == active_tty) flush_all();
}

// ── Public API ────────────────────────────────────────────────────────────────

void fbterm_init(void)
{
    fb_w    = fb_get_width();
    fb_h    = fb_get_height();
    fb_stride = fb_get_pitch() / 4;

    term_cols = (int)(fb_w / FBTERM_CHAR_W);
    term_rows = (int)(fb_h / FBTERM_CHAR_H);

    // clamp to sanity
    if (term_cols < 1) term_cols = 1;
    if (term_rows < 1) term_rows = 1;

    // initialise all TTY states
    for (int t = 0; t < FBTERM_TTY_COUNT; t++)
    {
        tty_st[t].cx = 0;
        tty_st[t].cy = 0;
        tty_st[t].fg = 7;   // light grey
        tty_st[t].bg = 0;   // black

        // fill backbuffer with bg color
        uint32_t bg = vga_palette[0];
        uint32_t total = fb_h * fb_stride;
        for (uint32_t i = 0; i < total; i++)
            tty_pixels[t][i] = bg;
    }

    for (int i = 0; i < MAX_PROCESSES; i++)
        fbterm_pid_tty[i] = -1;

    fbterm_pid_tty[0] = 0;
    active_tty = 0;

    // blit the blank screen
    flush_all();
}

void fbterm_putchar(char c)
{
    tty_state_t* s   = &tty_st[active_tty];
    int          tty = active_tty;

    if (c == '\n')
    {
        s->cx = 0;
        s->cy++;
    }
    else if (c == '\r')
    {
        s->cx = 0;
    }
    else if (c == '\t')
    {
        // 4-space tab stop
        int next = (s->cx + 4) & ~3;
        if (next >= term_cols) next = term_cols - 1;
        while (s->cx < next)
        {
            draw_glyph(s->cx, s->cy, ' ', s->fg, s->bg);
            flush_cell(s->cx, s->cy);
            s->cx++;
        }
    }
    else
    {
        draw_glyph(s->cx, s->cy, c, s->fg, s->bg);
        flush_cell(s->cx, s->cy);
        s->cx++;
    }

    // wrap
    if (s->cx >= term_cols)
    {
        s->cx = 0;
        s->cy++;
    }

    // scroll
    if (s->cy >= term_rows)
    {
        scroll_tty(tty);
        s->cy = term_rows - 1;
        s->cx = 0;
    }

    draw_cursor(tty);
}

void fbterm_erase(void)
{
    tty_state_t* s = &tty_st[active_tty];

    if (s->cx == 0 && s->cy == 0) return;

    erase_cursor(active_tty);

    // move cursor back
    if (s->cx > 0)
        s->cx--;
    else
    {
        s->cy--;
        s->cx = term_cols - 1;
    }

    // blank the cell
    draw_glyph(s->cx, s->cy, ' ', s->fg, s->bg);
    flush_cell(s->cx, s->cy);
    draw_cursor(active_tty);
}

void fbterm_clear(void)
{
    tty_state_t* s = &tty_st[active_tty];
    uint32_t bg = vga_palette[s->bg & 0x0F];

    uint32_t total = fb_h * fb_stride;
    for (uint32_t i = 0; i < total; i++)
        tty_pixels[active_tty][i] = bg;

    s->cx = 0;
    s->cy = 0;

    flush_all();
    fbterm_draw_indicator();
}

void fbterm_set_color(uint8_t vga_attr)
{
    // vga_attr: bits [3:0] = fg, bits [7:4] = bg
    tty_st[active_tty].fg = vga_attr & 0x0F;
    tty_st[active_tty].bg = (vga_attr >> 4) & 0x0F;
}

void fbterm_switch_tty(int tty)
{
    if (tty == active_tty) return;
    if (tty < 0 || tty >= FBTERM_TTY_COUNT) return;

    // No explicit save needed — backbuffers are always kept in sync.
    active_tty = tty;
    flush_all();
    fbterm_draw_indicator();
}

int fbterm_current_tty(void)
{
    return active_tty;
}

int fbterm_cols(void) { return term_cols; }
int fbterm_rows(void) { return term_rows; }

void fbterm_draw_indicator(void)
{
    // Draw a small colored "TTY1" / "TTY2" label in the top-right corner.
    const char* labels[FBTERM_TTY_COUNT] = {"TTY1", "TTY2", "TTY3", "TTY4"};
    const char* label = labels[active_tty];
    uint8_t     fg    = indicator_fg[active_tty];

    int col = term_cols - 4;  // 4 chars wide
    if (col < 0) col = 0;

    for (int i = 0; label[i]; i++)
    {
        draw_glyph(col + i, 0, label[i], fg, 0);
        flush_cell(col + i, 0);
    }
}
