#include "../tox.h"

void _start() {
    char args[256]; tox_get_args(args);
    if (!args[0]) { print("Usage: where <command>\n"); tox_exit(); }

    // Extract just the command name (basename of args)
    const char* cmd = args;
    for (const char* p = args; *p; p++) if (*p == '/') cmd = p + 1;

    // Search PATH env var, fall back to BSM dirs
    char path_env[256];
    if (tox_getenv("PATH", path_env, sizeof(path_env)) < 0)
        tox_strcpy(path_env, "/C:/BSM/SystemT:/C:/BSM/usr/lst");

    char dir[128];
    const char* p = path_env;
    while (*p) {
        int dlen = 0;
        while (*p && dlen < 127) {
            if (*p == ';') break;
            if (*p == ':') {
                int is_drive = (dlen == 2 && dir[0] == '/' &&
                    ((dir[1]>='A'&&dir[1]<='Z')||(dir[1]>='a'&&dir[1]<='z'))) ||
                    (dlen == 1 && ((dir[0]>='A'&&dir[0]<='Z')||(dir[0]>='a'&&dir[0]<='z')));
                if (is_drive) { dir[dlen++] = *p++; continue; }
                break;
            }
            dir[dlen++] = *p++;
        }
        dir[dlen] = 0;
        if (*p == ';' || *p == ':') p++;
        if (!dlen) continue;

        char full[256];
        tox_strcpy(full, dir); tox_strcat(full, "/"); tox_strcat(full, cmd);
        if (tox_stat(full) >= 0) {
            set_color(0x0A); print(full); print("\n"); set_color(0x07); tox_exit();
        }
        char nexpath[256];
        tox_strcpy(nexpath, full); tox_strcat(nexpath, ".nex");
        if (tox_stat(nexpath) >= 0) {
            set_color(0x0A); print(nexpath); print("\n"); set_color(0x07); tox_exit();
        }
        tox_strcat(full, ".elf");
        if (tox_stat(full) >= 0) {
            set_color(0x0A); print(full); print("\n"); set_color(0x07); tox_exit();
        }
    }

    set_color(0x0C); print("where: not found: "); print(cmd); print("\n");
    set_color(0x07); tox_exit();
}
