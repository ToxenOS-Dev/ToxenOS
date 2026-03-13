#include <stdint.h>

void print(const char* msg)
{
    __asm__ volatile(
        "movl $1, %%eax\n"
        "movl %0, %%ebx\n"
        "int $0x80\n"
        :
        : "r"(msg)
        : "eax", "ebx"
    );
}

char getchar()
{
    int c;
    __asm__ volatile(
        "movl $2, %%eax\n"
        "int $0x80\n"
        "movl %%eax, %0\n"
        : "=r"(c)
        :
        : "eax"
    );
    return (char)c;
}

void exit()
{
    __asm__ volatile("movl $0, %%eax\n" "int $0x80\n" ::: "eax");
}

void yield()
{
    __asm__ volatile("int $0x80" :: "a"(4));
}

void erase()
{
    __asm__ volatile("movl $5, %%eax\n" "int $0x80\n" ::: "eax");
}

void set_color(uint8_t color)
{
    __asm__ volatile(
        "movl $6, %%eax\n"
        "movl %0, %%ebx\n"
        "int $0x80\n"
        :
        : "r"((uint32_t)color)
        : "eax", "ebx"
    );
}

static int str_equal(const char* a, const char* b)
{
    int i;
    for (i = 0; a[i] && b[i]; i++)
        if (a[i] != b[i]) return 0;
    return a[i] == b[i];
}

static int str_len(const char* s)
{
    int i = 0;
    while (s[i]) i++;
    return i;
}

static void str_copy(char* dst, const char* src)
{
    int i = 0;
    while (src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

// check if string starts with prefix
static int str_starts(const char* str, const char* prefix)
{
    int i;
    for (i = 0; prefix[i]; i++)
        if (str[i] != prefix[i]) return 0;
    return 1;
}

static void cmd_echo(const char* args)
{
    print(args);
    print("\n");
}

static void cmd_clear()
{
    // syscall to clear screen
    __asm__ volatile(
        "movl $7, %%eax\n"
        "int $0x80\n"
        ::: "eax"
    );
}

static void cmd_help()
{
    set_color(0x0B);  // cyan
    print("ToxenOS Commands:\n");
    set_color(0x07);
    print("  ls          - list files\n");
    print("  cd <dir>    - change directory\n");
    print("  pcd         - print current directory\n");
    print("  shw <file>  - show file contents\n");
    print("  mkd <dir>   - make directory\n");
    print("  mkef <file> - make empty file\n");
    print("  rm <file>   - delete file\n");
    print("  cp <a> <b>  - copy file\n");
    print("  mv <a> <b>  - move/rename file\n");
    print("  echo <text> - print text\n");
    print("  clear       - clear screen\n");
    print("  uname       - OS info\n");
    print("  reboot      - restart\n");
    print("  shutdown    - power off\n");
}

static void cmd_uname()
{
    set_color(0x0A);  // green
    print("ToxenOS v0.1 - x86 32bit\n");
    set_color(0x07);
}

static void cmd_reboot()
{
    // write to keyboard controller to reboot
    __asm__ volatile(
        "movl $8, %%eax\n"
        "int $0x80\n"
        ::: "eax"
    );
}

static void cmd_shutdown()
{
    __asm__ volatile(
        "movl $9, %%eax\n"
        "int $0x80\n"
        ::: "eax"
    );
}

static void cmd_unknown(const char* cmd)
{
    set_color(0x0C);  // red
    print("Unknown command: ");
    print(cmd);
    print("\n");
    set_color(0x07);
}

static void run_command(char* buf)
{
    // skip leading spaces
    while (*buf == ' ') buf++;

    // empty command
    if (*buf == 0) return;

    // find args (everything after first space)
    char* args = buf;
    while (*args && *args != ' ') args++;
    if (*args == ' ')
    {
        *args = 0;  // null terminate command
        args++;     // args points to rest
    }

    // dispatch
    if      (str_equal(buf, "echo"))     cmd_echo(args);
    else if (str_equal(buf, "clear"))    cmd_clear();
    else if (str_equal(buf, "help"))     cmd_help();
    else if (str_equal(buf, "uname"))    cmd_uname();
    else if (str_equal(buf, "reboot"))   cmd_reboot();
    else if (str_equal(buf, "shutdown")) cmd_shutdown();
    else if (str_equal(buf, "ls"))       print("ls: not yet implemented\n");
    else if (str_equal(buf, "cd"))       print("cd: not yet implemented\n");
    else if (str_equal(buf, "pcd"))      print("pcd: not yet implemented\n");
    else if (str_equal(buf, "shw"))      print("shw: not yet implemented\n");
    else if (str_equal(buf, "mkd"))      print("mkd: not yet implemented\n");
    else if (str_equal(buf, "mkef"))     print("mkef: not yet implemented\n");
    else if (str_equal(buf, "rm"))       print("rm: not yet implemented\n");
    else if (str_equal(buf, "cp"))       print("cp: not yet implemented\n");
    else if (str_equal(buf, "mv"))       print("mv: not yet implemented\n");
    else                                 cmd_unknown(buf);
}

static void print_prompt()
{
    set_color(0x06);  // brown
    print("Tox");
    set_color(0x07);
    print("> ");
}

#define INPUT_MAX 256

void _start()
{
    char input[INPUT_MAX];
    int  input_len = 0;

    print_prompt();

    while (1)
    {
        char c = getchar();
        if (c == 0) {continue; }

        if (c == '\n')
        {
            print("\n");
            input[input_len] = 0;
            run_command(input);
            input_len = 0;
            print_prompt();
        }
        else if (c == 8)  // backspace
        {
            if (input_len > 0)
            {
                input_len--;
                erase();
            }
        }
        else if (input_len < INPUT_MAX - 1)
        {
            input[input_len++] = c;
            char buf[2] = {c, 0};
            print(buf);
        }
    }
}