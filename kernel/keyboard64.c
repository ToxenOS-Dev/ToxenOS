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
//
// M-next follow-up: that "AND" was unconditional -- every keypress fed
// keyboard_buffer64 regardless of who, if anyone, currently owns
// input64/display64. shell64 keeps running as a live sibling process in
// graphical mode (user64/init64.c) and reads keyboard_buffer64 via
// sys_getch()/tox_readline(), so typing while the compositor owned the
// screen was ALSO being interpreted as shell commands the whole time --
// invisible before M-next only because the old compositor's constant
// full-screen redraws painted over whatever shell64 printed within one
// frame (see kernel/console64.c's own comment for the other half of
// this bug). Fixed by feeding keyboard_buffer64 only while no graphical
// owner holds display64 -- input64_push() below is untouched and still
// unconditional, so a focused graphical client keeps receiving every
// keystroke exactly as before.
#include <stdint.h>
#include "../include/keyboard_buffer64.h"
#include "../include/input64.h"
#include "../include/display64.h"
#include "../include/ps2_64.h"
#include "../include/irq64.h"

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
    // behaved (no key-up events, no raw scancodes). M-next: only while
    // no graphical owner holds display64 -- see this file's own header
    // comment.
    if (ascii && display64_owner_pid() == 0) keyboard_buffer64_push(ascii);
}

// M+7A: registers with the generic input64 device model (diagnostic/
// enumeration only -- see include/input64.h's own header comment).
// keyboard64.c never had its own init() before this -- kernel/kernel64.c
// just registered the IRQ1 handler directly -- so this is a small,
// purely additive entry point, not a functional change to key handling.
void keyboard64_init(void) {
    irq64_request_legacy(1, keyboard64_irq, 0, "keyboard");

    static input64_device_t dev;
    dev.name[0]='p'; dev.name[1]='s'; dev.name[2]='2'; dev.name[3]='k';
    dev.name[4]='b'; dev.name[5]='d'; dev.name[6]=0;
    dev.kind = INPUT64_DEVICE_PS2_KEYBOARD;
    dev.capabilities = INPUT64_CAP_KEY;
    input64_register_device(&dev);
}

// M+11A: byte-level entry, fed by the shared 8042 receive path
// (ps2_64_rx_poll) for every keyboard-port byte regardless of which IRQ
// line fired. The parsing below is unchanged from the old IRQ1 handler.
void keyboard64_feed_byte(uint8_t sc)
{
    if (sc == 0xE0) { extended_pending = 1; return; }

    int extended = extended_pending;
    extended_pending = 0;

    int pressed = !(sc & 0x80);
    uint8_t code = sc & 0x7F;
    handle_key(code, extended, pressed);
}

// M+11A: thin irq64 thunk for IRQ1.
irq64_ret_t keyboard64_irq(void* ctx)
{
    (void)ctx;
    return ps2_64_service() ? IRQ64_RET_HANDLED : IRQ64_RET_NONE;
}
