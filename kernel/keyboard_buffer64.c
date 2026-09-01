// kernel/keyboard_buffer64.c — Milestone 10: minimal stdin ring buffer.
// Milestone 29: pure ASCII ring buffer now -- see the header comment in
// include/keyboard_buffer64.h for where translation moved to.
#include "../include/keyboard_buffer64.h"

#define KEYBOARD_BUFFER64_SIZE 64
static char buf[KEYBOARD_BUFFER64_SIZE];
static int  head = 0, tail = 0;

void keyboard_buffer64_push(char c) {
    int next = (tail + 1) % KEYBOARD_BUFFER64_SIZE;
    if (next != head) { buf[tail] = c; tail = next; }
    // else: buffer full -- drop the keystroke rather than overwrite
    // input the reader hasn't consumed yet.
}

int keyboard_buffer64_getch(void) {
    if (head == tail) return -1;
    char c = buf[head];
    head = (head + 1) % KEYBOARD_BUFFER64_SIZE;
    return (int)(unsigned char)c;
}
