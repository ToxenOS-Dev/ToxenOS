#include "../tox.h"

static void row(const char* lname, const char* largs,
                const char* rname, const char* rargs) {
    char buf[8]; int i, len = 4;
    print("    ");
    set_color(0x07); print(lname);
    set_color(0x08); if (largs[0]) { print(" "); print(largs); }
    for (i=0;lname[i];i++) len++;
    if (largs[0]) { len++; for (i=0;largs[i];i++) len++; }
    buf[1]=0;
    while (len++ < 26) { buf[0]=' '; print(buf); }
    set_color(0x07); print(rname);
    set_color(0x08); if (rargs[0]) { print(" "); print(rargs); }
    set_color(0x07); print("\n");
}

void _start() {
    set_color(0x0E); print("ToxenOS Commands\n\n"); set_color(0x07);

    set_color(0x0B); print("  Files & Directories\n"); set_color(0x07);
    row("ls",    "[dir]",        "cp",     "<src> <dst>");
    row("cd",    "<dir>",        "mv",     "<src> <dst>");
    row("cdb",   "",             "rname",  "<old> <new>");
    row("pcd",   "",             "rm",     "<path>");
    row("shw",   "<file>",       "mkef",   "<file>");
    row("hex",   "<file>",       "mkd",    "<dir>");
    row("file",  "<file>",       "rmkd",  "<dir>");
    row("find",  "[dir] <pat>",  "tree",   "[dir]");
    row("sif",   "<pat> <file>", "wc",     "<file>");
    row("chmod", "<mode> <file>","chmod",  "<file>");

    set_color(0x0B); print("\n  Trash\n"); set_color(0x07);
    row("rm",      "<file>",   "trash",   "");
    row("restore", "<file>",   "trash",   "clear");

    set_color(0x0B); print("\n  Processes\n"); set_color(0x07);
    row("proc",  "",        "top",      "");
    row("kill",  "<pid>",   "kill -9",  "<pid>");
    row("end",   "<pid>",   "jobs",     "");
    row("cmd &", "",        "uname",    "");

    set_color(0x0B); print("\n  System\n"); set_color(0x07);
    row("sysctl",   "",              "syslog",    "");
    row("date",     "",              "df",        "");
    row("free",     "",              "where",     "<cmd>");
    row("hostname", "[name]",        "",          "");
    row("reg set",  "<key> <val>",   "reg get",   "<key>");
    row("reg del",  "<key>",         "reg list",  "");
    row("reboot",   "",              "shutdown",  "");
    row("bmsg",     "",              "echo",      "<text>");

    set_color(0x0B); print("\n  Packages (tox)\n"); set_color(0x07);
    row("tox list",    "",       "tox install", "<path>");
    row("tox remove",  "<name>", "",            "");

    set_color(0x0B); print("\n  Network\n"); set_color(0x07);
    row("dns",  "<host>",       "ping",  "<host>");
    row("http", "<ip> [path]",  "https", "<ip> <host> [path]");

    set_color(0x0B); print("\n  Shell\n"); set_color(0x07);
    row("export", "<k>=<v>",   "env",     "");
    row("cmd | cmd",  "",      "Tab",     "autocomplete");
    row("cmd > file", "",      "Up/Down", "history");
    row("cmd < file", "",      "Ctrl+C",  "kill fg");

    print("\n");
    tox_exit();
}
