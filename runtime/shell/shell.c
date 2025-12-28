#include "runtime/shell/shell.h"
#include "runtime/shell/commands.h"
#include "devices/display/vga.h"
#include "devices/input/keyboard.h"

#define CMD_BUF_SIZE 128
#define HISTORY_SIZE 16

static char cmd_buf[CMD_BUF_SIZE];
static int  cmd_len = 0;

/* History */
static char history[HISTORY_SIZE][CMD_BUF_SIZE];
static int  history_count = 0;
static int  history_index = -1;

/* Compare strings */
static int streq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == b[i];
}

/* Scroll only during OUTPUT */
static void scroll_if_needed(void) {
    if (cursor_y >= VGA_HEIGHT) {
        vga_scroll();
        cursor_y = VGA_HEIGHT - 1;
    }
}

/* Print one output line safely */
void shell_print_line(const char* text, uint8_t color) {
    vga_print(text, 0, cursor_y++, color);
    scroll_if_needed();
}

/* Clear input area */
static void clear_input_line(void) {
    for (int i = 5; i < VGA_WIDTH; i++) {
        vga_put_char(' ', i, cursor_y, VGA_WHITE);
    }
    cursor_x = 5;
}

/* Draw prompt WITHOUT scrolling */
static void shell_prompt(void) {
    vga_print("Tox:", 0, cursor_y, VGA_BROWN);
    cursor_x = 5;
}

/* Redraw buffer */
static void redraw_cmd(void) {
    clear_input_line();
    for (int i = 0; i < cmd_len; i++) {
        vga_put_char(cmd_buf[i], cursor_x++, cursor_y, VGA_WHITE);
    }
}

void shell_init(void) {
    cmd_len = 0;
    history_count = 0;
    history_index = -1;
    shell_prompt();
}

void shell_input_char(char c) {

    /* HISTORY UP */
    if (c == '\x01') {
        if (history_count == 0) return;

        history_index = (history_index < 0)
            ? history_count - 1
            : (history_index > 0 ? history_index - 1 : 0);

        for (int i = 0; i < CMD_BUF_SIZE; i++)
            cmd_buf[i] = history[history_index][i];

        cmd_len = 0;
        while (cmd_buf[cmd_len]) cmd_len++;

        redraw_cmd();
        return;
    }

    /* HISTORY DOWN */
    if (c == '\x02') {
        if (history_index < 0) return;

        history_index++;
        if (history_index >= history_count) {
            history_index = -1;
            cmd_len = 0;
            clear_input_line();
            return;
        }

        for (int i = 0; i < CMD_BUF_SIZE; i++)
            cmd_buf[i] = history[history_index][i];

        cmd_len = 0;
        while (cmd_buf[cmd_len]) cmd_len++;

        redraw_cmd();
        return;
    }

    /* ENTER */
    if (c == '\n') {
        cmd_buf[cmd_len] = 0;

        if (cmd_len > 0 && history_count < HISTORY_SIZE) {
            for (int i = 0; i <= cmd_len; i++)
                history[history_count][i] = cmd_buf[i];
            history_count++;
        }

        history_index = -1;

        cursor_y++;
        scroll_if_needed();

        if (cmd_len == 0) {
            shell_prompt();
            return;
        }

        if (streq(cmd_buf, "help")) {
            shell_print_line("Available commands:", VGA_LIGHT_GREY);
            shell_print_line(" help   - show this message", VGA_LIGHT_GREY);
            shell_print_line(" clear  - clear the screen", VGA_LIGHT_GREY);
        } else if (streq(cmd_buf, "clear")) {
            vga_clear();
            cursor_y = 0;
        } else {
            shell_print_line("Unknown command", VGA_LIGHT_RED);
        }

        for (int i = 0; i < CMD_BUF_SIZE; i++)
            cmd_buf[i] = 0;

        cmd_len = 0;
        shell_prompt();
        return;
    }

    /* BACKSPACE */
    if (c == '\b') {
        if (cmd_len > 0)
            cmd_len--;
        return;
    }

    /* NORMAL */
    if (cmd_len < CMD_BUF_SIZE - 1)
        cmd_buf[cmd_len++] = c;
}
