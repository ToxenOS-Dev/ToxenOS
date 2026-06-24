// elf2nex64.c — Host-side tool to convert a GCC-built ELF64 binary into
// ToxenOS's native NEX64 format (Milestone 6). A fork of elf2nex.c, not
// a shared/parameterized version of it -- see the Milestone 6 plan for
// why: keeps the 32-bit elf2nex.c/nex.h untouched, and the two formats'
// distinct magics give "wrong arch" rejection for free in the kernel
// loader.
// Usage: elf2nex64 <input.elf64> <output.nex64>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../include/elf64.h"
#include "../include/nex64.h"

static uint8_t* read_file(const char* path, long* size_out)
{
    FILE* f = fopen(path, "rb");
    if (!f) { perror("open"); exit(1); }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t* buf = (uint8_t*)malloc((size_t)size);
    if (!buf) { fprintf(stderr, "elf2nex64: out of memory\n"); exit(1); }

    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "elf2nex64: short read on %s\n", path);
        exit(1);
    }
    fclose(f);

    *size_out = size;
    return buf;
}

int main(int argc, char** argv)
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input.elf64> <output.nex64>\n", argv[0]);
        return 1;
    }

    long elf_size;
    uint8_t* elf_buf = read_file(argv[1], &elf_size);

    if (elf_size < (long)sizeof(elf64_header_t)) {
        fprintf(stderr, "elf2nex64: %s too small to be an ELF64 file\n", argv[1]);
        return 1;
    }

    elf64_header_t* eh = (elf64_header_t*)elf_buf;
    if (eh->magic != ELF64_MAGIC) {
        fprintf(stderr, "elf2nex64: %s is not an ELF file\n", argv[1]);
        return 1;
    }
    if (eh->bits != ELFCLASS64 || eh->endian != ELFDATA2LSB ||
        eh->machine != EM_X86_64 || eh->type != ET_EXEC64) {
        fprintf(stderr, "elf2nex64: %s is not a little-endian ELF64 EM_X86_64 ET_EXEC binary\n", argv[1]);
        return 1;
    }
    if (eh->phentsize < sizeof(elf64_phdr_t)) {
        fprintf(stderr, "elf2nex64: unexpected program header size\n");
        return 1;
    }
    if ((long)eh->phoff + (long)eh->phnum * eh->phentsize > elf_size) {
        fprintf(stderr, "elf2nex64: program header table runs past end of file\n");
        return 1;
    }
    if (eh->entry < USER64_ELF_BASE || eh->entry >= USER64_ELF_BASE + USER64_ELF_MAX_SIZE) {
        fprintf(stderr, "elf2nex64: entry point 0x%llx outside user ELF64 region\n",
                (unsigned long long)eh->entry);
        return 1;
    }

    // Collect PT_LOAD segments, skipping linker-generated metadata
    // segments that fall entirely below USER64_ELF_BASE -- same filter
    // the kernel applies when loading (kernel/exec64.c).
    nex64_seg_t segs[32];
    int seg_count = 0;

    for (int i = 0; i < eh->phnum; i++) {
        elf64_phdr_t* ph = (elf64_phdr_t*)(elf_buf + eh->phoff + (uint64_t)i * eh->phentsize);
        if (ph->type != PT_LOAD64) continue;
        if (ph->memsz == 0) continue;
        if (ph->vaddr + ph->memsz <= USER64_ELF_BASE) continue;

        if (ph->filesz > ph->memsz) {
            fprintf(stderr, "elf2nex64: segment %d has filesz > memsz\n", i);
            return 1;
        }
        if ((long)ph->offset + (long)ph->filesz > elf_size) {
            fprintf(stderr, "elf2nex64: segment %d data runs past end of file\n", i);
            return 1;
        }
        if (ph->vaddr + ph->memsz > USER64_ELF_BASE + USER64_ELF_MAX_SIZE) {
            fprintf(stderr, "elf2nex64: segment %d exceeds user ELF64 region\n", i);
            return 1;
        }
        if (seg_count >= (int)(sizeof(segs) / sizeof(segs[0]))) {
            fprintf(stderr, "elf2nex64: too many loadable segments\n");
            return 1;
        }

        segs[seg_count].vaddr  = ph->vaddr;
        segs[seg_count].filesz = ph->filesz;
        segs[seg_count].memsz  = ph->memsz;
        segs[seg_count].flags  = ph->flags;  // PF64_X/PF64_W/PF64_R bit values match NEX64_PF_*
        segs[seg_count].offset = ph->offset; // fixed up below
        seg_count++;
    }

    if (seg_count == 0) {
        fprintf(stderr, "elf2nex64: no loadable segments found\n");
        return 1;
    }

    nex64_header_t nh;
    nh.magic     = NEX64_MAGIC;
    nh.version   = 1;
    nh.seg_count = (uint32_t)seg_count;
    nh.reserved  = 0;
    nh.entry     = eh->entry;

    // Fix up each segment's offset to point into the new file, and pack
    // segment data back-to-back right after the segment table.
    uint64_t data_off = (uint64_t)sizeof(nex64_header_t) + (uint64_t)seg_count * sizeof(nex64_seg_t);
    uint64_t orig_offsets[32];
    for (int i = 0; i < seg_count; i++) {
        orig_offsets[i] = segs[i].offset;
        segs[i].offset  = data_off;
        data_off += segs[i].filesz;
    }

    FILE* out = fopen(argv[2], "wb");
    if (!out) { perror("open output"); return 1; }

    fwrite(&nh, sizeof(nh), 1, out);
    fwrite(segs, sizeof(nex64_seg_t), (size_t)seg_count, out);
    for (int i = 0; i < seg_count; i++)
        fwrite(elf_buf + orig_offsets[i], 1, segs[i].filesz, out);

    fclose(out);
    free(elf_buf);

    printf("elf2nex64: %s -> %s (%d segment%s, entry 0x%llx)\n",
           argv[1], argv[2], seg_count, seg_count == 1 ? "" : "s",
           (unsigned long long)nh.entry);
    return 0;
}
