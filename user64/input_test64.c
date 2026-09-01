// ToxenOS/user64/input_test64.c — Milestone 29: structured input-event
// syscall ABI end-to-end test. Opens the global input handle
// (sys_input_open), then performs N genuinely BLOCKING reads
// (sys_handle_read on a HANDLE64_INPUT handle), printing each event's
// type/code/pressed/modifiers/ascii/buttons -- proves the whole path
// from a real PS/2 IRQ (kernel/keyboard64.c or kernel/mouse64.c) through
// kernel/input64.c's queue and the blocking-read syscall reaches
// userspace as a structured event, not raw scancodes/packets.
//
// Driven interactively (or via QEMU monitor `sendkey`/`mouse_move`/
// `mouse_button` injection) -- this process genuinely blocks between
// events exactly like a compositor's event loop would, never polling.
#include <stdint.h>
#include "tox64.h"

#define EVENT_COUNT 6
#define OK_EXIT 42

static int my_strlen(const char* s) { int i = 0; while (s[i]) i++; return i; }
static void put(const char* s) { sys_write(s, (uint64_t)my_strlen(s)); }

static void append_u64(char* out, int* pos, uint64_t v) {
    char rev[24]; int rn = 0;
    if (v == 0) rev[rn++] = '0';
    while (v > 0) { rev[rn++] = (char)('0' + (v % 10)); v /= 10; }
    while (rn > 0) out[(*pos)++] = rev[--rn];
}

static void append_str(char* out, int* pos, const char* s) {
    while (*s) out[(*pos)++] = *s++;
}

static void print_event(int idx, const input64_event_t* ev) {
    char line[192];
    int pos = 0;
    append_str(line, &pos, "  event[");
    append_u64(line, &pos, (uint64_t)idx);
    append_str(line, &pos, "] type=");
    append_u64(line, &pos, ev->type);
    append_str(line, &pos, " a=");
    append_u64(line, &pos, (uint64_t)(int64_t)ev->a);
    append_str(line, &pos, " b=");
    append_u64(line, &pos, (uint64_t)(int64_t)ev->b);
    append_str(line, &pos, " pressed=");
    append_u64(line, &pos, ev->pressed);
    append_str(line, &pos, " mods=");
    append_u64(line, &pos, ev->modifiers);
    append_str(line, &pos, " ascii=");
    append_u64(line, &pos, ev->ascii);
    append_str(line, &pos, " buttons=");
    append_u64(line, &pos, ev->buttons);
    append_str(line, &pos, " seq=");
    append_u64(line, &pos, ev->seq);
    append_str(line, &pos, "\n");
    line[pos] = 0;
    put(line);
}

void _start(void) {
    put("input_test64: starting\n");

    int64_t h = sys_input_open();
    if (h < 0) {
        put("input_test64: sys_input_open FAILED\n");
        sys_exit(1);
    }

    // A second open must fail while this one is still held -- proves
    // the single-consumer access policy (include/syscall64.h's
    // SYS64_INPUT_OPEN header comment).
    int64_t h2 = sys_input_open();
    if (h2 >= 0) {
        put("input_test64: FAIL second sys_input_open unexpectedly succeeded\n");
        sys_handle_close((int)h2);
        sys_handle_close((int)h);
        sys_exit(1);
    }
    put("input_test64: second sys_input_open correctly rejected (single-consumer) PASS\n");

    put("input_test64: waiting for events (blocking reads, no polling)...\n");
    for (int i = 0; i < EVENT_COUNT; i++) {
        input64_event_t ev;
        int64_t n = sys_handle_read((int)h, (char*)&ev, sizeof(ev));
        if (n != (int64_t)sizeof(ev)) {
            put("input_test64: sys_handle_read FAILED\n");
            sys_handle_close((int)h);
            sys_exit(1);
        }
        print_event(i, &ev);
    }

    sys_handle_close((int)h);
    put("input_test64: all events received, handle closed -- PASS\n");
    sys_exit(OK_EXIT);
    for (;;) { } // unreachable
}
