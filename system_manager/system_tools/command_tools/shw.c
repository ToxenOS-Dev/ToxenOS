// ToxenOS/system_manager/system_tools/command_tools/shw.c — Milestone 11:
// ToxenOS64's file-display command. ToxenOS's own name for this
// (confirmed via the 32-bit user/bin/shw.c) -- deliberately not "cat".
// Runs as a standalone NEX64 process, spawned by shell64; retrieves its
// one argument (the file path) via sys_get_args, exactly mirroring the
// 32-bit tox_get_args shape.
//
// Milestone 12: the argument may be a visible ToxenOS path
// (C:\System Manager\...) or a quoted one, or the legacy internal slash
// form -- toxpath64_to_internal handles all three before sys_open ever
// sees it. Error messages still show what the user actually typed.
#include <stdint.h>
#include "tox64.h"
#include "toxpath64.h"
#include "toxcolor64.h"

static int my_strlen(const char* s) {
    int i = 0;
    while (s[i]) i++;
    return i;
}

static void put(const char* s) {
    sys_write(s, (uint64_t)my_strlen(s));
}

// Milestone 15: every error message in this command renders bright
// red, reset to the console default immediately after -- matches the
// convention shell64.c and every other command_tools program now follow.
static void put_err(const char* prefix, const char* arg) {
    sys_set_color(TC64_RED_BRIGHT);
    put(prefix);
    put(arg);
    put("\n");
    sys_set_color(TC64_DEFAULT);
}

void _start(void) {
    char args[128];
    int64_t n = sys_get_args(args, sizeof(args));
    if (n < 0) args[0] = 0;
    toxpath64_strip_quotes(args);

    if (!args[0]) {
        put_err("shw: missing argument", "");
        sys_exit(1);
    }

    char path[128];
    toxpath64_to_internal(args, path, sizeof(path));

    int64_t fd = sys_open(path);
    if (fd < 0) {
        put_err("shw: cannot open ", args);
        sys_exit(1);
    }

    char buf[256];
    for (;;) {
        int64_t got = sys_read((int)fd, buf, sizeof(buf) - 1);
        if (got <= 0) break;
        buf[got] = 0;
        put(buf);
    }
    sys_close((int)fd);
    sys_exit(0);

    for (;;) { }  // unreachable
}
