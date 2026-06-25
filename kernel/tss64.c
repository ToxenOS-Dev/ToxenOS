// kernel/tss64.c — Milestone 3B: 64-bit TSS (RSP0 + IST1 only).
//
// Long mode has no hardware task-switching; the only reason a TSS exists
// here is to give the CPU a kernel stack to load on privilege-raising
// interrupts. RSP0 is used whenever an IDT gate's IST field is 0 (every
// gate except the double-fault one this milestone); IST1 is used only by
// the double-fault gate (vector 8), so a faulting/corrupt stack can never
// take double-fault delivery down with it.
#include <stdint.h>
#include "../include/tss64.h"
#include "../include/gdt64.h"

#define TSS64_RSP0_STACK_SIZE  (16 * 1024)
#define TSS64_IST1_STACK_SIZE  (8 * 1024)

static tss64_t tss64 __attribute__((aligned(16)));

static uint8_t rsp0_stack[TSS64_RSP0_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t ist1_stack[TSS64_IST1_STACK_SIZE] __attribute__((aligned(16)));

// gdt64[] lives in kernel/boot64.asm's .boot section; TSS64_SEL's two
// qword slots (.tss_lo/.tss_hi) are reserved there as zeros and filled
// in here once tss64's link-time address is known.
extern uint64_t gdt64[];

static inline void ltr(uint16_t selector) {
    __asm__ volatile ("ltr %0" : : "r"(selector));
}

void tss64_init(void) {
    for (uint32_t i = 0; i < sizeof(tss64); i++) ((uint8_t*)&tss64)[i] = 0;

    tss64.rsp0 = (uint64_t)(rsp0_stack + sizeof(rsp0_stack));
    tss64.ist1 = (uint64_t)(ist1_stack + sizeof(ist1_stack));
    tss64.iomap_base = sizeof(tss64);  // no I/O bitmap

    // Build the 16-byte TSS descriptor in place. Base is the TSS's own
    // linear (virtual) address -- paging is already active by the time
    // this runs, and the CPU uses linear addresses for TSS access, so
    // no physical-address conversion is needed here (unlike CR3, which
    // boot64.asm loads before paging exists).
    uint64_t base  = (uint64_t)&tss64;
    uint32_t limit = sizeof(tss64) - 1;

    uint64_t lo = 0;
    lo |= (limit & 0xFFFFull);
    lo |= (base & 0xFFFFFFull) << 16;
    lo |= 0x89ull << 40;                  // P=1,DPL=0,S=0,Type=1001 (64-bit TSS avail)
    lo |= ((uint64_t)(limit >> 16) & 0xFull) << 48;
    lo |= ((base >> 24) & 0xFFull) << 56;

    uint64_t hi = (base >> 32) & 0xFFFFFFFFull;

    gdt64[TSS64_SEL / 8]     = lo;
    gdt64[TSS64_SEL / 8 + 1] = hi;

    ltr(TSS64_SEL);
}

void tss64_set_kernel_stack(uint64_t rsp0) {
    tss64.rsp0 = rsp0;
}

uint64_t tss64_get_kernel_stack(void) {
    return tss64.rsp0;
}
