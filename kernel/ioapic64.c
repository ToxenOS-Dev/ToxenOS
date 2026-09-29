// kernel/ioapic64.c -- M+11A IOAPIC driver. See include/ioapic64.h.
#include "../include/ioapic64.h"
#include "../include/physmem64.h"
#include "../include/klog.h"

#define RTE_MASK_BIT (1u << 16)

volatile uint32_t ioapic64_op_count = 0;   // M+11B: every route/mask/unmask write (proves MSI never touches the IOAPIC)

uint64_t ioapic64_rte_encode(const ioapic64_rte_t* r) {
    uint64_t v = r->vector;
    v |= (uint64_t)(r->delivery & 7u) << 8;
    v |= (uint64_t)(r->dest_mode & 1u) << 11;
    v |= (uint64_t)(r->polarity & 1u) << 13;
    v |= (uint64_t)(r->trigger & 1u) << 15;
    v |= (uint64_t)(r->masked & 1u) << 16;
    v |= (uint64_t)r->dest << 56;
    return v; // delivery-status (12) and remote-IRR (14) are read-only: never encoded
}

void ioapic64_rte_decode(uint64_t v, ioapic64_rte_t* r) {
    r->vector     = (uint8_t)(v & 0xFF);
    r->delivery   = (uint8_t)((v >> 8) & 7);
    r->dest_mode  = (uint8_t)((v >> 11) & 1);
    r->pending    = (uint8_t)((v >> 12) & 1);
    r->polarity   = (uint8_t)((v >> 13) & 1);
    r->remote_irr = (uint8_t)((v >> 14) & 1);
    r->trigger    = (uint8_t)((v >> 15) & 1);
    r->masked     = (uint8_t)((v >> 16) & 1);
    r->dest       = (uint8_t)((v >> 56) & 0xFF);
}

int ioapic64_find(const ioapic64_t* arr, int n, uint32_t gsi, uint32_t* pin) {
    for (int i = 0; i < n; i++) {
        if (gsi >= arr[i].gsi_base && gsi - arr[i].gsi_base < arr[i].nr_entries) {
            if (pin) *pin = gsi - arr[i].gsi_base;
            return i;
        }
    }
    return -1;
}

int ioapic64_ranges_valid(const ioapic64_t* arr, int n) {
    for (int i = 0; i < n; i++) {
        if (arr[i].nr_entries == 0 || arr[i].nr_entries > IOAPIC64_MAX_ENTRIES) return 0;
        uint64_t ai = arr[i].gsi_base, ae = ai + arr[i].nr_entries;
        for (int j = i + 1; j < n; j++) {
            uint64_t aj = arr[j].gsi_base, ee = aj + arr[j].nr_entries;
            if (ai < ee && aj < ae) return 0;
        }
    }
    return 1;
}

// ── hardware ─────────────────────────────────────────────────────────
static ioapic64_t g_io[IOAPIC64_MAX];
static int g_n_io = 0;

static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    if (f & 0x200) __asm__ volatile ("sti" ::: "memory");
}

static uint32_t io_read(const ioapic64_t* io, uint32_t reg) {
    uint64_t f = irq_save();
    io->virt[0] = reg;
    uint32_t v = io->virt[4];
    irq_restore(f);
    return v;
}
static void io_write(const ioapic64_t* io, uint32_t reg, uint32_t val) {
    uint64_t f = irq_save();
    io->virt[0] = reg;
    io->virt[4] = val;
    irq_restore(f);
}

static void rte_write_raw(const ioapic64_t* io, uint32_t pin, uint64_t raw) {
    io_write(io, IOAPIC64_REG_RTE(pin) + 1, (uint32_t)(raw >> 32)); // destination first
    io_write(io, IOAPIC64_REG_RTE(pin),     (uint32_t)raw);         // vector/mask last
}

int ioapic64_init_all_masked(const madt64_info_t* m) {
    g_n_io = 0;
    if (!m || m->n_ioapic <= 0) return -1;
    for (int i = 0; i < m->n_ioapic; i++) {
        ioapic64_t* io = &g_io[i];
        io->id = m->ioapic[i].id;
        io->phys = m->ioapic[i].addr;
        io->gsi_base = m->ioapic[i].gsi_base;
        io->virt = (volatile uint32_t*)physmem64_map_mmio(io->phys, 0x1000);
        if (!io->virt) { klog("ioapic64: MMIO map failed\n"); return -1; }
        uint32_t ver = io_read(io, IOAPIC64_REG_VER);
        if (ver == 0xFFFFFFFFu || ver == 0) { klog("ioapic64: bogus VER register\n"); return -1; }
        io->version = (uint8_t)(ver & 0xFF);
        io->nr_entries = ((ver >> 16) & 0xFF) + 1;
        g_n_io = i + 1;
    }
    if (!ioapic64_ranges_valid(g_io, g_n_io)) { klog("ioapic64: invalid/overlapping GSI ranges\n"); g_n_io = 0; return -1; }
    // Mask every route: masked, edge, active-high, fixed, physical, vector 0.
    ioapic64_rte_t off = {0}; off.masked = 1;
    uint64_t raw = ioapic64_rte_encode(&off);
    for (int i = 0; i < g_n_io; i++)
        for (uint32_t p = 0; p < g_io[i].nr_entries; p++) rte_write_raw(&g_io[i], p, raw);
    // Read back one entry per IOAPIC to confirm it really is masked.
    for (int i = 0; i < g_n_io; i++) {
        if (!(io_read(&g_io[i], IOAPIC64_REG_RTE(0)) & RTE_MASK_BIT)) { klog("ioapic64: mask readback failed\n"); g_n_io = 0; return -1; }
    }
    return 0;
}

int ioapic64_route(uint32_t gsi, const ioapic64_rte_t* rte) {
    uint32_t pin;
    int i = ioapic64_find(g_io, g_n_io, gsi, &pin);
    if (i < 0) return -1;
    ioapic64_op_count++;
    rte_write_raw(&g_io[i], pin, ioapic64_rte_encode(rte));
    return 0;
}

static int set_mask(uint32_t gsi, int masked) {
    uint32_t pin;
    int i = ioapic64_find(g_io, g_n_io, gsi, &pin);
    if (i < 0) return -1;
    ioapic64_op_count++;
    uint32_t lo = io_read(&g_io[i], IOAPIC64_REG_RTE(pin));
    lo = masked ? (lo | RTE_MASK_BIT) : (lo & ~RTE_MASK_BIT);
    io_write(&g_io[i], IOAPIC64_REG_RTE(pin), lo);
    return 0;
}
int ioapic64_mask(uint32_t gsi)   { return set_mask(gsi, 1); }
int ioapic64_unmask(uint32_t gsi) { return set_mask(gsi, 0); }

int ioapic64_read_rte(uint32_t gsi, uint64_t* raw) {
    uint32_t pin;
    int i = ioapic64_find(g_io, g_n_io, gsi, &pin);
    if (i < 0) return -1;
    *raw = (uint64_t)io_read(&g_io[i], IOAPIC64_REG_RTE(pin)) |
           ((uint64_t)io_read(&g_io[i], IOAPIC64_REG_RTE(pin) + 1) << 32);
    return 0;
}

int ioapic64_count(void) { return g_n_io; }
const ioapic64_t* ioapic64_get(int i) { return (i >= 0 && i < g_n_io) ? &g_io[i] : 0; }
