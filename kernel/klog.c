// ToxenOS/kernel/klog.c
// Kernel log ring buffer — written by klog(), read by SYS_BMSG.
// Also writes to COM1 serial port for debugging.
//
// Ring buffer: newest data always survives; oldest wraps around when full.
// klog_read() linearises the ring into a flat buffer for userspace.
#include <stdint.h>
#include "../include/klog.h"

static char     klog_buf[KLOG_SIZE];
static int      klog_pos  = 0;   // next write position (wraps mod KLOG_SIZE)
static int      klog_total = 0;  // total bytes ever written (unbounded)
static int      serial_ready = 0;

static inline void serial_init(void) {
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)0x00), "d"((uint16_t)0x3F9));
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)0x80), "d"((uint16_t)0x3FB));
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)0x01), "d"((uint16_t)0x3F8));
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)0x00), "d"((uint16_t)0x3F9));
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)0x03), "d"((uint16_t)0x3FB));
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)0xC7), "d"((uint16_t)0x3FA));
    serial_ready = 1;
}

static inline void serial_putc(char c) {
    if (!serial_ready) return;
    uint8_t lsr;
    do {
        __asm__ volatile("inb %1, %0" : "=a"(lsr) : "d"((uint16_t)0x3FD));
    } while (!(lsr & 0x20));
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)c), "d"((uint16_t)0x3F8));
}

void klog(const char* msg)
{
    if (!serial_ready) serial_init();
    for (int i = 0; msg[i]; i++) {
        char c = msg[i];
        klog_buf[klog_pos] = c;
        klog_pos = (klog_pos + 1) % KLOG_SIZE;
        klog_total++;
        serial_putc(c);
    }
}

void klog_hex(const char* label, uint32_t val)
{
    const char* h = "0123456789ABCDEF";
    char buf[11];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 9; i >= 2; i--) { buf[i] = h[val & 0xF]; val >>= 4; }
    buf[10] = 0;
    klog(label);
    klog(buf);
    klog("\n");
}

// Linearise ring buffer into `out` (newest KLOG_SIZE-1 bytes, chronological).
int klog_read(char* out, int max)
{
    if (max <= 0) return 0;

    int filled = klog_total < KLOG_SIZE ? klog_total : KLOG_SIZE - 1;
    if (filled > max - 1) filled = max - 1;

    // Start position in ring: oldest byte is at klog_pos when ring is full,
    // or 0 when ring is not yet full.
    int start = (klog_total >= KLOG_SIZE) ? klog_pos : 0;

    for (int i = 0; i < filled; i++) {
        out[i] = klog_buf[(start + i) % KLOG_SIZE];
    }
    out[filled] = 0;
    return filled;
}

// Legacy accessors — kept for any callers that still use them.
const char* klog_get_buf(void)  { return klog_buf; }
int         klog_get_size(void) { return KLOG_SIZE; }
