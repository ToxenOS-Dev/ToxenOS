// kernel/lapic64.c — Milestone 24: LAPIC virtual-wire mode for the
// 64-bit kernel. Port of kernel/kernel.c's lapic_virtual_wire_init
// (32-bit), adapted to this kernel's own mapping mechanism: rather than
// a pre-allocated page-directory slot, the LAPIC's fixed physical MMIO
// base (0xFEE00000) is mapped on demand by extending the low identity
// map (pml4[0] -> pdpt_low), the same technique kernel/fbterm64.c's
// map_fb_phys already uses for the framebuffer LFB -- reused here
// rather than shared, since the two have nothing else in common and
// fbterm64.c's own helper is file-static.
#include <stdint.h>
#include "../include/lapic64.h"
#include "../include/physmem64.h"
#include "../include/klog.h"

#define LAPIC64_BASE 0xFEE00000ULL

#define PT_PRESENT        0x001ULL
#define PT_WRITABLE       0x002ULL
#define PT_CACHE_DISABLE  0x010ULL
#define PT_HUGE_2M        0x080ULL

// boot64.asm's PDPT for the low identity map (pml4[0] -> pdpt_low -> ...).
// .bootdata VMA == PA, so this symbol resolves to the physical/low-
// identity virtual address -- safe to dereference directly from C.
extern uint64_t pdpt_low[512];

// Extends the identity map to cover LAPIC64_BASE (marked cache-disable,
// since this is MMIO -- control-register writes must take effect
// immediately, not sit in a cache line) and returns that physical
// address as the virtual address (VA == PA under the identity map), or
// 0 on failure.
static uint64_t map_lapic_phys(uint64_t phys) {
    uint64_t addr = phys & ~0x1FFFFFULL; // 2MB-align down
    uint32_t gb_idx = (uint32_t)(addr >> 30);
    uint32_t pd_idx = (uint32_t)((addr >> 21) & 0x1FF);
    if (gb_idx >= 512) return 0;

    if (!(pdpt_low[gb_idx] & PT_PRESENT)) {
        uint64_t pd_phys = physmem64_alloc_page();
        if (!pd_phys) return 0;
        pdpt_low[gb_idx] = pd_phys | PT_PRESENT | PT_WRITABLE;
    }

    uint64_t* pd_ptr = (uint64_t*)physmem64_to_virt(pdpt_low[gb_idx] & ~0xFFFULL);
    if (!(pd_ptr[pd_idx] & PT_PRESENT))
        pd_ptr[pd_idx] = addr | PT_PRESENT | PT_WRITABLE | PT_CACHE_DISABLE | PT_HUGE_2M;

    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");

    return phys; // identity-mapped: VA == PA
}

void lapic64_virtual_wire_init(void) {
    uint64_t va = map_lapic_phys(LAPIC64_BASE);
    if (!va) {
        klog("lapic64: failed to map LAPIC MMIO -- IRQ delivery may fail on real hardware\n");
        return;
    }

    volatile uint32_t* lapic = (volatile uint32_t*)(uintptr_t)va;

    // SVR (0x0F0): software-enable LAPIC (bit 8), spurious vector 0xFF.
    lapic[0x0F0 / 4] = 0x1FFu;
    // TPR (0x080): task priority 0 -- accept all interrupt priorities.
    lapic[0x080 / 4] = 0;
    // LVT LINT0 (0x350): ExtINT delivery, edge-triggered, unmasked -- on
    // each 8259 interrupt the CPU does an INTA cycle to the PIC, which
    // supplies the actual vector (0x20+), exactly as if wired directly.
    lapic[0x350 / 4] = 0x700u;
    // LVT LINT1 (0x360): NMI delivery, edge-triggered, unmasked.
    lapic[0x360 / 4] = 0x400u;

    klog("lapic64: virtual wire mode (LINT0=ExtINT)\n");
}
