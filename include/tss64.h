#ifndef TSS64_H
#define TSS64_H

#include <stdint.h>

// Real x86_64 TSS (Intel SDM Vol3 Fig 8-11) -- 104 bytes. Long mode has
// no hardware task-switching, so this struct exists purely to hand the
// CPU two things: which kernel stack to load on a privilege-raising
// interrupt (RSP0, used when the IDT gate's IST field is 0) and the
// dedicated IST1-IST7 stacks used when a gate's IST field is nonzero.
typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} tss64_t;

_Static_assert(sizeof(tss64_t) == 104, "tss64_t must be exactly 104 bytes");

// Builds the TSS descriptor in gdt64[] (the .tss_lo/.tss_hi slots at
// TSS64_SEL reserved by boot64.asm), sets RSP0 and IST1, then loads the
// TSS via ltr. Per-task RSP0 updates later just write tss64.rsp0 -- no
// descriptor rebuild needed once this has run once.
void tss64_init(void);

#endif // TSS64_H
