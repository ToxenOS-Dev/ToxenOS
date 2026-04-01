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
#endif

// ── Memory allocator ─────────────────────────────────────────────────────────
// Simple bump allocator with free list
// Heap sits in BSS at a fixed address — no syscall needed

#define TOX_HEAP_SIZE (256 * 1024)  // 256 KB heap per process

typedef struct tox_block {
    uint32_t          size;   // payload size in bytes
    uint8_t           used;   // 1 = allocated, 0 = free
    struct tox_block* next;   // next block in list
} tox_block_t;

static uint8_t  _tox_heap[TOX_HEAP_SIZE];
static uint8_t  _tox_heap_init = 0;
static tox_block_t* _tox_heap_head = 0;

static inline void _tox_heap_setup() {
    _tox_heap_head = (tox_block_t*)_tox_heap;
    _tox_heap_head->size = TOX_HEAP_SIZE - sizeof(tox_block_t);
    _tox_heap_head->used = 0;
    _tox_heap_head->next = 0;
    _tox_heap_init = 1;
}

static inline void* malloc(uint32_t size) {
    if (!size) return 0;
    if (!_tox_heap_init) _tox_heap_setup();

    // Align to 4 bytes
    size = (size + 3) & ~3;

    tox_block_t* b = _tox_heap_head;
    while (b) {
        if (!b->used && b->size >= size) {
            // Split block if there's enough room left
            if (b->size >= size + sizeof(tox_block_t) + 4) {
                tox_block_t* next = (tox_block_t*)((uint8_t*)b + sizeof(tox_block_t) + size);
                next->size = b->size - size - sizeof(tox_block_t);
                next->used = 0;
                next->next = b->next;
                b->next    = next;
                b->size    = size;
            }
            b->used = 1;
            return (uint8_t*)b + sizeof(tox_block_t);
        }
        b = b->next;
    }
    return 0;  // out of memory
}

static inline void free(void* ptr) {
    if (!ptr) return;
    tox_block_t* b = (tox_block_t*)((uint8_t*)ptr - sizeof(tox_block_t));
    b->used = 0;

    // Coalesce with next block if also free
    while (b->next && !b->next->used) {
        b->size += sizeof(tox_block_t) + b->next->size;
        b->next  = b->next->next;
    }
}

static inline void* realloc(void* ptr, uint32_t new_size) {
    if (!ptr) return malloc(new_size);
    if (!new_size) { free(ptr); return 0; }

    tox_block_t* b = (tox_block_t*)((uint8_t*)ptr - sizeof(tox_block_t));
    if (b->size >= new_size) return ptr;  // already big enough

    void* new_ptr = malloc(new_size);
    if (!new_ptr) return 0;

    // Copy old data
    uint8_t* src = (uint8_t*)ptr;
    uint8_t* dst = (uint8_t*)new_ptr;
    for (uint32_t i = 0; i < b->size; i++) dst[i] = src[i];
    free(ptr);
    return new_ptr;
}
