// ToxenOS/user/login.c — login prompt
// Reads /C:/etc/users, verifies credentials, sets uid + USER, exec's shell.
#include "tox.h"

static int str_eq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int my_strlen(const char* s) { int i=0; while(s[i]) i++; return i; }

static void my_itoa(uint32_t v, char* buf) {
    if (!v) { buf[0]='0'; buf[1]=0; return; }
    char tmp[12]; int i=0;
    while (v) { tmp[i++]=(char)('0'+v%10); v/=10; }
    int j=0; while(i>0) buf[j++]=tmp[--i]; buf[j]=0;
}

static int read_visible(char* buf, int max) {
    int i = 0;
    while (i < max - 1) {
        char c = tox_getchar();
        if (c == '\n' || c == '\r') { buf[i]=0; print("\n"); return i; }
        if ((c == '\b' || c == 127) && i > 0) { i--; tox_erase(); continue; }
        if (c < 0x20 || c > 0x7E) continue;
        buf[i++] = c; char s[2]={c,0}; print(s);
    }
    buf[i]=0; return i;
}

static int read_pass(char* buf, int max) {
    int i = 0;
    while (i < max - 1) {
        char c = tox_getchar();
        if (c == '\n' || c == '\r') { buf[i]=0; print("\n"); return i; }
        if ((c == '\b' || c == 127) && i > 0) { i--; tox_erase(); continue; }
        if (c < 0x20 || c > 0x7E) continue;
        buf[i++] = c; print("*");
    }
    buf[i]=0; return i;
}

void _start() {
    // Drain keyboard buffer
    for (int _i=0; _i<200; _i++) yield();
    while (tox_keyavail()) tox_getchar();

    // Check if /C:/etc/users exists
    int usersz = tox_stat("/C:/etc/users");
    if (usersz <= 0) {
        // No user database — boot as root directly
        tox_setenv("USER", "root");
        tox_setuid(0);
        tox_set_admin(1);
        tox_exec("/C:/BSM/SystemT/shell.elf");
        tox_exit();
    }

    // Load users file
    char ubuf[4096]; ubuf[0]=0;
    if (usersz < 4095) {
        int fd = tox_open("/C:/etc/users", 1);
        int n = tox_read(fd, (uint8_t*)ubuf, (uint32_t)usersz);
        tox_close(fd); if(n<0)n=0; ubuf[n]=0;
    }

    // Banner
    set_color(0x0B);
    print("\n  ToxenOS ");
    char ver[32];
    if (tox_sysctl("os.version", ver, sizeof(ver)) < 0) tox_strcpy(ver, "1.0");
    print(ver);
    print("\n");
    set_color(0x07);

    for (;;) {
        // Prompt
        set_color(0x07); print("\nlogin: ");
        char username[64]; read_visible(username, sizeof(username));
        if (!username[0]) continue;

        print("Password: "); set_color(0x08);
        char password[64]; read_pass(password, sizeof(password));
        set_color(0x07);

        // Search users file: format is username:HASH:role\n
        const char* p = ubuf;
        uint32_t uid = 0;
        int found = 0;
        int is_admin_user = 0;
        char found_role[16];

        while (*p) {
            // Parse username field
            char uname[64]; int ui=0;
            while (*p && *p!=':' && *p!='\n' && ui<63) uname[ui++]=*p++;
            uname[ui]=0;
            if (*p==':') p++;

            // Parse hash field
            char hash[128]; int hi=0;
            while (*p && *p!=':' && *p!='\n' && hi<127) hash[hi++]=*p++;
            hash[hi]=0;
            if (*p==':') p++;

            // Parse role field
            char role[16]; int ri=0;
            while (*p && *p!='\n' && ri<15) role[ri++]=*p++;
            role[ri]=0;
            if (*p=='\n') p++;

            uid++;
            if (str_eq(uname, username)) {
                // Verify password
                if (verify_password(password, hash)) {
                    found = 1;
                    is_admin_user = str_eq(role, "admin");
                    tox_strcpy(found_role, role);
                    break;
                } else {
                    // Wrong password — stop searching (don't reveal if user exists)
                    break;
                }
            }
        }

        if (!found) {
            set_color(0x0C); print("Login incorrect.\n"); set_color(0x07);
            tox_sleep(1000);
            continue;
        }

        // Successful login
        tox_setenv("USER", username);
        tox_setuid(uid);
        if (is_admin_user) {
            tox_set_admin(1);
            tox_setenv("ROLE", "admin");
        } else {
            tox_setenv("ROLE", "user");
        }

        // Print uid into env
        char uidstr[12]; my_itoa(uid, uidstr);
        tox_setenv("UID", uidstr);

        set_color(0x0A);
        print("Welcome, "); print(username); print("!\n");
        set_color(0x07);

        tox_exec("/C:/BSM/SystemT/shell.elf");
        // If exec fails fall back to embedded shell
        tox_exit();
    }
}
