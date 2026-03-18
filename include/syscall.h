#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

// syscall numbers
#define SYS_EXIT     0
#define SYS_PRINT    1
#define SYS_GETCHAR  2
#define SYS_GETPID   3
#define SYS_YIELD   4
#define SYS_ERASE   5
#define SYS_SETCOLOR  6
#define SYS_CLEAR    7
#define SYS_REBOOT   8
#define SYS_SHUTDOWN 9
#define SYS_READDIR  10
#define SYS_MKDIR    11
#define SYS_OPEN     12
#define SYS_READ     13
#define SYS_WRITE    14
#define SYS_CLOSE    15
#define SYS_REMOVE   16
#define SYS_STAT 17
#define SYS_ISDIR 18
#define SYS_GET_TTY  19
#define SYS_MY_TTY   20

uint32_t __attribute__((cdecl)) syscall_handler(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx);

void syscall_init();

// kernel-side handlers
void sys_exit(int code);
void sys_print(const char* str);
char sys_getchar();
int  sys_getpid();

#endif