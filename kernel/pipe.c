// ToxenOS/kernel/pipe.c
// Kernel pipe — ring buffer with two VFS file descriptors.
#include <stdint.h>
#include "../include/pipe.h"
#include "../include/process.h"

static pipe_t pipes[MAX_PIPES];

void pipe_init()
{
    for (int i = 0; i < MAX_PIPES; i++)
        pipes[i].used = 0;
}

int pipe_alloc()
{
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!pipes[i].used) {
            pipes[i].head       = 0;
            pipes[i].tail       = 0;
            pipes[i].used       = 1;
            pipes[i].write_open = 1;
            pipes[i].read_open  = 1;
            return i;
        }
    }
    return -1;
}

void pipe_close_read(int idx)
{
    if (idx < 0 || idx >= MAX_PIPES || !pipes[idx].used) return;
    if (--pipes[idx].read_open <= 0) {
        if (pipes[idx].write_open <= 0)
            pipes[idx].used = 0;
    }
}

void pipe_close_write(int idx)
{
    if (idx < 0 || idx >= MAX_PIPES || !pipes[idx].used) return;
    if (--pipes[idx].write_open <= 0) {
        if (pipes[idx].read_open <= 0)
            pipes[idx].used = 0;
    }
}

// Returns bytes read, 0 on EOF (write end closed + empty), -1 on error.
int pipe_read(int idx, uint8_t* buf, uint32_t size)
{
    if (idx < 0 || idx >= MAX_PIPES || !pipes[idx].used) return -1;
    pipe_t* p = &pipes[idx];

    uint32_t count = 0;
    while (count < size) {
        if (p->head == p->tail) {
            // Buffer empty
            if (p->write_open <= 0) break;   // EOF — writer gone
            // Block until data arrives or writer closes
            scheduler();
            continue;
        }
        buf[count++] = p->buf[p->head];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
    }
    return (int)count;
}

// Returns bytes written, -1 if read end closed (broken pipe).
int pipe_write(int idx, const uint8_t* buf, uint32_t size)
{
    if (idx < 0 || idx >= MAX_PIPES || !pipes[idx].used) return -1;
    pipe_t* p = &pipes[idx];

    if (p->read_open <= 0) return -1;  // broken pipe

    uint32_t count = 0;
    while (count < size) {
        uint32_t next = (p->tail + 1) % PIPE_BUF_SIZE;
        if (next == p->head) {
            // Buffer full — yield and retry
            scheduler();
            if (p->read_open <= 0) return (int)count;  // reader gone
            continue;
        }
        p->buf[p->tail] = buf[count++];
        p->tail = next;
    }
    return (int)count;
}
