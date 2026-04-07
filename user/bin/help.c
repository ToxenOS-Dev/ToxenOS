#include "../tox.h"

// Print "  <white name><gray args>" padded to column 26, then right side
static void row(const char* lname, const char* largs,
                const char* rname, const char* rargs) {
    char buf[8];
    int i;
    // left cell: 4 spaces + name + args, padded to col 26
    print("    ");
    set_color(0x07); print(lname);
    set_color(0x08); if (largs[0]) { print(" "); print(largs); }
    // count chars printed so far
    int len = 4;
    for (i=0;lname[i];i++) len++;
    if (largs[0]) { len++; for (i=0;largs[i];i++) len++; }
    // pad to col 26
    buf[1]=0;
    while (len++ < 26) { buf[0]=' '; print(buf); }
    // right cell
    set_color(0x07); print(rname);
    set_color(0x08); if (rargs[0]) { print(" "); print(rargs); }
    set_color(0x07); print("\n");
}

void _start() {
    set_color(0x0E); print("ToxenOS Commands\n\n"); set_color(0x07);

    set_color(0x0B); print("  Files & Directories\n"); set_color(0x07);
    row("ls",    "[dir]",       "cp",    "<src> <dst>");
    row("cd",    "<dir>",       "mv",    "<src> <dst>");
    row("cdb",   "",            "rname", "<old> <new>");
    row("pcd",   "",            "rm",    "<path>");
    row("shw",   "<file>",      "mkef",  "<file>");
    row("hex",   "<file>",      "mkd",   "<dir>");
    row("file",  "<file>",      "tree",  "[dir]");
    row("find",  "[dir] <pat>", "sif",   "<pat> <file>");

    set_color(0x0B); print("\n  Processes\n"); set_color(0x07);
    row("proc",  "",      "uname",    "");
    row("top",   "",      "echo",     "<text>");
    row("end",   "<pid>", "bmsg",     "");
    row("reboot","",      "shutdown", "");

    set_color(0x0B); print("\n  Network\n"); set_color(0x07);
    row("dns",  "<hostname>",   "ping",  "<hostname>");
    row("http", "<ip> [path]",  "https", "<ip> <host> [path]");

    set_color(0x0B); print("\n  Shell\n"); set_color(0x07);
    row("cmd | cmd",  "", "Tab",     "autocomplete");
    row("cmd > file", "", "Up/Down", "history");
    row("cmd < file", "", "Ctrl+C",  "kill");

    print("\n");
    tox_exit();
}
