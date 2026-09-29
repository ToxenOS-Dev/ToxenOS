// kernel/ps2_64.c — Milestone 29: shared 8042 PS/2 controller bring-up.
// See include/ps2_64.h for why this exists (neither kernel/keyboard.c
// nor the original kernel/keyboard64.c ever did controller-level init).
#include "../include/ps2_64.h"
#include "../include/klog.h"

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_INPUT_FULL  0x02

#define PS2_CMD_READ_CONFIG    0x20
#define PS2_CMD_WRITE_CONFIG   0x60
#define PS2_CMD_DISABLE_AUX    0xA7
#define PS2_CMD_ENABLE_AUX     0xA8
#define PS2_CMD_DISABLE_KBD    0xAD
#define PS2_CMD_ENABLE_KBD     0xAE
#define PS2_CMD_WRITE_AUX      0xD4

#define PS2_WAIT_TIMEOUT 100000

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

// Bounded waits -- a wedged/missing controller must never hang the
// boot sequence (same "no infinite waits" discipline as Milestone 28's
// AHCI/NVMe polling loops). Timeout simply proceeds; the caller's
// subsequent read/write may then fail or read stale data, but the
// kernel keeps booting either way.
static int wait_input_clear(void) {
    for (int i = 0; i < PS2_WAIT_TIMEOUT; i++)
        if (!(inb(PS2_STATUS) & PS2_STATUS_INPUT_FULL)) return 0;
    return -1;
}
static int wait_output_full(void) {
    for (int i = 0; i < PS2_WAIT_TIMEOUT; i++)
        if (inb(PS2_STATUS) & PS2_STATUS_OUTPUT_FULL) return 0;
    return -1;
}

uint8_t ps2_64_read_data(void) {
    wait_output_full();
    return inb(PS2_DATA);
}

int ps2_64_output_full(void) {
    return (inb(PS2_STATUS) & PS2_STATUS_OUTPUT_FULL) != 0;
}

int ps2_64_rx_poll(const ps2_64_rx_t* rx) {
    uint8_t st = rx->read_status();
    if (!(st & PS2_STATUS_OBF)) return 0;       // nothing there: never touch 0x60
    uint8_t b = rx->read_data();
    if (st & PS2_STATUS_AUXDATA) rx->aux_sink(b);
    else                         rx->kbd_sink(b);
    return 1;
}

static uint8_t real_status(void) { return inb(PS2_STATUS); }
static uint8_t real_data(void)   { return inb(PS2_DATA); }
static const ps2_64_rx_t g_real_rx = { real_status, real_data, keyboard64_feed_byte, mouse64_feed_byte };

int ps2_64_service(void) {
    return ps2_64_rx_poll(&g_real_rx);
}

void ps2_64_write_kbd(uint8_t data) {
    wait_input_clear();
    outb(PS2_DATA, data);
}

void ps2_64_write_aux(uint8_t data) {
    wait_input_clear();
    outb(PS2_CMD, PS2_CMD_WRITE_AUX);
    wait_input_clear();
    outb(PS2_DATA, data);
}

void ps2_64_init(void) {
    // Disable both ports while configuring, so any in-flight byte from
    // either device can't be misinterpreted as a config-byte response.
    wait_input_clear(); outb(PS2_CMD, PS2_CMD_DISABLE_KBD);
    wait_input_clear(); outb(PS2_CMD, PS2_CMD_DISABLE_AUX);

    // Flush any stale output-buffer byte left over from boot firmware.
    while (inb(PS2_STATUS) & PS2_STATUS_OUTPUT_FULL) inb(PS2_DATA);

    // Read-modify-write the configuration byte: enable IRQ1 (port 1)
    // and IRQ12 (port 2) delivery, enable both device clocks (bits 4/5
    // are the "clock disabled" flags the two DISABLE commands above
    // just set). Deliberately leaves the translation bit (bit 6)
    // untouched -- Milestone 2-28's keyboard path already assumes Set 1
    // scancodes arrive at port 0x60 and has always worked under QEMU,
    // so whatever that bit's boot-time value is must already be
    // correct; changing it here would risk silently breaking scancode
    // decoding for no benefit this milestone needs.
    wait_input_clear(); outb(PS2_CMD, PS2_CMD_READ_CONFIG);
    uint8_t cfg = ps2_64_read_data();
    cfg |= 0x03;   // bit0 = port1 IRQ enable, bit1 = port2 IRQ enable
    cfg &= ~0x30;  // clear port1/port2 clock-disable bits
    wait_input_clear(); outb(PS2_CMD, PS2_CMD_WRITE_CONFIG);
    wait_input_clear(); outb(PS2_DATA, cfg);

    // Re-enable both ports.
    wait_input_clear(); outb(PS2_CMD, PS2_CMD_ENABLE_KBD);
    wait_input_clear(); outb(PS2_CMD, PS2_CMD_ENABLE_AUX);

    klog("ps2_64: controller initialized (both ports enabled)\n");
}
