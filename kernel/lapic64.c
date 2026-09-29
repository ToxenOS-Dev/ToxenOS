// kernel/lapic64.c — Milestone 24: LAPIC virtual-wire mode for the
// 64-bit kernel. Port of kernel/kernel.c's lapic_virtual_wire_init
// (32-bit), adapted to this kernel's own mapping mechanism: rather than
// a pre-allocated page-directory slot, the LAPIC's fixed physical MMIO
// base (0xFEE00000) is mapped on demand by extending the low identity
// map (pml4[0] -> pdpt_low) with its own file-static helper below.
// Milestone 28 later added a general-purpose equivalent for device MMIO
// (physmem64_map_mmio, used by AHCI/NVMe/VirtIO-blk and, since
// Milestone 29, kernel/display64.c's framebuffer mapping too) -- this
// file predates that and was never migrated onto it, so it remains its
// own separate, narrower mapping path.
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

static volatile uint32_t* g_lapic = 0;
volatile uint64_t lapic64_eoi_count = 0;

int lapic64_map(uint64_t phys) {
    uint64_t va = map_lapic_phys(phys);
    if (!va) return -1;
    g_lapic = (volatile uint32_t*)(uintptr_t)va;
    return 0;
}

uint32_t lapic64_read(uint32_t reg) {
    return g_lapic ? g_lapic[reg / 4] : 0xFFFFFFFFu;
}

void lapic64_write(uint32_t reg, uint32_t val) {
    if (g_lapic) g_lapic[reg / 4] = val;
}

uint32_t lapic64_id(void) {
    return lapic64_read(LAPIC_REG_ID) >> 24;
}

void lapic64_eoi(void) {
    lapic64_eoi_count++;
    lapic64_write(LAPIC_REG_EOI, 0);
}

// PIC-mode path (and PIC fallback after a rolled-back APIC transaction):
// unchanged M24 behavior -- LAPIC software-enabled with LINT0 = ExtINT
// so the 8259's INTA-supplied vectors reach the CPU.
void lapic64_virtual_wire_init(void) {
    if (lapic64_map(LAPIC64_BASE) != 0) {
        klog("lapic64: failed to map LAPIC MMIO -- IRQ delivery may fail on real hardware\n");
        return;
    }

    // SVR (0x0F0): software-enable LAPIC (bit 8), spurious vector 0xFF.
    lapic64_write(LAPIC_REG_SVR, 0x1FFu);
    // TPR (0x080): task priority 0 -- accept all interrupt priorities.
    lapic64_write(LAPIC_REG_TPR, 0);
    // LVT LINT0 (0x350): ExtINT delivery, edge-triggered, unmasked -- on
    // each 8259 interrupt the CPU does an INTA cycle to the PIC, which
    // supplies the actual vector (0x20+), exactly as if wired directly.
    lapic64_write(LAPIC_REG_LINT0, 0x700u);
    // LVT LINT1 (0x360): NMI delivery, edge-triggered, unmasked.
    lapic64_write(LAPIC_REG_LINT1, 0x400u);

    klog("lapic64: virtual wire mode (LINT0=ExtINT)\n");
}
