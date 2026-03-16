#include <stdint.h>

void print(const char* msg)
{
    __asm__ volatile("int $0x80" :: "a"(1), "b"(msg));
}

char getchar()
{
    int c;
    __asm__ volatile("int $0x80" : "=a"(c) : "a"(2));
    return (char)c;
}

void exit()
{
    __asm__ volatile("int $0x80" :: "a"(0));
}

void erase()
{
    __asm__ volatile("int $0x80" :: "a"(5));
}

void set_color(uint8_t color)
{
    __asm__ volatile("int $0x80" :: "a"(6), "b"((uint32_t)color));
}

int sys_open(const char* path, int flags)
{
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(12), "b"(path), "c"(flags));
    return ret;
}

int sys_read(int fd, uint8_t* buf, uint32_t size)
{
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(13), "b"(fd), "c"(buf), "d"(size));
    return ret;
}

int sys_write(int fd, const uint8_t* buf, uint32_t size)
{
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(14), "b"(fd), "c"(buf), "d"(size));
    return ret;
}

int sys_close(int fd)
{
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(15), "b"(fd));
    return ret;
}

int sys_stat(const char* path)
{
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(17), "b"(path));
    return ret;
}

int sys_readdir(const char* path, char* out, uint32_t index)
{
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(10), "b"(path), "c"(out), "d"(index));
    return ret;
}

int sys_mkdir(const char* path)
{
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(11), "b"(path));
    return ret;
}

int sys_remove(const char* path)
{
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(16), "b"(path));
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

static char cwd[256] = "/disk";

static void cmd_cd(const char* args)
{
    if (!args || args[0] == 0)
    {
        print("Usage: cd <dir>\n");
        return;
    }

    char new_path[256];
    if (args[0] == '/')
    {
        str_copy(new_path, args);
    }
    else
    {
        str_copy(new_path, cwd);
        int len = str_len(new_path);
        new_path[len] = '/';
        str_copy(new_path + len + 1, args);
    }

    if (sys_stat(new_path) < 0)
    {
        set_color(0x0C);
        print("cd: directory not found\n");
        set_color(0x07);
        return;
    }

    str_copy(cwd, new_path);
}

static void cmd_mkd(const char* args)
{
    print("cwd=");
    print(cwd);
    print("\n");
    print("args=");
    print(args);
    print("\n");

    if (!args || args[0] == 0)
    {
        print("Usage: mkd <dir>\n");
        return;
    }

    char path[256];
    str_copy(path, cwd);
    int len = str_len(path);
    path[len] = '/';
    str_copy(path + len + 1, args);

    if (sys_mkdir(path) < 0)
    {
        set_color(0x0C);
        print("mkd: failed to create directory\n");
        set_color(0x07);
        return;
    }

    set_color(0x0A);
    print("created: ");
    print(args);
    print("\n");
    set_color(0x07);
}

static void cmd_rm(const char* args)
{
    if (!args || args[0] == 0)
    {
        print("Usage: rm <file>\n");
        return;
    }

    char path[256];
    str_copy(path, cwd);
    int len = str_len(path);
    path[len] = '/';
    str_copy(path + len + 1, args);

    if (sys_remove(path) < 0)
    {
        set_color(0x0C);
        print("rm: failed\n");
        set_color(0x07);
        return;
    }

    set_color(0x0A);
    print("removed: ");
    print(args);
    print("\n");
    set_color(0x07);
}

static void cmd_cp(const char* args)
{
    if (!args || args[0] == 0)
    {
        print("Usage: cp <src> <dst>\n");
        return;
    }

    // split args into src and dst
    char src[256], dst[256];
    int i = 0;
    while (args[i] && args[i] != ' ') { src[i] = args[i]; i++; }
    src[i] = 0;
    if (args[i] == ' ') i++;
    int j = 0;
    while (args[i]) { dst[j++] = args[i++]; }
    dst[j] = 0;

    if (src[0] == 0 || dst[0] == 0)
    {
        print("Usage: cp <src> <dst>\n");
        return;
    }

    // build full paths
    char src_path[256], dst_path[256];
    str_copy(src_path, cwd);
    int len = str_len(src_path);
    src_path[len] = '/';
    str_copy(src_path + len + 1, src);

    str_copy(dst_path, cwd);
    len = str_len(dst_path);
    dst_path[len] = '/';
    str_copy(dst_path + len + 1, dst);

    int src_fd = sys_open(src_path, 1);  // O_READ
    if (src_fd < 0)
    {
        set_color(0x0C);
        print("cp: source not found\n");
        set_color(0x07);
        return;
    }

    int dst_fd = sys_open(dst_path, 2 | 4);  // O_WRITE | O_CREATE
    if (dst_fd < 0)
    {
        sys_close(src_fd);
        set_color(0x0C);
        print("cp: failed to create destination\n");
        set_color(0x07);
        return;
    }

    uint8_t buf[256];
    int bytes;
    while ((bytes = sys_read(src_fd, buf, 256)) > 0)
        sys_write(dst_fd, buf, bytes);

    sys_close(src_fd);
    sys_close(dst_fd);

    set_color(0x0A);
    print("copied: ");
    print(src);
    print(" -> ");
    print(dst);
    print("\n");
    set_color(0x07);
}

static void cmd_mv(const char* args)
{
    if (!args || args[0] == 0)
    {
        print("Usage: mv <src> <dst>\n");
        return;
    }

    // mv = cp + rm
    cmd_cp(args);

    // extract src from args to remove it
    char src[256];
    int i = 0;
    while (args[i] && args[i] != ' ') { src[i] = args[i]; i++; }
    src[i] = 0;

    char path[256];
    str_copy(path, cwd);
    int len = str_len(path);
    path[len] = '/';
    str_copy(path + len + 1, src);

    sys_remove(path);
}

static void cmd_echo(const char* args)
{
    print(args);
    print("\n");
}

static void cmd_clear()
{
    __asm__ volatile("int $0x80" :: "a"(7));
}

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
    __asm__ volatile("int $0x80" :: "a"(8));
}

static void cmd_shutdown()
{
    __asm__ volatile("int $0x80" :: "a"(9));
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
    else if (str_equal(buf, "cd"))   cmd_cd(args);
    else if (str_equal(buf, "pcd"))  cmd_pcd();
    else if (str_equal(buf, "shw"))  cmd_shw(args);
    else if (str_equal(buf, "mkd"))  cmd_mkd(args);
    else if (str_equal(buf, "mkef")) cmd_mkef(args);
    else if (str_equal(buf, "rm"))   cmd_rm(args);
    else if (str_equal(buf, "cp"))   cmd_cp(args);
    else if (str_equal(buf, "mv"))   cmd_mv(args);
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