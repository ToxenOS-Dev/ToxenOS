// kernel/keyboard64.c — IRQ1 handler.
// Milestone 29: full PS/2 Set 1 scancode handling -- proper 0xE0
// extended-prefix state machine, left/right Shift/Ctrl/Alt tracked
// independently, Caps Lock toggle, key-up (break code) events, and a
// structured input64_event_t pushed for every make/break instead of
// reducing straight to ASCII at this layer. ASCII translation still
// happens here (this file OWNS the scancode->character tables), but as
// a layer above the raw parsing, not fused into it -- and it feeds the
// input64 queue AND (for backward compatibility) keyboard_buffer64's
// ASCII ring, rather than the old design where keyboard_buffer64 did
// its own scancode parsing directly.
#include <stdint.h>
#include "../include/keyboard_buffer64.h"
#include "../include/input64.h"

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Unshifted / shifted ASCII for the base (non-extended) Set 1 make-code
// range. Index is the raw make code (0-127); 0 = no ASCII mapping.
static const char base_table[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=',8,
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
    'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\',
    'z','x','c','v','b','n','m',',','.','/',0,'*',0,' ',
};

static const char shift_table[128] = {
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+',8,
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
    'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
    'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' ',
};

static inline int is_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// ── Modifier state ────────────────────────────────────────────────────
static int lshift = 0, rshift = 0, lctrl = 0, rctrl = 0, lalt = 0, ralt = 0;
static int capslock = 0; // toggled state, not a live key-down flag
static int extended_pending = 0; // saw 0xE0, next byte belongs to it

static inline uint32_t current_modifiers(void) {
    uint32_t m = 0;
    if (lshift || rshift) m |= INPUT64_MOD_SHIFT;
    if (lctrl  || rctrl)  m |= INPUT64_MOD_CTRL;
    if (lalt   || ralt)   m |= INPUT64_MOD_ALT;
    if (capslock)         m |= INPUT64_MOD_CAPSLOCK;
    return m;
}

// Translates a base (non-extended) make code to ASCII given current
// modifier state, or 0 if this key has no character mapping. Caps Lock
// only inverts case for LETTERS (standard terminal behavior); the
// number/symbol row is governed by Shift alone regardless of Caps Lock.
// Ctrl reduces a letter to its control code (Ctrl+A=1 .. Ctrl+Z=26);
// Ctrl combined with a non-letter yields no ASCII, matching the
// existing 32-bit keyboard.c convention.
static char translate_ascii(uint8_t sc) {
    if (sc >= 128) return 0;
    int shift_state = lshift || rshift;
    char base = base_table[sc];
    int use_shift = is_letter(base) ? (shift_state ^ capslock) : shift_state;
    char c = use_shift ? shift_table[sc] : base;
    if (c == 0) return 0;

    if (lctrl || rctrl) {
        if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 1);
        if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 1);
        return 0;
    }
    return c;
}

// Maps a (possibly extended) make code to this milestone's stable
// logical keycode space -- see include/input64.h.
static inline uint32_t logical_keycode(uint8_t sc, int extended) {
    return extended ? (INPUT64_KEY_EXTENDED | sc) : (uint32_t)sc;
}

static void handle_key(uint8_t sc, int extended, int pressed) {
    // Modifier tracking happens on BOTH press and release, for every
    // scancode regardless of extended state, before anything else.
    if (!extended && sc == 0x2A) { lshift = pressed; }
    else if (!extended && sc == 0x36) { rshift = pressed; }
    else if (!extended && sc == 0x1D) { lctrl = pressed; }
    else if (extended  && sc == 0x1D) { rctrl = pressed; }
    else if (!extended && sc == 0x38) { lalt = pressed; }
    else if (extended  && sc == 0x38) { ralt = pressed; }
    else if (!extended && sc == 0x3A) { if (pressed) capslock = !capslock; }

    char ascii = (pressed && !extended) ? translate_ascii(sc) : 0;

    input64_event_t ev = {0};
    ev.type      = INPUT64_EVENT_KEY;
    ev.a         = (int32_t)logical_keycode(sc, extended);
    ev.pressed   = (uint32_t)pressed;
    ev.modifiers = current_modifiers();
    ev.ascii     = (uint32_t)(unsigned char)ascii;
    input64_push(&ev);

    // Compatibility path: SYS64_GETCH/tox_readline only ever see
    // translated ASCII characters on press, exactly as Milestone 10-28
    // behaved (no key-up events, no raw scancodes).
    if (ascii) keyboard_buffer64_push(ascii);
}

void keyboard64_handler(void)
{
    uint8_t sc = inb(0x60);

    if (sc == 0xE0) { extended_pending = 1; return; }

    int extended = extended_pending;
    extended_pending = 0;

    int pressed = !(sc & 0x80);
    uint8_t code = sc & 0x7F;
    handle_key(code, extended, pressed);
}
