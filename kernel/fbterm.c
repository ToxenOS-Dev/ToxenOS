// ToxenOS/kernel/fbterm.c
#include <stdint.h>
#include "../include/fbterm.h"
#include "../include/framebuffer.h"
#include "../include/font.h"
#include "../include/process.h"

static const uint32_t vga_palette[16] = {
    0x000000,0x0000AA,0x00AA00,0x00AAAA,
    0xAA0000,0xAA00AA,0xFF6600,0xAAAAAA,
    0x555555,0x5555FF,0x55FF55,0x55FFFF,
    0xFF5555,0xFF55FF,0xFFFF55,0xFFFFFF,
};

static uint32_t fb_w, fb_h, fb_pitch_bytes;
static uint32_t* fb_base;
static int term_cols, term_rows;

#define MAX_COLS 256
#define MAX_ROWS 100

typedef struct { char c; uint8_t fg; uint8_t bg; } cell_t;
static cell_t  cells[FBTERM_TTY_COUNT][MAX_ROWS][MAX_COLS];
static int     cx[FBTERM_TTY_COUNT], cy[FBTERM_TTY_COUNT];
static uint8_t cfg[FBTERM_TTY_COUNT], cbg[FBTERM_TTY_COUNT];
static int active_tty = 0;

int fbterm_pid_tty[MAX_PROCESSES];
static const uint8_t indicator_fg[FBTERM_TTY_COUNT] = {6,11,10,14};

static inline void put_px(uint32_t x, uint32_t y, uint32_t color)
{
    if (x >= fb_w || y >= fb_h) return;
    uint32_t* row = (uint32_t*)((uint8_t*)fb_base + y * fb_pitch_bytes);
    row[x] = color;
}

static void draw_cell(int col, int row, char c, uint8_t fg, uint8_t bg)
{
    uint32_t fgc = vga_palette[fg & 0xF];
    uint32_t bgc = vga_palette[bg & 0xF];
    if (c < 32 || c > 126) c = ' ';
    const uint8_t* glyph = font_get_glyph(c);
    uint32_t px0 = (uint32_t)col * FBTERM_CHAR_W;
    uint32_t py0 = (uint32_t)row * FBTERM_CHAR_H;
    for (int y = 0; y < FBTERM_CHAR_H; y++) {
        uint8_t bits = glyph[y];
        for (int x = 0; x < FBTERM_CHAR_W; x++)
            put_px(px0+x, py0+y, (bits & (0x80>>x)) ? fgc : bgc);
    }
}

static void redraw_all(void) {
    for (int r=0; r<term_rows; r++)
        for (int c=0; c<term_cols; c++)
            draw_cell(c, r,
                cells[active_tty][r][c].c,
                cells[active_tty][r][c].fg,
                cells[active_tty][r][c].bg);
}

static void clear_cells(int t) {
    for (int r=0; r<MAX_ROWS; r++)
        for (int c=0; c<MAX_COLS; c++)
            { cells[t][r][c].c=' '; cells[t][r][c].fg=7; cells[t][r][c].bg=0; }
}

static void scroll_cells(int t) {
    for (int r=0; r<term_rows-1; r++)
        for (int c=0; c<term_cols; c++)
            cells[t][r][c] = cells[t][r+1][c];
    for (int c=0; c<term_cols; c++)
        { cells[t][term_rows-1][c].c=' '; cells[t][term_rows-1][c].fg=7; cells[t][term_rows-1][c].bg=0; }
}

void fbterm_init(void)
{
    fb_w           = fb_get_width();
    fb_h           = fb_get_height();
    fb_pitch_bytes = fb_get_pitch();
    fb_base        = (uint32_t*)fb_get_addr();

    term_cols = (int)(fb_w / FBTERM_CHAR_W);
    term_rows = (int)(fb_h / FBTERM_CHAR_H);
    if (term_cols > MAX_COLS) term_cols = MAX_COLS;
    if (term_rows > MAX_ROWS) term_rows = MAX_ROWS;

    for (int t=0; t<FBTERM_TTY_COUNT; t++) {
        clear_cells(t);
        cx[t]=0; cy[t]=0; cfg[t]=7; cbg[t]=0;
    }
    for (int i=0; i<MAX_PROCESSES; i++) fbterm_pid_tty[i]=-1;
    fbterm_pid_tty[0]=0;
    active_tty=0;

    // clear screen to black
    for (uint32_t y=0; y<fb_h; y++) {
        uint32_t* row = (uint32_t*)((uint8_t*)fb_base + y * fb_pitch_bytes);
        for (uint32_t x=0; x<fb_w; x++)
            row[x] = 0x000000;
    }
}

void fbterm_putchar(char c)
{
    int t = active_tty;
    if (c=='\n') { cx[t]=0; cy[t]++; }
    else if (c=='\r') { cx[t]=0; }
    else {
        cells[t][cy[t]][cx[t]].c  = c;
        cells[t][cy[t]][cx[t]].fg = cfg[t];
        cells[t][cy[t]][cx[t]].bg = cbg[t];
        draw_cell(cx[t], cy[t], c, cfg[t], cbg[t]);
        cx[t]++;
    }
    if (cx[t] >= term_cols) { cx[t]=0; cy[t]++; }
    if (cy[t] >= term_rows) {
        scroll_cells(t);
        cy[t] = term_rows-1; cx[t]=0;
        if (t == active_tty) redraw_all();
    }
}

void fbterm_erase(void) {
    int t = active_tty;
    if (cx[t]==0 && cy[t]==0) return;
    if (cx[t]>0) cx[t]--; else { cy[t]--; cx[t]=term_cols-1; }
    cells[t][cy[t]][cx[t]].c = ' ';
    draw_cell(cx[t], cy[t], ' ', cfg[t], cbg[t]);
}

void fbterm_clear(void) {
    int t = active_tty;
    clear_cells(t); cx[t]=0; cy[t]=0;
    for (uint32_t y=0; y<fb_h; y++) {
        uint32_t* row = (uint32_t*)((uint8_t*)fb_base + y * fb_pitch_bytes);
        for (uint32_t x=0; x<fb_w; x++)
            row[x] = vga_palette[cbg[t]];
    }
}

void fbterm_set_color(uint8_t a) {
    cfg[active_tty] = a & 0xF;
    cbg[active_tty] = (a>>4) & 0xF;
}

void fbterm_switch_tty(int tty) {
    if (tty==active_tty || tty<0 || tty>=FBTERM_TTY_COUNT) return;
    active_tty = tty;
    redraw_all();
    fbterm_draw_indicator();
}

int  fbterm_current_tty(void) { return active_tty; }
int  fbterm_cols(void)        { return term_cols; }
int  fbterm_rows(void)        { return term_rows; }

void fbterm_draw_indicator(void) {
    const char* labels[FBTERM_TTY_COUNT] = {"TTY1","TTY2","TTY3","TTY4"};
    const char* label = labels[active_tty];
    uint8_t fg = indicator_fg[active_tty];
    int col = term_cols-4; if (col<0) col=0;
    for (int i=0; label[i]; i++)
        draw_cell(col+i, 0, label[i], fg, 0);
}
