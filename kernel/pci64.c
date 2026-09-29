// kernel/pci64.c — Milestone 28: PCI configuration-space enumeration.
// See include/pci64.h for the design rationale.
#include <stdint.h>
#include "../include/pci64.h"
#include "../include/heap64.h"
#include "../include/klog.h"
#include "../include/rustffi64.h"
#include "../include/pci_cap64.h"

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0,%1" :: "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t r; __asm__ volatile ("inl %1,%0" : "=a"(r) : "Nd"(port)); return r;
}

static uint32_t config_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return 0x80000000u
         | ((uint32_t)bus << 16)
         | ((uint32_t)slot << 11)
         | ((uint32_t)func << 8)
         | (offset & 0xFCu);
}

// M+11B: native-width config access. CF8 is shared mutable state, so
// EVERY access -- and, crucially, an entire read-modify-write -- runs in
// ONE interrupt-off critical section (save RFLAGS, cli, ..., restore the
// ORIGINAL IF). The backend ops below are RAW (no locking, no alignment
// checks): the locking lives in the generic layer, so a locked helper can
// compose raw read + raw write inside a single critical section, and so
// tests can substitute a fake backend that asserts IF==0 on every raw op.
// Sub-dword accesses are single native-width port accesses (byte lane
// selected via CFC+(off&3)/(off&2)) -- never a wider read-modify-write
// (which would rewrite adjacent fields and clear write-1-to-clear Status
// bits when only Command was meant to change).
static inline void outb_p(uint16_t port, uint8_t v) { __asm__ volatile ("outb %0,%1" :: "a"(v), "Nd"(port)); }
static inline void outw_p(uint16_t port, uint16_t v) { __asm__ volatile ("outw %0,%1" :: "a"(v), "Nd"(port)); }
static inline uint8_t  inb_p(uint16_t port) { uint8_t r;  __asm__ volatile ("inb %1,%0" : "=a"(r) : "Nd"(port)); return r; }
static inline uint16_t inw_p(uint16_t port) { uint16_t r; __asm__ volatile ("inw %1,%0" : "=a"(r) : "Nd"(port)); return r; }

static uint8_t  hw_r8 (uint8_t b, uint8_t s, uint8_t f, uint8_t o) { outl(PCI64_CONFIG_ADDRESS, config_address(b, s, f, o)); return inb_p((uint16_t)(PCI64_CONFIG_DATA + (o & 3))); }
static uint16_t hw_r16(uint8_t b, uint8_t s, uint8_t f, uint8_t o) { outl(PCI64_CONFIG_ADDRESS, config_address(b, s, f, o)); return inw_p((uint16_t)(PCI64_CONFIG_DATA + (o & 2))); }
static uint32_t hw_r32(uint8_t b, uint8_t s, uint8_t f, uint8_t o) { outl(PCI64_CONFIG_ADDRESS, config_address(b, s, f, o)); return inl(PCI64_CONFIG_DATA); }
static void hw_w8 (uint8_t b, uint8_t s, uint8_t f, uint8_t o, uint8_t v)  { outl(PCI64_CONFIG_ADDRESS, config_address(b, s, f, o)); outb_p((uint16_t)(PCI64_CONFIG_DATA + (o & 3)), v); }
static void hw_w16(uint8_t b, uint8_t s, uint8_t f, uint8_t o, uint16_t v) { outl(PCI64_CONFIG_ADDRESS, config_address(b, s, f, o)); outw_p((uint16_t)(PCI64_CONFIG_DATA + (o & 2)), v); }
static void hw_w32(uint8_t b, uint8_t s, uint8_t f, uint8_t o, uint32_t v) { outl(PCI64_CONFIG_ADDRESS, config_address(b, s, f, o)); outl(PCI64_CONFIG_DATA, v); }

static const pci64_cfg_backend_t g_hw_backend = { hw_r8, hw_r16, hw_r32, hw_w8, hw_w16, hw_w32 };
static const pci64_cfg_backend_t* g_be = &g_hw_backend;
static volatile uint32_t g_cfg_misaligned = 0;

const pci64_cfg_backend_t* pci64_cfg_set_backend(const pci64_cfg_backend_t* be) {
    const pci64_cfg_backend_t* old = g_be;
    g_be = be ? be : &g_hw_backend;
    return old;
}
uint32_t pci64_cfg_misaligned_count(void) { return g_cfg_misaligned; }

static volatile uint32_t g_lock_gen = 0;   // incremented per critical section (self-test: proves RMW == ONE section)
uint32_t pci64_cfg_lock_generation(void) { return g_lock_gen; }

static inline uint64_t cfg_lock(void) {
    uint64_t f;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    g_lock_gen++;
    return f;
}
static inline void cfg_unlock(uint64_t f) {
    if (f & 0x200) __asm__ volatile ("sti" ::: "memory");
}

// A misaligned access is REJECTED (reads return all-ones, writes are
// dropped) and counted -- never silently masked to a neighbouring field.
#define ALIGN_OR(off, mask, ret) do { if ((off) & (mask)) { g_cfg_misaligned++; return ret; } } while (0)

uint32_t pci64_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    ALIGN_OR(offset, 3, 0xFFFFFFFFu);
    uint64_t f = cfg_lock(); uint32_t v = g_be->read32(bus, slot, func, offset); cfg_unlock(f); return v;
}
uint16_t pci64_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    ALIGN_OR(offset, 1, 0xFFFFu);
    uint64_t f = cfg_lock(); uint16_t v = g_be->read16(bus, slot, func, offset); cfg_unlock(f); return v;
}
uint8_t pci64_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint64_t f = cfg_lock(); uint8_t v = g_be->read8(bus, slot, func, offset); cfg_unlock(f); return v;
}
void pci64_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    if (offset & 3) { g_cfg_misaligned++; return; }
    uint64_t f = cfg_lock(); g_be->write32(bus, slot, func, offset, value); cfg_unlock(f);
}
void pci64_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value) {
    if (offset & 1) { g_cfg_misaligned++; return; }
    uint64_t f = cfg_lock(); g_be->write16(bus, slot, func, offset, value); cfg_unlock(f);
}
void pci64_config_write8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t value) {
    uint64_t f = cfg_lock(); g_be->write8(bus, slot, func, offset, value); cfg_unlock(f);
}

// Atomic read-modify-write of one control field at its OWN width: raw read,
// modify only the requested bits, raw write -- all inside ONE critical
// section. Returns the value read (pre-modification).
uint8_t pci64_cfg_update8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t clear, uint8_t set) {
    uint64_t f = cfg_lock();
    uint8_t old = g_be->read8(bus, slot, func, offset);
    g_be->write8(bus, slot, func, offset, (uint8_t)((old & ~clear) | set));
    cfg_unlock(f);
    return old;
}
uint16_t pci64_cfg_update16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t clear, uint16_t set) {
    ALIGN_OR(offset, 1, 0xFFFFu);
    uint64_t f = cfg_lock();
    uint16_t old = g_be->read16(bus, slot, func, offset);
    g_be->write16(bus, slot, func, offset, (uint16_t)((old & ~clear) | set));
    cfg_unlock(f);
    return old;
}
uint32_t pci64_cfg_update32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t clear, uint32_t set) {
    ALIGN_OR(offset, 3, 0xFFFFFFFFu);
    uint64_t f = cfg_lock();
    uint32_t old = g_be->read32(bus, slot, func, offset);
    g_be->write32(bus, slot, func, offset, (old & ~clear) | set);
    cfg_unlock(f);
    return old;
}

static pci64_device_t* g_head = 0;
static int g_count = 0;
static int g_enumerated = 0;

// Decodes BAR `idx` (0..5) for `dev`, sizing it via the standard
// write-all-ones/read-back/restore probe. For a 64-bit memory BAR, also
// consumes and decodes bar[idx+1] (the high dword) and marks its own
// slot PCI64_BAR_NONE so the caller's loop skips it -- returns 1 in
// that case (two slots consumed), 0 otherwise (one slot consumed).
static int decode_bar(uint8_t bus, uint8_t slot, uint8_t func, int idx, pci64_bar_t* bars) {
    uint8_t offset = (uint8_t)(0x10 + idx * 4);
    uint32_t orig = pci64_config_read32(bus, slot, func, offset);

    if (orig == 0) { bars[idx].type = PCI64_BAR_NONE; return 0; }

    if (orig & 0x1) {
        // I/O BAR
        pci64_config_write32(bus, slot, func, offset, 0xFFFFFFFFu);
        uint32_t mask = pci64_config_read32(bus, slot, func, offset);
        pci64_config_write32(bus, slot, func, offset, orig);

        bars[idx].type = PCI64_BAR_IO;
        bars[idx].address = orig & ~0x3ULL;
        bars[idx].size = (~(mask & ~0x3u) + 1u) & 0xFFFFu;
        bars[idx].prefetchable = 0;
        return 0;
    }

    int bar_type = (orig >> 1) & 0x3; // 0 = 32-bit, 2 = 64-bit
    int prefetch = (orig >> 3) & 0x1;

    if (bar_type == 2) {
        uint8_t offset_hi = (uint8_t)(offset + 4);
        uint32_t orig_hi = pci64_config_read32(bus, slot, func, offset_hi);

        pci64_config_write32(bus, slot, func, offset, 0xFFFFFFFFu);
        pci64_config_write32(bus, slot, func, offset_hi, 0xFFFFFFFFu);
        uint32_t mask_lo = pci64_config_read32(bus, slot, func, offset);
        uint32_t mask_hi = pci64_config_read32(bus, slot, func, offset_hi);
        pci64_config_write32(bus, slot, func, offset, orig);
        pci64_config_write32(bus, slot, func, offset_hi, orig_hi);

        uint64_t mask64 = ((uint64_t)mask_hi << 32) | (mask_lo & ~0xFu);
        uint64_t addr64 = ((uint64_t)orig_hi << 32) | (orig & ~0xFu);

        bars[idx].type = PCI64_BAR_MEM64;
        bars[idx].address = addr64;
        bars[idx].size = (~mask64) + 1;
        bars[idx].prefetchable = prefetch;
        bars[idx + 1].type = PCI64_BAR_NONE; // consumed -- caller's loop must skip it
        return 1;
    }

    // 32-bit memory BAR
    pci64_config_write32(bus, slot, func, offset, 0xFFFFFFFFu);
    uint32_t mask = pci64_config_read32(bus, slot, func, offset);
    pci64_config_write32(bus, slot, func, offset, orig);

    bars[idx].type = PCI64_BAR_MEM32;
    bars[idx].address = orig & ~0xFULL;
    bars[idx].size = (uint64_t)((~(mask & ~0xFu)) + 1u);
    bars[idx].prefetchable = prefetch;
    return 0;
}

static void probe_function(uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t vendor = pci64_config_read16(bus, slot, func, 0x00);
    if (vendor == 0xFFFF) return; // nothing here

    pci64_device_t* dev = (pci64_device_t*)kmalloc(sizeof(pci64_device_t));
    if (!dev) { klog("pci64: out of memory during enumeration\n"); return; }

    dev->bus = bus; dev->slot = slot; dev->func = func;
    dev->irq_state = 0; dev->cap_malformed_logged = 0;
    dev->vendor_id = vendor;
    dev->device_id = pci64_config_read16(bus, slot, func, 0x02);

    uint32_t class_rev = pci64_config_read32(bus, slot, func, 0x08);
    dev->revision   = (uint8_t)(class_rev & 0xFF);
    dev->prog_if    = (uint8_t)((class_rev >> 8) & 0xFF);
    dev->subclass   = (uint8_t)((class_rev >> 16) & 0xFF);
    dev->class_code = (uint8_t)((class_rev >> 24) & 0xFF);

    dev->header_type = pci64_config_read8(bus, slot, func, 0x0E);
    dev->irq_line = pci64_config_read8(bus, slot, func, 0x3C);
    dev->irq_pin  = pci64_config_read8(bus, slot, func, 0x3D);

    // Header type 0x00 (normal device) is the only layout with 6
    // general BARs at 0x10-0x24; bridges (type 0x01) and CardBus
    // (0x02) have a different, shorter BAR layout this kernel doesn't
    // need to decode (no bridge/CardBus driver exists here) -- their
    // BARs are simply left PCI64_BAR_NONE.
    for (int i = 0; i < 6; i++) dev->bar[i].type = PCI64_BAR_NONE;
    if ((dev->header_type & 0x7F) == 0x00) {
        for (int i = 0; i < 6; ) {
            i += decode_bar(bus, slot, func, i, dev->bar) ? 2 : 1;
        }
    }

    dev->next = g_head;
    g_head = dev;
    g_count++;
}

int pci64_enumerate(void) {
    if (g_enumerated) return g_count;
    g_enumerated = 1;

    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint16_t vendor0 = pci64_config_read16((uint8_t)bus, (uint8_t)slot, 0, 0x00);
            if (vendor0 == 0xFFFF) continue;

            probe_function((uint8_t)bus, (uint8_t)slot, 0);

            uint8_t header_type = pci64_config_read8((uint8_t)bus, (uint8_t)slot, 0, 0x0E);
            if (header_type & 0x80) {
                for (int func = 1; func < 8; func++) {
                    uint16_t v = pci64_config_read16((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x00);
                    if (v != 0xFFFF) probe_function((uint8_t)bus, (uint8_t)slot, (uint8_t)func);
                }
            }
        }
    }

    klog("pci64: enumeration complete\n");
    return g_count;
}

void pci64_enable_device(pci64_device_t* dev) {
    // M+11B: one native 16-bit read-modify-write of Command only.
    pci64_cfg_update16(dev->bus, dev->slot, dev->func, 0x04, 0,
                       0x1 /* I/O space */ | 0x2 /* memory space */ | 0x4 /* bus master */);
}

pci64_device_t* pci64_iter(pci64_device_t* prev) {
    return prev ? prev->next : g_head;
}

pci64_device_t* pci64_find_class(uint8_t class_code, int subclass, int prog_if, pci64_device_t* prev) {
    for (pci64_device_t* d = pci64_iter(prev); d; d = d->next) {
        if (d->class_code != class_code) continue;
        if (subclass >= 0 && d->subclass != (uint8_t)subclass) continue;
        if (prog_if >= 0 && d->prog_if != (uint8_t)prog_if) continue;
        return d;
    }
    return 0;
}

pci64_device_t* pci64_find_device(uint16_t vendor_id, uint16_t device_id, pci64_device_t* prev) {
    for (pci64_device_t* d = pci64_iter(prev); d; d = d->next) {
        if (d->vendor_id == vendor_id && d->device_id == device_id) return d;
    }
    return 0;
}

int pci64_device_count(void) { return g_count; }

// M+2 / M+11B: compatibility wrapper over the pure walker in
// kernel/pci_cap64.c (strict pointer validation, loop/hop/alignment/bounds
// checks, over the native-width backend). Contract unchanged: the offset of
// the first matching capability after `start_after` (0 = from the head), or
// -1 for "none" OR "malformed" (a malformed list is logged once per device).
// M+11B change: misaligned capability pointers are now MALFORMED instead of
// being silently masked.
int pci64_find_capability(pci64_device_t* dev, uint8_t cap_id, uint8_t start_after) {
    pci_cfg_ro_t ro;
    pci_cfg_ro_for_dev(dev, &ro);
    int off = -1, matches = 0;
    int st = pci_cap_find(&ro, cap_id, start_after, &off, &matches);
    if (st == PCI_CAP_MALFORMED && !dev->cap_malformed_logged) {
        dev->cap_malformed_logged = 1;
        klog("pci64: malformed capability list on a device -- treated as no capability\n");
    }
    return st == PCI_CAP_FOUND ? off : -1;
}

static void hex32_to_str(uint32_t val, char* out) {
    const char* h = "0123456789ABCDEF";
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 8; i++) out[2 + (7 - i)] = h[(val >> (i * 4)) & 0xF];
    out[10] = 0;
}

static void hex64_to_str(uint64_t val, char* out) {
    const char* h = "0123456789ABCDEF";
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 16; i++) out[2 + (15 - i)] = h[(val >> (i * 4)) & 0xF];
    out[18] = 0;
}

static void dec_to_str_local(uint64_t val, char* out) {
    char tmp[24];
    int n = 0;
    if (val == 0) tmp[n++] = '0';
    while (val > 0 && n < 24) { tmp[n++] = (char)('0' + (val % 10)); val /= 10; }
    int j = 0;
    while (n > 0) out[j++] = tmp[--n];
    out[j] = 0;
}

void pci64_dump(void) {
    klog("pci64: dump ---\n");
    for (pci64_device_t* d = g_head; d; d = d->next) {
        char buf[24];
        klog("  "); dec_to_str_local(d->bus, buf); klog(buf);
        klog(":"); dec_to_str_local(d->slot, buf); klog(buf);
        klog("."); dec_to_str_local(d->func, buf); klog(buf);
        klog(" vendor="); hex32_to_str(d->vendor_id, buf); klog(buf);
        klog(" device="); hex32_to_str(d->device_id, buf); klog(buf);
        klog(" class="); hex32_to_str(d->class_code, buf); klog(buf);
        klog(" subclass="); hex32_to_str(d->subclass, buf); klog(buf);
        klog(" progif="); hex32_to_str(d->prog_if, buf); klog(buf);
        klog(" irq_line="); dec_to_str_local(d->irq_line, buf); klog(buf);
        klog(" irq_pin="); dec_to_str_local(d->irq_pin, buf); klog(buf);
        klog("\n");
        for (int i = 0; i < 6; i++) {
            if (d->bar[i].type == PCI64_BAR_NONE) continue;
            const char* t = d->bar[i].type == PCI64_BAR_IO ? "IO" :
                             d->bar[i].type == PCI64_BAR_MEM64 ? "MEM64" : "MEM32";
            klog("    bar["); dec_to_str_local((uint64_t)i, buf); klog(buf);
            klog("]="); klog(t);
            klog(" addr="); hex64_to_str(d->bar[i].address, buf); klog(buf);
            klog(" size="); hex64_to_str(d->bar[i].size, buf); klog(buf);
            klog(d->bar[i].prefetchable ? " prefetchable\n" : "\n");
        }
    }
    klog("pci64: dump end ---\n");
}

// Milestone 31: see include/rustffi64.h for why this is a dedicated
// flatten-and-copy function rather than handing Rust a raw
// pci64_device_t* to interpret itself.
void pci64_fill_rust_info(const pci64_device_t* dev, toxenos_pci_info_t* out) {
    out->bus = dev->bus; out->slot = dev->slot; out->func = dev->func;
    out->vendor_id = dev->vendor_id; out->device_id = dev->device_id;
    out->class_code = dev->class_code; out->subclass = dev->subclass;
    out->prog_if = dev->prog_if; out->revision = dev->revision;
    out->header_type = dev->header_type;
    out->irq_line = dev->irq_line; out->irq_pin = dev->irq_pin;
    for (int i = 0; i < 6; i++) {
        out->bar_types[i] = (uint32_t)dev->bar[i].type;
        out->bar_addrs[i] = dev->bar[i].address;
        out->bar_sizes[i] = dev->bar[i].size;
    }
}
