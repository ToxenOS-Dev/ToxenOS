#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

// syscall numbers
#define SYS_EXIT     0
#define SYS_PRINT    1
#define SYS_GETCHAR  2
#define SYS_GETPID   3

void syscall_init();

// kernel-side handlers
void sys_exit(int code);
void sys_print(const char* str);
char sys_getchar();
int  sys_getpid();

#endif