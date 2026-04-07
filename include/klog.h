#ifndef KLOG_H
#define KLOG_H

// klog — kernel ring buffer for boot and diagnostic messages.
// Written to by the kernel; read by userspace via SYS_BMSG.

#define KLOG_SIZE 4096

void        klog(const char* msg);
const char* klog_get_buf(void);
int         klog_get_size(void);

#endif // KLOG_H
