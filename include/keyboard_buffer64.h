#ifndef KEYBOARD_BUFFER64_H
#define KEYBOARD_BUFFER64_H

#include <stdint.h>

// Milestone 10: minimal stdin path for ToxenOS64; userland reads
// translated ASCII back out through sys_getch (kernel/syscall64.c).
//
// Milestone 29: this file is now JUST the ASCII ring buffer -- raw
// scancode parsing, extended (0xE0) handling, modifier tracking, and
// ASCII translation all moved to kernel/keyboard64.c's new structured
// input64_event_t pipeline (see include/input64.h). keyboard64.c calls
// keyboard_buffer64_push() for every KEY-press event that has a
// translated character, which is exactly the compatibility path
// SYS64_GETCH/tox_readline keep working through -- unchanged from
// their Milestone 10 behavior, they just no longer see raw scancodes
// even indirectly.
void keyboard_buffer64_push(char c);

// Non-blocking: returns the next buffered ASCII character (0-255), or -1
// if the buffer is currently empty. Deliberately never blocks -- int
// 0x80 is dispatched through an interrupt gate (see idt64_init), which
// clears IF for the duration of the syscall, so a syscall that spun here
// waiting for a key would also be spinning with IRQ1 unable to ever fire
// and fill the buffer. Blocking is done in userland instead
// (user64/tox64.h's tox_readline polls this via repeated syscalls, which
// is safe because IF is restored to 1 in ring3 between calls).
int keyboard_buffer64_getch(void);

#endif // KEYBOARD_BUFFER64_H
