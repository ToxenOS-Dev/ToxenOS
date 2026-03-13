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

int sys_open(const char* path, int flags)
{
    int ret;
    __asm__ volatile(
        "movl $12, %%eax\n"
        "movl %1, %%ebx\n"
        "movl %2, %%ecx\n"
        "int $0x80\n"
        "movl %%eax, %0\n"
        : "=r"(ret)
        : "r"(path), "r"(flags)
        : "eax", "ebx", "ecx"
    );
    return ret;
}

int sys_read(int fd, uint8_t* buf, uint32_t size)
{
    int ret;
    register int _fd   __asm__("ebx") = fd;
    register void* _buf __asm__("ecx") = buf;
    register uint32_t _size __asm__("edx") = size;
    __asm__ volatile(
        "movl $13, %%eax\n"
        "int $0x80\n"
        : "=a"(ret)
        : "r"(_fd), "r"(_buf), "r"(_size)
    );
    return ret;
}

int sys_close(int fd)
{
    int ret;
    __asm__ volatile(
        "movl $15, %%eax\n"
        "movl %1, %%ebx\n"
        "int $0x80\n"
        "movl %%eax, %0\n"
        : "=r"(ret)
        : "r"(fd)
        : "eax", "ebx"
    );
    return ret;
}

int sys_readdir(const char* path, char* out, uint32_t index)
{
    int ret;
    register const char* _path __asm__("ebx") = path;
    register char* _out        __asm__("ecx") = out;
    register uint32_t _index   __asm__("edx") = index;
    __asm__ volatile(
        "movl $10, %%eax\n"
        "int $0x80\n"
        : "=a"(ret)
        : "r"(_path), "r"(_out), "r"(_index)
    );
    return ret;
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

static char cwd[256] = "/disk";  // current working directory

static void cmd_pcd()
{
    print(cwd);
    print("\n");
}

static void cmd_ls()
{
    char entry[256];
    uint32_t i = 0;
    int found = 0;

    while (sys_readdir(cwd, entry, i) == 0)
    {
        print(entry);
        print("\n");
        i++;
        found = 1;
    }

    if (!found)
        print("(empty)\n");
}

static void cmd_shw(const char* args)
{
    if (!args || args[0] == 0)
    {
        print("Usage: shw <file>\n");
        return;
    }

    // build full path
    char path[256];
    str_copy(path, cwd);
    int len = str_len(path);
    path[len] = '/';
    str_copy(path + len + 1, args);

    int fd = sys_open(path, 1);  // VFS_O_READ = 1
    if (fd < 0)
    {
        set_color(0x0C);
        print("shw: file not found\n");
        set_color(0x07);
        return;
    }

    uint8_t buf[256];
    int bytes;
    while ((bytes = sys_read(fd, buf, 255)) > 0)
    {
        buf[bytes] = 0;
        print((char*)buf);
    }
    sys_close(fd);
    print("\n");
}

static void cmd_mkef(const char* args)
{
    if (!args || args[0] == 0)
    {
        print("Usage: mkef <file>\n");
        return;
    }

    char path[256];
    str_copy(path, cwd);
    int len = str_len(path);
    path[len] = '/';
    str_copy(path + len + 1, args);

    int fd = sys_open(path, 1 | 4);  // VFS_O_READ | VFS_O_CREATE
    if (fd < 0)
    {
        set_color(0x0C);
        print("mkef: failed to create file\n");
        set_color(0x07);
        return;
    }
    sys_close(fd);
    set_color(0x0A);
    print("created: ");
    print(args);
    print("\n");
    set_color(0x07);
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
    else if (str_equal(buf, "ls"))   cmd_ls();
    else if (str_equal(buf, "cd"))       print("cd: not yet implemented\n");
    else if (str_equal(buf, "pcd"))  cmd_pcd();
    else if (str_equal(buf, "shw"))  cmd_shw(args);
    else if (str_equal(buf, "mkd"))      print("mkd: not yet implemented\n");
    else if (str_equal(buf, "mkef")) cmd_mkef(args);
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