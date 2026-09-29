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
// M+7A: POINTER_MOVE renamed POINTER_REL (value unchanged) and a new
// POINTER_ABS added alongside it, so relative and absolute pointer
// motion are equally first-class event types instead of one being the
// original and the other a bolt-on -- see this header's own "device
// model" section below for how a consumer maps ABS device-space
// coordinates to logical display coordinates. No EV_SYN-equivalent
// event type: every input64_event_t is already a complete, self-
// contained sample (a producer that receives several raw sub-events
// per logical sample -- e.g. VirtIO-input's separate ABS_X/ABS_Y/
// BTN_LEFT reports between two EV_SYN/SYN_REPORT markers -- buffers
// them itself and pushes ONE combined event, exactly like
// kernel/mouse64.c already buffers 3 raw PS/2 bytes into one packet
// before ever calling input64_push()). Framing/synchronization is a
// producer-internal concern, not something input64's consumers need to
// see.
typedef enum {
    INPUT64_EVENT_KEY            = 1,
    INPUT64_EVENT_POINTER_REL    = 2, // was POINTER_MOVE
    INPUT64_EVENT_POINTER_BUTTON = 3,
    INPUT64_EVENT_POINTER_WHEEL  = 4,
    INPUT64_EVENT_POINTER_ABS    = 5, // M+7A: absolute position -- see input64_get_abs_range()
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
    int32_t  a;              // KEY: logical keycode | POINTER_REL: dx | POINTER_BUTTON: button bit | POINTER_WHEEL: delta | POINTER_ABS: raw device-space x
    int32_t  b;              // POINTER_REL: dy | POINTER_ABS: raw device-space y; unused (0) otherwise
    uint32_t pressed;       // KEY/POINTER_BUTTON: 1 = down, 0 = up; unused (0) otherwise
    uint32_t modifiers;     // shift/ctrl/alt/capslock bitmask -- KEY events only
    uint32_t ascii;         // translated character for KEY events (0 if none, or on release)
    uint32_t buttons;       // currently-held pointer button bitmask -- POINTER_* events only
} input64_event_t;

// ── M+7A: generic input device model ─────────────────────────────────
// A small registry, mirroring the same "driver registers a static/
// kmalloc'd descriptor for the kernel's lifetime" pattern
// include/gpu64.h's gpu64_device_t and kernel/blockdev64.c's
// blockdev64_t already establish. Diagnostic/enumeration only -- see
// input64_set_abs_pointer_active() below for the separate, fast,
// interrupt-context-safe check producers actually use to decide
// whether to push a REL/BUTTON event, which does NOT walk this list.
typedef enum {
    INPUT64_DEVICE_PS2_MOUSE      = 1,
    INPUT64_DEVICE_PS2_KEYBOARD   = 2,
    INPUT64_DEVICE_VIRTIO_POINTER = 3,
} input64_device_kind_t;

// Capability bits -- deliberately matching Linux evdev's own top-level
// event-type split (EV_KEY covers BOTH keyboard keys AND mouse/tablet
// buttons in that model, which is why there is no separate "buttons"
// bit here either) rather than inventing a ToxenOS-specific taxonomy.
#define INPUT64_CAP_KEY   0x01u
#define INPUT64_CAP_REL   0x02u
#define INPUT64_CAP_ABS   0x04u
#define INPUT64_CAP_WHEEL 0x08u

// Device-space range for an ABS-capable device's X/Y axes (e.g. a
// VirtIO-input tablet's own VIRTIO_INPUT_CFG_ABS_INFO, not any logical
// display resolution) -- see input64_get_abs_range()'s own comment on
// where normalization to display coordinates actually happens.
typedef struct {
    int32_t min_x, max_x, min_y, max_y;
} input64_abs_range_t;

typedef struct input64_device_s {
    char name[16];
    input64_device_kind_t kind;
    uint32_t capabilities;         // INPUT64_CAP_* bitmask
    input64_abs_range_t abs_range;  // meaningful only if capabilities & INPUT64_CAP_ABS
    struct input64_device_s* next;  // registry linkage -- kernel/input64.c owns this field
} input64_device_t;

// Registers `dev` (caller-owned, must remain valid for the kernel's
// lifetime -- static storage or kmalloc'd, never a stack local, same
// requirement gpu64_register/blockdev64_register already state).
// Returns 0, or -1 (NULL, or a duplicate name).
int input64_register_device(input64_device_t* dev);
input64_device_t* input64_find_device(const char* name);
input64_device_t* input64_iter_device(input64_device_t* prev); // NULL to start

// ── M+7A: pointer-source policy (§8) ─────────────────────────────────
// The simple, fast (interrupt-context-safe, no list walk) policy check
// kernel/mouse64.c's IRQ12 handler uses before pushing a POINTER_REL or
// POINTER_BUTTON event: "is a working absolute pointer device currently
// preferred?" -- see kernel/virtio_input64.c's own header comment for
// exactly when it calls the setter. Deliberately a single global flag,
// decided once when a VirtIO absolute pointer device finishes
// initializing successfully (mirroring compositor64's own g_hw_cursor_active
// "decide once, at boot" precedent) -- not a live, per-packet
// hot/cold-plug arbitration policy, which this milestone explicitly
// does not need. PS/2 keeps parsing and resyncing its own byte stream
// unconditionally either way; only whether it PUSHES events is gated,
// so it stays instantly available again if this flag is ever cleared.
void input64_set_abs_pointer_active(int active, const input64_abs_range_t* range);
int  input64_abs_pointer_active(void);
// Returns 0 with *out set to the active absolute device's range, or -1
// if no absolute pointer is currently active. What a consumer (e.g.
// compositor64, via SYS64_INPUT_GET_ABS_RANGE) calls ONCE at startup to
// learn how to map POINTER_ABS's raw device-space (a/b) into its own
// logical display coordinates -- never hardcoding a device's native
// range or the display's current resolution against each other inside
// any driver.
int input64_get_abs_range(input64_abs_range_t* out);

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

// Milestone 30: non-blocking counterpart -- same rationale as
// pipe64_try_read (no select()/poll() equivalent, and a compositor
// must multiplex this queue against one pipe per connected client).
// Returns 0 with *out set if an event was available, -1 if there is no
// current process, or -2 if the queue is empty (would-block).
int input64_try_read(input64_event_t* out);

// M+12B: side-effect-free readiness check for SYS64_HANDLE_WAIT_ANY --
// 1 if the queue is currently non-empty, 0 otherwise. Never consumes.
int input64_ready(void);

// M+12B: this stream's wait-channel identity, for
// kernel/syscall64.c's resolve_wait_any_chan to register against via
// process64_block_on_any -- the exact same address input64_read_blocking/
// input64_push already block/wake on, just exposed so a caller waiting
// on SEVERAL handles at once (not just this one) can name it.
void* input64_wait_chan(void);

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
