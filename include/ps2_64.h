#ifndef PS2_64_H
#define PS2_64_H

#include <stdint.h>

// Milestone 29: shared 8042 PS/2 controller bring-up, used by both
// kernel/keyboard64.c and kernel/mouse64.c. Neither the 32-bit nor the
// original 64-bit keyboard driver ever did this -- both just registered
// an IRQ1 handler and trusted whatever state BIOS/GRUB left the
// controller in, which happened to work for a keyboard-only setup under
// QEMU but leaves the auxiliary (mouse) port neither known-flushed nor
// enabled, and leaves the controller's IRQ-on-port-2 configuration bit
// unset. This brings the controller into a known, documented state:
// flush any stale output-buffer byte, disable both ports while
// configuring, read/modify/write the configuration byte to enable
// translation-independent IRQ1+IRQ12 delivery, then re-enable both
// ports.
//
// Must run once, before ps2_64_write_aux()/keyboard IRQ handling is
// relied upon, and before pic_unmask(1)/pic_unmask(12) — matches the
// existing kernel64.c boot ordering (PIC/IRQ registration happens
// before `sti`).
void ps2_64_init(void);

// Sends `data` to the auxiliary (mouse) device: writes the "next byte
// goes to port 2" controller command (0xD4) first, then the data byte,
// exactly like the standard PS/2 mouse initialization sequence. Used by
// kernel/mouse64.c; kept here since it goes through the same
// input-buffer-full/output-buffer-full handshake as every other
// controller/port-1 access.
void ps2_64_write_aux(uint8_t data);

// Raw port-1 (keyboard) command byte send, going through the same
// input-buffer-full wait as ps2_64_write_aux -- used by kernel/keyboard64.c
// if it ever needs to send a command to the keyboard itself (e.g. future
// LED updates); not required for basic scancode reception.
void ps2_64_write_kbd(uint8_t data);

// Blocking read of the next byte from the controller's output buffer
// (port 0x60), waiting for the output-buffer-full status bit. Used by
// both keyboard/mouse init sequences to read command ACK/response bytes
// (0xFA, device ID, self-test result, ...) -- NOT used by the steady-
// state IRQ handlers, which read 0x60 directly without waiting (the
// byte is already known to be there; see kernel/keyboard64.c/mouse64.c).
uint8_t ps2_64_read_data(void);

#endif // PS2_64_H
