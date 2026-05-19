#include "../tox.h"

static int str_eq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static const char* basename(const char* path) {
    const char* last = path;
    for (const char* p = path; *p; p++) if (*p == '/') last = p + 1;
    return last;
}

static void usage(void) {
    set_color(0x0B); print("tox - ToxenOS Elevated Runner & Package Manager\n"); set_color(0x07);
    print("  tox <cmd> [args]     run command with elevated privileges (like sudo)\n");
    print("  tox install <path>   install program to /BSM/usr/lst/\n");
    print("  tox remove  <name>   remove program from /BSM/usr/lst/\n");
    print("  tox list             list installed packages\n");
}

// Find a command in BSM paths, return full path in out. Returns 1 if found.
static int tox_find(const char* cmd, char* out) {
    const char* dirs[] = { "/C:/BSM/SystemT", "/C:/BSM/usr/lst", 0 };
    for (int i = 0; dirs[i]; i++) {
        tox_strcpy(out, dirs[i]); tox_strcat(out, "/"); tox_strcat(out, cmd);
        if (tox_stat(out) >= 0) return 1;
        tox_strcpy(out, dirs[i]); tox_strcat(out, "/"); tox_strcat(out, cmd);
        tox_strcat(out, ".elf");
        if (tox_stat(out) >= 0) return 1;
    }
    return 0;
}

void _start() {
    char args[512]; tox_get_args(args);
    if (!args[0]) { usage(); tox_exit(); }

    char cmd[32]; int ci = 0;
    while (args[ci] && args[ci] != ' ' && ci < 31) { cmd[ci] = args[ci]; ci++; }
    cmd[ci] = 0;
    while (args[ci] == ' ') ci++;
    const char* param = args + ci;

    if (str_eq(cmd, "list")) {
        set_color(0x0B); print("Installed packages (/BSM/usr/lst/):\n"); set_color(0x07);
        char name[128]; int count = 0;
        for (int i = 0; ; i++) {
            if (tox_readdir("/C:/BSM/usr/lst", name, (uint32_t)i) < 0) break;
            if (name[0] == '.') continue;
            set_color(0x0A); print("  "); print(name); print("\n"); set_color(0x07);
            count++;
        }
        if (count == 0) { set_color(0x08); print("  (none installed)\n"); set_color(0x07); }
        tox_exit();
    }

    if (str_eq(cmd, "install")) {
        if (!param[0]) { set_color(0x0C); print("tox: usage: tox install <path>\n"); set_color(0x07); tox_exit(); }
        // System install (--system flag) goes to BSM/SystemT and needs elevation
        int system_install = 0;
        if (param[0] == '-' && param[1] == '-') {
            if (param[2]=='s'&&param[3]=='y'&&param[4]=='s'&&param[5]=='t'&&
                param[6]=='e'&&param[7]=='m') {
                system_install = 1;
                while (*param && *param != ' ') param++;
                while (*param == ' ') param++;
            }
        }
        if (system_install && tox_elevate("install to /BSM/SystemT/") < 0) tox_exit();

        int size = tox_stat(param);
        if (size < 0) {
            set_color(0x0C); print("tox: not found: "); print(param); print("\n");
            set_color(0x07); tox_exit();
        }

        const char* name = basename(param);
        char dst[256];
        tox_strcpy(dst, "/C:/BSM/usr/lst/");
        tox_strcat(dst, name);

        uint8_t* buf = malloc((uint32_t)size);
        if (!buf) { set_color(0x0C); print("tox: out of memory\n"); set_color(0x07); tox_exit(); }

        int fd = tox_open(param, 1);
        tox_read(fd, buf, (uint32_t)size);
        tox_close(fd);

        fd = tox_open(dst, 2 | 4);
        if (fd < 0) {
            free(buf);
            set_color(0x0C); print("tox: install failed\n"); set_color(0x07); tox_exit();
        }
        tox_write(fd, buf, (uint32_t)size);
        tox_close(fd);
        free(buf);

        set_color(0x0A); print("installed: "); print(name); print("\n"); set_color(0x07);
        tox_exit();
    }

    if (str_eq(cmd, "remove")) {
        if (!param[0]) { set_color(0x0C); print("tox: usage: tox remove <name>\n"); set_color(0x07); tox_exit(); }

        char path[256];
        tox_strcpy(path, "/C:/BSM/usr/lst/");
        tox_strcat(path, param);

        if (tox_stat(path) < 0) {
            tox_strcpy(path, "/C:/BSM/usr/lst/");
            tox_strcat(path, param);
            tox_strcat(path, ".elf");
            if (tox_stat(path) < 0) {
                set_color(0x0C); print("tox: not installed: "); print(param); print("\n");
                set_color(0x07); tox_exit();
            }
        }
        tox_remove(path);
        set_color(0x0A); print("removed: "); print(param); print("\n"); set_color(0x07);
        tox_exit();
    }

    // sudo-mode: unknown subcommand = elevate and run as a command
    {
        char full_path[256];
        // cmd might be absolute path already
        if (cmd[0] == '/') {
            tox_strcpy(full_path, cmd);
        } else if (!tox_find(cmd, full_path)) {
            set_color(0x0C); print("tox: not found: "); print(cmd); print("\n");
            set_color(0x07); tox_exit();
        }

        // UAC prompt in user space (kernel can't block for keyboard input)
        char reason[128];
        tox_strcpy(reason, "run "); tox_strcat(reason, cmd);
        tox_strcat(reason, " with elevated privileges");

        print("\n");
        set_color(0x0B); print("[ToxenOS]"); set_color(0x07);
        print(" tox is requesting elevated privileges\n");
        set_color(0x08); print("  Action : "); set_color(0x07); print(reason); print("\n");
        set_color(0x08); print("  Allow? "); set_color(0x0A); print("(Y"); set_color(0x07);
        print("/"); set_color(0x0C); print("n"); set_color(0x07); print("): ");

        // Drain any leftover keys from previous input before reading response
        while (tox_keyavail()) tox_getchar();

        char c = tox_getchar();
        if (c == '\n' || c == '\r' || c == 'y' || c == 'Y' || c == ' ') c = 'y';
        else c = 'n';

        if (c == 'y') {
            print("y\n");
            set_color(0x0A); print("[ELEVATED]\n\n"); set_color(0x07);
            tox_elevate(reason);  // kernel: set is_admin=1 + log
        } else {
            print("n\n");
            set_color(0x0C); print("[DENIED]\n\n"); set_color(0x07);
            // Log denial
            int lfd = tox_open("/C:/etc/priv.log", 2 | 4 | 8);
            if (lfd >= 0) {
                tox_write(lfd, (uint8_t*)"[DENY]  tox -> ", 15);
                tox_write(lfd, (uint8_t*)reason, tox_strlen(reason));
                tox_write(lfd, (uint8_t*)"\n", 1);
                tox_close(lfd);
            }
            tox_exit();
        }

        // Spawn the command — is_admin inherited from elevated tox
        int tty = tox_my_tty();
        int pid = tox_spawn_args(full_path, tty, param[0] ? param : "");
        if (pid < 0) {
            set_color(0x0C); print("tox: failed to run: "); print(full_path); print("\n");
            set_color(0x07); tox_exit();
        }
        tox_wait(pid);
        tox_exit();
    }
}
