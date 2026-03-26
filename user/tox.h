// ToxenOS/user/tox.h
// Standard library for ToxenOS userspace programs.
// Include this in every external command program.
#ifndef TOX_H
#define TOX_H

#include <stdint.h>

// ── Output ────────────────────────────────────────────────────────────────────

static inline void print(const char* s)
{
    __asm__ volatile("int $0x80" :: "a"(1), "b"(s));
}

static inline void set_color(uint8_t c)
{
    __asm__ volatile("int $0x80" :: "a"(6), "b"((uint32_t)c));
}

static inline void tox_clear()
{
    __asm__ volatile("int $0x80" :: "a"(7));
}

// ── Process ───────────────────────────────────────────────────────────────────

static inline void exit()
{
    __asm__ volatile("int $0x80" :: "a"(0));
}

static inline void yield()
{
    __asm__ volatile("int $0x80" :: "a"(4));
}

static inline int getpid()
{
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(3)); return r;
}

// Get the argument string passed to this program
static inline int get_args(char* buf)
{
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(26), "b"(buf)); return r;
}

// ── Filesystem ────────────────────────────────────────────────────────────────

static inline int open(const char* path, int flags)
{
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(12), "b"(path), "c"(flags)); return r;
}

static inline int read(int fd, uint8_t* buf, uint32_t sz)
{
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(13), "b"(fd), "c"(buf), "d"(sz)); return r;
}

static inline int write(int fd, const uint8_t* buf, uint32_t sz)
{
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(14), "b"(fd), "c"(buf), "d"(sz)); return r;
}

static inline int close(int fd)
{
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(15), "b"(fd)); return r;
}

static inline int stat(const char* path)
{
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(17), "b"(path)); return r;
}

static inline int isdir(const char* path)
{
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(18), "b"(path)); return r;
}

static inline int readdir(const char* path, char* out, uint32_t idx)
{
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(10), "b"(path), "c"(out), "d"(idx)); return r;
}

static inline int mkdir(const char* path)
{
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(11), "b"(path)); return r;
}

static inline int remove(const char* path)
{
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(16), "b"(path)); return r;
}

// ── String helpers ────────────────────────────────────────────────────────────

static inline int tox_strlen(const char* s)
{
    int i = 0; while (s[i]) i++; return i;
}

static inline void tox_strcpy(char* d, const char* s)
{
    int i = 0; while (s[i]) { d[i] = s[i]; i++; } d[i] = 0;
}

static inline int tox_strcmp(const char* a, const char* b)
{
    int i; for (i=0; a[i]&&b[i]; i++) if (a[i]!=b[i]) return 1; return a[i]!=b[i];
}

static inline int tox_streq(const char* a, const char* b)
{
    return !tox_strcmp(a, b);
}

// Print a number as decimal
static inline void print_int(int n)
{
    if (n < 0) { print("-"); n = -n; }
    if (n == 0) { print("0"); return; }
    char buf[16]; int i = 0;
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    buf[i] = 0;
    // reverse
    for (int a=0,b=i-1; a<b; a++,b--) { char t=buf[a]; buf[a]=buf[b]; buf[b]=t; }
    print(buf);
}

// ── File type detection ───────────────────────────────────────────────────────

typedef enum {
    FTYPE_TEXT, FTYPE_SCRIPT, FTYPE_IMAGE, FTYPE_AUDIO,
    FTYPE_VIDEO, FTYPE_ARCHIVE, FTYPE_DATA, FTYPE_FONT,
    FTYPE_BINARY, FTYPE_UNKNOWN
} file_type_t;

static inline char tox_lc(char c) { return (c>='A'&&c<='Z') ? c+32 : c; }

static inline int ext_eq(const char* ext, const char* cmp)
{
    int i; for (i=0; ext[i]&&cmp[i]; i++) if (tox_lc(ext[i])!=cmp[i]) return 0;
    return ext[i]==cmp[i];
}

static inline file_type_t get_file_type(const char* name)
{
    int last=-1;
    for (int i=0; name[i]; i++) if (name[i]=='.') last=i;
    if (last<0) return FTYPE_UNKNOWN;
    const char* e = name+last+1;
    if (ext_eq(e,"txt")||ext_eq(e,"md")||ext_eq(e,"cfg")||ext_eq(e,"ini")||
        ext_eq(e,"log")||ext_eq(e,"csv")||ext_eq(e,"c")||ext_eq(e,"h")||
        ext_eq(e,"cpp")||ext_eq(e,"sh")||ext_eq(e,"py")||ext_eq(e,"html")||
        ext_eq(e,"css")||ext_eq(e,"js")||ext_eq(e,"json")||ext_eq(e,"xml")||
        ext_eq(e,"conf")||ext_eq(e,"rst")||ext_eq(e,"tsv")||ext_eq(e,"sql"))
        return FTYPE_TEXT;
    if (ext_eq(e,"txs")||ext_eq(e,"tui")||ext_eq(e,"tdui")) return FTYPE_SCRIPT;
    if (ext_eq(e,"png")||ext_eq(e,"jpg")||ext_eq(e,"jpeg")||ext_eq(e,"bmp")||
        ext_eq(e,"gif")||ext_eq(e,"ico")||ext_eq(e,"svg")||ext_eq(e,"tif")||
        ext_eq(e,"webp")||ext_eq(e,"ppm")||ext_eq(e,"raw")||ext_eq(e,"psd"))
        return FTYPE_IMAGE;
    if (ext_eq(e,"mp3")||ext_eq(e,"wav")||ext_eq(e,"ogg")||ext_eq(e,"flac")||
        ext_eq(e,"aac")||ext_eq(e,"m4a")||ext_eq(e,"mid"))
        return FTYPE_AUDIO;
    if (ext_eq(e,"mp4")||ext_eq(e,"mkv")||ext_eq(e,"avi")||ext_eq(e,"mov")||
        ext_eq(e,"wmv")||ext_eq(e,"webm"))
        return FTYPE_VIDEO;
    if (ext_eq(e,"zip")||ext_eq(e,"tar")||ext_eq(e,"gz")||ext_eq(e,"bz2")||
        ext_eq(e,"7z")||ext_eq(e,"rar")||ext_eq(e,"iso")||ext_eq(e,"img"))
        return FTYPE_ARCHIVE;
    if (ext_eq(e,"pdf")||ext_eq(e,"doc")||ext_eq(e,"docx")||ext_eq(e,"db")||
        ext_eq(e,"sqlite")||ext_eq(e,"yaml")||ext_eq(e,"toml")||ext_eq(e,"yml"))
        return FTYPE_DATA;
    if (ext_eq(e,"ttf")||ext_eq(e,"otf")||ext_eq(e,"woff")||ext_eq(e,"psf"))
        return FTYPE_FONT;
    if (ext_eq(e,"elf")||ext_eq(e,"bin")||ext_eq(e,"exe")||ext_eq(e,"o")||
        ext_eq(e,"so")||ext_eq(e,"a"))
        return FTYPE_BINARY;
    return FTYPE_UNKNOWN;
}

static inline uint8_t type_color(file_type_t t)
{
    switch(t) {
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

static inline const char* type_label(file_type_t t)
{
    switch(t) {
        case FTYPE_TEXT:    return " [text]";
        case FTYPE_SCRIPT:  return " [script]";
        case FTYPE_IMAGE:   return " [image]";
        case FTYPE_AUDIO:   return " [audio]";
        case FTYPE_VIDEO:   return " [video]";
        case FTYPE_ARCHIVE: return " [archive]";
        case FTYPE_DATA:    return " [data]";
        case FTYPE_FONT:    return " [font]";
        case FTYPE_BINARY:  return " [binary]";
        default:            return " [unknown]";
    }
}

#endif
