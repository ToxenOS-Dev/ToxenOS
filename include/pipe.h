#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>

#define PIPE_BUF_SIZE  4096
#define MAX_PIPES      16

// FD type flags stored in file_descriptor_t.type
#define FD_TYPE_FILE    0
#define FD_TYPE_PIPE_R  1   // read end of a pipe
#define FD_TYPE_PIPE_W  2   // write end of a pipe

typedef struct {
    uint8_t  buf[PIPE_BUF_SIZE];
    uint32_t head;        // next read position
    uint32_t tail;        // next write position
    int      used;
    int      write_open;  // number of write-ends still open
    int      read_open;   // number of read-ends still open
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
