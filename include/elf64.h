#ifndef ELF64_H
#define ELF64_H

#include <stdint.h>

// ELF64 structs, mirroring include/elf.h's style (named fields over the
// real binary layout) -- but NOTE: elf64_phdr_t's field order genuinely
// differs from elf_phdr_t (32-bit), not just field widths. Elf64_Phdr
// puts p_flags right after p_type; Elf32_Phdr puts it near the end.
// Copying the 32-bit struct shape verbatim here would misread every
// field past `type`.

#define ELF64_MAGIC   0x464C457F  // "\x7FELF" -- same first 4 bytes as ELF32
#define ELFCLASS64    2           // e_ident[EI_CLASS] -- 64-bit
#define ELFDATA2LSB   1           // e_ident[EI_DATA] -- little-endian

#define ET_EXEC64     2
#define EM_X86_64     62

#define PT_LOAD64     1

#define PF64_X        0x1
#define PF64_W        0x2
#define PF64_R        0x4

typedef struct {
    uint32_t magic;         // 0x7F 'E' 'L' 'F'
    uint8_t  bits;          // EI_CLASS -- must be ELFCLASS64
    uint8_t  endian;        // EI_DATA -- must be ELFDATA2LSB
    uint8_t  elf_version;
    uint8_t  os_abi;
    uint8_t  pad[8];
    uint16_t type;          // ET_EXEC64 etc
    uint16_t machine;       // must be EM_X86_64
    uint32_t version;
    uint64_t entry;         // entry point
    uint64_t phoff;         // program header offset
    uint64_t shoff;         // section header offset
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;     // program header entry size
    uint16_t phnum;         // number of program headers
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} __attribute__((packed)) elf64_header_t;

typedef struct {
    uint32_t type;          // PT_LOAD64 etc
    uint32_t flags;         // PF64_R/PF64_W/PF64_X -- right after type, unlike elf_phdr_t
    uint64_t offset;        // offset in file
    uint64_t vaddr;         // virtual address to load at
    uint64_t paddr;         // physical address (usually same/ignored)
    uint64_t filesz;        // bytes in file
    uint64_t memsz;         // bytes in memory (>= filesz)
    uint64_t align;
} __attribute__((packed)) elf64_phdr_t;

#endif // ELF64_H
