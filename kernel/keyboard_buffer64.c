// kernel/keyboard_buffer64.c — Milestone 10: minimal stdin ring buffer.
// Same PS/2 Set 1 scancode tables as kernel/keyboard.c's full 32-bit
// driver, trimmed to this milestone's scope: Shift tracked for case,
// Ctrl/extended/arrow keys and multi-key sequences not handled at all
// (silently ignored, not even buffered) -- no signal delivery, no
// process targeting, nothing the 32-bit driver does beyond raw
// scancode-to-ASCII. Drop-on-full instead of overwrite, same rule as
// the 32-bit ring buffer.
#include "../include/keyboard_buffer64.h"

#define KEYBOARD_BUFFER64_SIZE 64
static char buf[KEYBOARD_BUFFER64_SIZE];
static int  head = 0, tail = 0;
static int  shift_pressed = 0;

static const char scancode_table[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=',8,
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
    'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\',
    'z','x','c','v','b','n','m',',','.','/',0,'*',0,' ',
};

static const char shift_table[128] = {
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+',8,
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
    'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
    'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' ',
};

static void push(char c) {
    int next = (tail + 1) % KEYBOARD_BUFFER64_SIZE;
    if (next != head) { buf[tail] = c; tail = next; }
    // else: buffer full -- drop the keystroke rather than overwrite
    // input the reader hasn't consumed yet.
}

void keyboard_buffer64_on_scancode(uint8_t sc) {
    if (sc == 0x2A || sc == 0x36) { shift_pressed = 1; return; }
    if (sc == 0xAA || sc == 0xB6) { shift_pressed = 0; return; }
    if (sc & 0x80) return;   // key-up of anything else -- ignore
    if (sc >= 128) return;

    char c = shift_pressed ? shift_table[sc] : scancode_table[sc];
    if (c == 0) return;      // unmapped/special key -- ignore
    push(c);
}

int keyboard_buffer64_getch(void) {
    if (head == tail) return -1;
    char c = buf[head];
    head = (head + 1) % KEYBOARD_BUFFER64_SIZE;
    return (int)(unsigned char)c;
}
