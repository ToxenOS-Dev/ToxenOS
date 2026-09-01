#ifndef INPUT64_H
#define INPUT64_H

#include <stdint.h>

// Milestone 29: generic input-event layer shared conceptually by
// keyboard and mouse (and, eventually, USB HID -- not implemented this
// milestone, but any future driver that can produce an input64_event_t
// slots in here without the compositor ABI (SYS64_INPUT_OPEN / the
// HANDLE64_INPUT read path in kernel/syscall64.c) ever changing).
//
// Design:
//   - ONE global bounded ring buffer of events, not a queue per input
//     handle -- see the header comment on input64_open() for why this
//     milestone deliberately keeps input single-consumer.
//   - Producers (kernel/keyboard64.c's IRQ1 handler, kernel/mouse64.c's
//     IRQ12 handler) call input64_push() from interrupt context. It
//     never blocks and never allocates.
//   - Consumers call input64_read_blocking(), which pops one event,
//     blocking the calling process (a real scheduler block via
//     process64_block_on/wake_one -- Milestone 26 -- never a busy-wait)
//     while the queue is empty.
//   - Overflow policy: if the queue is full when a new event arrives,
//     the OLDEST buffered event is dropped to make room. Rationale: a
//     live input stream's most useful property is "reflects the
//     current state," so a consumer that has fallen behind should catch
//     up to recent events rather than eventually process a long queue
//     of stale ones. g_overflow_count (see input64_stats) tracks how
//     often this has happened, for diagnostics -- it is never fed back
//     into event delivery (no signaling/backpressure to producers this
//     milestone).
typedef enum {
    INPUT64_EVENT_KEY            = 1,
    INPUT64_EVENT_POINTER_MOVE   = 2,
    INPUT64_EVENT_POINTER_BUTTON = 3,
    INPUT64_EVENT_POINTER_WHEEL  = 4,
} input64_event_type_t;

// Modifier bitmask (input64_event_t::modifiers), valid on KEY events.
#define INPUT64_MOD_SHIFT    0x01u
#define INPUT64_MOD_CTRL     0x02u
#define INPUT64_MOD_ALT      0x04u
#define INPUT64_MOD_CAPSLOCK 0x08u

// Pointer button bitmask (input64_event_t::buttons: currently-held set;
// for a POINTER_BUTTON event, `code` names the single button that just
// changed state, one of these same bit values).
#define INPUT64_BTN_LEFT   0x01u
#define INPUT64_BTN_RIGHT  0x02u
#define INPUT64_BTN_MIDDLE 0x04u

// Logical keycode space (input64_event_t::code on KEY events): a plain
// PS/2 Set-1 make code for unextended keys, or 0x100 | (Set-1 make code)
// for keys that arrived with an 0xE0 prefix. This is a stable, PS/2-
// independent-in-spirit numbering ONLY in the sense that a future USB
// HID driver can populate the exact same field by translating its own
// usage codes into this space -- the compositor ABI (this struct) never
// needs to know which physical bus produced the event. Not an
// exhaustive enum -- most named keys the shell/test programs care about
// are listed below; anything else still carries a valid raw code in
// `code`, just without a symbolic name here.
#define INPUT64_KEY_EXTENDED   0x100u
#define INPUT64_KEY_UP         (INPUT64_KEY_EXTENDED | 0x48u)
#define INPUT64_KEY_DOWN       (INPUT64_KEY_EXTENDED | 0x50u)
#define INPUT64_KEY_LEFT       (INPUT64_KEY_EXTENDED | 0x4Bu)
#define INPUT64_KEY_RIGHT      (INPUT64_KEY_EXTENDED | 0x4Du)
#define INPUT64_KEY_HOME       (INPUT64_KEY_EXTENDED | 0x47u)
#define INPUT64_KEY_END        (INPUT64_KEY_EXTENDED | 0x4Fu)
#define INPUT64_KEY_PAGE_UP    (INPUT64_KEY_EXTENDED | 0x49u)
#define INPUT64_KEY_PAGE_DOWN  (INPUT64_KEY_EXTENDED | 0x51u)
#define INPUT64_KEY_INSERT     (INPUT64_KEY_EXTENDED | 0x52u)
#define INPUT64_KEY_DELETE     (INPUT64_KEY_EXTENDED | 0x53u)
#define INPUT64_KEY_LCTRL      0x1Du
#define INPUT64_KEY_RCTRL      (INPUT64_KEY_EXTENDED | 0x1Du)
#define INPUT64_KEY_LALT       0x38u
#define INPUT64_KEY_RALT       (INPUT64_KEY_EXTENDED | 0x38u)
#define INPUT64_KEY_LSHIFT     0x2Au
#define INPUT64_KEY_RSHIFT     0x36u
#define INPUT64_KEY_CAPSLOCK   0x3Au
#define INPUT64_KEY_ENTER      0x1Cu
#define INPUT64_KEY_BACKSPACE  0x0Eu
#define INPUT64_KEY_TAB        0x0Fu
#define INPUT64_KEY_ESCAPE     0x01u
#define INPUT64_KEY_SPACE      0x39u

// Fixed-size, fixed-layout event -- deliberately no unions (keeps the
// userspace ABI unambiguous): every field is always present, unused
// ones are always 0.
typedef struct {
    uint32_t type;         // input64_event_type_t
    uint32_t seq;           // monotonically incrementing sequence number
    int32_t  a;              // KEY: logical keycode | POINTER_MOVE: dx | POINTER_BUTTON: button bit | POINTER_WHEEL: delta
    int32_t  b;              // POINTER_MOVE: dy; unused (0) otherwise
    uint32_t pressed;       // KEY/POINTER_BUTTON: 1 = down, 0 = up; unused (0) otherwise
    uint32_t modifiers;     // shift/ctrl/alt/capslock bitmask -- KEY events only
    uint32_t ascii;         // translated character for KEY events (0 if none, or on release)
    uint32_t buttons;       // currently-held pointer button bitmask -- POINTER_* events only
} input64_event_t;

void input64_init(void);

// Producer side -- called only from kernel/keyboard64.c and
// kernel/mouse64.c's IRQ handlers. Never blocks; safe to call with
// interrupts already disabled (as IRQ handlers always are).
void input64_push(const input64_event_t* ev);

// Consumer side. Blocks (scheduler block, not polling) while the queue
// is empty; returns the next event in *out once one is available.
// Returns -1 only if there is no current process to block (kernel/idle
// context can't be a consumer).
int input64_read_blocking(input64_event_t* out);

// Milestone 29 access policy: for now ToxenOS has no credential system,
// so a simple single-consumer policy stands in for real access control
// -- see include/handle64.h/kernel/syscall64.c's SYS64_INPUT_OPEN.
// Returns 0 if ownership was granted (no one currently owns the input
// stream), -1 if it's already owned by someone else.
int input64_acquire(void);
void input64_release(void);

typedef struct {
    uint32_t queued;          // events currently buffered
    uint32_t capacity;        // ring buffer capacity
    uint32_t overflow_count;  // total events dropped (oldest-dropped) to date
    uint32_t owner_pid;       // current owner's pid, or 0 if unowned
} input64_stats_t;
void input64_stats(input64_stats_t* out);
void input64_dump(void);

#endif // INPUT64_H
