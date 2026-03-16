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

int sys_isdir(const char* path)
{
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(18), "b"(path));
    return ret;
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

    if (str_equal(args, "..") || str_equal(args, "back"))
    {
        // go up one level
        str_copy(new_path, cwd);
        int len = str_len(new_path);

        // find last slash
        int last = 0;
        for (int i = 0; i < len; i++)
            if (new_path[i] == '/') last = i;

        if (last == 0)
            str_copy(new_path, "/disk");  // can't go above /disk
        else
        {
            new_path[last] = 0;
            // don't go above /disk
            if (str_len(new_path) < str_len("/disk"))
                str_copy(new_path, "/disk");
        }
    }
    else if (args[0] == '/')
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

    char src_path[256], dst_path[256];
    str_copy(src_path, cwd);
    int len = str_len(src_path);
    src_path[len] = '/';
    str_copy(src_path + len + 1, src);

    // check if dst has extension
    int dst_has_ext = 0;
    for (int k = 0; dst[k]; k++)
        if (dst[k] == '.') { dst_has_ext = 1; break; }

    if (!dst_has_ext)
    {
        // dst is a directory — copy file into it keeping same name
        str_copy(dst_path, cwd);
        len = str_len(dst_path);
        dst_path[len] = '/';
        str_copy(dst_path + len + 1, dst);
        len = str_len(dst_path);
        dst_path[len] = '/';
        str_copy(dst_path + len + 1, src);
    }
    else
    {
        // dst is a filename — copy with new name
        str_copy(dst_path, cwd);
        len = str_len(dst_path);
        dst_path[len] = '/';
        str_copy(dst_path + len + 1, dst);
    }

    int src_fd = sys_open(src_path, 1);
    if (src_fd < 0)
    {
        set_color(0x0C);
        print("cp: source not found\n");
        set_color(0x07);
        return;
    }

    int dst_fd = sys_open(dst_path, 2 | 4);
    if (dst_fd < 0)
    {
        sys_close(src_fd);
        set_color(0x0C);
        print("cp: failed\n");
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

static void cmd_move(const char* args)
{
    if (!args || args[0] == 0)
    {
        print("Usage: move <file> <dir>\n");
        return;
    }

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
        print("Usage: move <file> <dir>\n");
        return;
    }

    char src_path[256], dst_path[256];
    str_copy(src_path, cwd);
    int len = str_len(src_path);
    src_path[len] = '/';
    str_copy(src_path + len + 1, src);

    // dst is always a directory — append src filename
    str_copy(dst_path, cwd);
    len = str_len(dst_path);
    dst_path[len] = '/';
    str_copy(dst_path + len + 1, dst);
    len = str_len(dst_path);
    dst_path[len] = '/';
    str_copy(dst_path + len + 1, src);

    int src_fd = sys_open(src_path, 1);
    if (src_fd < 0)
    {
        set_color(0x0C);
        print("move: source not found\n");
        set_color(0x07);
        return;
    }

    int dst_fd = sys_open(dst_path, 2 | 4);
    if (dst_fd < 0)
    {
        sys_close(src_fd);
        set_color(0x0C);
        print("move: failed\n");
        set_color(0x07);
        return;
    }

    uint8_t buf[256];
    int bytes;
    while ((bytes = sys_read(src_fd, buf, 256)) > 0)
        sys_write(dst_fd, buf, bytes);

    sys_close(src_fd);
    sys_close(dst_fd);
    sys_remove(src_path);

    set_color(0x0A);
    print("moved: ");
    print(src);
    print(" -> ");
    print(dst);
    print("\n");
    set_color(0x07);
}

static void cmd_rname(const char* args)
{
    if (!args || args[0] == 0)
    {
        print("Usage: rname <oldname> <newname>\n");
        return;
    }

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
        print("Usage: rname <oldname> <newname>\n");
        return;
    }

    char src_path[256], dst_path[256];
    str_copy(src_path, cwd);
    int len = str_len(src_path);
    src_path[len] = '/';
    str_copy(src_path + len + 1, src);

    str_copy(dst_path, cwd);
    len = str_len(dst_path);
    dst_path[len] = '/';
    str_copy(dst_path + len + 1, dst);

    int src_fd = sys_open(src_path, 1);
    if (src_fd < 0)
    {
        set_color(0x0C);
        print("rname: source not found\n");
        set_color(0x07);
        return;
    }

    int dst_fd = sys_open(dst_path, 2 | 4);
    if (dst_fd < 0)
    {
        sys_close(src_fd);
        set_color(0x0C);
        print("rname: failed\n");
        set_color(0x07);
        return;
    }

    uint8_t buf[256];
    int bytes;
    while ((bytes = sys_read(src_fd, buf, 256)) > 0)
        sys_write(dst_fd, buf, bytes);

    sys_close(src_fd);
    sys_close(dst_fd);
    sys_remove(src_path);

    set_color(0x0A);
    print("renamed: ");
    print(src);
    print(" -> ");
    print(dst);
    print("\n");
    set_color(0x07);
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

#define TEDIT_LINES 100
#define TEDIT_COLS  78

static char tedit_lines[TEDIT_LINES][TEDIT_COLS];
static int  tedit_line_len[TEDIT_LINES];
static int  tedit_num_lines = 0;
static int  tedit_cur_line  = 0;
static int  tedit_cur_col   = 0;

static void tedit_redraw(const char* filename)
{
    __asm__ volatile("int $0x80" :: "a"(7));  // clear screen

    // header
    set_color(0x0B);
    print("tedit: ");
    print(filename);
    print("  |  Ctrl+S = Save  Ctrl+Q = Quit\n");
    set_color(0x08);
    print("--------------------------------------------------------------------------------\n");
    set_color(0x07);

    // print all lines
    for (int i = 0; i < tedit_num_lines; i++)
    {
        tedit_lines[i][tedit_line_len[i]] = 0;
        print(tedit_lines[i]);
        print("\n");
    }
}

static void tedit_move_cursor(const char* filename)
{
    // redraw and reposition cursor by reprinting up to cursor pos
    tedit_redraw(filename);

    // reprint lines up to cursor line
    // cursor is already at correct line after redraw
    // we need to move cursor to correct column on correct line
    // since we can't directly set cursor, we redraw and print partial
    __asm__ volatile("int $0x80" :: "a"(7));

    set_color(0x0B);
    print("tedit: ");
    print(filename);
    print("  |  Ctrl+S = Save  Ctrl+Q = Quit\n");
    set_color(0x08);
    print("--------------------------------------------------------------------------------\n");
    set_color(0x07);

    for (int i = 0; i < tedit_cur_line; i++)
    {
        tedit_lines[i][tedit_line_len[i]] = 0;
        print(tedit_lines[i]);
        print("\n");
    }

    // print current line up to cursor col
    for (int i = 0; i < tedit_cur_col; i++)
    {
        char tmp[2] = {tedit_lines[tedit_cur_line][i], 0};
        print(tmp);
    }
}

static void cmd_tedit(const char* args)
{
    if (!args || args[0] == 0)
    {
        print("Usage: tedit <file>\n");
        return;
    }

    int has_ext = 0;
    for (int k = 0; args[k]; k++)
        if (args[k] == '.') { has_ext = 1; break; }

    if (!has_ext)
    {
        set_color(0x0C);
        print("tedit: filename must have an extension\n");
        set_color(0x07);
        return;
    }

    char path[256];
    str_copy(path, cwd);
    int len = str_len(path);
    path[len] = '/';
    str_copy(path + len + 1, args);

    // init lines
    tedit_num_lines = 1;
    tedit_cur_line  = 0;
    tedit_cur_col   = 0;
    for (int i = 0; i < TEDIT_LINES; i++)
    {
        tedit_line_len[i] = 0;
        tedit_lines[i][0] = 0;
    }

    // load existing file
    int fd = sys_open(path, 1);
    if (fd >= 0)
    {
        static uint8_t raw[4096];
        int raw_len = sys_read(fd, raw, 4095);
        sys_close(fd);
        if (raw_len < 0) raw_len = 0;

        int line = 0;
        int col  = 0;
        for (int i = 0; i < raw_len && line < TEDIT_LINES; i++)
        {
            if (raw[i] == '\n')
            {
                tedit_line_len[line] = col;
                line++;
                col = 0;
                if (line >= TEDIT_LINES) break;
            }
            else if (col < TEDIT_COLS - 1)
            {
                tedit_lines[line][col++] = raw[i];
            }
        }
        tedit_line_len[line] = col;
        tedit_num_lines = line + 1;
    }

    tedit_move_cursor(args);

    while (1)
    {
        char c = getchar();
        if (c == 0) continue;

        if (c == 17)  // Ctrl+Q
        {
            __asm__ volatile("int $0x80" :: "a"(7));
            return;
        }
        else if (c == 19)  // Ctrl+S
        {
            int wfd = sys_open(path, 2 | 4);
            if (wfd >= 0)
            {
                for (int i = 0; i < tedit_num_lines; i++)
                {
                    sys_write(wfd, (const uint8_t*)tedit_lines[i], tedit_line_len[i]);
                    if (i < tedit_num_lines - 1)
                        sys_write(wfd, (const uint8_t*)"\n", 1);
                }
                sys_close(wfd);
            }
            set_color(0x0A);
            print("\n[saved]");
            set_color(0x07);
        }
        else if (c == 0x01)  // up arrow
        {
            if (tedit_cur_line > 0)
            {
                tedit_cur_line--;
                if (tedit_cur_col > tedit_line_len[tedit_cur_line])
                    tedit_cur_col = tedit_line_len[tedit_cur_line];
                tedit_move_cursor(args);
            }
        }
        else if (c == 0x02)  // down arrow
        {
            if (tedit_cur_line < tedit_num_lines - 1)
            {
                tedit_cur_line++;
                if (tedit_cur_col > tedit_line_len[tedit_cur_line])
                    tedit_cur_col = tedit_line_len[tedit_cur_line];
                tedit_move_cursor(args);
            }
        }
        else if (c == 0x03)  // left arrow (we'll add scancode next)
        {
            if (tedit_cur_col > 0)
            {
                tedit_cur_col--;
                tedit_move_cursor(args);
            }
        }
        else if (c == 0x04)  // right arrow
        {
            if (tedit_cur_col < tedit_line_len[tedit_cur_line])
            {
                tedit_cur_col++;
                tedit_move_cursor(args);
            }
        }
        else if (c == '\n')
        {
            if (tedit_num_lines >= TEDIT_LINES) continue;

            // split current line at cursor
            // shift lines down
            for (int i = tedit_num_lines; i > tedit_cur_line + 1; i--)
            {
                str_copy(tedit_lines[i], tedit_lines[i - 1]);
                tedit_line_len[i] = tedit_line_len[i - 1];
            }

            // new line gets rest of current line
            int new_line = tedit_cur_line + 1;
            int rest_len = tedit_line_len[tedit_cur_line] - tedit_cur_col;
            for (int i = 0; i < rest_len; i++)
                tedit_lines[new_line][i] = tedit_lines[tedit_cur_line][tedit_cur_col + i];
            tedit_line_len[new_line] = rest_len;

            // truncate current line
            tedit_line_len[tedit_cur_line] = tedit_cur_col;

            tedit_num_lines++;
            tedit_cur_line++;
            tedit_cur_col = 0;
            tedit_move_cursor(args);
        }
        else if (c == 8)  // backspace
        {
            if (tedit_cur_col > 0)
            {
                // remove char before cursor
                int l = tedit_cur_line;
                for (int i = tedit_cur_col - 1; i < tedit_line_len[l] - 1; i++)
                    tedit_lines[l][i] = tedit_lines[l][i + 1];
                tedit_line_len[l]--;
                tedit_cur_col--;
                tedit_move_cursor(args);
            }
            else if (tedit_cur_line > 0)
            {
                // merge with previous line
                int prev = tedit_cur_line - 1;
                int prev_len = tedit_line_len[prev];
                int cur_len  = tedit_line_len[tedit_cur_line];

                if (prev_len + cur_len < TEDIT_COLS - 1)
                {
                    for (int i = 0; i < cur_len; i++)
                        tedit_lines[prev][prev_len + i] = tedit_lines[tedit_cur_line][i];
                    tedit_line_len[prev] = prev_len + cur_len;

                    // shift lines up
                    for (int i = tedit_cur_line; i < tedit_num_lines - 1; i++)
                    {
                        str_copy(tedit_lines[i], tedit_lines[i + 1]);
                        tedit_line_len[i] = tedit_line_len[i + 1];
                    }
                    tedit_num_lines--;
                    tedit_cur_line--;
                    tedit_cur_col = prev_len;
                    tedit_move_cursor(args);
                }
            }
        }
        else if (c >= 32)
        {
            int l = tedit_cur_line;
            if (tedit_line_len[l] < TEDIT_COLS - 1)
            {
                // insert char at cursor
                for (int i = tedit_line_len[l]; i > tedit_cur_col; i--)
                    tedit_lines[l][i] = tedit_lines[l][i - 1];
                tedit_lines[l][tedit_cur_col] = c;
                tedit_line_len[l]++;
                tedit_cur_col++;
                tedit_move_cursor(args);
            }
        }
    }
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
        // build full path to check type
        char full[256];
        str_copy(full, cwd);
        int len = str_len(full);
        full[len] = '/';
        str_copy(full + len + 1, entry);

        if (sys_isdir(full) == 1)
        {
            set_color(0x09);  // blue for directories
            print(entry);
            print("/\n");
        }
        else
        {
            set_color(0x06);  // orange for files
            print(entry);
            print("\n");
        }
        set_color(0x07);

        i++;
        found = 1;
    }

    if (!found)
    {
        set_color(0x07);
        print("(empty)\n");
    }
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

    // check for extension
    int has_ext = 0;
    for (int i = 0; args[i]; i++)
        if (args[i] == '.') { has_ext = 1; break; }

    if (!has_ext)
    {
        set_color(0x0C);
        print("mkef: filename must have an extension (e.g. file.txt)\n");
        set_color(0x07);
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
    print("  cdb         - go back one directory\n");
    print("  mv <f> <d> - move file to directory\n");
    print("  rname <a> <b>- rename file\n");
    print("  echo <text> - print text\n");
    print("  clear       - clear screen\n");
    print("  uname       - OS info\n");
    print("  reboot      - restart\n");
    print("  tedit <file>- open text editor\n");
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
    else if (str_equal(buf, "cdb"))  cmd_cd("..");
    else if (str_equal(buf, "pcd"))  cmd_pcd();
    else if (str_equal(buf, "shw"))  cmd_shw(args);
    else if (str_equal(buf, "mkd"))  cmd_mkd(args);
    else if (str_equal(buf, "mkef")) cmd_mkef(args);
    else if (str_equal(buf, "rm"))   cmd_rm(args);
    else if (str_equal(buf, "cp"))   cmd_cp(args);
    else if (str_equal(buf, "mv"))  cmd_move(args);
    else if (str_equal(buf, "rname")) cmd_rname(args);
    else if (str_equal(buf, "tedit")) cmd_tedit(args);
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
#define HISTORY_MAX 16
static char history[HISTORY_MAX][INPUT_MAX];
static int  history_count = 0;
static int  history_idx   = -1;

static void history_add(const char* cmd)
{
    if (cmd[0] == 0) return;
    // shift history up
    if (history_count < HISTORY_MAX)
        history_count++;
    else
    {
        for (int i = 0; i < HISTORY_MAX - 1; i++)
            str_copy(history[i], history[i + 1]);
    }
    str_copy(history[history_count - 1], cmd);
}


void _start()
{
    char input[INPUT_MAX];
    int  input_len = 0;

    print_prompt();

    while (1)
    {
        char c = getchar();
        if (c == 0) continue;

        if (c == '\n')
        {
            print("\n");
            input[input_len] = 0;
            history_add(input);
            history_idx = -1;
            run_command(input);
            input_len = 0;
            print_prompt();
        }
        else if (c == 8)  // backspace
        {
            if (input_len > 0) { input_len--; erase(); }
        }
        else if (c == 0x01)  // up arrow
        {
            if (history_count == 0) continue;

            // move back in history
            if (history_idx == -1)
                history_idx = history_count - 1;
            else if (history_idx > 0)
                history_idx--;

            // clear current input
            while (input_len > 0) { input_len--; erase(); }

            // print history entry
            str_copy(input, history[history_idx]);
            input_len = str_len(input);
            print(input);
        }
        else if (c == 0x02)  // down arrow
        {
            if (history_idx == -1) continue;

            // clear current input
            while (input_len > 0) { input_len--; erase(); }

            if (history_idx < history_count - 1)
            {
                history_idx++;
                str_copy(input, history[history_idx]);
                input_len = str_len(input);
                print(input);
            }
            else
            {
                history_idx = -1;
                input[0] = 0;
                input_len = 0;
            }
        }
        else if (c >= 32 && input_len < INPUT_MAX - 1)
        {
            input[input_len++] = c;
            char buf[2] = {c, 0};
            print(buf);
        }
    }
}