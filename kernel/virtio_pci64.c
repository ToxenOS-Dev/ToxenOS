// kernel/virtio_pci64.c — M+2: modern VirtIO-PCI capability discovery,
// validation, and MMIO mapping. See include/virtio_pci64.h for the full
// design rationale (why this is the ONE place that ever interprets a
// virtio_pci_cap, and why status/feature/queue logic deliberately lives
// in Rust instead).
#include <stdint.h>
#include "../include/virtio_pci64.h"
#include "../include/physmem64.h"
#include "../include/klog.h"

static void dec_to_str_local(uint64_t val, char* out) {
    char tmp[24];
    int n = 0;
    if (val == 0) tmp[n++] = '0';
    while (val > 0 && n < 24) { tmp[n++] = (char)('0' + (val % 10)); val /= 10; }
    int j = 0;
    while (n > 0) out[j++] = tmp[--n];
    out[j] = 0;
}
static void hex64_to_str(uint64_t val, char* out) {
    const char* h = "0123456789ABCDEF";
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 16; i++) out[2 + (15 - i)] = h[(val >> (i * 4)) & 0xF];
    out[18] = 0;
}

// One raw virtio_pci_cap header, read field-by-field via the existing
// config-space accessors (never a struct overlay onto a local buffer --
// this keeps every field access explicit about its own byte offset,
// matching kernel/pci64.c's own decode_bar() style, and sidesteps any
// question of this compiler's struct packing/endianness for a type nothing
// else in this codebase needs to share).
typedef struct {
    uint8_t  cap_len;
    uint8_t  cfg_type;
    uint8_t  bar;
    uint32_t offset;
    uint32_t length;
} raw_virtio_cap_t;

static void read_raw_cap(pci64_device_t* dev, uint8_t cap_off, raw_virtio_cap_t* out) {
    // virtio_pci_cap layout (spec-mandated, all little-endian, which is
    // exactly what pci64_config_read32's own bus-order guarantees on
    // this x86_64 target):
    //   +0 cap_vndr, +1 cap_next, +2 cap_len, +3 cfg_type,
    //   +4 bar, +5 id, +6..7 padding, +8 offset (u32), +12 length (u32)
    out->cap_len  = pci64_config_read8(dev->bus, dev->slot, dev->func, (uint8_t)(cap_off + 2));
    out->cfg_type = pci64_config_read8(dev->bus, dev->slot, dev->func, (uint8_t)(cap_off + 3));
    out->bar      = pci64_config_read8(dev->bus, dev->slot, dev->func, (uint8_t)(cap_off + 4));
    out->offset   = pci64_config_read32(dev->bus, dev->slot, dev->func, (uint8_t)(cap_off + 8));
    out->length   = pci64_config_read32(dev->bus, dev->slot, dev->func, (uint8_t)(cap_off + 12));
}

// Pure bounds check, deliberately factored out of validate_and_map so it
// can be exercised directly with synthetic (bar, offset, length) triples
// in this file's own self-test -- no real device, no config-space I/O,
// no MMIO mapping, just the arithmetic every capability's offset/length
// must satisfy against its own BAR's reported size. `offset`/`length`
// are the device-supplied (i.e. untrusted) uint32_t values; `bar_size` is
// this KERNEL's own already-probed BAR size (kernel/pci64.c's
// decode_bar()), never device-supplied at this point. Returns 1 if
// [offset, offset+length) fits entirely inside [0, bar_size) with no
// wraparound, 0 otherwise. The comparison order matters: `offset >
// bar_size` is checked FIRST, which is what makes `bar_size - offset` on
// the next line safe from underflow (offset is now known <= bar_size).
static int region_fits_bar(uint64_t bar_size, uint32_t offset, uint32_t length) {
    if (length == 0) return 0;
    if ((uint64_t)offset > bar_size) return 0;
    if ((uint64_t)length > bar_size - offset) return 0;
    return 1;
}

// Validates and maps one already-read raw capability into `region`.
// `min_cap_len` is the smallest cap_len this cfg_type's own structure
// can legitimately have (16 for the base virtio_pci_cap; 20 for
// NOTIFY_CFG, which appends notify_off_multiplier) -- a smaller value
// means the capability's own declared length is truncated relative to
// what this cfg_type requires, and the whole capability is rejected
// rather than read partially. Every failure path leaves `region->present`
// at 0 (its caller-side zero-initialized default) and returns without
// mapping anything.
static void validate_and_map(pci64_device_t* dev, uint8_t cap_off, const raw_virtio_cap_t* cap,
                              uint8_t min_cap_len, virtio_pci64_region_t* region) {
    if (cap->cap_len < min_cap_len) {
        klog("virtio_pci64: capability truncated for its own cfg_type -- rejected\n");
        return;
    }
    if (cap->bar >= 6) {
        klog("virtio_pci64: capability names an out-of-range BAR index -- rejected\n");
        return;
    }
    pci64_bar_t* bar = &dev->bar[cap->bar];
    if (bar->type != PCI64_BAR_MEM32 && bar->type != PCI64_BAR_MEM64) {
        klog("virtio_pci64: capability's BAR is not MMIO -- rejected\n");
        return;
    }
    // Overflow-checked BEFORE use, per this milestone's own instruction:
    // offset/length are untrusted uint32_t values from the device itself
    // (or a malicious/buggy emulation of one) -- offset + length must not
    // wrap, and the resulting range must fit entirely inside the BAR's
    // own reported size. See region_fits_bar's own comment for why this
    // specific comparison order is what makes it overflow-safe.
    if (!region_fits_bar(bar->size, cap->offset, cap->length)) {
        klog("virtio_pci64: capability offset+length does not fit inside its BAR -- rejected\n");
        return;
    }

    uint64_t phys = bar->address + cap->offset;
    void* mapped = physmem64_map_mmio(phys, cap->length);
    if (!mapped) {
        klog("virtio_pci64: physmem64_map_mmio failed for a validated capability\n");
        return;
    }

    region->present   = 1;
    region->mmio_base = (uint64_t)(uintptr_t)mapped;
    region->length    = cap->length;

    if (cap->cfg_type == VIRTIO_PCI64_CAP_NOTIFY_CFG) {
        region->notify_off_multiplier = pci64_config_read32(dev->bus, dev->slot, dev->func, (uint8_t)(cap_off + 16));
    }
}

int virtio_pci64_probe(pci64_device_t* dev, virtio_pci64_transport_info_t* out) {
    for (uint64_t i = 0; i < sizeof(*out); i++) ((char*)out)[i] = 0;

    int found_any = 0;
    int cap_off = pci64_find_capability(dev, PCI64_CAP_ID_VENDOR_SPECIFIC, 0);
    if (cap_off < 0) return -1; // no vendor-specific capabilities at all -- not a modern VirtIO device

    while (cap_off >= 0) {
        found_any = 1;
        raw_virtio_cap_t cap;
        read_raw_cap(dev, (uint8_t)cap_off, &cap);

        switch (cap.cfg_type) {
        case VIRTIO_PCI64_CAP_COMMON_CFG:
            if (!out->common.present) validate_and_map(dev, (uint8_t)cap_off, &cap, 16, &out->common);
            break;
        case VIRTIO_PCI64_CAP_NOTIFY_CFG:
            if (!out->notify.present) validate_and_map(dev, (uint8_t)cap_off, &cap, 20, &out->notify);
            break;
        case VIRTIO_PCI64_CAP_ISR_CFG:
            if (!out->isr.present) validate_and_map(dev, (uint8_t)cap_off, &cap, 16, &out->isr);
            break;
        case VIRTIO_PCI64_CAP_DEVICE_CFG:
            if (!out->device.present) validate_and_map(dev, (uint8_t)cap_off, &cap, 16, &out->device);
            break;
        case VIRTIO_PCI64_CAP_PCI_CFG:
            // Discovered only, per this milestone's scope -- never
            // mapped (it isn't MMIO-backed at all; it's a config-space
            // window mechanism this codebase has no need for, since
            // every region it could reach is already directly mappable).
            out->pci_cfg.present = 1;
            break;
        default:
            klog("virtio_pci64: unrecognized vendor-specific cfg_type -- skipped\n");
            break;
        }

        cap_off = pci64_find_capability(dev, PCI64_CAP_ID_VENDOR_SPECIFIC, (uint8_t)cap_off);
    }

    if (!found_any) return -1;
    out->ok = (uint32_t)(out->common.present && out->notify.present && out->isr.present);
    return 0;
}

void virtio_pci64_dump(const virtio_pci64_transport_info_t* info) {
    char buf[24];
    klog("virtio_pci64: dump ---\n");
    klog("  ok="); dec_to_str_local(info->ok, buf); klog(buf); klog("\n");
    const struct { const char* name; const virtio_pci64_region_t* r; } regions[] = {
        {"common", &info->common}, {"notify", &info->notify}, {"isr", &info->isr},
        {"device", &info->device}, {"pci_cfg", &info->pci_cfg},
    };
    for (int i = 0; i < 5; i++) {
        if (!regions[i].r->present) continue;
        klog("  "); klog(regions[i].name);
        klog(": mmio_base="); hex64_to_str(regions[i].r->mmio_base, buf); klog(buf);
        klog(" length="); dec_to_str_local(regions[i].r->length, buf); klog(buf);
        if (regions[i].r->notify_off_multiplier) {
            klog(" notify_off_multiplier="); dec_to_str_local(regions[i].r->notify_off_multiplier, buf); klog(buf);
        }
        klog("\n");
    }
    klog("virtio_pci64: dump end ---\n");
}

// ── Self-test: pure bounds-check arithmetic, no real device needed ───
// Everything ELSE in this file (pci64_find_capability's loop/alignment/
// truncation defenses, a real device rejecting FEATURES_OK, a real
// device resetting mid-negotiation) genuinely needs either a
// deliberately-malformed PCI device or fault injection this kernel has
// no way to construct against a real, well-behaved QEMU device -- those
// are verified by code inspection instead (see this milestone's own
// report). This function covers what CAN be exercised with synthetic
// values alone: region_fits_bar()'s overflow-sensitive arithmetic,
// including inputs specifically chosen to wrap a naive `offset + length`
// comparison if the ordering/subtraction discipline in its own comment
// were ever weakened by a future edit.
static int test_region_fits_bar(void) {
    int ok = 1;
    // Exactly fits: offset 0, length == bar_size.
    if (!region_fits_bar(0x1000, 0, 0x1000)) ok = 0;
    // Exactly fits at the tail end: offset+length == bar_size precisely.
    if (!region_fits_bar(0x1000, 0x800, 0x800)) ok = 0;
    // One byte past the end -- must be rejected.
    if (region_fits_bar(0x1000, 0x800, 0x801)) ok = 0;
    // Zero length -- rejected regardless of offset.
    if (region_fits_bar(0x1000, 0, 0)) ok = 0;
    // offset already beyond the BAR entirely.
    if (region_fits_bar(0x1000, 0x2000, 1)) ok = 0;
    // offset exactly AT bar_size with nonzero length -- still rejected
    // (there is no byte at index bar_size within a [0, bar_size) BAR).
    if (region_fits_bar(0x1000, 0x1000, 1)) ok = 0;
    // The classic overflow bait: offset + length as raw uint32_t
    // arithmetic would wrap to a small number and pass a naive
    // "offset + length <= bar_size" check. A correct implementation
    // must reject this regardless.
    if (region_fits_bar(0x1000, 0xFFFFFFFFu, 0x2)) ok = 0;
    // A huge, non-overflowing length that simply doesn't fit.
    if (region_fits_bar(0x1000, 0, 0xFFFFFFFFu)) ok = 0;
    // A large BAR (this kernel stores bar->size as a full uint64_t, so a
    // 64-bit MEM64 BAR genuinely can be this big) with a small,
    // perfectly valid region inside it.
    if (!region_fits_bar(0x100000000ULL, 0xFFFFFFFFu, 1)) ok = 0;

    return ok;
}

int virtio_pci64_selftest(void) {
    int pass = 0, fail = 0;
    klog("virtio_pci64_selftest: starting\n");

    int r = test_region_fits_bar();
    klog("virtio_pci64_selftest: region_fits_bar overflow/bounds arithmetic ");
    klog(r ? "PASS\n" : "FAIL\n");
    if (r) pass++; else fail++;

    char passbuf[24], failbuf[24];
    dec_to_str_local((uint64_t)pass, passbuf);
    dec_to_str_local((uint64_t)fail, failbuf);
    klog("virtio_pci64_selftest: pass="); klog(passbuf);
    klog(" fail="); klog(failbuf); klog("\n");
    return fail == 0;
}
