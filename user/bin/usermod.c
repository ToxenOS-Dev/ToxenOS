#include "../tox.h"

static int str_eq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int read_pass(char* buf, int max) {
    int i = 0;
    while (i < max - 1) {
        char c = tox_getchar();
        if (c == '\n' || c == '\r') { buf[i]=0; print("\n"); return i; }
        if ((c=='\b'||c==127)&&i>0) { i--; tox_erase(); continue; }
        if (c < 0x20 || c > 0x7E) continue;
        buf[i++]=c; print("*");
    }
    buf[i]=0; return i;
}

void _start() {
    char args[128]; tox_get_args(args);
    if (!args[0]) {
        print("Usage: usermod <username> admin|user\n");
        print("       Grants or revokes admin role.\n");
        print("       Requires current admin password.\n");
        tox_exit();
    }

    // Parse: username role
    char target[64], role[16];
    int i=0,j=0;
    while (args[i]&&args[i]!=' '&&i<63) target[j++]=args[i++]; target[j]=0;
    while (args[i]==' ') i++;
    j=0;
    while (args[i]&&j<15) role[j++]=args[i++]; role[j]=0;

    if (!target[0]||!role[0]) {
        set_color(0x0C); print("usermod: usage: usermod <user> admin|user\n"); set_color(0x07); tox_exit();
    }
    if (!str_eq(role,"admin")&&!str_eq(role,"user")) {
        set_color(0x0C); print("usermod: role must be 'admin' or 'user'\n"); set_color(0x07); tox_exit();
    }

    // Must be running as admin
    if (!tox_is_admin()) {
        set_color(0x0C); print("usermod: permission denied — admin account required\n"); set_color(0x07); tox_exit();
    }

    // Confirm with admin password
    char admin_name[64]; admin_name[0]=0;
    tox_getenv("USER", admin_name, sizeof(admin_name));
    print("Confirm with admin password for "); print(admin_name); print(": ");
    char confirm_pass[64]; read_pass(confirm_pass, sizeof(confirm_pass));

    // Verify admin password against /etc/users
    int size = tox_stat("/C:/etc/users");
    if (size <= 0) { set_color(0x0C); print("usermod: cannot read /etc/users\n"); set_color(0x07); tox_exit(); }
    char* buf = malloc((uint32_t)size+1);
    if (!buf) { set_color(0x0C); print("usermod: out of memory\n"); set_color(0x07); tox_exit(); }
    int fd = tox_open("/C:/etc/users",1);
    int n = tox_read(fd,(uint8_t*)buf,(uint32_t)size);
    tox_close(fd); if(n<0)n=0; buf[n]=0;

    // Check admin password and find/update target user
    int admin_ok=0, target_found=0;
    char out[4096]; int oi=0;
    const char* p=buf;
    while(*p) {
        char uname[64],pwd[64],urole[16]; int ni=0,pi=0,ri=0;
        const char* ls=p;
        while(*p&&*p!=':'&&*p!='\n'&&ni<63) uname[ni++]=*p++;uname[ni]=0;
        if(*p==':'){p++; while(*p&&*p!=':'&&*p!='\n'&&pi<63) pwd[pi++]=*p++;}pwd[pi]=0;
        if(*p==':'){p++; while(*p&&*p!='\n'&&ri<15) urole[ri++]=*p++;}urole[ri]=0;
        while(*p&&*p!='\n') p++; if(*p=='\n') p++;

        if (str_eq(uname,admin_name)&&str_eq(pwd,confirm_pass)) admin_ok=1;

        // Build output line
        const char* wr = str_eq(uname,target) ? role : urole;
        if (str_eq(uname,target)) target_found=1;
        for(int k=0;uname[k]&&oi<4094;k++) out[oi++]=uname[k];
        out[oi++]=':';
        for(int k=0;pwd[k]&&oi<4094;k++) out[oi++]=pwd[k];
        out[oi++]=':';
        for(int k=0;wr[k]&&oi<4094;k++) out[oi++]=wr[k];
        out[oi++]='\n';
        (void)ls;
    }
    out[oi]=0;
    free(buf);

    if (!admin_ok) {
        set_color(0x0C); print("usermod: incorrect admin password\n"); set_color(0x07); tox_exit();
    }
    if (!target_found) {
        set_color(0x0C); print("usermod: user not found: "); print(target); print("\n"); set_color(0x07); tox_exit();
    }

    fd=tox_open("/C:/etc/users",2|4);
    if(fd>=0){tox_write(fd,(uint8_t*)out,(uint32_t)oi);tox_close(fd);}

    set_color(0x0A); print("usermod: "); print(target);
    print(" is now "); print(role); print("\n"); set_color(0x07);
    tox_exit();
}
