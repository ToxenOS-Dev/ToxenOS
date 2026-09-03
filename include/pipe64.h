#ifndef PIPE64_H
#define PIPE64_H

#include <stdint.h>

// Milestone 26: a real kernel pipe object -- bounded, kmalloc'd ring
// buffer with reference-counted ends and real scheduler blocking (via
// kernel/process64.c's generalized process64_block_on/wake_* -- see
// that header for the "hold cli across check+block" contract every
// function below relies on). NOT a userspace polling abstraction: a
// blocked reader/writer is genuinely PROCESS64_BLOCKED and off the run
// queue, woken only when the relevant condition actually changes.
//
// A pipe end is never addressed directly by userspace -- see
// include/handle64.h for why (small per-process handle numbers, never
// a raw pipe64_t* or a global pipe ID).
#define PIPE64_BUF_SIZE 256

typedef struct pipe64 {
    uint8_t* buffer;      // kmalloc'd, PIPE64_BUF_SIZE bytes
    uint32_t capacity;
    uint32_t head;        // next byte to WRITE goes here
    uint32_t tail;        // next byte to READ comes from here
    uint32_t count;       // bytes currently buffered (0 == empty, ==capacity == full)
    uint32_t readers;     // open HANDLE64_PIPE_READ references
    uint32_t writers;     // open HANDLE64_PIPE_WRITE references
    // Dummy bytes whose ADDRESSES are this pipe's two wait-channel
    // identities (see process64_block_on) -- one for blocked readers,
    // one for blocked writers, distinguishable despite living in the
    // same kmalloc'd object.
    uint8_t  read_chan;
    uint8_t  write_chan;
    struct pipe64* dbg_next; // intrusive list of every live pipe, diagnostics only
} pipe64_t;

// Creates a new pipe: kmalloc's the struct and its ring buffer,
// readers = writers = 1 (the creating process's own read+write ends).
// Returns 0 with *out set, or -1 (out of memory) -- nothing is left
// allocated on failure.
int pipe64_create(pipe64_t** out);

// +1 reference on the read/write end -- called when a handle
// referencing that end is duplicated (Milestone 26 spawn inheritance).
void pipe64_add_ref_read(pipe64_t* p);
void pipe64_add_ref_write(pipe64_t* p);

// -1 reference. Closing the last writer wakes every blocked reader
// (they'll observe EOF once the buffer drains); closing the last
// reader wakes every blocked writer (they'll observe "broken pipe").
// The pipe itself (buffer + struct) is freed once BOTH counts reach
// zero -- never while a blocked peer might still reference it, since a
// blocked peer's own open handle is one of the two counts.
void pipe64_close_read(pipe64_t* p);
void pipe64_close_write(pipe64_t* p);

// Reads up to `len` bytes into `kbuf` (a KERNEL buffer -- the syscall
// layer copies out to userspace afterward, same pattern as the
// existing sys64_read/txfs64_read pair). Blocks (real scheduler block,
// not polling) while the pipe is empty and at least one writer
// remains, looping internally to satisfy the whole request where
// possible -- a single call CAN span multiple blocking waits if the
// caller asks for more than fits in the ring buffer at once. Returns
// the number of bytes actually collected: exactly `len` if nothing
// interrupts it, fewer (possibly 0) only once the pipe empties AND no
// writer remains (EOF). len == 0 returns 0 immediately.
int64_t pipe64_read(pipe64_t* p, uint8_t* kbuf, uint64_t len);

// Milestone 30: non-blocking counterpart, needed because ToxenOS has no
// select()/poll()/epoll() equivalent and no threads -- a userspace
// compositor multiplexing its input handle plus one pipe per connected
// client has no way to wait on "whichever of these is ready first"
// other than polling each with a non-blocking read. Never blocks, and
// deliberately ALL-OR-NOTHING (unlike pipe64_read, which happily
// returns a short read): either all `len` bytes are already buffered,
// consumed atomically, and `len` is returned, or NOTHING is consumed
// and -2 is returned (pipe not yet full enough, but at least one
// writer remains). This matters because the compositor protocol
// (user64/wmproto64.h) is fixed-size messages over a byte-stream pipe
// -- a short read here would silently desync the framing (the next
// read would start mid-message), which the caller has no way to detect
// or recover from. Returns 0 only for genuine EOF (fewer than `len`
// bytes buffered AND no writer remains -- there can never be a full
// `len` again). len == 0 returns 0 immediately.
int64_t pipe64_try_read(pipe64_t* p, uint8_t* kbuf, uint64_t len);

// Writes up to `len` bytes from `kbuf`. Blocks while the pipe is full
// and at least one reader remains. Returns -1 if there are no readers
// at all -- checked both up front (immediate failure, nothing
// written) and again after every blocking wait (a reader that
// disappears mid-write stops the write and returns whatever was
// already delivered; only a write that manages to deliver ZERO bytes
// because of this reports -1 rather than 0, so a genuine "nothing
// happened" is distinguishable from "len was 0"). len == 0 returns 0
// immediately (even with no readers).
int64_t pipe64_write(pipe64_t* p, const uint8_t* kbuf, uint64_t len);

// Milestone 32.1: non-blocking counterpart to pipe64_write, mirroring
// pipe64_try_read's contract exactly (see above) -- needed because a
// process that both reads AND writes several pipes in one non-blocking
// poll loop (kernel/../user64/compositor64.c's main loop is the
// motivating case: it must never block delivering an event to one
// client just because that client isn't draining its end) has no way
// to know in advance whether a write would fit. Deliberately
// ALL-OR-NOTHING like pipe64_try_read -- a partial write would desync
// the receiver's fixed-size message framing exactly like a short read
// would. Never blocks. Returns `len` if the whole write fit and was
// copied in immediately, -2 if there isn't enough free space right now
// but at least one reader remains (caller should retry later, e.g. via
// a small per-connection outgoing queue -- see compositor64.c), or -1
// if there are no readers at all (broken pipe, matches pipe64_write's
// -1 for the same condition). len == 0 returns 0 immediately.
int64_t pipe64_try_write(pipe64_t* p, const uint8_t* kbuf, uint64_t len);

// Diagnostics: logs every live pipe's address, capacity/count,
// reader/writer counts, and any pids currently blocked reading or
// writing it, via klog(). Development use only, same precedent as
// process64_dump()/heap64_dump() -- never called on the normal boot
// path.
void pipe64_dump(void);

// Runs the Milestone 26 pipe self-test suite. Logs each case and a
// final tally via klog(). Returns 1 if every case passed.
int pipe64_selftest(void);

#endif // PIPE64_H
