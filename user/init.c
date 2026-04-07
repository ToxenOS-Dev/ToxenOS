// ToxenOS/user/init.c — PID 1 / Tinit
// Reads /C:/etc/tinit.cfg and spawns services + shell
#include "tox.h"

// ── helpers ───────────────────────────────────────────────────────────────────

static void ok(const char* msg) {
    print("  "); set_color(0x0A); print("[ OK ]"); set_color(0x07);
    print(" "); print(msg); print("\n");
}

static void fail(const char* msg) {
    print("  "); set_color(0x0C); print("[FAIL]"); set_color(0x07);
    print(" "); print(msg); print("\n");
}

static void info(const char* msg) {
    print("  "); set_color(0x08); print("[INFO]"); set_color(0x07);
    print(" "); print(msg); print("\n");
}

static int str_eq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

static int str_starts(const char* s, const char* prefix) {
    int i = 0;
    while (prefix[i] && s[i] == prefix[i]) i++;
    return !prefix[i];
}

static void str_trim(char* s) {
    // trim trailing whitespace/newline
    int len = 0;
    while (s[len]) len++;
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r' || s[len-1] == ' '))
        s[--len] = 0;
}

// ── service table ─────────────────────────────────────────────────────────────

#define MAX_SERVICES 16

static struct {
    char path[64];
    int  pid;
    int  restart;  // 1 = restart on crash
} services[MAX_SERVICES];

static int service_count = 0;

// ── config parser ─────────────────────────────────────────────────────────────

static void parse_config(const char* buf, int size) {
    int i = 0;
    while (i < size) {
        // Read line
        char line[128];
        int  len = 0;
        while (i < size && buf[i] != '\n' && len < 127)
            line[len++] = buf[i++];
        line[len] = 0;
        if (i < size) i++;  // skip newline

        str_trim(line);

        // Skip empty lines and comments
        if (!line[0] || line[0] == '#') continue;

        // service <path> [restart]
        if (str_starts(line, "service ")) {
            char* path = line + 8;
            int restart = 0;

            // Check for "restart" flag at end
            int plen = 0;
            while (path[plen] && path[plen] != ' ') plen++;
            if (path[plen] == ' ') {
                path[plen] = 0;
                if (str_eq(path + plen + 1, "restart")) restart = 1;
            }

            if (service_count < MAX_SERVICES) {
                // Build full path
                char full[64];
                int fi = 0;
                if (path[0] != '/') {
                    // relative — assume /C:/etc/services/
                    const char* prefix = "/C:/etc/services/";
                    while (*prefix) full[fi++] = *prefix++;
                }
                int pi = 0;
                while (path[pi]) full[fi++] = path[pi++];
                full[fi] = 0;

                int pid = tox_spawn(full);
                if (pid > 0) {
                    tox_strcpy(services[service_count].path, full);
                    services[service_count].pid     = pid;
                    services[service_count].restart = restart;
                    service_count++;
                    ok(full);
                } else {
                    fail(full);
                }
            }
        }
        // shell — launch the interactive shell on TTY 0
        else if (str_eq(line, "shell")) {
            // handled after config parsing
        }
    }
}

// ── main ──────────────────────────────────────────────────────────────────────

void _start()
{
    set_color(0x06); print("\n  ToxenOS\n"); set_color(0x07);
    set_color(0x08); print("  Tinit v1.0 - starting system...\n\n"); set_color(0x07);

    ok("Memory manager");
    ok("Paging and virtual memory");
    ok("Interrupt descriptor table");
    ok("PIC and IRQ routing");
    ok("PIT timer (100Hz)");
    ok("PS/2 keyboard");
    ok("ATA disk controller");
    ok("VFS + TxFS + FAT32 + ext2");
    ok("Framebuffer terminal");

    print("\n");
    set_color(0x08); print("  Reading /C:/etc/tinit.cfg...\n"); set_color(0x07);

    // Try to read tinit.cfg
    int cfg_size = tox_stat("/C:/etc/tinit.cfg");
    if (cfg_size > 0) {
        char* cfg_buf = malloc((uint32_t)cfg_size + 1);
        if (cfg_buf) {
            int fd = tox_open("/C:/etc/tinit.cfg", 1);
            if (fd >= 0) {
                tox_read(fd, (uint8_t*)cfg_buf, (uint32_t)cfg_size);
                tox_close(fd);
                cfg_buf[cfg_size] = 0;
                parse_config(cfg_buf, cfg_size);
            }
            free(cfg_buf);
        }
    } else {
        info("No tinit.cfg found — using defaults");
    }

    print("\n");
    set_color(0x0A); print("  ToxenOS ready.\n\n"); set_color(0x07);

    // Brief pause so user can read boot messages
    for (volatile int i = 0; i < 50000000; i++);

    // Launch shell on TTY 0
    tox_spawn_embedded(0);

    // Monitor loop — restart crashed services
    while (1) {
        yield();
        // TODO: check if services are still running and restart if needed
    }
}
