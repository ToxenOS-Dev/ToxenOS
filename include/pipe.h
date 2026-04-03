#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>
#include "waitqueue.h"

#define PIPE_BUF_SIZE  4096
#define MAX_PIPES      16

#define FD_TYPE_FILE    0
#define FD_TYPE_PIPE_R  1
#define FD_TYPE_PIPE_W  2

typedef struct {
    uint8_t      buf[PIPE_BUF_SIZE];
    uint32_t     head;
    uint32_t     tail;
    int          used;
    int          write_open;
    int          read_open;
    wait_queue_t readers;   // processes blocked waiting for data
    wait_queue_t writers;   // processes blocked waiting for space
} pipe_t;

void pipe_init();
int  pipe_alloc();                          // returns pipe index or -1
void pipe_close_read(int idx);
void pipe_close_write(int idx);
int  pipe_read(int idx, uint8_t* buf, uint32_t size);
int  pipe_write(int idx, const uint8_t* buf, uint32_t size);

// Create a pipe, filling rfd/wfd with VFS file descriptors
int vfs_pipe(int* rfd, int* wfd);

#endif
