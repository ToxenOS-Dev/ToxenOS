// ToxenOS/user/tox.h — userspace standard library for ToxenOS programs
// Include this in any user program that needs syscalls, strings, or memory.
#ifndef TOX_H
#define TOX_H
#include <stdint.h>

// ── Syscall ABI ───────────────────────────────────────────────────────────────
// All syscalls go through int 0x80.  Arguments go in ebx, ecx, edx.
// Return value comes back in eax.
#define SYSCALL_VECTOR 0x80

// Zero through four argument syscall wrappers.
// Use these instead of writing inline asm everywhere.
#define SYSCALL0(n) ({ \
    int _r; __asm__ volatile("int $0x80":"=a"(_r):"a"(n)); _r; })

#define SYSCALL1(n,a) ({ \
    int _r; __asm__ volatile("int $0x80":"=a"(_r):"a"(n),"b"((uint32_t)(a))); _r; })

#define SYSCALL2(n,a,b) ({ \
    int _r; __asm__ volatile("int $0x80":"=a"(_r):"a"(n),"b"((uint32_t)(a)),"c"((uint32_t)(b))); _r; })

#define SYSCALL3(n,a,b,c) ({ \
    int _r; __asm__ volatile("int $0x80":"=a"(_r):"a"(n),"b"((uint32_t)(a)),"c"((uint32_t)(b)),"d"((uint32_t)(c))); _r; })

// Syscall numbers — kept in sync with include/syscall.h
#define SYS_EXIT          0
#define SYS_PRINT         1
#define SYS_GETCHAR       2
#define SYS_GETPID        3
#define SYS_YIELD         4
#define SYS_ERASE         5
#define SYS_SETCOLOR      6
#define SYS_CLEAR         7
#define SYS_REBOOT        8
#define SYS_SHUTDOWN      9
#define SYS_READDIR       10
#define SYS_MKDIR         11
#define SYS_OPEN          12
#define SYS_READ          13
#define SYS_WRITE         14
#define SYS_CLOSE         15
#define SYS_REMOVE        16
#define SYS_STAT          17
#define SYS_ISDIR         18
#define SYS_GET_TTY       19
#define SYS_MY_TTY        20
#define SYS_EXEC          21
#define SYS_SPAWN         22
#define SYS_WAIT          23
#define SYS_SPAWN_TTY     24
#define SYS_SPAWN_EMBEDDED 25
#define SYS_GET_ARGS      26
#define SYS_SPAWN_ARGS    27
#define SYS_IS_ALIVE      28
#define SYS_KEYAVAIL      29
#define SYS_BMSG          30
#define SYS_PROC_LIST     31
#define SYS_KILL          32
#define SYS_SIGINT_TARGET 33
#define SYS_PIPE          34
#define SYS_SPAWN_PIPE    35
#define SYS_SLEEP         36
#define SYS_SBRK          37
#define SYS_SPAWN_INHERIT 38
#define SYS_NET_SEND_UDP  39
#define SYS_NET_POLL      40
#define SYS_NET_GET_IP    41
#define SYS_NET_UDP_RECV  42
#define SYS_TCP_CONNECT   44
#define SYS_TCP_SEND      45
#define SYS_TCP_RECV      46
#define SYS_TCP_CLOSE     47
#define SYS_PING          48
#define SYS_TLS_CONNECT   49
#define SYS_TLS_SEND      50
#define SYS_TLS_RECV      51
#define SYS_TLS_CLOSE     52
#define SYS_PAGE_FLAGS    53   // query page table flags for a virtual address (debug)
#define SYS_GETENV        54   // get environment variable
#define SYS_SETENV        55   // set environment variable
#define SYS_SYSCTL        56   // read kernel parameter
#define SYS_SYMLINK       57   // create symbolic link
#define SYS_READLINK      58   // read symlink target
#define SYS_GETTIME       59   // read CMOS RTC → formatted date/time string
#define SYS_CHMOD         60   // set file permission bits
#define SYS_GETMODE       61   // get file permission bits
#define SYS_ELEVATE       62   // request elevated privileges (UAC-style prompt)
#define SYS_IS_ADMIN      63   // returns 1 if current process is elevated
#define SYS_SET_ADMIN     64   // trusted: set is_admin flag (login use only)
#define SYS_PBKDF2        65   // PBKDF2-SHA256 password hashing
#define SYS_GETUID        75   // return current process uid
#define SYS_SETUID        76   // set current process uid (login only)
#define SYS_WHOAMI        77   // copy username string to buf
#define SYS_WAIT_STATUS   78   // wait(pid) and return exit code
#define SYS_ENV_LIST      79   // enumerate kernel env vars by index

// ── Output ────────────────────────────────────────────────────────────────────
static inline void print(const char* s)   { SYSCALL1(SYS_PRINT, s); }
static inline void set_color(uint8_t c)   { SYSCALL1(SYS_SETCOLOR, c); }
static inline void tox_clear(void)        { SYSCALL0(SYS_CLEAR); }
static inline char tox_getchar(void)      { return (char)SYSCALL0(SYS_GETCHAR); }
static inline void tox_erase(void)        { SYSCALL0(SYS_ERASE); }
static inline int  tox_keyavail(void)     { return SYSCALL0(SYS_KEYAVAIL); }

// ── Process ───────────────────────────────────────────────────────────────────
static inline void tox_exit(void)         { SYSCALL0(SYS_EXIT); }
static inline void yield(void)            { SYSCALL0(SYS_YIELD); }
static inline int  tox_getpid(void)       { return SYSCALL0(SYS_GETPID); }
static inline void tox_wait(int pid)      { SYSCALL1(SYS_WAIT, pid); }
static inline int  tox_kill(int pid)      { return SYSCALL1(SYS_KILL, pid); }
static inline int  tox_is_alive(int pid)  { return SYSCALL1(SYS_IS_ALIVE, pid); }
static inline void tox_sleep(uint32_t ms) { SYSCALL1(SYS_SLEEP, ms); }
static inline void tox_reboot(void)       { SYSCALL0(SYS_REBOOT); }
static inline void tox_shutdown(void)     { SYSCALL0(SYS_SHUTDOWN); }
static inline void tox_sigint_target(int pid) { SYSCALL1(SYS_SIGINT_TARGET, pid); }

static inline int tox_spawn(const char* path)
    { return SYSCALL1(SYS_SPAWN, path); }
static inline int tox_exec(const char* path)
    { return SYSCALL1(SYS_EXEC, path); }
static inline int tox_spawn_tty(const char* path, int tty)
    { return SYSCALL2(SYS_SPAWN_TTY, path, tty); }
static inline int tox_spawn_embedded(int tty)
    { return SYSCALL1(SYS_SPAWN_EMBEDDED, tty); }
static inline int tox_spawn_args(const char* path, int tty, const char* args)
    { return SYSCALL3(SYS_SPAWN_ARGS, path, tty, args); }
static inline int tox_spawn_inherit(const char* path, const char* args, const int* ilist)
    { return SYSCALL3(SYS_SPAWN_INHERIT, path, args, ilist); }
static inline int tox_my_tty(void)
    { return SYSCALL0(SYS_MY_TTY); }
static inline int tox_get_args(char* buf)
    { return SYSCALL1(SYS_GET_ARGS, buf); }
static inline int tox_proc_list(uint8_t* buf, uint32_t sz)
    { return SYSCALL2(SYS_PROC_LIST, buf, sz); }
static inline int tox_bmsg(char* buf, uint32_t sz)
    { return SYSCALL2(SYS_BMSG, buf, sz); }

// Spawn with overridden stdin/stdout fds (for pipelines)
static inline int tox_spawn_pipe(const char* path, const char* args, int stdin_fd, int stdout_fd) {
    uint32_t packed = ((uint32_t)(uint16_t)stdout_fd << 16) | (uint16_t)(uint32_t)stdin_fd;
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_SPAWN_PIPE),"b"(path),"c"(args),"d"(packed));
    return r;
}

// ── Filesystem ────────────────────────────────────────────────────────────────
static inline int tox_open(const char* p, int f)
    { return SYSCALL2(SYS_OPEN, p, f); }
static inline int tox_read(int fd, uint8_t* buf, uint32_t sz)
    { return SYSCALL3(SYS_READ, fd, buf, sz); }
static inline int tox_write(int fd, const uint8_t* buf, uint32_t sz)
    { return SYSCALL3(SYS_WRITE, fd, buf, sz); }
static inline int tox_close(int fd)
    { return SYSCALL1(SYS_CLOSE, fd); }
static inline int tox_stat(const char* p)
    { return SYSCALL1(SYS_STAT, p); }
static inline int tox_isdir(const char* p)
    { return SYSCALL1(SYS_ISDIR, p); }
static inline int tox_readdir(const char* p, char* out, uint32_t idx)
    { return SYSCALL3(SYS_READDIR, p, out, idx); }
static inline int tox_mkdir(const char* p)
    { return SYSCALL1(SYS_MKDIR, p); }
static inline int tox_remove(const char* p)
    { return SYSCALL1(SYS_REMOVE, p); }
static inline int tox_pipe(int* rfd, int* wfd)
    { return SYSCALL2(SYS_PIPE, rfd, wfd); }

// ── String helpers ────────────────────────────────────────────────────────────
// One canonical set — shell.c and init.c should use these instead of local copies.
static inline int tox_strlen(const char* s) {
    int i = 0; while (s[i]) i++; return i;
}
static inline void tox_strcpy(char* d, const char* s) {
    int i = 0; while (s[i]) { d[i] = s[i]; i++; } d[i] = 0;
}
static inline int tox_strcmp(const char* a, const char* b) {
    int i; for (i = 0; a[i] && b[i]; i++) if (a[i] != b[i]) return 1;
    return a[i] != b[i];
}
static inline int tox_strncmp(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return 1;
        if (!a[i]) break;
    }
    return 0;
}
static inline void tox_strcat(char* d, const char* s) {
    tox_strcpy(d + tox_strlen(d), s);
}
static inline int tox_starts_with(const char* s, const char* prefix) {
    int i = 0; while (prefix[i] && s[i] == prefix[i]) i++; return !prefix[i];
}
static inline void tox_memset(void* dst, uint8_t val, uint32_t n) {
    uint8_t* p = (uint8_t*)dst; while (n--) *p++ = val;
}
static inline void tox_memcpy(void* dst, const void* src, uint32_t n) {
    uint8_t* d = (uint8_t*)dst; const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
}

static inline void print_int(int n) {
    if (n < 0) { print("-"); n = -n; }
    if (n == 0) { print("0"); return; }
    char buf[16]; int i = 0;
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    buf[i] = 0;
    for (int a = 0, b = i - 1; a < b; a++, b--) {
        char t = buf[a]; buf[a] = buf[b]; buf[b] = t;
    }
    print(buf);
}

static inline void print_hex(uint32_t val) {
    const char* h = "0123456789ABCDEF";
    char buf[11] = "0x00000000"; buf[10] = 0;
    for (int i = 9; i >= 2; i--) { buf[i] = h[val & 0xF]; val >>= 4; }
    print(buf);
}

// ── File type helpers ─────────────────────────────────────────────────────────
typedef enum {
    FTYPE_TEXT, FTYPE_SCRIPT, FTYPE_IMAGE, FTYPE_AUDIO,
    FTYPE_VIDEO, FTYPE_ARCHIVE, FTYPE_DATA, FTYPE_FONT,
    FTYPE_BINARY, FTYPE_UNKNOWN
} file_type_t;

static inline char _lc(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static inline int _exteq(const char* e, const char* c) {
    int i; for (i = 0; e[i] && c[i]; i++) if (_lc(e[i]) != c[i]) return 0;
    return e[i] == c[i];
}
static inline file_type_t get_file_type(const char* name) {
    int last = -1; for (int i = 0; name[i]; i++) if (name[i] == '.') last = i;
    if (last < 0) return FTYPE_UNKNOWN;
    const char* e = name + last + 1;
    if (_exteq(e,"txt")||_exteq(e,"md")||_exteq(e,"c")||_exteq(e,"h")||
        _exteq(e,"cpp")||_exteq(e,"cfg")||_exteq(e,"ini")||_exteq(e,"log")||
        _exteq(e,"csv")||_exteq(e,"py")||_exteq(e,"json")||
        _exteq(e,"xml")||_exteq(e,"conf")||_exteq(e,"toml")||_exteq(e,"yaml"))
        return FTYPE_TEXT;
    if (_exteq(e,"sh")||_exteq(e,"txs")||_exteq(e,"tui")) return FTYPE_SCRIPT;
    if (_exteq(e,"png")||_exteq(e,"jpg")||_exteq(e,"bmp")||_exteq(e,"gif")) return FTYPE_IMAGE;
    if (_exteq(e,"mp3")||_exteq(e,"wav")||_exteq(e,"ogg")||_exteq(e,"flac")) return FTYPE_AUDIO;
    if (_exteq(e,"mp4")||_exteq(e,"mkv")||_exteq(e,"avi")) return FTYPE_VIDEO;
    if (_exteq(e,"zip")||_exteq(e,"tar")||_exteq(e,"gz")||_exteq(e,"iso")) return FTYPE_ARCHIVE;
    if (_exteq(e,"pdf")||_exteq(e,"db")||_exteq(e,"sqlite")) return FTYPE_DATA;
    if (_exteq(e,"ttf")||_exteq(e,"otf")||_exteq(e,"psf")) return FTYPE_FONT;
    if (_exteq(e,"elf")||_exteq(e,"bin")||_exteq(e,"o")) return FTYPE_BINARY;
    return FTYPE_UNKNOWN;
}
static inline uint8_t type_color(file_type_t t) {
    switch (t) {
        case FTYPE_TEXT:    return 0x0F;
        case FTYPE_SCRIPT:  return 0x0E;
        case FTYPE_IMAGE:   return 0x0D;
        case FTYPE_AUDIO:   return 0x0A;
        case FTYPE_VIDEO:   return 0x09;
        case FTYPE_ARCHIVE: return 0x0B;
        case FTYPE_DATA:    return 0x03;
        case FTYPE_FONT:    return 0x05;
        case FTYPE_BINARY:  return 0x08;
        default:            return 0x07;
    }
}
static inline const char* type_label(file_type_t t) {
    switch (t) {
        case FTYPE_TEXT:    return " [text]";
        case FTYPE_SCRIPT:  return " [script]";
        case FTYPE_IMAGE:   return " [image]";
        case FTYPE_AUDIO:   return " [audio]";
        case FTYPE_VIDEO:   return " [video]";
        case FTYPE_ARCHIVE: return " [archive]";
        case FTYPE_DATA:    return " [data]";
        case FTYPE_FONT:    return " [font]";
        case FTYPE_BINARY:  return " [binary]";
        default:            return "";
    }
}

// ── Network ───────────────────────────────────────────────────────────────────
static inline void     tox_net_poll(void)      { SYSCALL0(SYS_NET_POLL); }
static inline uint32_t tox_net_get_ip(void)    { return (uint32_t)SYSCALL0(SYS_NET_GET_IP); }
static inline int tox_net_udp_recv(uint16_t port, uint8_t* buf,
                                   uint16_t maxlen, uint32_t timeout_ms) {
    struct { uint16_t maxlen; uint16_t pad; uint32_t timeout_ms; }
        s = {maxlen, 0, timeout_ms};
    return SYSCALL3(SYS_NET_UDP_RECV, port, buf, &s);
}
static inline int tox_net_udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                                   const uint8_t* data, uint16_t len) {
    struct { const uint8_t* p; uint16_t l; } s = {data, len};
    uint32_t ports = ((uint32_t)src_port << 16) | dst_port;
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_NET_SEND_UDP),"b"(dst_ip),"c"(ports),"d"(&s));
    return r;
}
static inline int  tox_ping(uint32_t ip, uint16_t seq, uint32_t timeout_ms)
    { return SYSCALL3(SYS_PING, ip, seq, timeout_ms); }

// ── TCP ───────────────────────────────────────────────────────────────────────
static inline int tox_tcp_connect(uint32_t ip, uint16_t port)
    { return SYSCALL2(SYS_TCP_CONNECT, ip, port); }
static inline int tox_tcp_send(int sock, const uint8_t* buf, uint32_t len)
    { return SYSCALL3(SYS_TCP_SEND, sock, buf, len); }
static inline int tox_tcp_recv(int sock, uint8_t* buf, uint16_t maxlen, uint32_t timeout_ms) {
    struct { uint16_t maxlen; uint16_t pad; uint32_t timeout_ms; }
        s = {maxlen, 0, timeout_ms};
    return SYSCALL3(SYS_TCP_RECV, sock, buf, &s);
}
static inline void tox_tcp_close(int sock) { SYSCALL1(SYS_TCP_CLOSE, sock); }

// ── TLS ───────────────────────────────────────────────────────────────────────
static inline int tox_tls_connect(uint32_t ip, uint16_t port, const char* hostname)
    { return SYSCALL3(SYS_TLS_CONNECT, ip, port, hostname); }
static inline int tox_tls_send(int sock, const uint8_t* buf, uint32_t len)
    { return SYSCALL3(SYS_TLS_SEND, sock, buf, len); }
static inline int tox_tls_recv(int sock, uint8_t* buf, uint16_t maxlen, uint32_t timeout_ms) {
    struct { uint16_t maxlen; uint16_t pad; uint32_t timeout_ms; }
        s = {maxlen, 0, timeout_ms};
    return SYSCALL3(SYS_TLS_RECV, sock, buf, &s);
}
static inline void tox_tls_close(int sock) { SYSCALL1(SYS_TLS_CLOSE, sock); }

// ── DNS resolver ─────────────────────────────────────────────────────────────
// Resolve a hostname to an IPv4 address (queries 8.8.8.8:53 via UDP).
// If the string is already a dotted-decimal IP, it is parsed and returned directly.
// Returns the IP as a uint32_t (network byte order host-endian), 0 on failure.
static inline uint32_t tox_resolve(const char* host) {
    // Already an IP if it starts with a digit
    if (host[0] >= '0' && host[0] <= '9') {
        uint32_t r = 0;
        const char* s = host;
        for (int i = 0; i < 4; i++) {
            uint32_t n = 0;
            while (*s >= '0' && *s <= '9') { n = n*10 + (uint32_t)(*s-'0'); s++; }
            if (*s == '.') s++;
            r = (r << 8) | (n & 0xFF);
        }
        return r;
    }
    static uint8_t _tx[256], _rx[512];
    int pos = 0;
    _tx[pos++]=0x12; _tx[pos++]=0x34; // ID
    _tx[pos++]=0x01; _tx[pos++]=0x00; // flags: standard query, recursion desired
    _tx[pos++]=0x00; _tx[pos++]=0x01; // QDCOUNT=1
    _tx[pos++]=0x00; _tx[pos++]=0x00; // ANCOUNT=0
    _tx[pos++]=0x00; _tx[pos++]=0x00; // NSCOUNT=0
    _tx[pos++]=0x00; _tx[pos++]=0x00; // ARCOUNT=0
    // Encode QNAME
    const char* p = host;
    while (*p) {
        int l = 0; const char* q = p;
        while (*q && *q != '.') { q++; l++; }
        _tx[pos++] = (uint8_t)l;
        for (int i = 0; i < l; i++) _tx[pos++] = (uint8_t)p[i];
        p += l; if (*p == '.') p++;
    }
    _tx[pos++] = 0;
    _tx[pos++] = 0x00; _tx[pos++] = 0x01; // QTYPE=A
    _tx[pos++] = 0x00; _tx[pos++] = 0x01; // QCLASS=IN
    uint32_t dns_ip = (8u<<24|8u<<16|8u<<8|8u);
    for (int attempt = 0; attempt < 3; attempt++) {
        int r = tox_net_udp_send(dns_ip, 5353, 53, _tx, (uint16_t)pos);
        if (r < 0) { tox_net_poll(); tox_sleep(100); tox_net_poll(); continue; }
        int rlen = tox_net_udp_recv(5353, _rx, 512, 3000);
        if (rlen < 12) continue;
        uint16_t rid = (uint16_t)(_rx[0]<<8|_rx[1]);
        if (rid != 0x1234) continue;
        uint16_t ancount = (uint16_t)(_rx[6]<<8|_rx[7]);
        int rpos = 12;
        // Skip question name
        while (rpos < rlen && _rx[rpos]) {
            if ((_rx[rpos] & 0xC0) == 0xC0) { rpos += 2; goto _dns_ans; }
            rpos += _rx[rpos] + 1;
        }
        rpos++; // null terminator
        _dns_ans: rpos += 4; // skip QTYPE+QCLASS
        for (int i = 0; i < ancount && rpos < rlen; i++) {
            if ((_rx[rpos] & 0xC0) == 0xC0) rpos += 2;
            else { while (rpos < rlen && _rx[rpos]) rpos += _rx[rpos]+1; rpos++; }
            if (rpos + 10 > rlen) break;
            uint16_t type  = (uint16_t)(_rx[rpos]<<8|_rx[rpos+1]);
            uint16_t rdlen = (uint16_t)(_rx[rpos+8]<<8|_rx[rpos+9]);
            rpos += 10;
            if (type == 1 && rdlen == 4)
                return ((uint32_t)_rx[rpos]<<24)|((uint32_t)_rx[rpos+1]<<16)|
                       ((uint32_t)_rx[rpos+2]<<8)|(uint32_t)_rx[rpos+3];
            rpos += rdlen;
        }
    }
    return 0;
}

// Returns the page table flags for a virtual address (kernel use, debug).
// Bits: 0=present, 1=writable, 2=user-accessible. Returns 0 if not mapped.
static inline uint32_t tox_page_flags(uint32_t vaddr)
    { return (uint32_t)SYSCALL1(SYS_PAGE_FLAGS, vaddr); }

// ── sbrk-backed heap (malloc / free / realloc) ────────────────────────────────
static inline uint32_t _tox_sbrk(int32_t inc) {
    uint32_t r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_SBRK),"b"(inc)); return r;
}

typedef struct _tox_blk {
    uint32_t         size;
    uint8_t          used;
    struct _tox_blk* next;
} _tox_blk_t;

static _tox_blk_t* _heap_head = 0;

static inline int _heap_grow(uint32_t need) {
    uint32_t grow = (need + sizeof(_tox_blk_t) + 0xFFFu) & ~0xFFFu;
    uint32_t base = _tox_sbrk((int32_t)grow);
    if (base == (uint32_t)-1) return 0;
    _tox_blk_t* b = (_tox_blk_t*)base;
    b->size = grow - sizeof(_tox_blk_t);
    b->used = 0; b->next = 0;
    if (!_heap_head) { _heap_head = b; }
    else { _tox_blk_t* p = _heap_head; while (p->next) p = p->next; p->next = b; }
    return 1;
}

static inline void* malloc(uint32_t size) {
    if (!size) return 0;
    size = (size + 3) & ~3u;
    _tox_blk_t* b = _heap_head;
    while (b) {
        if (!b->used && b->size >= size) {
            if (b->size >= size + sizeof(_tox_blk_t) + 4) {
                _tox_blk_t* n = (_tox_blk_t*)((uint8_t*)b + sizeof(_tox_blk_t) + size);
                n->size = b->size - size - sizeof(_tox_blk_t);
                n->used = 0; n->next = b->next;
                b->next = n; b->size = size;
            }
            b->used = 1;
            return (uint8_t*)b + sizeof(_tox_blk_t);
        }
        b = b->next;
    }
    if (!_heap_grow(size)) return 0;
    return malloc(size);
}

static inline void free(void* ptr) {
    if (!ptr) return;
    _tox_blk_t* b = (_tox_blk_t*)((uint8_t*)ptr - sizeof(_tox_blk_t));
    b->used = 0;
    while (b->next && !b->next->used) {
        b->size += sizeof(_tox_blk_t) + b->next->size;
        b->next  = b->next->next;
    }
}

static inline void* realloc(void* ptr, uint32_t new_size) {
    if (!ptr) return malloc(new_size);
    if (!new_size) { free(ptr); return 0; }
    _tox_blk_t* b = (_tox_blk_t*)((uint8_t*)ptr - sizeof(_tox_blk_t));
    if (b->size >= new_size) return ptr;
    void* n = malloc(new_size);
    if (!n) return 0;
    uint8_t* s = (uint8_t*)ptr; uint8_t* d = (uint8_t*)n;
    for (uint32_t i = 0; i < b->size; i++) d[i] = s[i];
    free(ptr); return n;
}

// ── Environment variables ─────────────────────────────────────────────────────
// Get the value of an environment variable into buf (max maxlen bytes).
// Returns length written, or -1 if not set.
static inline int tox_getenv(const char* name, char* buf, uint32_t maxlen)
    { return SYSCALL3(SYS_GETENV, name, buf, maxlen); }

// Set (or create) an environment variable.  Empty value clears it.
static inline int tox_setenv(const char* name, const char* value)
    { return SYSCALL2(SYS_SETENV, name, value); }

// ── sysctl ────────────────────────────────────────────────────────────────────
// Read a kernel parameter by dotted name (e.g. "kern.version", "net.ip").
// Writes the value as a null-terminated string into buf.
// Returns length, or -1 if key not found.
static inline int tox_sysctl(const char* key, char* buf, uint32_t maxlen)
    { return SYSCALL3(SYS_SYSCTL, key, buf, maxlen); }

// ── Date/time ────────────────────────────────────────────────────────────────
// Read CMOS RTC. Writes "YYYY-MM-DD HH:MM:SS" into buf. Returns length or -1.
static inline int tox_gettime(char* buf, uint32_t maxlen)
    { return SYSCALL2(SYS_GETTIME, buf, maxlen); }

// ── Elevation / UAC ──────────────────────────────────────────────────────────
// Request elevated privileges. Kernel shows (Y/n) prompt, logs result.
// Returns 0 if granted, -1 if denied.
static inline int tox_elevate(const char* reason)
    { return SYSCALL1(SYS_ELEVATE, reason); }
// Returns 1 if the current process has been elevated.
static inline int tox_is_admin(void)
    { return SYSCALL0(SYS_IS_ADMIN); }
// Set admin flag directly — only call from trusted login code
static inline void tox_set_admin(int val)
    { SYSCALL1(SYS_SET_ADMIN, val); }
static inline uint32_t tox_getuid(void)
    { return (uint32_t)SYSCALL0(SYS_GETUID); }
static inline void tox_setuid(uint32_t uid)
    { SYSCALL1(SYS_SETUID, uid); }
static inline int tox_whoami(char* buf, uint32_t maxlen)
    { return SYSCALL2(SYS_WHOAMI, buf, maxlen); }
static inline int tox_wait_status(int pid)
    { return (int)SYSCALL1(SYS_WAIT_STATUS, pid); }
// Enumerate kernel env: copies key (32B) and val (128B) at index i.
// Returns 0 on success, -1 when index is out of range.
static inline int tox_envlist(int i, char* key, char* val)
    { return (int)SYSCALL3(SYS_ENV_LIST, i, key, val); }

// ── Disk access (for installer) ───────────────────────────────────────────────
#define SYS_INSTALL_DRIVE  73
#define SYS_INSTALL_CHUNK  74
#define SYS_DISK_SECTORS   70
#define SYS_DISK_WRITE     71
#define SYS_DISK_READ      72
static inline int tox_install_drive(uint8_t target)
    { return SYSCALL1(SYS_INSTALL_DRIVE, target); }
static inline int tox_install_chunk(uint8_t drive, uint32_t start, uint32_t count)
    { return SYSCALL3(SYS_INSTALL_CHUNK, drive, start, count); }
static inline int tox_disk_read(uint8_t drive, uint32_t lba,
                                 const uint8_t* buf, uint32_t sectors) {
    uint32_t params[2]; params[0]=(uint32_t)buf; params[1]=sectors;
    return SYSCALL3(SYS_DISK_READ, drive, lba, params);
}
static inline uint32_t tox_disk_sectors(uint8_t drive)
    { return SYSCALL1(SYS_DISK_SECTORS, drive); }
static inline int tox_disk_write(uint8_t drive, uint32_t lba,
                                  const uint8_t* buf, uint32_t sectors) {
    uint32_t params[2]; params[0]=(uint32_t)buf; params[1]=sectors;
    return SYSCALL3(SYS_DISK_WRITE, drive, lba, params);
}

// ── TxFS Snapshots ────────────────────────────────────────────────────────────
#define SYS_SNAP_CREATE    66
#define SYS_SNAP_RESTORE   67
#define SYS_SNAP_DELETE    68
#define SYS_SNAP_LIST      69
static inline int tox_snap_create(const char* name)
    { return SYSCALL1(SYS_SNAP_CREATE, name); }
static inline int tox_snap_restore(const char* name)
    { return SYSCALL1(SYS_SNAP_RESTORE, name); }
static inline int tox_snap_delete(const char* name)
    { return SYSCALL1(SYS_SNAP_DELETE, name); }
static inline int tox_snap_list(char names[][64], uint32_t* ts, int max)
    { return SYSCALL3(SYS_SNAP_LIST, names, ts, max); }

// ── PBKDF2-SHA256 password hashing ───────────────────────────────────────────
// Returns 0 on success. out must be 32 bytes.
static inline int tox_pbkdf2(const uint8_t* pwd, uint32_t plen,
                              const uint8_t* salt, uint32_t slen,
                              uint32_t iter, uint8_t* out, uint32_t olen) {
    uint32_t params[7];
    params[0]=(uint32_t)pwd;  params[1]=plen;
    params[2]=(uint32_t)salt; params[3]=slen;
    params[4]=iter;
    params[5]=(uint32_t)out;  params[6]=olen;
    return SYSCALL1(SYS_PBKDF2, params);
}

#define PBKDF2_ITERATIONS 50000
#define PBKDF2_HASH_LEN   32
#define PBKDF2_SALT_LEN   16

// Hex helpers
static inline char _nibble(uint8_t v) { return v<10?'0'+v:'a'+v-10; }
static inline uint8_t _unhex(char c) {
    if(c>='0'&&c<='9') return (uint8_t)(c-'0');
    if(c>='a'&&c<='f') return (uint8_t)(c-'a'+10);
    if(c>='A'&&c<='F') return (uint8_t)(c-'A'+10);
    return 0;
}
static inline void bin2hex(const uint8_t* b, int n, char* out) {
    for(int i=0;i<n;i++){out[i*2]=_nibble(b[i]>>4);out[i*2+1]=_nibble(b[i]&0xF);}
    out[n*2]=0;
}
static inline void hex2bin(const char* h, int n, uint8_t* out) {
    for(int i=0;i<n;i++) out[i]=(uint8_t)((_unhex(h[i*2])<<4)|_unhex(h[i*2+1]));
}

// Generate a salt using timer + pid as entropy
static inline void gen_salt(uint8_t* salt, uint32_t len) {
    char tbuf[32]; tox_gettime(tbuf, sizeof(tbuf));
    uint32_t t = 0;
    for(int i=0;tbuf[i];i++) t = t*31 + (uint8_t)tbuf[i];
    uint32_t p = (uint32_t)tox_getpid();
    uint32_t x = t ^ (p<<16) ^ (p>>3) ^ 0xDEADBEEFu;
    for(uint32_t i=0;i<len;i++){x=x*1664525u+1013904223u;salt[i]=(uint8_t)(x>>13);}
}

// Hash a password → "$pbkdf2$ITER$SALT_HEX$HASH_HEX"
// out must be at least 128 bytes
static inline int hash_password(const char* password, char* out) {
    uint8_t salt[PBKDF2_SALT_LEN], hash[PBKDF2_HASH_LEN];
    gen_salt(salt, PBKDF2_SALT_LEN);
    if (tox_pbkdf2((const uint8_t*)password, (uint32_t)tox_strlen(password),
                    salt, PBKDF2_SALT_LEN, PBKDF2_ITERATIONS, hash, PBKDF2_HASH_LEN) != 0)
        return -1;
    char salt_hex[PBKDF2_SALT_LEN*2+1], hash_hex[PBKDF2_HASH_LEN*2+1];
    bin2hex(salt, PBKDF2_SALT_LEN, salt_hex);
    bin2hex(hash, PBKDF2_HASH_LEN, hash_hex);
    // format: $pbkdf2$ITER$SALTHEX$HASHHEX
    tox_strcpy(out, "$pbkdf2$");
    // append iterations as decimal
    char ibuf[16]; int ii=14; ibuf[15]=0;
    uint32_t iv=PBKDF2_ITERATIONS;
    do { ibuf[ii--]='0'+(int)(iv%10); iv/=10; } while(iv);
    tox_strcat(out, ibuf+ii+1);
    tox_strcat(out, "$"); tox_strcat(out, salt_hex);
    tox_strcat(out, "$"); tox_strcat(out, hash_hex);
    return 0;
}

// Verify a password against a stored hash. Returns 1 if match.
static inline int verify_password(const char* password, const char* stored) {
    // Plain text password (migration / no hash prefix)
    if (stored[0] != '$') return tox_strcmp(password, stored) == 0;
    // Parse $pbkdf2$ITER$SALTHEX$HASHHEX
    if (stored[0]!='$'||stored[1]!='p'||stored[2]!='b') return 0;
    const char* p = stored + 8; // skip "$pbkdf2$"
    uint32_t iter = 0;
    while (*p && *p != '$') { iter = iter*10 + (uint32_t)(*p-'0'); p++; }
    if (*p == '$') p++;
    const char* salt_hex = p;
    while (*p && *p != '$') p++;
    if (*p != '$') return 0;
    int salt_hex_len = (int)(p - salt_hex);
    p++;
    const char* hash_hex = p;
    uint8_t salt[PBKDF2_SALT_LEN], expected[PBKDF2_HASH_LEN], computed[PBKDF2_HASH_LEN];
    hex2bin(salt_hex, salt_hex_len/2, salt);
    hex2bin(hash_hex, PBKDF2_HASH_LEN, expected);
    if (tox_pbkdf2((const uint8_t*)password, (uint32_t)tox_strlen(password),
                    salt, (uint32_t)(salt_hex_len/2), iter,
                    computed, PBKDF2_HASH_LEN) != 0) return 0;
    // Constant-time comparison
    uint8_t diff = 0;
    for (int i=0;i<PBKDF2_HASH_LEN;i++) diff |= computed[i]^expected[i];
    return diff == 0;
}

// ── File permissions ─────────────────────────────────────────────────────────
// Set permission bits (Unix-style octal: 0644, 0755, etc.)
static inline int tox_chmod(const char* path, uint32_t mode)
    { return SYSCALL2(SYS_CHMOD, path, mode); }
// Get permission bits. Returns mode or -1.
static inline int tox_getmode(const char* path)
    { return SYSCALL1(SYS_GETMODE, path); }

// ── Symlinks ──────────────────────────────────────────────────────────────────
// Create a symbolic link at 'path' pointing to 'target'.
static inline int tox_symlink(const char* target, const char* path)
    { return SYSCALL2(SYS_SYMLINK, target, path); }

// Read the target of a symbolic link into buf (not null-terminated by spec,
// but we add a null for convenience).  Returns bytes written, or -1.
static inline int tox_readlink(const char* path, char* buf, uint32_t maxlen)
    { return SYSCALL3(SYS_READLINK, path, buf, maxlen); }

#endif // TOX_H
