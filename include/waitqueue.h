#ifndef WAITQUEUE_H
#define WAITQUEUE_H

#include <stdint.h>

#define WAIT_QUEUE_MAX 16  // max processes sleeping on one queue

typedef struct {
    int pids[WAIT_QUEUE_MAX];  // sleeping process IDs (-1 = empty slot)
    int count;
} wait_queue_t;

// Initialise a queue to empty
void wq_init(wait_queue_t* wq);

// Put the calling process to sleep on this queue.
// Returns when wq_wake_one() or wq_wake_all() is called.
void wq_sleep(wait_queue_t* wq);

// Wake the first sleeping process on the queue
void wq_wake_one(wait_queue_t* wq);

// Wake every sleeping process on the queue
void wq_wake_all(wait_queue_t* wq);

#endif
