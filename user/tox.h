// ToxenOS/user/tox.h — standard library for external commands
#ifndef TOX_H
#define TOX_H
#include <stdint.h>

// ── Output ────────────────────────────────────────────────────────────────────
static inline void print(const char* s) {
    __asm__ volatile("int $0x80" :: "a"(1), "b"(s));
}
static inline void set_color(uint8_t c) {
    __asm__ volatile("int $0x80" :: "a"(6), "b"((uint32_t)c));
}

// ── Process ───────────────────────────────────────────────────────────────────
static inline void tox_exit() {
    __asm__ volatile("int $0x80" :: "a"(0));
}
static inline void yield() {
    __asm__ volatile("int $0x80" :: "a"(4));
}
static inline void tox_clear() {
    __asm__ volatile("int $0x80" :: "a"(7));
}
static inline int get_args(char* buf) {
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(26), "b"(buf)); return r;
}

// ── Filesystem ────────────────────────────────────────────────────────────────
static inline int tox_open(const char* p, int f) {
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(12), "b"(p), "c"(f)); return r;
}
static inline int tox_read(int fd, uint8_t* buf, uint32_t sz) {
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(13), "b"(fd), "c"(buf), "d"(sz)); return r;
}
static inline int tox_write(int fd, const uint8_t* buf, uint32_t sz) {
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(14), "b"(fd), "c"(buf), "d"(sz)); return r;
}
static inline int tox_close(int fd) {
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(15), "b"(fd)); return r;
}
static inline int tox_stat(const char* p) {
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(17), "b"(p)); return r;
}
static inline int tox_isdir(const char* p) {
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(18), "b"(p)); return r;
}
static inline int tox_readdir(const char* p, char* out, uint32_t idx) {
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(10), "b"(p), "c"(out), "d"(idx)); return r;
}
static inline int tox_mkdir(const char* p) {
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(11), "b"(p)); return r;
}
static inline int tox_remove(const char* p) {
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(16), "b"(p)); return r;
}
static inline void tox_wait(int pid) {
    __asm__ volatile("int $0x80" :: "a"(23), "b"(pid));
}
static inline void tox_sigint_target(int pid) {
    __asm__ volatile("int $0x80" :: "a"(33), "b"(pid));
}
static inline int tox_pipe(int* rfd, int* wfd) {
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(34), "b"(rfd), "c"(wfd)); return r;
}
static inline void tox_sleep(uint32_t ms) {
    __asm__ volatile("int $0x80" :: "a"(36), "b"(ms));
}
static inline uint32_t tox_sbrk(int32_t inc) {
    uint32_t r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(37), "b"(inc)); return r;
}
// Spawn with FD inheritance. ilist = {child_fd, parent_gfd, ..., -1}
static inline int tox_spawn_inherit(const char* path, const char* args, const int* ilist) {
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(38), "b"(path), "c"(args), "d"(ilist)); return r;
}
// Network
static inline uint32_t tox_net_get_ip() {
    uint32_t r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(41)); return r;
}
static inline void tox_net_poll() {
    __asm__ volatile("int $0x80" :: "a"(40));
}
static inline int tox_net_udp_recv(uint16_t port, uint8_t* buf,
                                    uint16_t maxlen, uint32_t timeout_ms) {
    struct { uint16_t maxlen; uint16_t pad; uint32_t timeout_ms; }
        s = {maxlen, 0, timeout_ms};
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(42), "b"(port), "c"(buf), "d"(&s));
    return r;
}
// TCP
static inline int tox_tcp_connect(uint32_t ip, uint16_t port) {
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(44), "b"(ip), "c"(port)); return r;
}
static inline int tox_tcp_send(int sock, const uint8_t* buf, uint32_t len) {
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(45), "b"(sock), "c"(buf), "d"(len)); return r;
}
static inline int tox_tcp_recv(int sock, uint8_t* buf, uint16_t maxlen, uint32_t timeout_ms) {
    struct { uint16_t maxlen; uint16_t pad; uint32_t timeout_ms; } s = {maxlen, 0, timeout_ms};
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(46), "b"(sock), "c"(buf), "d"(&s)); return r;
}
static inline void tox_tcp_close(int sock) {
    __asm__ volatile("int $0x80" :: "a"(47), "b"(sock));
}
static inline int tox_ping(uint32_t ip, uint16_t seq, uint32_t timeout_ms) {
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(48), "b"(ip), "c"(seq), "d"(timeout_ms));
    return r;
}
static inline int tox_net_udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                                    const uint8_t* data, uint16_t len) {
    struct { const uint8_t* p; uint16_t l; } s = {data, len};
    uint32_t ports = ((uint32_t)src_port << 16) | dst_port;
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(39), "b"(dst_ip), "c"(ports), "d"(&s));
    return r;
}

// ── Strings ───────────────────────────────────────────────────────────────────
static inline int tox_strlen(const char* s) {
    int i=0; while(s[i]) i++; return i;
}
static inline void tox_strcpy(char* d, const char* s) {
    int i=0; while(s[i]){d[i]=s[i];i++;} d[i]=0;
}
static inline int tox_strcmp(const char* a, const char* b) {
    int i; for(i=0;a[i]&&b[i];i++) if(a[i]!=b[i]) return 1; return a[i]!=b[i];
}
static inline void tox_strcat(char* d, const char* s) {
    int i=tox_strlen(d); tox_strcpy(d+i, s);
}
static inline void print_int(int n) {
    if(n<0){print("-");n=-n;}
    if(n==0){print("0");return;}
    char buf[16]; int i=0;
    while(n>0){buf[i++]='0'+(n%10);n/=10;}
    buf[i]=0;
    for(int a=0,b=i-1;a<b;a++,b--){char t=buf[a];buf[a]=buf[b];buf[b]=t;}
    print(buf);
}

// ── File types ────────────────────────────────────────────────────────────────
typedef enum {
    FTYPE_TEXT, FTYPE_SCRIPT, FTYPE_IMAGE, FTYPE_AUDIO,
    FTYPE_VIDEO, FTYPE_ARCHIVE, FTYPE_DATA, FTYPE_FONT,
    FTYPE_BINARY, FTYPE_UNKNOWN
} file_type_t;

static inline char _lc(char c){return(c>='A'&&c<='Z')?c+32:c;}
static inline int _exteq(const char* e,const char* c){
    int i; for(i=0;e[i]&&c[i];i++) if(_lc(e[i])!=c[i]) return 0; return e[i]==c[i];
}
static inline file_type_t get_file_type(const char* name) {
    int last=-1; for(int i=0;name[i];i++) if(name[i]=='.') last=i;
    if(last<0) return FTYPE_UNKNOWN;
    const char* e=name+last+1;
    if(_exteq(e,"txt")||_exteq(e,"md")||_exteq(e,"c")||_exteq(e,"h")||
       _exteq(e,"cpp")||_exteq(e,"cfg")||_exteq(e,"ini")||_exteq(e,"log")||
       _exteq(e,"csv")||_exteq(e,"sh")||_exteq(e,"py")||_exteq(e,"json")||
       _exteq(e,"xml")||_exteq(e,"conf")||_exteq(e,"toml")||_exteq(e,"yaml"))
        return FTYPE_TEXT;
    if(_exteq(e,"txs")||_exteq(e,"tui")) return FTYPE_SCRIPT;
    if(_exteq(e,"png")||_exteq(e,"jpg")||_exteq(e,"bmp")||_exteq(e,"gif"))
        return FTYPE_IMAGE;
    if(_exteq(e,"mp3")||_exteq(e,"wav")||_exteq(e,"ogg")||_exteq(e,"flac"))
        return FTYPE_AUDIO;
    if(_exteq(e,"mp4")||_exteq(e,"mkv")||_exteq(e,"avi")) return FTYPE_VIDEO;
    if(_exteq(e,"zip")||_exteq(e,"tar")||_exteq(e,"gz")||_exteq(e,"iso"))
        return FTYPE_ARCHIVE;
    if(_exteq(e,"pdf")||_exteq(e,"db")||_exteq(e,"sqlite")) return FTYPE_DATA;
    if(_exteq(e,"ttf")||_exteq(e,"otf")||_exteq(e,"psf")) return FTYPE_FONT;
    if(_exteq(e,"elf")||_exteq(e,"bin")||_exteq(e,"o")) return FTYPE_BINARY;
    return FTYPE_UNKNOWN;
}
static inline uint8_t type_color(file_type_t t) {
    switch(t){
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
    switch(t){
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

// ── Time ──────────────────────────────────────────────────────────────────────
// (tox_sleep is defined above in the process section)

// ── Memory — sbrk-backed heap ─────────────────────────────────────────────────
// Uses SYS_SBRK (37) to grow the heap on demand instead of a fixed BSS array.
// malloc/free/realloc are usable by any program that includes tox.h.

static inline uint32_t _tox_sbrk(int32_t inc) {
    uint32_t r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(37), "b"(inc));
    return r;
}

typedef struct _tox_blk {
    uint32_t         size;  // payload bytes
    uint8_t          used;
    struct _tox_blk* next;
} _tox_blk_t;

static _tox_blk_t* _heap_head = 0;

// Grow the heap by at least `need` bytes, add a new free block.
static inline int _heap_grow(uint32_t need) {
    // Round up to 4KB pages
    uint32_t grow = (need + sizeof(_tox_blk_t) + 0xFFF) & ~0xFFFu;
    uint32_t base = _tox_sbrk((int32_t)grow);
    if (base == (uint32_t)-1) return 0;

    _tox_blk_t* b = (_tox_blk_t*)base;
    b->size = grow - sizeof(_tox_blk_t);
    b->used = 0;
    b->next = 0;

    // Append to end of list
    if (!_heap_head) {
        _heap_head = b;
    } else {
        _tox_blk_t* p = _heap_head;
        while (p->next) p = p->next;
        p->next = b;
    }
    return 1;
}

static inline void* malloc(uint32_t size) {
    if (!size) return 0;
    size = (size + 3) & ~3u;

    // Try existing free blocks first
    _tox_blk_t* b = _heap_head;
    while (b) {
        if (!b->used && b->size >= size) {
            if (b->size >= size + sizeof(_tox_blk_t) + 4) {
                _tox_blk_t* n = (_tox_blk_t*)((uint8_t*)b + sizeof(_tox_blk_t) + size);
                n->size = b->size - size - sizeof(_tox_blk_t);
                n->used = 0;
                n->next = b->next;
                b->next = n;
                b->size = size;
            }
            b->used = 1;
            return (uint8_t*)b + sizeof(_tox_blk_t);
        }
        b = b->next;
    }

    // No suitable block — grow heap
    if (!_heap_grow(size)) return 0;
    return malloc(size);  // retry after growing
}

static inline void free(void* ptr) {
    if (!ptr) return;
    _tox_blk_t* b = (_tox_blk_t*)((uint8_t*)ptr - sizeof(_tox_blk_t));
    b->used = 0;
    // Coalesce forward
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
    uint8_t* s = (uint8_t*)ptr;
    uint8_t* d = (uint8_t*)n;
    for (uint32_t i = 0; i < b->size; i++) d[i] = s[i];
    free(ptr);
    return n;
}

#endif
