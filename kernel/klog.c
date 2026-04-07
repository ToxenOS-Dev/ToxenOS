// ToxenOS/kernel/klog.c
// Kernel log ring buffer — written by klog(), read by SYS_BMSG.
#include <stdint.h>
#include "../include/klog.h"

static char klog_buf[KLOG_SIZE];
static int  klog_pos = 0;

void klog(const char* msg)
{
    for (int i = 0; msg[i] && klog_pos < KLOG_SIZE - 1; i++)
        klog_buf[klog_pos++] = msg[i];
    klog_buf[klog_pos] = 0;
}

// klog_buf and klog_pos are accessed directly by syscall.c for SYS_BMSG.
// Expose them as a single accessor to avoid making the buffer public.
const char* klog_get_buf(void)  { return klog_buf; }
int         klog_get_size(void) { return KLOG_SIZE; }
