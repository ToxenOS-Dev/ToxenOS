// ToxenOS/kernel/waitqueue.c
// Simple wait queue — processes sleep here until woken by an event.
#include <stdint.h>
#include "../include/waitqueue.h"
#include "../include/process.h"

void wq_init(wait_queue_t* wq)
{
    for (int i = 0; i < WAIT_QUEUE_MAX; i++)
        wq->pids[i] = -1;
    wq->count = 0;
}

void wq_sleep(wait_queue_t* wq)
{
    process_t* me = process_current();

    // Add ourselves to the queue
    for (int i = 0; i < WAIT_QUEUE_MAX; i++) {
        if (wq->pids[i] == -1) {
            wq->pids[i] = (int)me->pid;
            wq->count++;
            break;
        }
    }

    // Sleep until woken
    me->state = PROCESS_WAITING;
    me->waiting_for = -1;  // not waiting on a pid, waiting on a queue
    scheduler();
    // When we return here we have been woken
}

void wq_wake_one(wait_queue_t* wq)
{
    for (int i = 0; i < WAIT_QUEUE_MAX; i++) {
        int pid = wq->pids[i];
        if (pid >= 0 && pid < MAX_PROCESSES) {
            if (processes[pid].state == PROCESS_WAITING) {
                processes[pid].state = PROCESS_READY;
                wq->pids[i] = -1;
                wq->count--;
                return;
            }
            // Process is gone — clean up the slot
            wq->pids[i] = -1;
            wq->count--;
        }
    }
}

void wq_wake_all(wait_queue_t* wq)
{
    for (int i = 0; i < WAIT_QUEUE_MAX; i++) {
        int pid = wq->pids[i];
        if (pid >= 0 && pid < MAX_PROCESSES) {
            if (processes[pid].state == PROCESS_WAITING)
                processes[pid].state = PROCESS_READY;
            wq->pids[i] = -1;
        }
    }
    wq->count = 0;
}
