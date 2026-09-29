// kernel/mouse64.c — Milestone 29: PS/2 mouse driver for x86_64.
// See include/mouse64.h for the design summary.
#include "../include/mouse64.h"
#include "../include/ps2_64.h"
#include "../include/input64.h"
#include "../include/irq64.h"
#include "../include/klog.h"

static int      g_detected = 0;
static int      g_has_wheel = 0;
static int      g_packet_size = 3;
static uint32_t g_packets_received = 0;
static uint32_t g_resync_drops = 0;
static uint32_t g_prev_buttons = 0;
static uint8_t packet_buf[4];
static int     packet_idx = 0;

static void process_packet(const uint8_t* p)
{
    g_packets_received++;
    uint8_t b0 = p[0];
#ifdef MOUSE64_DEBUG
    klog_hex("mouse64: packet b0=", p[0]);
    klog_hex("mouse64: packet b1=", p[1]);
    klog_hex("mouse64: packet b2=", p[2]);
    klog_hex("mouse64: prev_buttons=", g_prev_buttons);
#endif

    // Overflow bits set -- the device itself flags this sample as
    // unreliable; discard the motion rather than report a garbage jump.
    int overflowed = (b0 & 0xC0) != 0;

    int32_t dx = (int32_t)(int8_t)((b0 & 0x10) ? (p[1] | 0xFFFFFF00u) : p[1]);
    int32_t dy_raw = (int32_t)(int8_t)((b0 & 0x20) ? (p[2] | 0xFFFFFF00u) : p[2]);
    // PS/2 reports +Y as "up"; flip so +Y means "down", matching the
    // framebuffer/screen coordinate convention the future compositor
    // will want (this is the only transform applied here -- clamping
    // to actual screen bounds is deliberately a higher-layer concern,
    // not this raw packet parser's).
    int32_t dy = -dy_raw;

    if (overflowed) { dx = 0; dy = 0; }

    uint32_t buttons = 0;
    if (b0 & 0x01) buttons |= INPUT64_BTN_LEFT;
    if (b0 & 0x02) buttons |= INPUT64_BTN_RIGHT;
    if (b0 & 0x04) buttons |= INPUT64_BTN_MIDDLE;

    // M+7A: PS/2 stays the always-available fallback (§13), but never
    // double-drives the cursor alongside a working absolute pointer
    // device -- see include/input64.h's own header comment on
    // input64_abs_pointer_active(). Packet assembly/resync above this
    // point is completely unaffected either way, so PS/2 is instantly
    // usable again the moment this flag clears.
    int suppressed = input64_abs_pointer_active();

    if (!suppressed && (dx != 0 || dy != 0)) {
        input64_event_t ev = {0};
        ev.type    = INPUT64_EVENT_POINTER_REL;
        ev.a       = dx;
        ev.b       = dy;
        ev.buttons = buttons;
        input64_push(&ev);
    }

    if (buttons != g_prev_buttons) {
        if (!suppressed) {
            static const uint32_t bits[3] = { INPUT64_BTN_LEFT, INPUT64_BTN_RIGHT, INPUT64_BTN_MIDDLE };
            uint32_t changed = buttons ^ g_prev_buttons;
            for (int i = 0; i < 3; i++) {
                if (!(changed & bits[i])) continue;
                input64_event_t ev = {0};
                ev.type    = INPUT64_EVENT_POINTER_BUTTON;
                ev.a       = (int32_t)bits[i];
                ev.pressed = (buttons & bits[i]) ? 1u : 0u;
                ev.buttons = buttons;
                input64_push(&ev);
            }
        }
        g_prev_buttons = buttons;
    }

    if (!suppressed && g_has_wheel && g_packet_size == 4) {
        int8_t wheel = (int8_t)p[3];
        if (wheel != 0) {
            input64_event_t ev = {0};
            ev.type    = INPUT64_EVENT_POINTER_WHEEL;
            ev.a       = wheel;
            ev.buttons = buttons;
            input64_push(&ev);
        }
    }
}

// M+11A: byte-level entry, fed by the shared 8042 receive path for every
// auxiliary-port byte (AUXDATA set), regardless of which IRQ line fired.
// The receive path only ever delivers a byte that is really in the output
// buffer, so the old g_discard_first_packet workaround -- which discarded
// the first byte precisely because an IRQ12 could fire with nothing new in
// 0x60 and the blind read returned stale data -- is no longer needed and is
// gone; packet framing/resync below is unchanged.
void mouse64_feed_byte(uint8_t data)
{
#ifdef MOUSE64_DEBUG
    klog_hex("mouse64: raw byte=", data);
#endif

    if (packet_idx == 0 && !(data & 0x08)) {
        // Not a valid first byte (the protocol invariant bit is clear)
        // -- drop it and keep waiting for a genuine packet start. This
        // is the resynchronization path: a single lost/extra byte
        // self-heals within one packet instead of permanently
        // misaligning every subsequent packet.
        g_resync_drops++;
        return;
    }

    packet_buf[packet_idx++] = data;
    if (packet_idx < g_packet_size) return;
    packet_idx = 0;
    process_packet(packet_buf);
}

int mouse64_init(void)
{
    g_detected = 0;

    ps2_64_write_aux(0xFF); // reset
    uint8_t ack = ps2_64_read_data();
    if (ack != 0xFA) {
        klog("mouse64: no PS/2 mouse detected (no ACK to reset)\n");
        return -1;
    }
    ps2_64_read_data(); // self-test result (0xAA expected)
    ps2_64_read_data(); // device ID (0x00 for a standard mouse)
    g_detected = 1;

    // IntelliMouse wheel negotiation (the standard "magic" sample-rate
    // sequence, then Get Device ID -- a device that responds with ID 3
    // supports the 4th, wheel-delta packet byte) is DELIBERATELY
    // DISABLED here, not merely skipped. Milestone 30 testing found
    // that under this QEMU version, negotiating it successfully (ID
    // comes back as 3) does not reliably match the actual packet
    // stream this environment's mouse-event injection produces: the
    // driver ends up expecting 4-byte packets against a 3-byte stream,
    // and the byte-3/packet-0 framing permanently drifts (each
    // "packet" boundary shifts by one real byte, so packet_idx==0's
    // bit-3 resync check only catches the drift probabilistically
    // rather than immediately) -- observed as spurious/garbled
    // POINTER_BUTTON and POINTER_WHEEL events out of pure motion input.
    // Wheel support was always explicitly optional (Milestone 29's
    // scope), so the robust choice is to stay on the universally
    // correct standard 3-byte packet format rather than chase a
    // negotiation whose real-world reliability this milestone could
    // not establish. Revisit if a genuine need for wheel events arises.

    ps2_64_write_aux(0xF6); ps2_64_read_data(); // Set Defaults, ACK
    ps2_64_write_aux(0xF4); ps2_64_read_data(); // Enable Data Reporting, ACK

    // Drain any byte the device already pushed now that reporting is
    // active, on general principle (cheap, bounded, and harmless either
    // way) -- see ps2_64_output_full()'s header comment.
    for (int i = 0; i < 16 && ps2_64_output_full(); i++) ps2_64_read_data();

    irq64_request_legacy(12, mouse64_irq, 0, "mouse");

    // M+7A: register with the generic input64 device model (diagnostic/
    // enumeration only -- see include/input64.h's own header comment).
    // Static storage, not a stack local: must remain valid for the
    // kernel's lifetime, same requirement every other input64_device_t
    // registrant follows.
    static input64_device_t dev;
    dev.name[0]='p'; dev.name[1]='s'; dev.name[2]='2'; dev.name[3]='m';
    dev.name[4]='o'; dev.name[5]='u'; dev.name[6]='s'; dev.name[7]='e'; dev.name[8]=0;
    dev.kind = INPUT64_DEVICE_PS2_MOUSE;
    dev.capabilities = INPUT64_CAP_REL | INPUT64_CAP_KEY;
    input64_register_device(&dev);

    klog(g_has_wheel ? "mouse64: PS/2 mouse detected (IntelliMouse, wheel supported)\n"
                      : "mouse64: PS/2 mouse detected (standard 3-byte packets)\n");
    return 0;
}

void mouse64_stats(mouse64_stats_t* out) {
    if (!out) return;
    out->detected         = (uint32_t)g_detected;
    out->has_wheel        = (uint32_t)g_has_wheel;
    out->packets_received = g_packets_received;
    out->resync_drops     = g_resync_drops;
}

void mouse64_dump(void) {
    mouse64_stats_t s;
    mouse64_stats(&s);
    klog("mouse64: dump ---\n");
    klog_hex("  detected:  ", s.detected);
    klog_hex("  has_wheel: ", s.has_wheel);
    klog_hex("  packets:   ", s.packets_received);
    klog_hex("  resyncs:   ", s.resync_drops);
    klog("mouse64: dump end ---\n");
}

// M+11A: thin irq64 thunk for IRQ12.
irq64_ret_t mouse64_irq(void* ctx)
{
    (void)ctx;
    return ps2_64_service() ? IRQ64_RET_HANDLED : IRQ64_RET_NONE;
}
