// ToxenOS/user/shell.c
// Thin shell — builtins only, everything else runs from /C:/bin/
#include <stdint.h>

// ── Syscall wrappers ──────────────────────────────────────────────────────────

static void print(const char* s)
    { __asm__ volatile("int $0x80" :: "a"(1), "b"(s)); }
static void set_color(uint8_t c)
    { __asm__ volatile("int $0x80" :: "a"(6), "b"((uint32_t)c)); }
static void erase()
    { __asm__ volatile("int $0x80" :: "a"(5)); }
static void tox_clear()
    { __asm__ volatile("int $0x80" :: "a"(7)); }
static void yield()
    { __asm__ volatile("int $0x80" :: "a"(4)); }
static int sys_stat(const char* p)
    { int r; __asm__ volatile("int $0x80":"=a"(r):"a"(17),"b"(p)); return r; }
static int sys_isdir(const char* p)
    { int r; __asm__ volatile("int $0x80":"=a"(r):"a"(18),"b"(p)); return r; }
static int sys_my_tty()
    { int r; __asm__ volatile("int $0x80":"=a"(r):"a"(20)); return r; }
static int sys_spawn_tty_args(const char* path, int tty, const char* args)
    { int r; __asm__ volatile("int $0x80":"=a"(r):"a"(27),"b"(path),"c"(tty),"d"(args)); return r; }
static int is_alive(int pid)
    { int r; __asm__ volatile("int $0x80":"=a"(r):"a"(28),"b"(pid)); return r; }
static int key_available()
    { int r; __asm__ volatile("int $0x80":"=a"(r):"a"(29)); return r; }
static char getchar()
    { int r; __asm__ volatile("int $0x80":"=a"(r):"a"(2)); return (char)r; }

// Arrow key codes from keyboard.c
#define KEY_UP    0x01
#define KEY_DOWN  0x02
#define KEY_LEFT  0x03
#define KEY_RIGHT 0x04

// ── String helpers ────────────────────────────────────────────────────────────

static int str_len(const char* s) { int i=0; while(s[i]) i++; return i; }
static void str_copy(char* d, const char* s) { int i=0; while(s[i]){d[i]=s[i];i++;} d[i]=0; }
static int str_equal(const char* a, const char* b) {
    int i; for(i=0;a[i]&&b[i];i++) if(a[i]!=b[i]) return 0; return a[i]==b[i];
}

// ── History ───────────────────────────────────────────────────────────────────

#define INPUT_MAX   256
#define HISTORY_MAX 16

static char history[HISTORY_MAX][INPUT_MAX];
static int  history_count = 0;
static int  history_pos   = -1;  // -1 = not browsing

static void history_push(const char* cmd)
{
    if (!cmd[0]) return;
    // don't store duplicate of last entry
    if (history_count > 0) {
        int last = (history_count - 1) % HISTORY_MAX;
        if (str_equal(history[last], cmd)) return;
    }
    int slot = history_count % HISTORY_MAX;
    str_copy(history[slot], cmd);
    history_count++;
}

// offset 0 = newest, 1 = one older, etc.
static const char* history_get(int offset)
{
    if (offset < 0 || offset >= history_count || offset >= HISTORY_MAX)
        return 0;
    int idx = ((history_count - 1 - offset) % HISTORY_MAX + HISTORY_MAX) % HISTORY_MAX;
    return history[idx];
}

// ── State ─────────────────────────────────────────────────────────────────────

static char cwd[256] = "/C:";

// ── Prompt ───────────────────────────────────────────────────────────────────

static void print_prompt()
{
    // Find drive letter: cwd starts with /X: so drive is cwd[1]
    // Display as [T] \\X:\path\to\dir\>
    set_color(0x0A); print("[T] \\\\");
    // Print drive letter and colon
    char drive[3] = {cwd[1], cwd[2], 0};  // e.g. "C:"
    print(drive);
    // Print rest of path after /X: converting / to backslash
    const char* p = cwd + 3;  // skip "/C:"
    while (*p) {
        if (*p == '/') print("\\");
        else { char buf[2] = {*p, 0}; print(buf); }
        p++;
    }
    set_color(0x0A); print("\\");
    set_color(0x07); print("> ");
}

static void erase_n(int n)
{
    for (int i = 0; i < n; i++) erase();
}

// ── Builtins ─────────────────────────────────────────────────────────────────

static void cmd_cd(const char* args)
{
    if (!args || !args[0]) { print("Usage: cd <dir>\n"); return; }

    char newpath[256];
    if (args[0] == '/') {
        str_copy(newpath, args);
    } else {
        str_copy(newpath, cwd);
        int l = str_len(newpath);
        newpath[l] = '/';
        str_copy(newpath + l + 1, args);
    }

    if (sys_isdir(newpath) != 1) {
        set_color(0x0C); print("cd: not a directory\n"); set_color(0x07);
        return;
    }
    str_copy(cwd, newpath);
}

// ── External command dispatch ────────────────────────────────────────────────

static void run_external(const char* cmd, const char* args)
{
    char path[64];
    str_copy(path, "/C:/bin/");
    int plen = str_len(path);
    str_copy(path + plen, cmd);
    int flen = str_len(path);
    path[flen]   = '.';
    path[flen+1] = 'e';
    path[flen+2] = 'l';
    path[flen+3] = 'f';
    path[flen+4] = 0;

    if (sys_stat(path) < 0) {
        set_color(0x0C); print("Unknown command: "); print(cmd); print("\n");
        set_color(0x07); return;
    }

    char full_args[256];
    if (args && args[0]) {
        if (args[0] != '/') {
            str_copy(full_args, cwd);
            int l = str_len(full_args);
            full_args[l] = '/';
            str_copy(full_args + l + 1, args);
        } else {
            str_copy(full_args, args);
        }
    } else {
        str_copy(full_args, cwd);
    }

    int tty = sys_my_tty();
    if (tty < 0) tty = 0;
    int pid = sys_spawn_tty_args(path, tty, full_args);
    if (pid < 0) {
        set_color(0x0C); print("spawn failed\n"); set_color(0x07);
    } else {
        int loops = 0;
        while (is_alive(pid)) {
            yield();
            if (++loops > 100000) {
                set_color(0x0E); print("timeout\n"); set_color(0x07);
                break;
            }
        }
    }
}

// ── Command router ────────────────────────────────────────────────────────────

static void run_command(char* input)
{
    while (*input == ' ') input++;
    if (!*input) return;

    char* args = input;
    while (*args && *args != ' ') args++;
    if (*args == ' ') { *args = 0; args++; }
    else args = "";

    if      (str_equal(input, "cd"))       cmd_cd(args);
    else if (str_equal(input, "cdb"))      cmd_cd("..");
    else if (str_equal(input, "clear"))    tox_clear();
    else if (str_equal(input, "reboot"))
        __asm__ volatile("movb $0xFE,%%al; outb %%al,$0x64":::"eax");
    else if (str_equal(input, "shutdown"))
        __asm__ volatile("movw $0x2000,%%ax; movw $0x604,%%dx; outw %%ax,%%dx":::"eax","edx");
    else
        run_external(input, args);
}

// ── Entry point ───────────────────────────────────────────────────────────────

void _start()
{
    char input[INPUT_MAX];
    int  len = 0;

    print_prompt();

    while (1)
    {
        if (!key_available()) { yield(); continue; }
        char c = getchar();
        if (c == 0) continue;

        // ── arrow keys (sent as 0x01–0x04 by keyboard.c) ─────────────────────
        if (c == KEY_UP || c == KEY_DOWN)
        {
            int next_pos = history_pos + (c == KEY_UP ? 1 : -1);

            if (next_pos < -1) next_pos = -1;
            if (next_pos >= history_count) next_pos = history_count - 1;
            if (next_pos >= HISTORY_MAX)   next_pos = HISTORY_MAX - 1;

            // erase what's currently on the line
            erase_n(len);
            len = 0;

            if (next_pos == -1) {
                history_pos = -1;  // empty line
            } else {
                const char* entry = history_get(next_pos);
                if (entry) {
                    history_pos = next_pos;
                    str_copy(input, entry);
                    len = str_len(input);
                    print(input);
                }
            }
            continue;
        }

        if (c == KEY_LEFT || c == KEY_RIGHT) continue;  // ignore for now

        // ── normal input ─────────────────────────────────────────────────────
        if (c == '\n')
        {
            print("\n");
            input[len] = 0;
            history_push(input);
            history_pos = -1;
            run_command(input);
            len = 0;
            print_prompt();
        }
        else if (c == 8)  // backspace
        {
            if (len > 0) { len--; erase(); }
        }
        else if (c == '\t')
        {
            continue;
        }
        else if (len < INPUT_MAX - 1)
        {
            input[len++] = c;
            char buf[2] = {c, 0};
            print(buf);
        }
    }
}
