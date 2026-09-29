// kernel/acpi64.c -- M+11A minimal ACPI table discovery. See include/acpi64.h.
#include "../include/acpi64.h"
#include "../include/physmem64.h"
#include "../include/klog.h"

static const char* g_status = "not run";
const char* acpi64_last_status(void) { return g_status; }

static int sig_eq(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

int acpi64_checksum_ok(const void* p, uint32_t len) {
    const uint8_t* b = (const uint8_t*)p;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum = (uint8_t)(sum + b[i]);
    return sum == 0;
}

int acpi64_rsdp_valid(const void* p, uint32_t avail) {
    if (!p || avail < 20) return 0;
    const acpi_rsdp_t* r = (const acpi_rsdp_t*)p;
    if (!sig_eq(r->sig, "RSD PTR ", 8)) return 0;
    if (!acpi64_checksum_ok(p, 20)) return 0;
    if (r->revision >= 2) {
        if (avail < 36 || r->length < 36 || r->length > avail) return 0;
        if (!acpi64_checksum_ok(p, r->length)) return 0;
    }
    return 1;
}

int acpi64_sdt_valid(const void* p, uint64_t avail, const char* sig) {
    if (!p || avail < sizeof(acpi_sdt_header_t)) return 0;
    const acpi_sdt_header_t* h = (const acpi_sdt_header_t*)p;
    if (sig && !sig_eq(h->sig, sig, 4)) return 0;
    if (h->length < sizeof(acpi_sdt_header_t) || h->length > ACPI64_SDT_MAX_LEN) return 0;
    if (h->length > avail) return 0;
    return acpi64_checksum_ok(p, h->length);
}

int acpi64_scan_rsdp(const uint8_t* buf, uint32_t len) {
    for (uint32_t off = 0; off + 20 <= len; off += 16)
        if (acpi64_rsdp_valid(buf + off, len - off)) return (int)off;
    return -1;
}

const acpi_sdt_header_t* acpi64_root_find(uint64_t root_phys, int is_xsdt,
                                          const char* sig, acpi64_map_fn map) {
    if (!root_phys || !map) return 0;
    const acpi_sdt_header_t* hdr = (const acpi_sdt_header_t*)map(root_phys, sizeof(acpi_sdt_header_t));
    if (!hdr) return 0;
    uint32_t rlen = hdr->length;
    if (rlen < sizeof(acpi_sdt_header_t) || rlen > ACPI64_SDT_MAX_LEN) return 0;
    const acpi_sdt_header_t* root = (const acpi_sdt_header_t*)map(root_phys, rlen);
    if (!root || !acpi64_sdt_valid(root, rlen, is_xsdt ? "XSDT" : "RSDT")) return 0;

    uint32_t esz = is_xsdt ? 8 : 4;
    uint32_t n = (rlen - (uint32_t)sizeof(acpi_sdt_header_t)) / esz;
    const uint8_t* ents = (const uint8_t*)root + sizeof(acpi_sdt_header_t);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t phys = 0;
        if (is_xsdt) { for (int k = 7; k >= 0; k--) phys = (phys << 8) | ents[i * 8 + (uint32_t)k]; }
        else         { for (int k = 3; k >= 0; k--) phys = (phys << 8) | ents[i * 4 + (uint32_t)k]; }
        if (!phys) continue;
        const acpi_sdt_header_t* th = (const acpi_sdt_header_t*)map(phys, sizeof(acpi_sdt_header_t));
        if (!th || !sig_eq(th->sig, sig, 4)) continue;
        uint32_t tlen = th->length;
        if (tlen < sizeof(acpi_sdt_header_t) || tlen > ACPI64_SDT_MAX_LEN) continue;
        const acpi_sdt_header_t* t = (const acpi_sdt_header_t*)map(phys, tlen);
        if (t && acpi64_sdt_valid(t, tlen, sig)) return t;
    }
    return 0;
}

// ── real-hardware glue ───────────────────────────────────────────────
static const void* real_map(uint64_t phys, uint64_t len) {
    if (!physmem64_map_mmio(phys, len)) return 0;
    return physmem64_to_virt(phys);
}

// Multiboot2 tag walk (same layout mb2_find_fb uses): tag 14 = RSDPv1
// copy, tag 15 = RSDPv2 copy. Returns a pointer to the tag payload.
static const uint8_t* mb2_find_rsdp(uint64_t mb_info_addr, uint32_t type, uint32_t* out_len) {
    if (!mb_info_addr) return 0;
    uint32_t total = *(const uint32_t*)(uintptr_t)mb_info_addr;
    const uint8_t* p   = (const uint8_t*)(uintptr_t)(mb_info_addr + 8);
    const uint8_t* end = (const uint8_t*)(uintptr_t)(mb_info_addr + total);
    while (p + 8 <= end) {
        uint32_t t = *(const uint32_t*)p, sz = *(const uint32_t*)(p + 4);
        if (t == 0) break;
        if (sz < 8 || p + sz > end) break;
        if (t == type) { *out_len = sz - 8; return p + 8; }
        p += (sz + 7u) & ~7u;
    }
    return 0;
}

static const acpi_sdt_header_t* madt_via_rsdp(const acpi_rsdp_t* r) {
    if (r->revision >= 2 && r->xsdt_addr) {
        const acpi_sdt_header_t* m = acpi64_root_find(r->xsdt_addr, 1, "APIC", real_map);
        if (m) return m;
    }
    if (r->rsdt_addr) return acpi64_root_find(r->rsdt_addr, 0, "APIC", real_map);
    return 0;
}

const acpi_sdt_header_t* acpi64_find_madt(uint64_t mb_info_addr) {
    static const uint32_t tags[2] = { 15, 14 };
    for (int i = 0; i < 2; i++) {
        uint32_t len = 0;
        const uint8_t* p = mb2_find_rsdp(mb_info_addr, tags[i], &len);
        if (p && acpi64_rsdp_valid(p, len)) {
            const acpi_sdt_header_t* m = madt_via_rsdp((const acpi_rsdp_t*)p);
            if (m) { g_status = "MADT via multiboot2 RSDP"; return m; }
        }
    }
    // BIOS fallback: EBDA first KB, then 0xE0000-0xFFFFF (identity-mapped low memory).
    uint32_t ebda = (uint32_t)(*(const volatile uint16_t*)(uintptr_t)0x40E) << 4;
    if (ebda >= 0x80000 && ebda < 0xA0000) {
        int off = acpi64_scan_rsdp((const uint8_t*)(uintptr_t)ebda, 1024);
        if (off >= 0) {
            const acpi_sdt_header_t* m = madt_via_rsdp((const acpi_rsdp_t*)(uintptr_t)(ebda + (uint32_t)off));
            if (m) { g_status = "MADT via BIOS EBDA RSDP"; return m; }
        }
    }
    int off = acpi64_scan_rsdp((const uint8_t*)(uintptr_t)0xE0000, 0x20000);
    if (off >= 0) {
        const acpi_sdt_header_t* m = madt_via_rsdp((const acpi_rsdp_t*)(uintptr_t)(0xE0000 + (uint32_t)off));
        if (m) { g_status = "MADT via BIOS area RSDP"; return m; }
    }
    g_status = "no valid RSDP/MADT found";
    return 0;
}
