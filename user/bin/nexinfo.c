// user/bin/nexinfo.c — NEX format viewer: prints header/segment info for a
// .nex file. Unlike `hex`, this understands the format instead of dumping
// raw bytes. Inspects the exact path given — no command-resolver extension
// guessing (same exactness contract as `rm`).
#include "../tox.h"
#include "../../include/nex.h"
#include "../../include/elf.h"

static void print_flags(uint32_t flags) {
    print(flags & NEX_PF_R ? "R" : "-");
    print(flags & NEX_PF_W ? "W" : "-");
    print(flags & NEX_PF_X ? "X" : "-");
}

void _start() {
    char args[256]; tox_get_args(args);
    if (!args[0]) {
        print("usage: nexinfo <file.nex>\n");
        tox_exit();
    }

    int size = tox_stat(args);
    if (size < 0) {
        set_color(0x0C); print("nexinfo: cannot open: "); print(args); print("\n");
        set_color(0x07); tox_exit();
    }
    if (size < (int)sizeof(nex_header_t)) {
        set_color(0x0C); print("nexinfo: invalid file: too small for a NEX header\n");
        set_color(0x07); tox_exit();
    }

    uint8_t* buf = malloc((uint32_t)size);
    if (!buf) {
        set_color(0x0C); print("nexinfo: out of memory\n");
        set_color(0x07); tox_exit();
    }

    int fd = tox_open(args, 1);
    if (fd < 0) {
        set_color(0x0C); print("nexinfo: cannot open: "); print(args); print("\n");
        free(buf); set_color(0x07); tox_exit();
    }
    int n = tox_read(fd, buf, (uint32_t)size);
    tox_close(fd);
    if (n < (int)sizeof(nex_header_t)) {
        set_color(0x0C); print("nexinfo: read failed: "); print(args); print("\n");
        free(buf); set_color(0x07); tox_exit();
    }

    nex_header_t* hdr = (nex_header_t*)buf;

    if (hdr->magic == ELF_MAGIC) {
        set_color(0x0C); print("not a NEX file: ELF executable\n");
        set_color(0x07); free(buf); tox_exit();
    }
    if (hdr->magic != NEX_MAGIC) {
        set_color(0x0C); print("not a NEX file: bad magic\n");
        set_color(0x07); free(buf); tox_exit();
    }
    if (hdr->version != 1) {
        set_color(0x0C); print("nexinfo: unsupported NEX version: ");
        print_int(hdr->version); print("\n");
        set_color(0x07); free(buf); tox_exit();
    }

    uint32_t seg_table_off = sizeof(nex_header_t);
    uint32_t seg_table_sz  = (uint32_t)hdr->seg_count * sizeof(nex_seg_t);
    if (seg_table_sz > (uint32_t)size - seg_table_off) {
        set_color(0x0C); print("nexinfo: invalid file: segment table runs past end of file\n");
        set_color(0x07); free(buf); tox_exit();
    }

    set_color(0x0B); print(args); print(": NEX executable\n"); set_color(0x07);
    print("  magic:    NEX\\x01\n");
    print("  version:  "); print_int(hdr->version); print("\n");
    print("  entry:    "); print_hex(hdr->entry); print("\n");
    print("  segments: "); print_int(hdr->seg_count); print("\n");

    for (int i = 0; i < hdr->seg_count; i++) {
        nex_seg_t* seg = (nex_seg_t*)(buf + seg_table_off + (uint32_t)i * sizeof(nex_seg_t));
        print("\n  segment "); print_int(i); print(":\n");
        print("    vaddr:   ");  print_hex(seg->vaddr);            print("\n");
        print("    offset:  ");  print_hex(seg->offset);           print("\n");
        print("    filesz:  ");  print_int((int)seg->filesz);      print(" bytes\n");
        print("    memsz:   ");  print_int((int)seg->memsz);       print(" bytes\n");
        print("    flags:   ");  print_flags(seg->flags);          print("\n");
    }

    free(buf);
    tox_exit();
}
