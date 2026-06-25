#ifndef KEYBOARD_BUFFER64_H
#define KEYBOARD_BUFFER64_H

#include <stdint.h>

// Milestone 10: minimal stdin path for ToxenOS64. kernel/keyboard64.c's
// IRQ1 handler feeds raw PS/2 Set 1 scancodes in here; userland reads
// translated ASCII back out through sys_getch (kernel/syscall64.c).
// US QWERTY layout, Shift tracked for case -- no Ctrl, no extended/arrow
// keys, no multi-key sequences. Anything not in the translation table is
// silently dropped, matching this milestone's "ignore unsupported
// special keys for now" scope.
void keyboard_buffer64_on_scancode(uint8_t sc);

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
