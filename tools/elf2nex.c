// elf2nex.c — Host-side tool to convert a GCC-built ELF32 binary into
// ToxenOS's native NEX format.
// Usage: elf2nex <input.elf> <output.nex>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../include/elf.h"
#include "../include/nex.h"

#define USER_ELF_BASE     0x10000000u   // keep in sync with include/memmap.h
#define USER_ELF_MAX_SIZE (4u * 1024u * 1024u)

static uint8_t* read_file(const char* path, long* size_out)
{
    FILE* f = fopen(path, "rb");
    if (!f) { perror("open"); exit(1); }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t* buf = (uint8_t*)malloc((size_t)size);
    if (!buf) { fprintf(stderr, "elf2nex: out of memory\n"); exit(1); }

    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "elf2nex: short read on %s\n", path);
        exit(1);
    }
    fclose(f);

    *size_out = size;
    return buf;
}

int main(int argc, char** argv)
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input.elf> <output.nex>\n", argv[0]);
        return 1;
    }

    long elf_size;
    uint8_t* elf_buf = read_file(argv[1], &elf_size);

    if (elf_size < (long)sizeof(elf_header_t)) {
        fprintf(stderr, "elf2nex: %s too small to be an ELF file\n", argv[1]);
        return 1;
    }

    elf_header_t* eh = (elf_header_t*)elf_buf;
    if (eh->magic != ELF_MAGIC) {
        fprintf(stderr, "elf2nex: %s is not an ELF file\n", argv[1]);
        return 1;
    }
    if (eh->bits != 1 || eh->endian != 1 || eh->machine != EM_386 || eh->type != ET_EXEC) {
        fprintf(stderr, "elf2nex: %s is not a little-endian ELF32 EM_386 ET_EXEC binary\n", argv[1]);
        return 1;
    }
    if (eh->phentsize < sizeof(elf_phdr_t)) {
        fprintf(stderr, "elf2nex: unexpected program header size\n");
        return 1;
    }
    if ((long)eh->phoff + (long)eh->phnum * eh->phentsize > elf_size) {
        fprintf(stderr, "elf2nex: program header table runs past end of file\n");
        return 1;
    }
    if (eh->entry < USER_ELF_BASE || eh->entry >= USER_ELF_BASE + USER_ELF_MAX_SIZE) {
        fprintf(stderr, "elf2nex: entry point 0x%x outside user ELF region\n", eh->entry);
        return 1;
    }

    // Collect PT_LOAD segments, skipping linker-generated metadata segments
    // that fall entirely below USER_ELF_BASE — same filter the kernel
    // applies when loading ELF directly (see load_elf_into_dir).
    nex_seg_t segs[32];
    int seg_count = 0;

    for (int i = 0; i < eh->phnum; i++) {
        elf_phdr_t* ph = (elf_phdr_t*)(elf_buf + eh->phoff + (uint32_t)i * eh->phentsize);
        if (ph->type != PT_LOAD) continue;
        if (ph->memsz == 0) continue;
        if (ph->vaddr + ph->memsz <= USER_ELF_BASE) continue;

        if (ph->filesz > ph->memsz) {
            fprintf(stderr, "elf2nex: segment %d has filesz > memsz\n", i);
            return 1;
        }
        if ((long)ph->offset + (long)ph->filesz > elf_size) {
            fprintf(stderr, "elf2nex: segment %d data runs past end of file\n", i);
            return 1;
        }
        if (ph->vaddr + ph->memsz > USER_ELF_BASE + USER_ELF_MAX_SIZE) {
            fprintf(stderr, "elf2nex: segment %d exceeds user ELF region\n", i);
            return 1;
        }
        if (seg_count >= (int)(sizeof(segs) / sizeof(segs[0]))) {
            fprintf(stderr, "elf2nex: too many loadable segments\n");
            return 1;
        }

        segs[seg_count].vaddr  = ph->vaddr;
        segs[seg_count].filesz = ph->filesz;
        segs[seg_count].memsz  = ph->memsz;
        segs[seg_count].flags  = ph->flags;  // PF_X/PF_W/PF_R bit values match NEX_PF_*
        segs[seg_count].offset = ph->offset; // fixed up below
        seg_count++;
    }

    if (seg_count == 0) {
        fprintf(stderr, "elf2nex: no loadable segments found\n");
        return 1;
    }

    nex_header_t nh;
    nh.magic     = NEX_MAGIC;
    nh.version   = 1;
    nh.seg_count = (uint16_t)seg_count;
    nh.entry     = eh->entry;

    // Fix up each segment's offset to point into the new file, and pack
    // segment data back-to-back right after the segment table.
    uint32_t data_off = (uint32_t)sizeof(nex_header_t) + (uint32_t)seg_count * sizeof(nex_seg_t);
    uint32_t orig_offsets[32];
    for (int i = 0; i < seg_count; i++) {
        orig_offsets[i] = segs[i].offset;
        segs[i].offset  = data_off;
        data_off += segs[i].filesz;
    }

    FILE* out = fopen(argv[2], "wb");
    if (!out) { perror("open output"); return 1; }

    fwrite(&nh, sizeof(nh), 1, out);
    fwrite(segs, sizeof(nex_seg_t), (size_t)seg_count, out);
    for (int i = 0; i < seg_count; i++)
        fwrite(elf_buf + orig_offsets[i], 1, segs[i].filesz, out);

    fclose(out);
    free(elf_buf);

    printf("elf2nex: %s -> %s (%d segment%s, entry 0x%x)\n",
           argv[1], argv[2], seg_count, seg_count == 1 ? "" : "s", nh.entry);
    return 0;
}
