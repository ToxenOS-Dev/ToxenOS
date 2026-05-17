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
        _exteq(e,"csv")||_exteq(e,"sh")||_exteq(e,"py")||_exteq(e,"json")||
        _exteq(e,"xml")||_exteq(e,"conf")||_exteq(e,"toml")||_exteq(e,"yaml"))
        return FTYPE_TEXT;
    if (_exteq(e,"txs")||_exteq(e,"tui")) return FTYPE_SCRIPT;
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

// ── Symlinks ──────────────────────────────────────────────────────────────────
// Create a symbolic link at 'path' pointing to 'target'.
static inline int tox_symlink(const char* target, const char* path)
    { return SYSCALL2(SYS_SYMLINK, target, path); }

// Read the target of a symbolic link into buf (not null-terminated by spec,
// but we add a null for convenience).  Returns bytes written, or -1.
static inline int tox_readlink(const char* path, char* buf, uint32_t maxlen)
    { return SYSCALL3(SYS_READLINK, path, buf, maxlen); }

#endif // TOX_H
