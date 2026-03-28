// ToxenOS/user/shell.c
// Thin shell — builtins only, everything else runs from /disk/bin/
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
static void sys_wait(int pid)
    { __asm__ volatile("int $0x80"::"a"(23),"b"(pid)); }
static int is_alive(int pid)
    { int r; __asm__ volatile("int $0x80":"=a"(r):"a"(28),"b"(pid)); return r; }
static int key_available()
    { int r; __asm__ volatile("int $0x80":"=a"(r):"a"(29)); return r; }
static char getchar()
    { int r; __asm__ volatile("int $0x80":"=a"(r):"a"(2)); return (char)r; }

// ── String helpers ────────────────────────────────────────────────────────────

static int str_len(const char* s) { int i=0; while(s[i]) i++; return i; }
static void str_copy(char* d, const char* s) { int i=0; while(s[i]){d[i]=s[i];i++;} d[i]=0; }
static int str_equal(const char* a, const char* b) {
    int i; for(i=0;a[i]&&b[i];i++) if(a[i]!=b[i]) return 0; return a[i]==b[i];
}

// ── State ─────────────────────────────────────────────────────────────────────

#define INPUT_MAX 256
static char cwd[256] = "/disk";

// ── Prompt ───────────────────────────────────────────────────────────────────

static void print_prompt()
{
    set_color(0x0A); print("[T] \\\\root");
    // print path after /disk, converting / to backslash
    const char* p = cwd + 5; // skip "/disk"
    while (*p) {
        if (*p == '/') print("\\");
        else {
            char buf[2] = {*p, 0};
            print(buf);
        }
        p++;
    }
    set_color(0x0A); print("\\");
    set_color(0x07); print("> ");
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
    // Build path: /disk/bin/<cmd>
    char path[64];
    str_copy(path, "/disk/bin/");
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

    // Build args: for path commands, prepend cwd
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
        // No args — pass cwd so programs like ls and pcd know where they are
        str_copy(full_args, cwd);
    }

    int tty = sys_my_tty();
    if (tty < 0) tty = 0;
    int pid = sys_spawn_tty_args(path, tty, full_args);
    if (pid < 0) {
        set_color(0x0C); print("spawn failed\n"); set_color(0x07);
    } else {
        // Poll from userspace until process dies
        int loops = 0;
        while (is_alive(pid)) {
            yield();
            loops++;
            if (loops > 100000) {
                set_color(0x0E); print("timeout\n"); set_color(0x07);
                break;
            }
        }
    }
}

// ── Command router ────────────────────────────────────────────────────────────

static void run_command(char* input)
{
    // Skip leading spaces
    while (*input == ' ') input++;
    if (!*input) return;

    // Split into cmd and args
    char* args = input;
    while (*args && *args != ' ') args++;
    if (*args == ' ') { *args = 0; args++; }
    else args = "";

    // Builtins
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

        if (c == '\n')
        {
            print("\n");
            input[len] = 0;
            run_command(input);
            len = 0;
            print_prompt();
        }
        else if (c == 8)  // backspace
        {
            if (len > 0) { len--; erase(); }
        }
        else if (c == '\t') // tab — ignore for now
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
