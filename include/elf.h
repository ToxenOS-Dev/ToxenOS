#ifndef ELF_H
#define ELF_H

#include <stdint.h>

// ELF magic
#define ELF_MAGIC       0x464C457F  // "\x7FELF"

// e_type
#define ET_EXEC         2           // executable

// e_machine
#define EM_386          3           // x86

// program header types
#define PT_LOAD         1           // loadable segment

// program header flags
#define PF_X            0x1         // execute
#define PF_W            0x2         // write
#define PF_R            0x4         // read

typedef struct
{
    uint32_t magic;         // 0x7F 'E' 'L' 'F'
    uint8_t  bits;          // 1=32bit, 2=64bit
    uint8_t  endian;        // 1=little, 2=big
    uint8_t  elf_version;
    uint8_t  os_abi;
    uint8_t  pad[8];
    uint16_t type;          // ET_EXEC etc
    uint16_t machine;       // EM_386 etc
    uint32_t version;
    uint32_t entry;         // entry point
    uint32_t phoff;         // program header offset
    uint32_t shoff;         // section header offset
    uint32_t flags;
    uint16_t ehsize;        // ELF header size
    uint16_t phentsize;     // program header entry size
    uint16_t phnum;         // number of program headers
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} __attribute__((packed)) elf_header_t;

typedef struct
{
    uint32_t type;          // PT_LOAD etc
    uint32_t offset;        // offset in file
    uint32_t vaddr;         // virtual address to load at
    uint32_t paddr;         // physical address (usually same)
    uint32_t filesz;        // bytes in file
    uint32_t memsz;         // bytes in memory (>= filesz)
    uint32_t flags;         // PF_R, PF_W, PF_X
    uint32_t align;
} __attribute__((packed)) elf_phdr_t;

int elf_load(const char* path, uint32_t* entry_out);

#endif