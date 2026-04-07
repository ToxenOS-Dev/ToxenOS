#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

#define SYS_EXIT      0
#define SYS_PRINT     1
#define SYS_GETCHAR   2
#define SYS_GETPID    3
#define SYS_YIELD     4
#define SYS_ERASE     5
#define SYS_SETCOLOR  6
#define SYS_CLEAR     7
#define SYS_REBOOT    8
#define SYS_SHUTDOWN  9
#define SYS_READDIR   10
#define SYS_MKDIR     11
#define SYS_OPEN      12
#define SYS_READ      13
#define SYS_WRITE     14
#define SYS_CLOSE     15
#define SYS_REMOVE    16
#define SYS_STAT      17
#define SYS_ISDIR     18
#define SYS_GET_TTY   19
#define SYS_MY_TTY    20
#define SYS_EXEC      21   // exec(path) — replace current process with ELF
#define SYS_SPAWN     22   // spawn(path) — launch ELF as new process, return pid
#define SYS_WAIT      23   // wait(pid)   — block until process exits
#define SYS_SPAWN_TTY      24   // spawn_tty(path, tty) — spawn on specific TTY
#define SYS_SPAWN_EMBEDDED 25  // spawn embedded shell on given TTY
#define SYS_GET_ARGS       26  // get_args(buf) — copy process args to user buf
#define SYS_SPAWN_ARGS     27  // spawn_args(path, tty, args)
#define SYS_IS_ALIVE       28  // is_alive(pid) — 1 if process is running
#define SYS_KEYAVAIL       29  // keyavail() — 1 if a key is waiting
#define SYS_BMSG           30  // bmsg(buf, size) — read kernel log
#define SYS_PROC_LIST      31  // proc_list(buf, size) — get process list
#define SYS_KILL           32  // kill(pid) — terminate a process
#define SYS_SIGINT_TARGET  33  // sigint_target(pid) — set Ctrl+C target
#define SYS_PIPE           34  // pipe(rfd*, wfd*) — create a pipe
#define SYS_SPAWN_PIPE     35  // spawn_pipe(path, args, stdin_fd, stdout_fd)
#define SYS_SLEEP          36  // sleep(ms)
#define SYS_SBRK           37  // sbrk(increment) — grow heap
#define SYS_SPAWN_INHERIT  38  // spawn_inherit(path, args, ilist[])
#define SYS_NET_SEND_UDP   39  // send UDP packet
#define SYS_NET_POLL       40  // poll for incoming packets
#define SYS_NET_GET_IP     41  // get our IP address
#define SYS_NET_UDP_RECV   42

uint32_t __attribute__((cdecl)) syscall_handler(uint32_t eax, uint32_t ebx,
                                                  uint32_t ecx, uint32_t edx);
void syscall_init();

void sys_exit(int code);
void sys_print(const char* str);
char sys_getchar();
int  sys_getpid();

#endif
#define SYS_TCP_CONNECT    44
#define SYS_TCP_SEND       45
#define SYS_TCP_RECV       46
#define SYS_TCP_CLOSE      47
#define SYS_PING           48  // ping(ip, count)
#define SYS_TLS_CONNECT    49
#define SYS_TLS_SEND       50
#define SYS_TLS_RECV       51
#define SYS_TLS_CLOSE      52
