// kernel/mouse64.c — Milestone 29: PS/2 mouse driver for x86_64.
// See include/mouse64.h for the design summary.
#include "../include/mouse64.h"
#include "../include/ps2_64.h"
#include "../include/input64.h"
#include "../include/irq64.h"
#include "../include/pic.h"
#include "../include/klog.h"

static int      g_detected = 0;
static int      g_has_wheel = 0;
static int      g_packet_size = 3;
static uint32_t g_packets_received = 0;
static uint32_t g_resync_drops = 0;
static uint32_t g_prev_buttons = 0;

static uint8_t packet_buf[4];
static int     packet_idx = 0;

static void set_sample_rate(uint8_t rate) {
    ps2_64_write_aux(0xF3); ps2_64_read_data(); // ACK
    ps2_64_write_aux(rate); ps2_64_read_data(); // ACK
}

static void process_packet(const uint8_t* p)
{
    g_packets_received++;
    uint8_t b0 = p[0];

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

    if (dx != 0 || dy != 0) {
        input64_event_t ev = {0};
        ev.type    = INPUT64_EVENT_POINTER_MOVE;
        ev.a       = dx;
        ev.b       = dy;
        ev.buttons = buttons;
        input64_push(&ev);
    }

    if (buttons != g_prev_buttons) {
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
        g_prev_buttons = buttons;
    }

    if (g_has_wheel && g_packet_size == 4) {
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

void mouse64_handler(void)
{
    // Read directly (no wait -- IRQ12 firing means the byte is already
    // in the output buffer), mirroring keyboard64_handler's convention.
    uint8_t data;
    __asm__ volatile ("inb $0x60, %0" : "=a"(data));

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

    // IntelliMouse wheel negotiation: the standard "magic" sample-rate
    // sequence, then Get Device ID -- a device that responds with ID 3
    // supports the 4th (wheel) packet byte. Not required for basic
    // operation; falls back to plain 3-byte packets otherwise.
    set_sample_rate(200);
    set_sample_rate(100);
    set_sample_rate(80);
    ps2_64_write_aux(0xF2);
    ps2_64_read_data(); // ACK
    uint8_t id = ps2_64_read_data();
    if (id == 3) {
        g_has_wheel = 1;
        g_packet_size = 4;
    }

    ps2_64_write_aux(0xF6); ps2_64_read_data(); // Set Defaults, ACK
    ps2_64_write_aux(0xF4); ps2_64_read_data(); // Enable Data Reporting, ACK

    irq64_register(12, mouse64_handler);
    pic_unmask(12);

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
