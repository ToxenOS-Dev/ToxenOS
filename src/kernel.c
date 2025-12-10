// File: src/kernel.c
// Clean version: welcome text at TOP of screen, no animation.

#include <stdint.h>
#include <stddef.h>

/* VGA memory */
static volatile uint16_t *VGA = (uint16_t*)0xB8000;
static const size_t COLS = 80;
static const size_t ROWS = 25;

/* I/O helpers */
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Enable text cursor */
static void enable_cursor(uint8_t start, uint8_t end) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, (inb(0x3D5) & 0xC0) | start);

    outb(0x3D4, 0x0B);
    outb(0x3D5, (inb(0x3D5) & 0xE0) | end);
}

/* Set cursor position */
static void set_cursor_pos(uint16_t pos) {
    outb(0x3D4, 0x0F);
    outb(0x3D5, pos & 0xFF);

    outb(0x3D4, 0x0E);
    outb(0x3D5, (pos >> 8) & 0xFF);
}

/* Cursor row/col state */
static size_t cur_row = 0, cur_col = 0;

static void move_cursor(size_t row, size_t col) {
    if (row >= ROWS) row = ROWS - 1;
    if (col >= COLS) col = COLS - 1;
    cur_row = row;
    cur_col = col;
    set_cursor_pos(row * COLS + col);
}

/* Screen utils */
static void clear_screen(uint8_t attr) {
    uint16_t blank = ((uint16_t)attr << 8) | ' ';
    for (size_t i = 0; i < COLS * ROWS; i++)
        VGA[i] = blank;
}

static size_t kstrlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static size_t center_col(const char *s) {
    size_t len = kstrlen(s);
    return (COLS - len) / 2;
}

static void print_center(const char *s, size_t row, uint8_t attr) {
    size_t col = center_col(s);
    for (size_t i = 0; s[i] && col + i < COLS; i++)
        VGA[row * COLS + col + i] = (attr << 8) | s[i];
}

/* Basic keyboard polling */
static const char sc_map[128] = {
  0,27,'1','2','3','4','5','6','7','8','9','0','-','=', '\b',
  '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
  'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\','z','x',
  'c','v','b','n','m',',','.','/',0,'*',0,' ',
};

static char poll_keyboard(void) {
    if (!(inb(0x64) & 1)) return 0;
    uint8_t sc = inb(0x60);
    if (sc & 0x80) return 0;   // ignore release codes
    return sc_map[sc];
}

/* Scroll screen when reaching bottom */
static void scroll_if_needed(void) {
    if (cur_row < ROWS) return;

    for (size_t r = 1; r < ROWS; r++)
        for (size_t c = 0; c < COLS; c++)
            VGA[(r - 1)*COLS + c] = VGA[r*COLS + c];

    uint16_t blank = (0x0F << 8) | ' ';
    for (size_t c = 0; c < COLS; c++)
        VGA[(ROWS - 1)*COLS + c] = blank;

    cur_row = ROWS - 1;
}

/* Print char with backspace + newline support */
static void putc(char ch) {
    if (ch == '\n') {
        cur_col = 0;
        cur_row++;
        scroll_if_needed();
        move_cursor(cur_row, cur_col);
        return;
    }
    if (ch == '\b') {
        if (cur_col > 0) {
            cur_col--;
            VGA[cur_row*COLS + cur_col] = (0x0F << 8) | ' ';
            move_cursor(cur_row, cur_col);
        }
        return;
    }
    VGA[cur_row*COLS + cur_col] = (0x0F << 8) | ch;
    cur_col++;
    if (cur_col >= COLS) {
        cur_col = 0;
        cur_row++;
        scroll_if_needed();
    }
    move_cursor(cur_row, cur_col);
}

static void puts(const char *s) {
    while (*s) putc(*s++);
}

/* -------------------------
   KERNEL ENTRY POINT
--------------------------*/
void kernel_main(void) {

    enable_cursor(14, 15); // underline cursor

    /* 1. Clear screen */
    clear_screen(0x07);

    /* 2. WELCOME AT THE VERY TOP */
    print_center("welcome to ToxenOS", 0, 0x0B);
    print_center("v0.0.1",             1, 0x0F);

    /* 3. Console starts at row 3 */
    move_cursor(3, 0);
    puts("ToxenOS> ");

    char buf[128];
    size_t len = 0;

    for (;;) {
        char c = poll_keyboard();
        if (!c) { __asm__("hlt"); continue; }

        if (c == '\n') {
            putc('\n');
            buf[len] = 0;
            if (len > 0) {
                puts("You typed: ");
                puts(buf);
                putc('\n');
            }
            len = 0;
            puts("ToxenOS> ");
            continue;
        }

        if (c == '\b') {
            if (len > 0) {
                len--;
                putc('\b');
            }
            continue;
        }

        if (c >= 32 && len < sizeof(buf) - 1) {
            buf[len++] = c;
            putc(c);
        }
    }
}
