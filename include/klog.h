#ifndef KLOG_H
#define KLOG_H

// klog — kernel ring buffer for boot and diagnostic messages.
// Written to by the kernel; read by userspace via SYS_BMSG.

#define KLOG_SIZE 65536

void        klog(const char* msg);
void        klog_hex(const char* label, uint32_t val);
const char* klog_get_buf(void);
int         klog_get_size(void);
// Linearise the ring buffer into `out` (max `max` bytes), NUL-terminated.
// Returns number of bytes written (not counting NUL).
int         klog_read(char* out, int max);

#endif // KLOG_H
