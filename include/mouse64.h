#ifndef MOUSE64_H
#define MOUSE64_H

#include <stdint.h>

// Milestone 29: PS/2 auxiliary-device (mouse) driver for x86_64. No
// prior ToxenOS mouse driver exists (32-bit or 64-bit) -- this is a
// fresh implementation of the standard PS/2 mouse protocol.
//
// Initialization sequence (see kernel/mouse64.c for the full detail):
//   1. Reset the device (0xFF), read its self-test result.
//   2. Attempt the standard IntelliMouse "magic" sample-rate sequence
//      (200, 100, 80) followed by Get Device ID (0xF2) to negotiate a
//      4th (wheel) packet byte -- falls back to plain 3-byte packets if
//      the device doesn't respond as an IntelliMouse. Not a blocker: a
//      device that doesn't support this just gets no wheel events.
//   3. Set Defaults (0xF6) and Enable Data Reporting (0xF4).
//   4. Register IRQ12 and unmask it.
// Requires ps2_64_init() to have already run (enables port 2 and its
// IRQ in the 8042 configuration byte).
//
// Packet synchronization: byte 0 of every packet always has bit 3 set
// (an 8042/PS2-mouse protocol invariant); mouse64_handler() uses this
// to detect and recover from a lost/misaligned byte -- any byte seen
// where a packet's first byte is expected, but which doesn't have bit
// 3 set, is dropped and the handler keeps waiting for a genuine first
// byte, rather than assembling a corrupt packet.
int mouse64_init(void);

void mouse64_handler(void);

typedef struct {
    uint32_t detected;         // 1 if a mouse responded during init
    uint32_t has_wheel;        // 1 if IntelliMouse wheel negotiation succeeded
    uint32_t packets_received;
    uint32_t resync_drops;     // bytes discarded while resynchronizing
} mouse64_stats_t;
void mouse64_stats(mouse64_stats_t* out);
void mouse64_dump(void);

#endif // MOUSE64_H
