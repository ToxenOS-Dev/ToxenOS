#include "../tox.h"

static int seq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static void usage(void) {
    set_color(0x0B); print("snap - TxFS Snapshot Manager\n"); set_color(0x07);
    print("  snap create [name]   create snapshot (default: 'snap')\n");
    print("  snap list            list all snapshots\n");
    print("  snap restore <name>  restore filesystem to snapshot\n");
    print("  snap delete <name>   delete a snapshot\n");
}

void _start() {
    char args[128]; tox_get_args(args);
    if (!args[0]) { usage(); tox_exit(); }

    // Parse subcommand and optional name
    char cmd[32], name[64];
    int i = 0, j = 0;
    while (args[i] && args[i] != ' ' && i < 31) cmd[j++] = args[i++]; cmd[j]=0;
    while (args[i] == ' ') i++;
    j = 0;
    while (args[i] && j < 63) name[j++] = args[i++]; name[j]=0;
    if (!name[0]) tox_strcpy(name, "snap"); // default name

    if (seq(cmd, "create")) {
        int slot = tox_snap_create(name);
        if (slot < 0) {
            set_color(0x0C); print("snap: failed (disk full or max snapshots reached)\n"); set_color(0x07);
        } else {
            set_color(0x0A); print("snapshot created: "); print(name); print("\n"); set_color(0x07);
        }
        tox_exit();
    }

    if (seq(cmd, "list")) {
        static char names[10][64];
        static uint32_t ts[10];
        int count = tox_snap_list(names, ts, 10);
        if (count == 0) {
            set_color(0x08); print("no snapshots\n"); set_color(0x07);
        } else {
            set_color(0x0B); print("Snapshots:\n"); set_color(0x07);
            for (int k = 0; k < count; k++) {
                set_color(0x0A); print("  "); print(names[k]);
                set_color(0x08); print("  (tick "); print_int((int)ts[k]); print(")\n");
                set_color(0x07);
            }
        }
        tox_exit();
    }

    if (seq(cmd, "restore")) {
        if (!name[0] || seq(name, "snap")) {
            // if name is default and user didn't provide one, show usage
            if (seq(cmd, "restore") && args[i] == 0 && !seq(name,"snap")) {
                set_color(0x0C); print("snap: usage: snap restore <name>\n"); set_color(0x07); tox_exit();
            }
        }
        set_color(0x0E); print("Restoring snapshot '"); print(name); print("'...\n"); set_color(0x07);
        if (tox_snap_restore(name) < 0) {
            set_color(0x0C); print("snap: not found: "); print(name); print("\n"); set_color(0x07);
        } else {
            set_color(0x0A); print("restored: "); print(name); print("\n");
            print("Reboot for full effect.\n"); set_color(0x07);
        }
        tox_exit();
    }

    if (seq(cmd, "delete")) {
        if (tox_snap_delete(name) < 0) {
            set_color(0x0C); print("snap: not found: "); print(name); print("\n"); set_color(0x07);
        } else {
            set_color(0x0A); print("deleted: "); print(name); print("\n"); set_color(0x07);
        }
        tox_exit();
    }

    usage();
    tox_exit();
}
