// ToxenOS Installer — copies system to a target ATA drive
// Usage: install          (interactive)
//        install <drive>  (drive = 1, 2, 3)
#include "../tox.h"

static int seq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static void print_size(uint32_t sectors) {
    uint32_t mb = (sectors / 2) / 1024;
    if (mb >= 1024) { print_int((int)(mb/1024)); print(" GB"); }
    else { print_int((int)mb); print(" MB"); }
}

static int read_line(char* buf, int max) {
    int i = 0;
    while (i < max-1) {
        char c = tox_getchar();
        if (c == '\n' || c == '\r') { buf[i]=0; print("\n"); return i; }
        if ((c=='\b'||c==127)&&i>0) { i--; tox_erase(); continue; }
        if (c < 0x20 || c > 0x7E) continue;
        buf[i++]=c; char s[2]={c,0}; print(s);
    }
    buf[i]=0; return i;
}

// Files to copy from source to destination drive
static const char* system_files[] = {
    "/BSM/SystemT/ls.elf",    "/BSM/SystemT/rm.elf",
    "/BSM/SystemT/mkef.elf",  "/BSM/SystemT/mkd.elf",
    "/BSM/SystemT/echo.elf",  "/BSM/SystemT/shw.elf",
    "/BSM/SystemT/cp.elf",    "/BSM/SystemT/mv.elf",
    "/BSM/SystemT/tree.elf",  "/BSM/SystemT/sysctl.elf",
    "/BSM/SystemT/proc.elf",  "/BSM/SystemT/top.elf",
    "/BSM/SystemT/kill.elf",  "/BSM/SystemT/date.elf",
    "/BSM/SystemT/df.elf",    "/BSM/SystemT/free.elf",
    "/BSM/SystemT/wc.elf",    "/BSM/SystemT/chmod.elf",
    "/BSM/SystemT/adduser.elf","/BSM/SystemT/passwd.elf",
    "/BSM/SystemT/tox.elf",   "/BSM/SystemT/snap.elf",
    "/BSM/SystemT/ipcfg.elf", "/BSM/SystemT/install.elf",
    "/BSM/SystemT/help.elf",  "/BSM/SystemT/reg.elf",
    "/BSM/SystemT/syslog.elf","/BSM/SystemT/trash.elf",
    "/BSM/SystemT/restore.elf","/BSM/SystemT/rmkd.elf",
    "/BSM/SystemT/where.elf", "/BSM/SystemT/find.elf",
    "/BSM/SystemT/hex.elf",   "/BSM/SystemT/file.elf",
    "/BSM/SystemT/uname.elf", "/BSM/SystemT/hostname.elf",
    "/BSM/SystemT/bmsg.elf",  "/BSM/SystemT/usermod.elf",
    "/shell.elf",             "/init.elf",
    0
};

static int copy_file(const char* src, const char* dst) {
    int size = tox_stat(src);
    if (size < 0) return -1;
    uint8_t* buf = malloc((uint32_t)size+1);
    if (!buf) return -1;
    int fd = tox_open(src, 1);
    tox_read(fd, buf, (uint32_t)size);
    tox_close(fd);
    fd = tox_open(dst, 2|4);
    if (fd < 0) { free(buf); return -1; }
    tox_write(fd, buf, (uint32_t)size);
    tox_close(fd);
    free(buf);
    return size;
}

void _start() {
    char args[16]; tox_get_args(args);

    set_color(0x0B); print("\n  ToxenOS Installer\n\n"); set_color(0x07);

    // Detect drives
    print("  Detecting drives...\n\n");
    int found = 0;
    for (int d = 0; d <= 3; d++) {
        uint32_t sec = tox_disk_sectors((uint8_t)d);
        if (sec > 0) {
            set_color(0x0A); print("  Drive "); print_int(d); print(": ");
            set_color(0x07); print_size(sec); print("\n");
            found++;
        }
    }
    if (!found) {
        set_color(0x0C); print("  No drives detected.\n"); set_color(0x07);
        tox_exit();
    }
    print("\n");
    print("  Drive 0 = boot/install media (don't install here)\n");
    print("  Drive 1+ = target for installation\n\n");

    // Pick target drive
    char input[8];
    int target = -1;
    while (target < 1 || target > 3) {
        print("  Install to drive number (1-3): ");
        read_line(input, sizeof(input));
        if (input[0] >= '1' && input[0] <= '3') target = input[0] - '0';
        else { set_color(0x0C); print("  Invalid drive.\n"); set_color(0x07); }
    }

    uint32_t target_sec = tox_disk_sectors((uint8_t)target);
    if (target_sec == 0) {
        set_color(0x0C); print("  Drive not found.\n"); set_color(0x07); tox_exit();
    }

    // Confirm
    set_color(0x0E);
    print("\n  WARNING: This will ERASE drive "); print_int(target);
    print(" ("); print_size(target_sec); print(")!\n");
    set_color(0x07);
    print("  Type YES to continue: ");
    read_line(input, sizeof(input));
    if (!(input[0]=='Y'&&input[1]=='E'&&input[2]=='S'&&!input[3])) {
        print("  Cancelled.\n"); tox_exit();
    }

    // Determine target drive letter
    char drive_letter[4];
    drive_letter[0] = '/';
    drive_letter[1] = (char)('C' + target); // D:, E:, F:...
    drive_letter[2] = ':';
    drive_letter[3] = 0;

    // The target drive should already be mounted (kernel auto-mounts drives 1-3)
    // Check if it's accessible, if not try /D: /E: /F:
    char dst_base[8];
    int mounted = 0;
    for (int dl = 1; dl <= 3; dl++) {
        dst_base[0]='/'; dst_base[1]=(char)('C'+dl); dst_base[2]=':'; dst_base[3]=0;
        if (tox_isdir(dst_base) >= 0) {
            // Find the one that corresponds to our target drive
            // (heuristic: use first available after /C:)
            if (dl == target) { mounted = 1; break; }
        }
    }

    if (!mounted) {
        set_color(0x0C);
        print("  Drive not mounted. The kernel auto-mounts drives on boot.\n");
        print("  Please ensure the drive is connected and reboot.\n");
        set_color(0x07); tox_exit();
    }

    set_color(0x0B); print("\n  Installing ToxenOS to "); print(dst_base); print(":\n\n");

    // Create directory structure
    char path[256];
    tox_strcpy(path, dst_base); tox_strcat(path, "/BSM"); tox_mkdir(path);
    tox_strcpy(path, dst_base); tox_strcat(path, "/BSM/SystemT"); tox_mkdir(path);
    tox_strcpy(path, dst_base); tox_strcat(path, "/BSM/usr"); tox_mkdir(path);
    tox_strcpy(path, dst_base); tox_strcat(path, "/BSM/usr/lst"); tox_mkdir(path);
    tox_strcpy(path, dst_base); tox_strcat(path, "/Trash"); tox_mkdir(path);
    tox_strcpy(path, dst_base); tox_strcat(path, "/etc"); tox_mkdir(path);

    // Copy system files
    int copied = 0, failed = 0;
    for (int i = 0; system_files[i]; i++) {
        // Build destination path
        char dst[256]; tox_strcpy(dst, dst_base); tox_strcat(dst, system_files[i]);
        // Create parent dir if needed
        char* last_slash = dst;
        for (char* p = dst; *p; p++) if (*p == '/') last_slash = p;
        if (last_slash > dst) {
            char parent[256]; int plen = (int)(last_slash - dst);
            for (int k = 0; k < plen && k < 255; k++) parent[k] = dst[k];
            parent[plen] = 0;
            tox_mkdir(parent);
        }
        int bytes = copy_file(system_files[i], dst);
        if (bytes >= 0) {
            set_color(0x0A); print("  ✓ ");
            // print just the filename
            const char* fname = system_files[i];
            for (const char* p = system_files[i]; *p; p++) if (*p=='/') fname=p+1;
            print(fname); print("\n"); set_color(0x07);
            copied++;
        } else {
            set_color(0x0C); print("  ✗ "); print(system_files[i]); print("\n");
            set_color(0x07); failed++;
        }
    }

    // Copy /etc/users template
    tox_strcpy(path, dst_base); tox_strcat(path, "/etc/users");
    copy_file("/etc/users", path);

    print("\n");
    set_color(0x0A); print_int(copied); print(" files installed");
    if (failed > 0) { set_color(0x0C); print(", "); print_int(failed); print(" failed"); }
    print("\n\n"); set_color(0x07);

    set_color(0x0B);
    print("  Installation complete!\n\n");
    set_color(0x07);
    print("  To boot ToxenOS from this drive:\n");
    print("  1. Set drive "); print_int(target); print(" as boot device in BIOS/UEFI\n");
    print("  2. The ToxenOS ISO bootloader (GRUB) will also boot this drive\n");
    print("  3. Or use the ISO and select 'Install ToxenOS' on next boot\n\n");

    tox_exit();
}
