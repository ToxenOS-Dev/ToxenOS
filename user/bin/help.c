// user/bin/help.c — ToxenOS help system.
// A small static registry drives everything: the category overview, the
// per-category command tables, and the per-command detail pages. Add a new
// command to HELP_TABLE and it shows up everywhere automatically.
#include "../tox.h"

// ── pager state ───────────────────────────────────────────────────────────────
#define PAGE_LINES 18
static int pager_row = 0;
static int paging    = 0;

static void pager_line(void) {
    if (!paging) return;
    pager_row++;
    if (pager_row >= PAGE_LINES) {
        set_color(0x08); print("-- more -- (any key)");
        tox_getchar();
        print("\r                    \r");
        set_color(0x07);
        pager_row = 0;
    }
}

// ── registry ──────────────────────────────────────────────────────────────────
typedef struct {
    const char* name;     // command as typed
    const char* category; // one of CATEGORIES[] below
    const char* usage;    // args portion only, e.g. "<src> <dst>" ("" if none)
    const char* desc;     // short description (category table + detail page)
    const char* example;  // optional full example line ("" if none)
} help_entry_t;

static const help_entry_t HELP_TABLE[] = {
    // ── files ──────────────────────────────────────────────────────────────
    { "ls",     "files",  "[dir]",            "list files and directories",                          "ls /C:/BSM" },
    { "tree",   "files",  "[dir]",            "list files and directories recursively",               "" },
    { "cd",     "files",  "<dir>",            "change the current directory",                         "cd /C:/BSM" },
    { "pcd",    "files",  "",                 "print the current directory",                          "" },
    { "cdb",    "files",  "",                 "go up to the parent directory",                        "" },
    { "cp",     "files",  "<src> <dst>",      "copy a file",                                          "" },
    { "mv",     "files",  "<src> <dst>",      "move or rename a file",                                "" },
    { "rname",  "files",  "<old> <new>",      "rename a file in place",                               "" },
    { "rm",     "files",  "<path>",           "delete a file or directory — exact path only, never expands to .nex/.elf", "rm /C:/old.txt" },
    { "mkd",    "files",  "<dir>",            "create a directory",                                   "" },
    { "rmkd",   "files",  "<dir>",            "remove an empty directory",                            "" },
    { "touch",  "files",  "<file> [...]",     "create a file, or refresh it if it already exists",    "" },
    { "mkef",   "files",  "<file>",           "create a new empty file",                              "" },
    { "find",   "files",  "[dir] <pattern>",  "search for files by name pattern",                     "find /C:/BSM *.nex" },
    { "file",   "files",  "<name>",           "show the type of a file",                              "" },
    { "shw",    "files",  "<file>",           "print a text file to the screen",                      "" },
    { "wc",     "files",  "<file> [...]",     "count lines, words, and bytes in a file",              "" },
    { "sif",    "files",  "<pattern> <file>", "search for a pattern inside a file",                   "" },
    { "chmod",  "files",  "<mode> <path>",    "change a file's permission mode",                      "chmod 644 file.txt" },
    { "edit",   "files",  "<file>",           "open the text editor",                                 "" },
    { "snap",   "files",  "<cmd> [name]",     "create, list, restore, or delete filesystem snapshots", "snap create backup1" },

    // ── proc ───────────────────────────────────────────────────────────────
    { "proc",   "proc",   "",                 "list running processes",                               "" },
    { "top",    "proc",   "",                 "live-updating process list (press q to quit)",         "" },
    { "kill",   "proc",   "[-9] <pid>",       "stop a running process",                               "kill -9 12" },
    { "end",    "proc",   "<pid>",            "stop a running process",                               "" },
    { "jobs",   "proc",   "",                 "list background jobs started with &",                  "" },
    { "uname",  "proc",   "",                 "print OS and version information",                     "" },

    // ── system ─────────────────────────────────────────────────────────────
    { "date",     "system", "",               "show the current date and time",                       "" },
    { "df",       "system", "",               "show filesystem disk usage",                           "" },
    { "free",     "system", "",               "show memory usage",                                    "" },
    { "uptime",   "system", "",               "show how long the system has been running",            "" },
    { "reboot",   "system", "",               "restart the system",                                   "" },
    { "shutdown", "system", "",               "power off the system",                                 "" },
    { "hostname", "system", "[name]",         "show or set the system hostname",                       "" },
    { "whoami",   "system", "",               "print the current username",                           "" },
    { "adduser",  "system", "<name>",         "create a new user account (admin only)",               "" },
    { "passwd",   "system", "[user]",         "change a user's password",                             "" },
    { "usermod",  "system", "<user> <role>",  "change a user's role (admin/user)",                    "" },
    { "reg",      "system", "<cmd> [args]",   "read or write the system registry (set/get/del/list)", "reg get hostname" },

    // ── pkg ────────────────────────────────────────────────────────────────
    { "tox",    "pkg",    "<cmd> [args]",     "run elevated, or install/remove/list packages",        "tox install /C:/pkg.elf" },

    // ── net ────────────────────────────────────────────────────────────────
    { "ipcfg",  "net",    "",                 "show network interface configuration",                 "" },
    { "ping",   "net",    "<host>",           "send ICMP echo requests to a host",                    "ping 10.0.2.2" },
    { "dns",    "net",    "<hostname>",       "resolve a hostname to an IP address",                  "" },
    { "http",   "net",    "<host> [path]",    "fetch a URL over plain HTTP",                          "" },
    { "https",  "net",    "<host> [path]",    "fetch a URL over HTTPS (TLS)",                         "" },

    // ── shell ──────────────────────────────────────────────────────────────
    { "echo",     "shell", "<text>",          "print text to the screen",                             "" },
    { "env",      "shell", "",                "list environment variables",                           "" },
    { "export",   "shell", "<k>=<v>",         "set an environment variable",                          "export PATH=/C:/bin" },
    { "alias",    "shell", "[k=v]",           "list, or create, a command alias",                     "" },
    { "unalias",  "shell", "<name>",          "remove a command alias",                                "" },
    { "source",   "shell", "<script.sh>",     "run a shell script in the current shell",              "" },
    { "run",      "shell", "<script.ts>",     "run a .ts script as a new process",                    "" },
    { "ts",       "shell", "<script.ts>",     "ToxenOS script interpreter",                            "" },
    { "clear",    "shell", "",                "clear the screen",                                     "" },
    { "exit",     "shell", "",                "log out of the current shell session",                 "" },
    { "logout",   "shell", "",                "log out of the current shell session",                 "" },

    // ── dev ────────────────────────────────────────────────────────────────
    { "hex",            "dev", "<file>",      "print the raw bytes of a file (hex + ASCII dump)",     "hex /C:/shell.nex" },
    { "nexinfo",        "dev", "<file.nex>",  "inspect a .nex executable's NEX header and segments",  "nexinfo /C:/shell.nex" },
    { "sysctl",         "dev", "[key]",       "read kernel tunables/diagnostics",                     "" },
    { "syslog",         "dev", "",            "show the kernel log",                                  "" },
    { "bmsg",           "dev", "",            "show buffered kernel boot messages",                   "" },
    { "where",          "dev", "<command>",   "show which file a command would run",                  "where ls" },
    { "isolation_test", "dev", "",            "verify kernel/user page table isolation",              "" },
    { "stresstest",     "dev", "",            "spawn/exit stress test for process slot leaks",        "" },
    { "memtest",        "dev", "",            "test sbrk-backed malloc/free/realloc",                 "" },
    { "pipetest",       "dev", "",            "test wait-queue pipe blocking",                         "" },
    { "nettest",        "dev", "",            "test UDP networking",                                  "" },
    { "sleeptest",      "dev", "",            "test sleep and scheduler blocking",                    "" },
};
#define HELP_COUNT (int)(sizeof(HELP_TABLE) / sizeof(HELP_TABLE[0]))

typedef struct { const char* key; const char* title; const char* blurb; } category_t;
static const category_t CATEGORIES[] = {
    { "files",  "Files & Directories", "browse, edit, copy, and delete files"        },
    { "proc",   "Processes",           "see and control what's running"              },
    { "system", "System",              "users, time, registry, power"                },
    { "pkg",    "Packages",            "install and manage software"                 },
    { "net",    "Network",             "ping, DNS, HTTP/HTTPS"                       },
    { "shell",  "Shell",               "pipes, redirection, aliases, history, env"   },
    { "dev",    "Developer Tools",     "inspect binaries, logs, and kernel state"    },
};
#define CATEGORY_COUNT (int)(sizeof(CATEGORIES) / sizeof(CATEGORIES[0]))

// ── string helpers ────────────────────────────────────────────────────────────
static int seq(const char* a, const char* b) {
    while (*a && *b) if (*a++ != *b++) return 0;
    return *a == 0 && *b == 0;
}

static const category_t* find_category(const char* name) {
    for (int i = 0; i < CATEGORY_COUNT; i++)
        if (seq(CATEGORIES[i].key, name)) return &CATEGORIES[i];
    return 0;
}

static const help_entry_t* find_command(const char* name) {
    for (int i = 0; i < HELP_COUNT; i++)
        if (seq(HELP_TABLE[i].name, name)) return &HELP_TABLE[i];
    return 0;
}

// ── printers ───────────────────────────────────────────────────────────────────
static void hdr(const char* title) {
    set_color(0x0B); print(title); set_color(0x07); print("\n");
    pager_line();
}

// One command per line: "  name usage   description"
static void cmdrow(const char* name, const char* usage, const char* desc) {
    print("  ");
    set_color(0x0F); print(name);
    int len = 2 + tox_strlen(name);
    if (usage[0]) {
        print(" "); set_color(0x08); print(usage);
        len += 1 + tox_strlen(usage);
    }
    set_color(0x08);
    while (len < 26) { print(" "); len++; }
    print("  ");
    set_color(0x07); print(desc); print("\n");
    pager_line();
}

static void print_category(const char* key) {
    const category_t* cat = find_category(key);
    if (!cat) return;
    hdr(cat->title);
    for (int i = 0; i < HELP_COUNT; i++)
        if (seq(HELP_TABLE[i].category, key))
            cmdrow(HELP_TABLE[i].name, HELP_TABLE[i].usage, HELP_TABLE[i].desc);

    if (seq(key, "shell")) {
        print("\n");
        set_color(0x0B); print("  Shell features:\n"); set_color(0x07);
        cmdrow("cmd | cmd",   "", "pipe one command's output into another");
        cmdrow("cmd > file",  "", "redirect output to a file (>> to append)");
        cmdrow("cmd < file",  "", "redirect a file to a command's input");
        cmdrow("cmd &",       "", "run a command in the background");
        cmdrow("Up / Down",   "", "recall previous commands from history");
        cmdrow("Tab",         "", "auto-complete a command or path");
        cmdrow("Ctrl+C",      "", "stop the foreground command");
    }
    print("\n");
}

static void print_command(const help_entry_t* e) {
    set_color(0x0E); print(e->name); set_color(0x07);
    if (e->usage[0]) { print(" "); set_color(0x08); print(e->usage); set_color(0x07); }
    print("\n\n");

    print("  "); print(e->desc); print("\n\n");

    set_color(0x0B); print("  Usage:   "); set_color(0x07);
    print(e->name);
    if (e->usage[0]) { print(" "); print(e->usage); }
    print("\n");

    if (e->example[0]) {
        set_color(0x0B); print("  Example: "); set_color(0x07);
        print(e->example); print("\n");
    }

    set_color(0x08); print("\n  category: "); print(e->category);
    print("  (see: help "); print(e->category); print(")\n");
    set_color(0x07);
}

static void print_overview(void) {
    set_color(0x0E); print("ToxenOS Help\n"); set_color(0x07);
    print("Run "); set_color(0x0F); print("help <topic>"); set_color(0x07);
    print(" for a category, or "); set_color(0x0F); print("help <command>");
    set_color(0x07); print(" for details about one command.\n\n");

    for (int i = 0; i < CATEGORY_COUNT; i++) {
        print("  "); set_color(0x0F); print("help ");
        print(CATEGORIES[i].key);
        int len = 7 + tox_strlen(CATEGORIES[i].key);
        set_color(0x08);
        while (len < 16) { print(" "); len++; }
        print("  "); print(CATEGORIES[i].blurb); print("\n");
        set_color(0x07);
    }
    print("\n");
    set_color(0x08); print("  Tip: "); set_color(0x07);
    print("help all"); set_color(0x08); print(" lists every command.\n");
    set_color(0x07);
}

// ── entry point ───────────────────────────────────────────────────────────────
void _start() {
    static char argbuf[64];
    tox_get_args(argbuf);
    const char* arg = argbuf;
    while (*arg == ' ') arg++;

    if (!arg[0]) { print_overview(); tox_exit(); }

    paging = 1;
    pager_row = 0;

    if (seq(arg, "all")) {
        set_color(0x0E); print("ToxenOS Help — all commands\n\n"); set_color(0x07);
        pager_row = 2;
        for (int i = 0; i < CATEGORY_COUNT; i++) print_category(CATEGORIES[i].key);
        tox_exit();
    }

    const category_t* cat = find_category(arg);
    if (cat) {
        set_color(0x0E); print("ToxenOS Help"); set_color(0x07); print("\n\n");
        pager_row = 2;
        print_category(arg);
        tox_exit();
    }

    const help_entry_t* e = find_command(arg);
    if (e) {
        print_command(e);
        tox_exit();
    }

    set_color(0x0C);
    print("unknown help topic: "); print(arg); print("\n");
    set_color(0x07);
    print("Run 'help' to list topics.\n");
    tox_exit();
}
