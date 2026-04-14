// ToxenOS/kernel/klog.c
// Kernel log ring buffer — written by klog(), read by SYS_BMSG.
// Also writes to COM1 serial port for QEMU -serial stdio debugging.
#include <stdint.h>
#include "../include/klog.h"

static char klog_buf[KLOG_SIZE];
static int  klog_pos = 0;
static int  serial_ready = 0;

static inline void serial_init(void) {
    // COM1 = 0x3F8, init at 115200 baud
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)0x00), "d"((uint16_t)0x3F9)); // disable ints
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)0x80), "d"((uint16_t)0x3FB)); // DLAB on
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)0x01), "d"((uint16_t)0x3F8)); // baud lo
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)0x00), "d"((uint16_t)0x3F9)); // baud hi
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)0x03), "d"((uint16_t)0x3FB)); // 8N1
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)0xC7), "d"((uint16_t)0x3FA)); // FIFO
    serial_ready = 1;
}

static inline void serial_putc(char c) {
    if (!serial_ready) return;
    // Wait for transmit empty
    uint8_t lsr;
    do {
        __asm__ volatile("inb %1, %0" : "=a"(lsr) : "d"((uint16_t)0x3FD));
    } while (!(lsr & 0x20));
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)c), "d"((uint16_t)0x3F8));
}

void klog(const char* msg)
{
    if (!serial_ready) serial_init();
    for (int i = 0; msg[i] && klog_pos < KLOG_SIZE - 1; i++) {
        klog_buf[klog_pos++] = msg[i];
        serial_putc(msg[i]);
    }
    klog_buf[klog_pos] = 0;
}

const char* klog_get_buf(void)  { return klog_buf; }
int         klog_get_size(void) { return KLOG_SIZE; }
