// ToxenOS/kernel/pipe.c
// Kernel pipe — ring buffer with wait-queue blocking.
#include <stdint.h>
#include "../include/pipe.h"
#include "../include/process.h"
#include "../include/waitqueue.h"

static pipe_t pipes[MAX_PIPES];

void pipe_init()
{
    for (int i = 0; i < MAX_PIPES; i++) {
        pipes[i].used = 0;
        wq_init(&pipes[i].readers);
        wq_init(&pipes[i].writers);
    }
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
            wq_init(&pipes[i].readers);
            wq_init(&pipes[i].writers);
            return i;
        }
    }
    return -1;
}

void pipe_close_read(int idx)
{
    if (idx < 0 || idx >= MAX_PIPES || !pipes[idx].used) return;
    if (--pipes[idx].read_open <= 0) {
        wq_wake_all(&pipes[idx].writers);
        if (pipes[idx].write_open <= 0)
            pipes[idx].used = 0;
    }
}

void pipe_close_write(int idx)
{
    if (idx < 0 || idx >= MAX_PIPES || !pipes[idx].used) return;
    if (--pipes[idx].write_open <= 0) {
        wq_wake_all(&pipes[idx].readers);
        if (pipes[idx].read_open <= 0)
            pipes[idx].used = 0;
    }
}

int pipe_read(int idx, uint8_t* buf, uint32_t size)
{
    if (idx < 0 || idx >= MAX_PIPES || !pipes[idx].used) return -1;
    pipe_t* p = &pipes[idx];
    uint32_t count = 0;
    while (count < size) {
        if (p->head == p->tail) {
            if (p->write_open <= 0) break;
            wq_sleep(&p->readers);
            continue;
        }
        buf[count++] = p->buf[p->head];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
        wq_wake_one(&p->writers);
    }
    return (int)count;
}

int pipe_write(int idx, const uint8_t* buf, uint32_t size)
{
    if (idx < 0 || idx >= MAX_PIPES || !pipes[idx].used) return -1;
    pipe_t* p = &pipes[idx];
    if (p->read_open <= 0) return -1;
    uint32_t count = 0;
    while (count < size) {
        uint32_t next = (p->tail + 1) % PIPE_BUF_SIZE;
        if (next == p->head) {
            wq_sleep(&p->writers);
            if (p->read_open <= 0) return (int)count;
            continue;
        }
        p->buf[p->tail] = buf[count++];
        p->tail = next;
        wq_wake_one(&p->readers);
    }
    return (int)count;
}
