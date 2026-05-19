#include "../tox.h"

void _start() {
    char args[64]; tox_get_args(args);

    if (!args[0]) {
        // Show current hostname
        char buf[64];
        if (tox_getenv("hostname", buf, sizeof(buf)) >= 0)
            { set_color(0x0B); print(buf); print("\n"); set_color(0x07); }
        else
            { set_color(0x0B); print("toxenos\n"); set_color(0x07); }
        tox_exit();
    }

    // Set hostname: store in kernel env + persist to /etc/reg
    tox_setenv("hostname", args);

    // Persist to /etc/reg so it survives reading by reg list
    // Use same format: hostname=<value>
    int size = tox_stat("/C:/etc/reg");
    char buf[4096]; buf[0] = 0;
    if (size > 0 && size < 4095) {
        int fd = tox_open("/C:/etc/reg", 1);
        int n = tox_read(fd, (uint8_t*)buf, (uint32_t)size);
        tox_close(fd);
        buf[n < 0 ? 0 : n] = 0;
    }

    // Remove existing hostname line
    char tmp[4096]; int ti = 0;
    const char* p = buf;
    while (*p) {
        const char* line = p;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        // skip hostname= lines
        if (line[0]=='h'&&line[1]=='o'&&line[2]=='s'&&line[3]=='t'&&
            line[4]=='n'&&line[5]=='a'&&line[6]=='m'&&line[7]=='e'&&line[8]=='=')
            continue;
        int llen = (int)(p - line);
        for (int i = 0; i < llen && ti < 4094; i++) tmp[ti++] = line[i];
    }
    // Append new hostname line
    const char* key = "hostname=";
    for (int i = 0; key[i] && ti < 4094; i++) tmp[ti++] = key[i];
    for (int i = 0; args[i] && ti < 4094; i++) tmp[ti++] = args[i];
    tmp[ti++] = '\n'; tmp[ti] = 0;

    int fd = tox_open("/C:/etc/reg", 2|4);
    if (fd >= 0) { tox_write(fd, (uint8_t*)tmp, (uint32_t)ti); tox_close(fd); }

    set_color(0x0A); print("hostname set to: "); print(args); print("\n"); set_color(0x07);
    tox_exit();
}
