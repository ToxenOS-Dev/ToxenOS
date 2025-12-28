#include "runtime/shell/shell.h"
#include "runtime/shell/commands.h"
#include "devices/display/vga.h"
#include "devices/input/keyboard.h"

#define CMD_BUF_SIZE 128

static char cmd_buf[CMD_BUF_SIZE];
static int cmd_len = 0;

/* simple string compare */
static int streq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i])
            return 0;
        i++;
    }
    return a[i] == b[i];
}

static void shell_prompt(void) {
    vga_print("Tox:", 0, cursor_y, VGA_BROWN);
    cursor_x = 5;
}

void shell_init(void) {
    cmd_len = 0;
    shell_prompt();
}

void shell_input_char(char c) {

    /* ENTER */
    if (c == '\n') {
        cmd_buf[cmd_len] = 0;

        cursor_y++;
        cursor_x = 0;

        if (cmd_len == 0) {
            shell_prompt();
            return;
        }

        if (streq(cmd_buf, "help")) {
            cmd_help();
        } else if (streq(cmd_buf, "clear")) {
            cmd_clear();
        } else {
            vga_print("Unknown command", 0, cursor_y++, VGA_LIGHT_RED);
        }

        cmd_len = 0;
        shell_prompt();
        return;
    }

    /* BACKSPACE */
    if (c == '\b') {
        if (cmd_len > 0) {
            cmd_len--;
        }
        return;
    }

    /* NORMAL CHARACTER */
    if (cmd_len < CMD_BUF_SIZE - 1) {
        cmd_buf[cmd_len++] = c;
    }
}
