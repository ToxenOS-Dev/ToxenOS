#ifndef MOUSE64_H
#define MOUSE64_H

#include <stdint.h>

// Milestone 29: PS/2 auxiliary-device (mouse) driver for x86_64. No
// prior ToxenOS mouse driver exists (32-bit or 64-bit) -- this is a
// fresh implementation of the standard PS/2 mouse protocol.
//
// Initialization sequence (see kernel/mouse64.c for the full detail):
//   1. Reset the device (0xFF), read its self-test result.
//   2. Set Defaults (0xF6) and Enable Data Reporting (0xF4).
//   3. Register IRQ12 and unmask it.
// Always uses the standard 3-byte packet format (no wheel support --
// see kernel/mouse64.c's header comment on why the IntelliMouse 4-byte
// negotiation was deliberately removed after Milestone 30 testing found
// it desyncs packet framing under this QEMU version's mouse-event
// injection path).
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
    uint32_t has_wheel;        // always 0 -- wheel negotiation is disabled, see above
    uint32_t packets_received;
    uint32_t resync_drops;     // bytes discarded while resynchronizing
} mouse64_stats_t;
void mouse64_stats(mouse64_stats_t* out);
void mouse64_dump(void);

#endif // MOUSE64_H
