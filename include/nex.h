#ifndef NEX_H
#define NEX_H

#include <stdint.h>

// ToxenOS native executable format ("NEX").
//
// Unlike ELF, this carries only what the loader needs: an entry point and a
// flat table of loadable segments. No sections, no string tables, no
// relocations. Produced from ELF binaries by tools/elf2nex.

#define NEX_MAGIC 0x0158454Eu   // bytes 'N' 'E' 'X' 0x01, little-endian read

// segment flags (match include/elf.h's PF_* bit values)
#define NEX_PF_X  0x1
#define NEX_PF_W  0x2
#define NEX_PF_R  0x4

typedef struct
{
    uint32_t magic;      // NEX_MAGIC
    uint16_t version;    // format version, currently 1
    uint16_t seg_count;  // number of nex_seg_t entries following the header
    uint32_t entry;      // entry point (virtual address)
} __attribute__((packed)) nex_header_t;

typedef struct
{
    uint32_t vaddr;
    uint32_t offset;     // offset into file of segment data
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;      // NEX_PF_*
} __attribute__((packed)) nex_seg_t;

#endif // NEX_H
