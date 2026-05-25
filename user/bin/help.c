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
        // erase the prompt
        print("\r                    \r");
        set_color(0x07);
        pager_row = 0;
    }
}

// ── helpers ───────────────────────────────────────────────────────────────────
static void hdr(const char* title) {
    set_color(0x0B); print(title); set_color(0x07); print("\n");
    pager_line();
}

static void row(const char* lname, const char* largs,
                const char* rname, const char* rargs) {
    char buf[2]; int len = 4;
    buf[1] = 0;
    print("  ");
    set_color(0x0F); print(lname); set_color(0x08);
    if (largs[0]) { print(" "); print(largs); }
    for (int i = 0; lname[i]; i++) len++;
    if (largs[0]) { len++; for (int i = 0; largs[i]; i++) len++; }
    while (len++ < 28) { buf[0] = ' '; print(buf); }
    set_color(0x0F); print(rname); set_color(0x08);
    if (rargs[0]) { print(" "); print(rargs); }
    set_color(0x07); print("\n");
    pager_line();
}


// ── category printers ─────────────────────────────────────────────────────────
static void cat_files(void) {
    hdr("Files & Directories");
    row("edit",  "<file>",        "",       "text editor");
    row("ls",    "[dir]",         "tree",   "[dir]");
    row("cd",    "<dir>",         "pcd",    "");
    row("cdb",   "",              "shw",    "<file>");
    row("cp",    "<src> <dst>",   "mv",     "<src> <dst>");
    row("rm",    "<path>",        "rname",  "<old> <new>");
    row("mkef",  "<file>",        "mkd",    "<dir>");
    row("rmkd",  "<dir>",         "find",   "[dir] <pat>");
    row("hex",   "<file>",        "wc",     "<file>");
    row("file",  "<file>",        "sif",    "<pat> <file>");
    row("chmod", "<mode> <path>", "snap",   "[name]");
}

static void cat_trash(void) {
    hdr("Trash");
    row("trash",   "",       "restore", "<file>");
    row("trash",   "clear",  "",        "");
}

static void cat_proc(void) {
    hdr("Processes");
    row("proc",    "",         "top",      "");
    row("kill",    "<pid>",    "kill -9",  "<pid>");
    row("end",     "<pid>",    "jobs",     "");
    row("cmd &",   "",         "uname",    "");
}

static void cat_system(void) {
    hdr("System");
    row("sysctl",   "",               "syslog",   "");
    row("date",     "",               "df",       "");
    row("free",     "",               "uptime",   "");
    row("bmsg",     "",               "echo",     "<text>");
    row("reboot",   "",               "shutdown", "");
    row("hostname", "[name]",         "where",    "<cmd>");
    row("adduser",  "<name>",         "passwd",   "[user]");
    row("usermod",  "<user> <role>",  "",         "");
    row("reg set",  "<key> <val>",    "reg get",  "<key>");
    row("reg del",  "<key>",          "reg list", "");
}

static void cat_pkg(void) {
    hdr("Packages");
    row("tox list",    "",       "tox install", "<path>");
    row("tox remove",  "<name>", "tox get",     "<name>");
}

static void cat_net(void) {
    hdr("Network");
    row("ipcfg",  "",             "dns",   "<host>");
    row("ping",   "<host>",       "http",  "<ip> [path]");
    row("https",  "<ip> <host>",  "",      "");
}

static void cat_shell(void) {
    hdr("Shell");
    row("export",    "<k>=<v>",  "env",      "");
    row("alias",     "<k>=<v>",  "unalias",  "<name>");
    row("source",    "<file>",   "ts",       "<script.ts>");
    row("run",       "<file.ts>","",         "");
    row("cmd | cmd", "",         "cmd &",    "bg");
    row("cmd > file","",         "cmd < file","");
    row("Tab",       "complete", "Up/Down",  "history");
    row("Ctrl+C",    "kill fg",  "",         "");
}

// ── string helpers ────────────────────────────────────────────────────────────
static int seq(const char* a, const char* b) {
    while (*a && *b) if (*a++ != *b++) return 0;
    return *a == 0 && *b == 0;
}

// ── entry point ───────────────────────────────────────────────────────────────
void _start() {
    static char argbuf[64];
    tox_get_args(argbuf);
    const char* arg = argbuf;
    while (*arg == ' ') arg++;

    if (!arg || !arg[0]) {
        // Show category index only
        set_color(0x0E); print("ToxenOS Help\n\n"); set_color(0x07);
        set_color(0x07); print("  "); set_color(0x0F); print("help files");
        set_color(0x08); print("    "); print("ls, cd, cp, mv, rm, mkd ...\n");
        set_color(0x07); print("  "); set_color(0x0F); print("help trash");
        set_color(0x08); print("    "); print("rm, restore, trash clear\n");
        set_color(0x07); print("  "); set_color(0x0F); print("help proc");
        set_color(0x08); print("     "); print("proc, kill, top, jobs\n");
        set_color(0x07); print("  "); set_color(0x0F); print("help system");
        set_color(0x08); print("   "); print("sysctl, date, reg, users ...\n");
        set_color(0x07); print("  "); set_color(0x0F); print("help pkg");
        set_color(0x08); print("      "); print("tox list/install/remove\n");
        set_color(0x07); print("  "); set_color(0x0F); print("help net");
        set_color(0x08); print("      "); print("ipcfg, ping, dns, http\n");
        set_color(0x07); print("  "); set_color(0x0F); print("help shell");
        set_color(0x08); print("    "); print("pipes, redirect, aliases, history\n");
        set_color(0x07); print("\n");
        tox_exit();
    }

    paging = 1;
    pager_row = 0;

    set_color(0x0E); print("ToxenOS Help"); set_color(0x07); print("\n\n");
    pager_row = 2;

    if      (seq(arg,"files"))  cat_files();
    else if (seq(arg,"trash"))  cat_trash();
    else if (seq(arg,"proc"))   cat_proc();
    else if (seq(arg,"system")) cat_system();
    else if (seq(arg,"pkg"))    cat_pkg();
    else if (seq(arg,"net"))    cat_net();
    else if (seq(arg,"shell"))  cat_shell();
    else {
        set_color(0x0C);
        print("Unknown category: "); print(arg); print("\n");
        set_color(0x07);
        print("Categories: files  trash  proc  system  pkg  net  shell\n");
    }

    print("\n");
    tox_exit();
}
