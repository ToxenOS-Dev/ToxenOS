// ToxenOS Installer — full-featured Linux-style interactive installer
#include "../tox.h"

#define TXFS_MAGIC 0x54584653u
#define TOTAL_BLOCKS 25600
#define BAR_WIDTH    40

// ── helpers ───────────────────────────────────────────────────────────────────

static void print_size(uint32_t sectors) {
    uint32_t mb = (sectors / 2) / 1024;
    if (mb >= 1024) { print_int((int)(mb/1024)); print(" GB"); }
    else { print_int((int)mb); print(" MB"); }
}

static void hr(void) {
    set_color(0x08);
    print("  ─────────────────────────────────────────────\n");
    set_color(0x07);
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

static int read_pass(char* buf, int max) {
    int i = 0;
    while (i < max-1) {
        char c = tox_getchar();
        if (c == '\n' || c == '\r') { buf[i]=0; print("\n"); return i; }
        if ((c=='\b'||c==127)&&i>0) { i--; tox_erase(); continue; }
        if (c < 0x20 || c > 0x7E) continue;
        buf[i++]=c; print("*");
    }
    buf[i]=0; return i;
}

static void drain_keys(void) {
    tox_sleep(500);
    while (tox_keyavail()) tox_getchar();
    tox_sleep(100);
    while (tox_keyavail()) tox_getchar();
}

static char wait_yn(void) {
    // Only accept explicit y/Y/n/N — ignore Enter and scan codes
    char c = 0;
    while (c != 'y' && c != 'Y' && c != 'n' && c != 'N') {
        c = tox_getchar();
        if (c < 0x20 || c > 0x7E) c = 0;
    }
    char s[2]={c,0}; print(s); print("\n");
    return c;
}

// Detect filesystem on drive: 1=TxFS, 0=empty/unknown, -1=no drive
static int detect_fs(uint8_t drive) {
    static uint8_t sbuf[4096];
    uint32_t params[2]; params[0]=(uint32_t)sbuf; params[1]=8; // 8 sectors = 1 block
    if (tox_disk_read(drive, 8, sbuf, 8) < 0) return -1; // block 1 = superblock
    uint32_t* p = (uint32_t*)sbuf;
    if (p[0] == TXFS_MAGIC) return 1;
    return 0;
}

// Orange progress bar
static void progress_bar(int done, int total) {
    int filled = (done * BAR_WIDTH) / total;
    int pct    = (done * 100) / total;
    print("\r  ");
    set_color(0x08); print("[");
    for (int i = 0; i < BAR_WIDTH; i++) {
        if (i < filled) { set_color(0x06); print("#"); }
        else { set_color(0x08); print("-"); }
    }
    set_color(0x08); print("] ");
    set_color(0x07); print_int(pct); print("%  ");
}

// Add user to /etc/users with PBKDF2 hash
static void add_user_to_file(const char* username, const char* password, const char* role) {
    char hashed[128];
    set_color(0x08); print("  Hashing password...\n"); set_color(0x07);
    if (hash_password(password, hashed) < 0) tox_strcpy(hashed, password);

    int fd = tox_open("/C:/etc/users", 2|4|8); // append
    if (fd < 0) return;
    tox_write(fd, (uint8_t*)username, (uint32_t)tox_strlen(username));
    tox_write(fd, (uint8_t*)":", 1);
    tox_write(fd, (uint8_t*)hashed, (uint32_t)tox_strlen(hashed));
    tox_write(fd, (uint8_t*)":", 1);
    tox_write(fd, (uint8_t*)role, (uint32_t)tox_strlen(role));
    tox_write(fd, (uint8_t*)"\n", 1);
    tox_close(fd);
}

// ── main ──────────────────────────────────────────────────────────────────────

void _start() {
    tox_clear();
    print("\n\n");
    set_color(0x0B); print("  Welcome to the ToxenOS Installer\n"); set_color(0x07);
    hr(); print("\n");

    // ── Step 1: Detect disks ──────────────────────────────────────────────────
    set_color(0x08); print("  Available disks:\n\n"); set_color(0x07);
    int ndisks = 0;
    for (int d = 0; d <= 3; d++) {
        uint32_t sec = tox_disk_sectors((uint8_t)d);
        if (sec == 0) continue;
        int fs = detect_fs((uint8_t)d);
        set_color(0x08); print("    ["); set_color(0x0B);
        print_int(d); set_color(0x08); print("]  Drive "); set_color(0x07);
        print_int(d); print("   ");
        set_color(d==0 ? 0x08 : 0x0A); print_size(sec); set_color(0x08);
        if (d == 0)      print("   (source — do not select)");
        else if (fs==1)  { set_color(0x0E); print("   TxFS filesystem"); }
        else             print("   empty / unknown");
        set_color(0x07); print("\n");
        ndisks++;
    }
    if (ndisks < 2) {
        print("\n"); set_color(0x0C);
        print("  No target disk found. Connect a second disk and reboot.\n");
        set_color(0x07); tox_exit();
    }
    print("\n");

    // ── Step 2: Select target ─────────────────────────────────────────────────
    char input[64];
    int target = -1;
    while (target < 1 || target > 3) {
        print("  Select installation target (1-3): ");
        read_line(input, 8);
        if (input[0] >= '1' && input[0] <= '3') {
            int t = input[0] - '0';
            if (tox_disk_sectors((uint8_t)t) > 0) target = t;
            else { set_color(0x0C); print("  No disk at slot "); print_int(t); print(".\n"); set_color(0x07); }
        } else { set_color(0x0C); print("  Enter 1, 2, or 3.\n"); set_color(0x07); }
    }

    // ── Step 3: User account ──────────────────────────────────────────────────
    print("\n");
    set_color(0x0B); print("  Create a user account\n"); set_color(0x07);
    hr();
    print("  This account will be available on the installed system.\n\n");

    char username[64]={0}, password[64]={0}, hostname[64]="toxenos";
    int create_user = 0;

    print("  Username (leave blank to skip): ");
    read_line(username, sizeof(username));
    if (username[0]) {
        create_user = 1;
        while (1) {
            print("  Password: "); read_pass(password, sizeof(password));
            char p2[64]; print("  Confirm:  "); read_pass(p2, sizeof(p2));
            if (tox_strcmp(password, p2) == 0) break;
            set_color(0x0C); print("  Passwords do not match.\n"); set_color(0x07);
        }
    }

    // ── Step 4: Hostname ──────────────────────────────────────────────────────
    print("\n  Hostname [toxenos]: ");
    read_line(input, sizeof(input));
    if (input[0]) tox_strcpy(hostname, input);

    // ── Step 5: Summary + confirm ─────────────────────────────────────────────
    print("\n");
    set_color(0x0B); print("  Installation Summary\n"); set_color(0x07);
    hr();
    set_color(0x08); print("  Target  : "); set_color(0x07);
    print("Drive "); print_int(target); print(" (");
    print_size(tox_disk_sectors((uint8_t)target)); print(")\n");
    set_color(0x08); print("  Hostname: "); set_color(0x07); print(hostname); print("\n");
    if (create_user) {
        set_color(0x08); print("  User    : "); set_color(0x0A); print(username);
        set_color(0x08); print(" (new account)\n"); set_color(0x07);
    }
    set_color(0x0E);
    print("\n  WARNING: ALL DATA on Drive "); print_int(target); print(" will be ERASED!\n\n");
    set_color(0x07);
    print("  Proceed with installation? (Y/n): ");

    drain_keys();
    char c = wait_yn();
    if (c == 'n' || c == 'N') { print("\n  Installation cancelled.\n"); tox_exit(); }
    print("\n");

    // ── Step 6: Apply changes ─────────────────────────────────────────────────
    // Add user to /etc/users BEFORE block copy so it's included
    if (create_user) {
        set_color(0x08); print("  Adding user account..."); set_color(0x07);
        add_user_to_file(username, password, "user");
        set_color(0x0A); print(" done\n"); set_color(0x07);
    }

    // Persist hostname to /etc/reg BEFORE copy
    {
        // Write "hostname=<name>\n" to /etc/reg
        char line[128]; tox_strcpy(line, "hostname="); tox_strcat(line, hostname);
        tox_strcat(line, "\n");
        // Use existing reg set logic inline
        int sz = tox_stat("/C:/etc/reg");
        char buf[4096]; buf[0]=0;
        if (sz > 0 && sz < 4095) {
            int fd = tox_open("/C:/etc/reg", 1);
            int n = tox_read(fd, (uint8_t*)buf, (uint32_t)sz);
            tox_close(fd); if(n<0)n=0; buf[n]=0;
        }
        // Remove existing hostname= line
        char tmp[4096]; int ti=0;
        const char* p = buf;
        while (*p) {
            const char* ls=p; while(*p&&*p!='\n')p++; if(*p=='\n')p++;
            if(ls[0]=='h'&&ls[1]=='o'&&ls[2]=='s'&&ls[3]=='t'&&ls[4]=='n'&&
               ls[5]=='a'&&ls[6]=='m'&&ls[7]=='e'&&ls[8]=='=') continue;
            int ll=(int)(p-ls); for(int k=0;k<ll&&ti<4094;k++) tmp[ti++]=ls[k];
        }
        int ll=0; while(line[ll]&&ti<4094) tmp[ti++]=line[ll++];
        tmp[ti]=0;
        int fd=tox_open("/C:/etc/reg",2|4); if(fd>=0){tox_write(fd,(uint8_t*)tmp,(uint32_t)ti);tox_close(fd);}
        tox_setenv("hostname", hostname);
    }

    // ── Step 7: Block copy with orange progress bar ───────────────────────────
    set_color(0x0B); print("  Installing ToxenOS...\n\n"); set_color(0x07);

    // Copy in 100 chunks of 256 blocks each (total = 25600)
    int chunks = 100;
    int blocks_per_chunk = TOTAL_BLOCKS / chunks;

    progress_bar(0, chunks);

    for (int i = 0; i < chunks; i++) {
        uint32_t start = (uint32_t)(i * blocks_per_chunk);
        tox_install_chunk((uint8_t)target, start, (uint32_t)blocks_per_chunk);
        progress_bar(i+1, chunks);
    }

    print("\n\n");

    // ── Step 8: Done ──────────────────────────────────────────────────────────
    hr();
    set_color(0x0A); print("\n  Installation complete!\n\n"); set_color(0x07);
    print("  What to do next:\n\n");
    print("  1. Power off the machine\n");
    print("  2. Remove the USB/install media\n");
    print("  3. Set Drive "); print_int(target); print(" as boot device in BIOS/UEFI\n");
    print("  4. Boot ToxenOS\n");
    print("\n");
    set_color(0x08);
    print("  Tip: Log in as 'admin' with your existing password.\n");
    if (create_user) {
        print("  Tip: Your new account '"); print(username);
        print("' is ready to use.\n");
    }
    set_color(0x07); print("\n");
    tox_exit();
}
