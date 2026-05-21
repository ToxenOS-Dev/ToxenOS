// ToxenOS/user/init.c — PID 1
// Login system with user roles (admin/user) and first-boot setup.
#include "tox.h"

static int str_eq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}
static int str_empty(const char* s) { return !s || !s[0]; }

static int read_visible(char* buf, int max) {
    int i = 0;
    while (i < max - 1) {
        char c = tox_getchar();
        if (c == '\n' || c == '\r') { buf[i]=0; print("\n"); return i; }
        if ((c=='\b'||c==127) && i>0) { i--; tox_erase(); continue; }
        if (c < 0x20 || c > 0x7E) continue;
        buf[i++]=c; char s[2]={c,0}; print(s);
    }
    buf[i]=0; return i;
}

static int read_pass(char* buf, int max) {
    int i = 0;
    while (i < max - 1) {
        char c = tox_getchar();
        if (c == '\n' || c == '\r') { buf[i]=0; print("\n"); return i; }
        if ((c=='\b'||c==127) && i>0) { i--; tox_erase(); continue; }
        if (c < 0x20 || c > 0x7E) continue;
        buf[i++]=c; print("*");
    }
    buf[i]=0; return i;
}

static void spaces(int n) { while (n-- > 0) print(" "); }

#define MAX_USERS 16
static char u_name[MAX_USERS][64];
static char u_pwd [MAX_USERS][128]; // PBKDF2 hashes are ~112 chars
static char u_role[MAX_USERS][16];  // "admin" or "user"
static int  u_count = 0;

static void load_users(void) {
    u_count = 0;
    int size = tox_stat("/C:/etc/users");
    if (size <= 0 || size > 4095) {
        tox_strcpy(u_name[0], "admin"); u_pwd[0][0]=0;
        tox_strcpy(u_role[0], "admin"); u_count=1; return;
    }
    char* buf = malloc((uint32_t)size+1);
    if (!buf) return;
    int fd = tox_open("/C:/etc/users", 1);
    int n = tox_read(fd, (uint8_t*)buf, (uint32_t)size);
    tox_close(fd); if (n<0) n=0; buf[n]=0;
    const char* p = buf;
    while (*p && u_count < MAX_USERS) {
        int ni=0,pi=0,ri=0;
        while (*p&&*p!=':'&&*p!='\n'&&ni<63) u_name[u_count][ni++]=*p++;
        u_name[u_count][ni]=0;
        if (*p==':') { p++;
            while (*p&&*p!=':'&&*p!='\n'&&pi<127) u_pwd[u_count][pi++]=*p++;
        }
        u_pwd[u_count][pi]=0;
        if (*p==':') { p++;
            while (*p&&*p!='\n'&&ri<15) u_role[u_count][ri++]=*p++;
        }
        u_role[u_count][ri]=0;
        if (str_empty(u_role[u_count])) tox_strcpy(u_role[u_count],"user");
        while (*p&&*p!='\n') p++;
        if (*p=='\n') p++;
        if (ni>0) u_count++;
    }
    free(buf);
    if (!u_count) {
        tox_strcpy(u_name[0],"admin"); u_pwd[0][0]=0;
        tox_strcpy(u_role[0],"admin"); u_count=1;
    }
}

// Rewrite /etc/users from the in-memory tables
static void save_users(void) {
    int fd = tox_open("/C:/etc/users", 2|4);
    if (fd < 0) return;
    for (int i = 0; i < u_count; i++) {
        tox_write(fd,(uint8_t*)u_name[i],(uint32_t)tox_strlen(u_name[i]));
        tox_write(fd,(uint8_t*)":",1);
        tox_write(fd,(uint8_t*)u_pwd[i],(uint32_t)tox_strlen(u_pwd[i]));
        tox_write(fd,(uint8_t*)":",1);
        tox_write(fd,(uint8_t*)u_role[i],(uint32_t)tox_strlen(u_role[i]));
        tox_write(fd,(uint8_t*)"\n",1);
    }
    tox_close(fd);
}

static void drain_keyboard(void) {
    for (int _d=0;_d<1000;_d++) yield();
    while (tox_keyavail()) tox_getchar();
    for (int _d=0;_d<200;_d++) yield();
    while (tox_keyavail()) tox_getchar();
}


// All content left-aligned from col 49 (center of 128-col terminal)
#define LOGIN_COL 49

static void show_user_screen(void) {
    tox_clear();
    for (int i=0;i<18;i++) print("\n");

    // Welcome banner centered
    spaces(LOGIN_COL);
    set_color(0x07); print("==== Welcome to ");
    set_color(0x06); print("ToxenOS");
    set_color(0x07); print(" ====\n\n\n");

    // Login prompt
    spaces(LOGIN_COL);
}

void _start() {
    tox_setenv("PATH",  "/C:/BSM/SystemT:/C:/BSM/usr/lst");
    tox_setenv("HOME",  "/C:");
    tox_setenv("SHELL", "/shell.elf");
    tox_setenv("TERM",  "toxterm");
    tox_setenv("OS",    "ToxenOS");

    // Load persisted hostname
    {
        int sz = tox_stat("/C:/etc/reg");
        if (sz>0&&sz<4096) {
            char* rb=malloc((uint32_t)sz+1);
            if (rb) {
                int fd=tox_open("/C:/etc/reg",1);
                int nn=tox_read(fd,(uint8_t*)rb,(uint32_t)sz);
                tox_close(fd); if(nn<0)nn=0; rb[nn]=0;
                const char* p=rb;
                while(*p) {
                    if(p[0]=='h'&&p[1]=='o'&&p[2]=='s'&&p[3]=='t'&&
                       p[4]=='n'&&p[5]=='a'&&p[6]=='m'&&p[7]=='e'&&p[8]=='=') {
                        p+=9; char hn[64]; int hi=0;
                        while(*p&&*p!='\n'&&hi<63) hn[hi++]=*p++;
                        hn[hi]=0; if(hi>0) tox_setenv("hostname",hn); break;
                    }
                    while(*p&&*p!='\n') p++;
                    if(*p=='\n') p++;
                }
                free(rb);
            }
        }
    }

    drain_keyboard();
    load_users();

    // Main login loop
    while (1) {
        load_users();  // reload every time so new users (adduser) appear immediately
        show_user_screen();

        char username[64], password[64];
        // Linux-style: "hostname login:"
        char hn[64]; hn[0]=0;
        tox_getenv("hostname", hn, sizeof(hn));
        if (!hn[0]) tox_strcpy(hn, "toxenos");
        set_color(0x0B); print(hn); set_color(0x07); print(" login: ");
        read_visible(username, sizeof(username));
        if (!username[0]) continue;

        spaces(LOGIN_COL); print("Password: ");
        read_pass(password, sizeof(password));

        // Find user and verify (supports both plaintext and PBKDF2 hashes)
        int found=-1;
        for (int i=0;i<u_count;i++) {
            if (str_eq(u_name[i],username) && verify_password(password, u_pwd[i])) {
                found=i; break;
            }
        }

        if (found<0) {
            spaces(LOGIN_COL); set_color(0x0C); print("Login incorrect.\n"); set_color(0x07);
            for (int _d=0;_d<300;_d++) yield();
            continue;
        }

        // Set user env and privileges
        tox_setenv("USER", username);
        tox_setenv("USERROLE", u_role[found]);

        // Admin users get is_admin=1 automatically
        if (str_eq(u_role[found],"admin")) {
            tox_set_admin(1);
        }

        spaces(LOGIN_COL);
        set_color(0x0A); print("Welcome, "); print(username);
        if (str_eq(u_role[found],"admin")) {
            set_color(0x06); print("  [admin]");
        }
        set_color(0x07); print("\n\n");
        for (int _d=0;_d<200;_d++) yield();
        tox_clear();

        int shell_pid = tox_spawn_embedded(0);
        tox_wait(shell_pid);
        drain_keyboard();
    }
}
