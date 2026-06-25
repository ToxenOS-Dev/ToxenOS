// ToxenOS/system_manager/system_tools/command_tools/nexinfo.c —
// Milestone 11: NEX64 format viewer, the 64-bit analogue of the 32-bit
// user/bin/nexinfo.c (design inspiration only -- the on-disk layout is
// completely different, see include/nex64.h). User programs don't
// include kernel headers, so the magic numbers and header/segment
// layout are defined locally here, mirroring the 32-bit precedent of a
// self-contained struct. No malloc/heap/brk syscall exists yet, so this
// reads into a static buffer instead (kernel/exec64.c's own EXEC64_FILE_MAX
// load cap is 64KB, but nexinfo only ever needs the header + segment
// table, comfortably under the 4KB read here).
#include <stdint.h>
#include "tox64.h"

#define NEX64_MAGIC 0x0258454Eu  // must match include/nex64.h
#define ELF64_MAGIC 0x464C457Fu  // must match include/elf64.h

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t seg_count;
    uint32_t reserved;
    uint64_t entry;
} __attribute__((packed)) nex64_header_t;

typedef struct {
    uint64_t vaddr;
    uint64_t offset;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t flags;
} __attribute__((packed)) nex64_seg_t;

#define NEX64_PF_R 0x4
#define NEX64_PF_W 0x2
#define NEX64_PF_X 0x1

#define BUF_SIZE 4096
static uint8_t buf[BUF_SIZE];

static int my_strlen(const char* s) {
    int i = 0;
    while (s[i]) i++;
    return i;
}

static void put(const char* s) {
    sys_write(s, (uint64_t)my_strlen(s));
}

static void put_uint(uint64_t v) {
    char rev[24];
    int rn = 0;
    if (v == 0) rev[rn++] = '0';
    while (v > 0) { rev[rn++] = (char)('0' + (v % 10)); v /= 10; }
    char out[24];
    int n = 0;
    while (rn > 0) out[n++] = rev[--rn];
    out[n] = 0;
    put(out);
}

static void put_hex(uint64_t v) {
    const char* h = "0123456789ABCDEF";
    char out[19];
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 16; i++) { out[2 + (15 - i)] = h[v & 0xF]; v >>= 4; }
    out[18] = 0;
    put(out);
}

static void put_flags(uint64_t flags) {
    char s[4];
    s[0] = (flags & NEX64_PF_R) ? 'R' : '-';
    s[1] = (flags & NEX64_PF_W) ? 'W' : '-';
    s[2] = (flags & NEX64_PF_X) ? 'X' : '-';
    s[3] = 0;
    put(s);
}

void _start(void) {
    char args[128];
    int64_t n = sys_get_args(args, sizeof(args));
    if (n < 0) args[0] = 0;

    if (!args[0]) {
        put("nexinfo: missing argument\n");
        sys_exit(1);
    }

    int64_t fd = sys_open(args);
    if (fd < 0) {
        put("nexinfo: cannot open "); put(args); put("\n");
        sys_exit(1);
    }

    uint64_t total = 0;
    for (;;) {
        if (total >= BUF_SIZE) break;
        int64_t got = sys_read((int)fd, (char*)(buf + total), BUF_SIZE - total);
        if (got <= 0) break;
        total += (uint64_t)got;
    }
    sys_close((int)fd);

    if (total < 4) {
        put("nexinfo: unrecognized format\n");
        sys_exit(1);
    }

    uint32_t magic = ((uint32_t*)buf)[0];

    if (magic == ELF64_MAGIC) {
        put(args); put(": not NEX64 -- ELF64 executable\n");
        sys_exit(1);
    }

    if (magic != NEX64_MAGIC || total < sizeof(nex64_header_t)) {
        put(args); put(": unrecognized format\n");
        sys_exit(1);
    }

    nex64_header_t* hdr = (nex64_header_t*)buf;
    uint64_t seg_table_off = sizeof(nex64_header_t);
    uint64_t seg_table_sz  = (uint64_t)hdr->seg_count * sizeof(nex64_seg_t);
    if (seg_table_off + seg_table_sz > total) {
        put(args); put(": invalid file -- segment table runs past what was read\n");
        sys_exit(1);
    }

    put(args); put(": NEX64 executable\n");
    put("  magic:    "); put_hex(hdr->magic); put("\n");
    put("  version:  "); put_uint(hdr->version); put("\n");
    put("  entry:    "); put_hex(hdr->entry); put("\n");
    put("  segments: "); put_uint(hdr->seg_count); put("\n");

    for (uint32_t i = 0; i < hdr->seg_count; i++) {
        nex64_seg_t* seg = (nex64_seg_t*)(buf + seg_table_off + (uint64_t)i * sizeof(nex64_seg_t));
        put("\n  segment "); put_uint(i); put(":\n");
        put("    vaddr:   "); put_hex(seg->vaddr); put("\n");
        put("    offset:  "); put_hex(seg->offset); put("\n");
        put("    filesz:  "); put_uint(seg->filesz); put(" bytes\n");
        put("    memsz:   "); put_uint(seg->memsz); put(" bytes\n");
        put("    flags:   "); put_flags(seg->flags); put("\n");
    }

    sys_exit(0);

    for (;;) { }  // unreachable
}
