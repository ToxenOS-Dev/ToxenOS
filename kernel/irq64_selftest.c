// kernel/irq64_selftest.c -- M+11A boot-time tests for the interrupt
// foundation. Everything here is deterministic and hardware-independent
// except the 0xFF spurious test, which really executes `int 0xFF` through
// the installed IDT gate.
#include <stdint.h>
#include "../include/irq64.h"
#include "../include/vector64.h"
#include "../include/acpi64.h"
#include "../include/madt64.h"
#include "../include/ioapic64.h"
#include "../include/irqmode64.h"
#include "../include/pic_chip64.h"
#include "../include/lapic64.h"
#include "../include/ps2_64.h"
#include "../include/klog.h"

static int g_pass, g_fail, g_group_fail;

static void check(int cond, const char* what) {
    if (cond) { g_pass++; return; }
    g_fail++; g_group_fail++;
    klog("  irq64 selftest FAIL: "); klog(what); klog("\n");
}
#define CHECK(c) check((c), #c)

static void group_begin(void) { g_group_fail = 0; }
static void group_end(const char* name) {
    klog(g_group_fail ? "irq64 selftest FAIL: " : "irq64 selftest PASS: ");
    klog(name); klog("\n");
}

static void zero(void* p, unsigned n) { volatile uint8_t* b = (volatile uint8_t*)p; for (unsigned i = 0; i < n; i++) b[i] = 0; }

// ═════════════════════════ 1. vector allocator ══════════════════════
static void test_vectors(void) {
    group_begin();
    static vector64_map_t m;
    irq_vector_map_init(&m);

    // Reservation table.
    int ok = 1;
    for (int v = 0x00; v <= 0x1F; v++) ok &= irq_vector_state(&m, v) == VEC_EXC;
    for (int v = 0x20; v <= 0x2F; v++) ok &= irq_vector_state(&m, v) == VEC_LEGACY_FREE;
    for (int v = 0xEF; v <= 0xFE; v++) ok &= irq_vector_state(&m, v) == VEC_SYSTEM;
    CHECK(ok);
    CHECK(irq_vector_state(&m, 0x80) == VEC_SYSCALL);
    CHECK(irq_vector_state(&m, 0xFF) == VEC_SPURIOUS);
    CHECK(irq_vector_free_count(&m) == VECTOR64_DYN_EXPECTED);

    // Exhaustion: exactly 190 vectors, all in 0x30..0xEE, NEVER 0x80.
    int seen80 = 0, in_range = 1, n = 0, first = -1, last = -1;
    static uint8_t got[256];
    zero(got, sizeof(got));
    for (;;) {
        int v = irq_vector_alloc(&m, 100 + n);
        if (v < 0) break;
        if (v == 0x80) seen80 = 1;
        if (v < VECTOR64_DYN_FIRST || v > VECTOR64_DYN_LAST) in_range = 0;
        if (got[v]) in_range = 0;              // no duplicates
        got[v] = 1;
        if (first < 0) first = v;
        last = v; n++;
    }
    CHECK(n == 190);
    CHECK(!seen80);
    CHECK(in_range);
    CHECK(first == 0x30 && last == 0xEE);
    CHECK(!got[0x80]);
    CHECK(irq_vector_state(&m, 0x80) == VEC_SYSCALL);
    CHECK(irq_vector_alloc(&m, 1) == -1);
    CHECK(m.exhausted_count >= 1);
    CHECK(irq_vector_owner(&m, 0x30) == 100);

    // Free/reuse across the 0x80 hole; lowest-free-first.
    CHECK(irq_vector_free(&m, 0x81) == 0);
    CHECK(irq_vector_free(&m, 0x7F) == 0);
    CHECK(irq_vector_alloc(&m, 7) == 0x7F);
    CHECK(irq_vector_alloc(&m, 8) == 0x81);
    CHECK(irq_vector_alloc(&m, 9) == -1);
    CHECK(irq_vector_free(&m, 0x7F) == 0);
    CHECK(irq_vector_alloc(&m, 10) == 0x7F);      // reuse
    CHECK(irq_vector_owner(&m, 0x7F) == 10);

    // Frees of things that are not ALLOCATED are rejected and change nothing.
    uint32_t bad0 = m.bad_free_count;
    CHECK(irq_vector_free(&m, 0x80) == -1);
    CHECK(irq_vector_free(&m, 0x21) == -1);
    CHECK(irq_vector_free(&m, 0xFF) == -1);
    CHECK(irq_vector_free(&m, 0x03) == -1);
    CHECK(irq_vector_free(&m, 0xF0) == -1);
    CHECK(irq_vector_free(&m, 256) == -1);
    CHECK(irq_vector_free(&m, -1) == -1);
    CHECK(m.bad_free_count == bad0 + 7);
    CHECK(irq_vector_state(&m, 0x80) == VEC_SYSCALL && irq_vector_state(&m, 0xFF) == VEC_SPURIOUS);
    CHECK(irq_vector_free(&m, 0x40) == 0);
    CHECK(irq_vector_free(&m, 0x40) == -1);        // double free

    // Legacy fixed-vector ownership.
    irq_vector_map_init(&m);
    CHECK(irq_vector_claim_legacy(&m, 1, 1) == 0x21);
    CHECK(irq_vector_state(&m, 0x21) == VEC_LEGACY_OWNED);
    CHECK(irq_vector_claim_legacy(&m, 1, 99) == -1);   // already owned
    CHECK(m.dup_claim_count == 1);
    CHECK(irq_vector_claim_legacy(&m, 16, 0) == -1);
    CHECK(irq_vector_claim_legacy(&m, -1, 0) == -1);
    CHECK(irq_vector_claim_legacy(&m, 0, 0) == 0x20);
    CHECK(irq_vector_claim_legacy(&m, 15, 15) == 0x2F);
    CHECK(irq_vector_free(&m, 0x21) == -1);            // generic free can't take a legacy vector
    for (int i = 0; i < 190; i++) { int v = irq_vector_alloc(&m, i); if (v < 0x30) { CHECK(0); break; } }
    CHECK(irq_vector_release_legacy(&m, 0x21) == 0);
    CHECK(irq_vector_release_legacy(&m, 0x21) == -1);
    CHECK(irq_vector_claim_legacy(&m, 1, 1) == 0x21);  // reclaimable
    group_end("vector allocator (reservation, 0x80 hole, 190-vector exhaustion, reuse, legacy ownership)");
}

// ═════════════════════════ 2. ACPI / MADT ═══════════════════════════
static void fix_sum(uint8_t* t, uint32_t len, uint32_t cksum_off) {
    t[cksum_off] = 0;
    uint8_t s = 0; for (uint32_t i = 0; i < len; i++) s = (uint8_t)(s + t[i]);
    t[cksum_off] = (uint8_t)(0 - s);
}
static void w16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void w64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

// Builds an SDT header (sig, length filled later) into buf.
static void hdr(uint8_t* b, const char* sig, uint32_t len) {
    zero(b, 36);
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)sig[i];
    w32(b + 4, len); b[8] = 1;
}

typedef struct { uint8_t b[512]; uint32_t len; } tbl_t;
static void madt_begin(tbl_t* t, uint32_t lapic_addr, uint32_t flags) {
    zero(t, sizeof(*t));
    hdr(t->b, "APIC", 44);
    w32(t->b + 36, lapic_addr); w32(t->b + 40, flags);
    t->len = 44;
}
static void e_lapic(tbl_t* t, uint8_t acpi, uint8_t apic, uint32_t fl) {
    uint8_t* e = t->b + t->len; e[0] = 0; e[1] = 8; e[2] = acpi; e[3] = apic; w32(e + 4, fl); t->len += 8;
}
static void e_ioapic(tbl_t* t, uint8_t id, uint32_t addr, uint32_t gsi) {
    uint8_t* e = t->b + t->len; e[0] = 1; e[1] = 12; e[2] = id; e[3] = 0; w32(e + 4, addr); w32(e + 8, gsi); t->len += 12;
}
static void e_iso(tbl_t* t, uint8_t bus, uint8_t src, uint32_t gsi, uint16_t fl) {
    uint8_t* e = t->b + t->len; e[0] = 2; e[1] = 10; e[2] = bus; e[3] = src; w32(e + 4, gsi); w16(e + 8, fl); t->len += 10;
}
static void e_nmi(tbl_t* t, uint8_t acpi, uint16_t fl, uint8_t lint) {
    uint8_t* e = t->b + t->len; e[0] = 4; e[1] = 6; e[2] = acpi; w16(e + 3, fl); e[5] = lint; t->len += 6;
}
static void e_raw(tbl_t* t, uint8_t type, uint8_t len, uint32_t fill) {
    uint8_t* e = t->b + t->len; e[0] = type; e[1] = len; for (uint32_t i = 2; i < fill; i++) e[i] = 0; t->len += fill;
}
static void madt_end(tbl_t* t) { w32(t->b + 4, t->len); fix_sum(t->b, t->len, 9); }

static madt64_info_t g_mi;   // static: large

static void build_good_madt(tbl_t* t) {
    madt_begin(t, 0xFEE00000, 1);
    e_lapic(t, 0, 0, 1);
    e_ioapic(t, 1, 0xFEC00000, 0);
    e_iso(t, 0, 0, 2, 0);          // IRQ0 -> GSI2, conforms
    e_iso(t, 0, 9, 9, 0xF);        // IRQ9 -> GSI9 level active-low
    e_iso(t, 1, 5, 20, 0xF);       // NOT ISA bus: must not apply to ISA IRQ5
    e_nmi(t, 0xFF, 0, 1);
    madt_end(t);
}

static void test_madt(void) {
    group_begin();
    static tbl_t t;

    build_good_madt(&t);
    CHECK(madt64_parse(t.b, t.len, &g_mi) == 0);
    CHECK(g_mi.n_lapic == 1 && g_mi.n_ioapic == 1 && g_mi.n_iso == 3 && g_mi.n_nmi == 1);
    CHECK(g_mi.lapic_phys == 0xFEE00000 && (g_mi.flags & 1));
    CHECK(g_mi.ioapic[0].addr == 0xFEC00000 && g_mi.ioapic[0].gsi_base == 0 && g_mi.ioapic[0].id == 1);
    CHECK(g_mi.iso[0].source == 0 && g_mi.iso[0].gsi == 2);
    CHECK(g_mi.nmi[0].acpi_id == 0xFF && g_mi.nmi[0].lint == 1);
    CHECK(madt64_parse(t.b, t.len + 100, &g_mi) == 0);    // avail > length is fine

    // Header-level rejection.
    CHECK(madt64_parse(0, 100, &g_mi) == MADT_ERR_NULL);
    CHECK(madt64_parse(t.b, 43, &g_mi) == MADT_ERR_TOO_SHORT);
    CHECK(madt64_parse(t.b, t.len - 1, &g_mi) == MADT_ERR_LENGTH);        // length > avail
    { tbl_t b = t; b.b[0] = 'X'; CHECK(madt64_parse(b.b, b.len, &g_mi) == MADT_ERR_SIGNATURE); }
    { tbl_t b = t; b.b[20] ^= 0x55; CHECK(madt64_parse(b.b, b.len, &g_mi) == MADT_ERR_CHECKSUM); }
    { tbl_t b = t; w32(b.b + 4, 40); fix_sum(b.b, 40, 9); CHECK(madt64_parse(b.b, b.len, &g_mi) == MADT_ERR_LENGTH); }

    // Entry-level bounds.
    { madt_begin(&t, 0xFEE00000, 1); e_raw(&t, 0, 0, 2); madt_end(&t);
      CHECK(madt64_parse(t.b, t.len, &g_mi) == MADT_ERR_ENTRY_LEN); }           // len 0
    { madt_begin(&t, 0xFEE00000, 1); e_raw(&t, 0, 1, 2); madt_end(&t);
      CHECK(madt64_parse(t.b, t.len, &g_mi) == MADT_ERR_ENTRY_LEN); }           // len 1
    { madt_begin(&t, 0xFEE00000, 1); e_raw(&t, 0x7F, 40, 6); madt_end(&t);
      CHECK(madt64_parse(t.b, t.len, &g_mi) == MADT_ERR_ENTRY_LEN); }           // runs past end
    { madt_begin(&t, 0xFEE00000, 1); t.b[t.len] = 0; t.len += 1; madt_end(&t);
      CHECK(madt64_parse(t.b, t.len, &g_mi) == MADT_ERR_ENTRY_LEN); }           // lone trailing byte
    { madt_begin(&t, 0xFEE00000, 1); e_raw(&t, 1, 8, 8); madt_end(&t);
      CHECK(madt64_parse(t.b, t.len, &g_mi) == MADT_ERR_ENTRY_SHORT); }         // IOAPIC needs 12
    { madt_begin(&t, 0xFEE00000, 1); e_raw(&t, 2, 8, 8); madt_end(&t);
      CHECK(madt64_parse(t.b, t.len, &g_mi) == MADT_ERR_ENTRY_SHORT); }         // ISO needs 10
    { madt_begin(&t, 0xFEE00000, 1); e_raw(&t, 0, 6, 6); madt_end(&t);
      CHECK(madt64_parse(t.b, t.len, &g_mi) == MADT_ERR_ENTRY_SHORT); }         // LAPIC needs 8
    { madt_begin(&t, 0xFEE00000, 1); e_iso(&t, 0, 3, 3, 0x2); madt_end(&t);
      CHECK(madt64_parse(t.b, t.len, &g_mi) == MADT_ERR_ISO_FLAGS); }           // reserved polarity 10
    { madt_begin(&t, 0xFEE00000, 1); e_iso(&t, 0, 3, 3, 0x8); madt_end(&t);
      CHECK(madt64_parse(t.b, t.len, &g_mi) == MADT_ERR_ISO_FLAGS); }           // reserved trigger 10

    // Unknown types are skipped by their own length; parsing continues.
    { madt_begin(&t, 0xFEE00000, 1);
      e_raw(&t, 9, 16, 16); e_raw(&t, 0x7F, 4, 4);
      e_ioapic(&t, 2, 0xFEC10000, 24); madt_end(&t);
      CHECK(madt64_parse(t.b, t.len, &g_mi) == 0);
      CHECK(g_mi.unknown_entries == 2 && g_mi.n_ioapic == 1 && g_mi.ioapic[0].gsi_base == 24); }

    // Type-5 LAPIC address override wins over the 32-bit header address.
    { madt_begin(&t, 0xFEE00000, 1);
      uint8_t* e = t.b + t.len; e[0] = 5; e[1] = 12; w16(e + 2, 0); w64(e + 4, 0x00000000FED12000ull); t.len += 12;
      madt_end(&t);
      CHECK(madt64_parse(t.b, t.len, &g_mi) == 0);
      CHECK(g_mi.lapic_phys == 0xFED12000ull && g_mi.lapic_addr_overridden); }

    // Fixed arrays never overflow.
    { madt_begin(&t, 0xFEE00000, 1);
      for (int i = 0; i < 30; i++) e_iso(&t, 0, (uint8_t)(i % 16), (uint32_t)i, 0);
      madt_end(&t);   // 30*10+44 = 344 < 512
      CHECK(madt64_parse(t.b, t.len, &g_mi) == 0 && g_mi.n_iso == 30 && g_mi.truncated == 0);
      for (int i = 0; i < 5; i++) e_iso(&t, 0, 1, 1, 0);
      madt_end(&t);   // 35 ISOs -> 32 kept
      CHECK(madt64_parse(t.b, t.len, &g_mi) == 0 && g_mi.n_iso == MADT64_MAX_ISO && g_mi.truncated == 3); }

    // ISA IRQ -> GSI resolution (incl. IRQ0 -> GSI2).
    build_good_madt(&t);
    madt64_parse(t.b, t.len, &g_mi);
    uint32_t gsi; int pol, trg;
    CHECK(madt64_resolve_isa(&g_mi, 0, &gsi, &pol, &trg) == 0 && gsi == 2 && pol == 0 && trg == 0);
    CHECK(madt64_resolve_isa(&g_mi, 1, &gsi, &pol, &trg) == 0 && gsi == 1 && pol == 0 && trg == 0);
    CHECK(madt64_resolve_isa(&g_mi, 9, &gsi, &pol, &trg) == 0 && gsi == 9 && pol == 1 && trg == 1);
    CHECK(madt64_resolve_isa(&g_mi, 5, &gsi, &pol, &trg) == 0 && gsi == 5 && pol == 0 && trg == 0); // bus 1 ISO ignored
    CHECK(madt64_resolve_isa(&g_mi, 12, &gsi, &pol, &trg) == 0 && gsi == 12);
    CHECK(madt64_resolve_isa(&g_mi, 16, &gsi, &pol, &trg) == -1);
    CHECK(madt64_resolve_isa(&g_mi, -1, &gsi, &pol, &trg) == -1);
    // Flag decode table.
    CHECK(madt64_decode_flags(0x0, &pol, &trg) == 0 && pol == 0 && trg == 0);   // conforms -> ISA edge/high
    CHECK(madt64_decode_flags(0x5, &pol, &trg) == 0 && pol == 0 && trg == 0);
    CHECK(madt64_decode_flags(0xF, &pol, &trg) == 0 && pol == 1 && trg == 1);
    CHECK(madt64_decode_flags(0xD, &pol, &trg) == 0 && pol == 0 && trg == 1);
    CHECK(madt64_decode_flags(0x7, &pol, &trg) == 0 && pol == 1 && trg == 0);
    CHECK(madt64_decode_flags(0x2, &pol, &trg) == -1);
    CHECK(madt64_decode_flags(0x8, &pol, &trg) == -1);
    // LAPIC lookup.
    CHECK(madt64_find_lapic(&g_mi, 0) != 0 && madt64_find_lapic(&g_mi, 7) == 0);
    group_end("MADT parse (checksum, bounds, unknown types, overrides, truncation) + ISA IRQ->GSI (IRQ0->GSI2)");
}

// Synthetic physical memory for the RSDT/XSDT walk.
static uint8_t g_arena[4096];
#define ARENA_PHYS 0x100000ull
static const void* arena_map(uint64_t phys, uint64_t len) {
    if (phys < ARENA_PHYS || phys + len > ARENA_PHYS + sizeof(g_arena) || phys + len < phys) return 0;
    return g_arena + (phys - ARENA_PHYS);
}
static void put_table(uint32_t off, const char* sig, uint32_t len, int good_sum) {
    hdr(g_arena + off, sig, len);
    fix_sum(g_arena + off, len, 9);
    if (!good_sum) g_arena[off + 10] ^= 0xFF;
}

static void test_acpi(void) {
    group_begin();
    static uint8_t rsdp[36];

    zero(rsdp, sizeof(rsdp));
    for (int i = 0; i < 8; i++) rsdp[i] = (uint8_t)"RSD PTR "[i];
    rsdp[15] = 0; w32(rsdp + 16, 0x1234);
    fix_sum(rsdp, 20, 8);
    CHECK(acpi64_rsdp_valid(rsdp, 20));                       // v1
    CHECK(!acpi64_rsdp_valid(rsdp, 19));
    rsdp[15] = 2; w32(rsdp + 20, 36); w64(rsdp + 24, 0x5000);
    fix_sum(rsdp, 20, 8); fix_sum(rsdp, 36, 32);
    CHECK(acpi64_rsdp_valid(rsdp, 36));                       // v2
    CHECK(!acpi64_rsdp_valid(rsdp, 20));                      // v2 but truncated
    rsdp[32] ^= 1;
    CHECK(!acpi64_rsdp_valid(rsdp, 36));                      // bad extended checksum
    rsdp[32] ^= 1; rsdp[0] = 'X';
    CHECK(!acpi64_rsdp_valid(rsdp, 36));                      // bad signature

    // scan_rsdp over a buffer with the RSDP at offset 48.
    { static uint8_t buf[256]; zero(buf, sizeof(buf));
      for (int i = 0; i < 8; i++) buf[48 + i] = (uint8_t)"RSD PTR "[i];
      w32(buf + 48 + 16, 0x1000); fix_sum(buf + 48, 20, 8);
      CHECK(acpi64_scan_rsdp(buf, sizeof(buf)) == 48);
      buf[48 + 9] ^= 1;
      CHECK(acpi64_scan_rsdp(buf, sizeof(buf)) == -1); }

    // sdt_valid.
    put_table(0, "APIC", 60, 1);
    CHECK(acpi64_sdt_valid(g_arena, 60, "APIC"));
    CHECK(!acpi64_sdt_valid(g_arena, 59, "APIC"));            // length > avail
    CHECK(!acpi64_sdt_valid(g_arena, 60, "FACP"));
    CHECK(acpi64_sdt_valid(g_arena, 60, 0));
    put_table(0, "APIC", 60, 0);
    CHECK(!acpi64_sdt_valid(g_arena, 60, "APIC"));            // bad checksum
    put_table(0, "APIC", 20, 1);
    CHECK(!acpi64_sdt_valid(g_arena, 60, "APIC"));            // length < header
    CHECK(!acpi64_sdt_valid(g_arena, 10, 0));                 // header does not fit

    // RSDT (32-bit entries) -> tables; MADT is the 3rd of 4.
    zero(g_arena, sizeof(g_arena));
    put_table(0x100, "FACP", 60, 1);
    put_table(0x200, "SSDT", 60, 1);
    put_table(0x300, "APIC", 80, 1);
    put_table(0x400, "HPET", 60, 0);             // bad checksum
    uint32_t rlen = 36 + 4 * 5;
    hdr(g_arena + 0, "RSDT", rlen);
    w32(g_arena + 36 + 0, (uint32_t)ARENA_PHYS + 0x100);
    w32(g_arena + 36 + 4, (uint32_t)ARENA_PHYS + 0x200);
    w32(g_arena + 36 + 8, 0);                                  // NULL entry skipped
    w32(g_arena + 36 + 12, (uint32_t)ARENA_PHYS + 0x300);
    w32(g_arena + 36 + 16, (uint32_t)ARENA_PHYS + 0x400);
    fix_sum(g_arena, rlen, 9);
    const acpi_sdt_header_t* f = acpi64_root_find(ARENA_PHYS, 0, "APIC", arena_map);
    CHECK(f && (const uint8_t*)f == g_arena + 0x300);
    CHECK(acpi64_root_find(ARENA_PHYS, 0, "MCFG", arena_map) == 0);   // absent
    CHECK(acpi64_root_find(ARENA_PHYS, 0, "HPET", arena_map) == 0);   // present but bad checksum
    CHECK(acpi64_root_find(ARENA_PHYS, 1, "APIC", arena_map) == 0);   // wrong root type
    CHECK(acpi64_root_find(0, 0, "APIC", arena_map) == 0);
    CHECK(acpi64_root_find(ARENA_PHYS + 0x10000, 0, "APIC", arena_map) == 0);  // unmappable root
    // Entry pointing outside mapped memory is skipped, not dereferenced.
    w32(g_arena + 36 + 12, 0x7FFF0000u); fix_sum(g_arena, rlen, 9);
    CHECK(acpi64_root_find(ARENA_PHYS, 0, "APIC", arena_map) == 0);

    // XSDT (64-bit entries).
    zero(g_arena, sizeof(g_arena));
    put_table(0x300, "APIC", 80, 1);
    uint32_t xlen = 36 + 8 * 2;
    hdr(g_arena + 0, "XSDT", xlen);
    w64(g_arena + 36, 0xFFFFFFFF00000000ull);                  // unmappable 64-bit
    w64(g_arena + 44, ARENA_PHYS + 0x300);
    fix_sum(g_arena, xlen, 9);
    f = acpi64_root_find(ARENA_PHYS, 1, "APIC", arena_map);
    CHECK(f && (const uint8_t*)f == g_arena + 0x300);
    // Root length lies (huge): rejected.
    w32(g_arena + 4, 0x7FFFFFFF);
    CHECK(acpi64_root_find(ARENA_PHYS, 1, "APIC", arena_map) == 0);
    group_end("ACPI RSDP/RSDT/XSDT validation and bounds");
}

// ═════════════════════════ 3. IOAPIC pure logic ═════════════════════
static void test_ioapic_pure(void) {
    group_begin();
    ioapic64_t a[3];
    zero(a, sizeof(a));
    a[0].gsi_base = 0;  a[0].nr_entries = 24;
    a[1].gsi_base = 24; a[1].nr_entries = 8;
    uint32_t pin = 99;
    CHECK(ioapic64_find(a, 2, 0, &pin) == 0 && pin == 0);
    CHECK(ioapic64_find(a, 2, 23, &pin) == 0 && pin == 23);
    CHECK(ioapic64_find(a, 2, 24, &pin) == 1 && pin == 0);
    CHECK(ioapic64_find(a, 2, 31, &pin) == 1 && pin == 7);
    CHECK(ioapic64_find(a, 2, 32, &pin) == -1);
    CHECK(ioapic64_find(a, 2, 0xFFFFFFFFu, &pin) == -1);
    CHECK(ioapic64_find(a, 0, 0, &pin) == -1);
    CHECK(ioapic64_ranges_valid(a, 2));
    a[1].gsi_base = 20;   CHECK(!ioapic64_ranges_valid(a, 2));                // overlap
    a[1].gsi_base = 24;   a[1].nr_entries = 0;  CHECK(!ioapic64_ranges_valid(a, 2));   // empty
    a[1].nr_entries = 300; CHECK(!ioapic64_ranges_valid(a, 2));               // absurd
    a[1].nr_entries = 8;
    a[2].gsi_base = 0xFFFFFFF8u; a[2].nr_entries = 16;                        // would wrap in 32-bit
    CHECK(ioapic64_ranges_valid(a, 3));
    CHECK(ioapic64_find(a, 3, 2, &pin) == 0);                                  // no wrap-around false match

    // Redirection entry encode/decode.
    ioapic64_rte_t r = {0};
    r.vector = 0x20; r.polarity = 1; r.trigger = 1; r.masked = 1; r.dest = 0xAB;
    uint64_t raw = ioapic64_rte_encode(&r);
    CHECK(raw == (0x20ull | (1ull << 13) | (1ull << 15) | (1ull << 16) | (0xABull << 56)));
    ioapic64_rte_t d;
    ioapic64_rte_decode(raw, &d);
    CHECK(d.vector == 0x20 && d.polarity == 1 && d.trigger == 1 && d.masked == 1 && d.dest == 0xAB &&
          d.delivery == 0 && d.dest_mode == 0 && d.remote_irr == 0 && d.pending == 0);
    // Read-only status bits are decoded but never encoded.
    ioapic64_rte_decode(raw | (1ull << 12) | (1ull << 14), &d);
    CHECK(d.pending == 1 && d.remote_irr == 1);
    CHECK(ioapic64_rte_encode(&d) == raw);
    // All-zero fields -> plain unmasked edge/high fixed vector.
    zero(&r, sizeof(r)); r.vector = 0x2C;
    CHECK(ioapic64_rte_encode(&r) == 0x2C);
    // Round trip across vectors/destinations/modes.
    int rt = 1;
    for (int v = 0; v < 256; v += 5) {
        for (int x = 0; x < 64; x++) {
            ioapic64_rte_t s = {0}, o;
            s.vector = (uint8_t)v; s.delivery = (uint8_t)(x & 7); s.dest_mode = (uint8_t)((x >> 3) & 1);
            s.polarity = (uint8_t)(x & 1); s.trigger = (uint8_t)((x >> 1) & 1); s.masked = (uint8_t)((x >> 2) & 1);
            s.dest = (uint8_t)(v ^ x);
            ioapic64_rte_decode(ioapic64_rte_encode(&s), &o);
            if (o.vector != s.vector || o.delivery != s.delivery || o.dest_mode != s.dest_mode ||
                o.polarity != s.polarity || o.trigger != s.trigger || o.masked != s.masked || o.dest != s.dest) rt = 0;
        }
    }
    CHECK(rt);
    CHECK(IOAPIC64_REG_RTE(0) == 0x10 && IOAPIC64_REG_RTE(1) == 0x12 && IOAPIC64_REG_RTE(23) == 0x3E);
    group_end("IOAPIC GSI-range mapping + redirection entry encode/decode");
}

// ═════════════════════════ 4. PIC spurious ══════════════════════════
static void test_pic_spurious(void) {
    group_begin();
    CHECK(pic_chip_classify(7, 0x00, 0x00) == PIC_SPUR_NO_EOI);        // spurious IRQ7: no EOI at all
    CHECK(pic_chip_classify(7, 0x80, 0x00) == PIC_SPUR_NONE);          // real IRQ7
    CHECK(pic_chip_classify(7, 0x7F, 0xFF) == PIC_SPUR_NO_EOI);        // ISR bit 7 is what matters
    CHECK(pic_chip_classify(15, 0x04, 0x00) == PIC_SPUR_MASTER_EOI);   // spurious IRQ15: master EOI only
    CHECK(pic_chip_classify(15, 0x00, 0x80) == PIC_SPUR_NONE);         // real IRQ15
    for (int i = 0; i < 16; i++) {
        if (i == 7 || i == 15) continue;
        CHECK(pic_chip_classify(i, 0, 0) == PIC_SPUR_NONE);            // no other line can be spurious
    }
    group_end("8259 spurious IRQ7 (no EOI) / IRQ15 (master EOI only) classification");
}

// ═════════════════ 5. dispatch flows (fake chip) ════════════════════
static char g_log[512];
static int  g_log_n;
static void lg(char c) { if (g_log_n < (int)sizeof(g_log) - 1) g_log[g_log_n++] = c; g_log[g_log_n] = 0; }
static void log_reset(void) { g_log_n = 0; g_log[0] = 0; }
static int  log_is(const char* s) { int i = 0; while (s[i] && g_log[i] == s[i]) i++; return s[i] == 0 && g_log[i] == 0; }

static int g_chip_spur, g_chip_spur_ret;
static void fc_mask(irq64_desc_t* d)   { (void)d; lg('M'); }
static void fc_unmask(irq64_desc_t* d) { (void)d; lg('U'); }
static void fc_eoi(irq64_desc_t* d)    { (void)d; lg('E'); }
static int  fc_spur(irq64_desc_t* d)   { (void)d; g_chip_spur++; return g_chip_spur_ret; }
static const irq64_chip_t fake_chip     = { "fake", fc_mask, fc_unmask, fc_eoi, 0, 0, 0 };
static const irq64_chip_t fake_spur_chip = { "fake-spur", fc_mask, fc_unmask, fc_eoi, fc_spur, 0, 0 };

static irq64_ret_t g_h_ret;
static int g_h_calls;
static void* g_h_ctx_seen;
static irq64_ret_t fake_handler(void* ctx) { lg('H'); g_h_calls++; g_h_ctx_seen = ctx; return g_h_ret; }

static int g_route_rc, g_route_trigger;
static int fake_route(irq64_table_t* t, irq64_desc_t* d, uint8_t isa, uint8_t vec) {
    (void)t; (void)vec;
    d->hwirq = isa + 100u; d->trigger = (uint8_t)g_route_trigger; d->polarity = IRQ64_POL_LOW;
    lg('R');
    return g_route_rc;
}
static int g_orphan_calls; static uint8_t g_orphan_vec;
static void fake_orphan(uint8_t v) { g_orphan_calls++; g_orphan_vec = v; lg('O'); }

static irq64_table_t g_ft;   // static: ~20KB

static void test_dispatch(void) {
    group_begin();
    int ctxv = 0;

    // Legacy request/route/unmask ordering and vector ownership.
    irq64_table_init(&g_ft, &fake_chip);
    g_ft.route = fake_route; g_ft.orphan_eoi = fake_orphan;
    g_route_rc = 0; g_route_trigger = IRQ64_TRIG_EDGE; g_h_ret = IRQ64_RET_HANDLED; g_h_calls = 0;
    log_reset();
    CHECK(irq64_table_request_legacy(&g_ft, 1, fake_handler, &ctxv, "kbd") == 0);
    CHECK(log_is("RU"));                                            // routed masked, THEN unmasked
    CHECK(g_ft.desc[1].vector == 0x21 && g_ft.vec_to_irq[0x21] == 1);
    CHECK(irq_vector_state(&g_ft.vmap, 0x21) == VEC_LEGACY_OWNED);
    CHECK(g_ft.desc[1].hwirq == 101 && g_ft.desc[1].requested);
    CHECK(irq64_table_request_legacy(&g_ft, 1, fake_handler, 0, "dup") == -2);   // duplicate IRQ
    CHECK(irq64_table_request_legacy(&g_ft, 16, fake_handler, 0, "x") == -1);
    CHECK(irq64_table_request_legacy(&g_ft, 3, 0, 0, "x") == -1);                // no handler

    // Route failure releases the vector and leaves the IRQ requestable.
    g_route_rc = -1; log_reset();
    CHECK(irq64_table_request_legacy(&g_ft, 4, fake_handler, 0, "bad") == -4);
    CHECK(log_is("R"));
    CHECK(irq_vector_state(&g_ft.vmap, 0x24) == VEC_LEGACY_FREE && !g_ft.desc[4].requested);
    g_route_rc = 0;
    CHECK(irq64_table_request_legacy(&g_ft, 4, fake_handler, 0, "ok") == 0);
    CHECK(irq64_table_free_legacy(&g_ft, 4) == 0);
    CHECK(irq_vector_state(&g_ft.vmap, 0x24) == VEC_LEGACY_FREE && !g_ft.desc[4].requested);
    CHECK(irq64_table_free_legacy(&g_ft, 4) == -2);

    // Edge: EOI first, then the handler; ctx passed through; HANDLED counted.
    log_reset(); g_h_calls = 0;
    irq64_table_dispatch(&g_ft, 0x21);
    CHECK(log_is("EH") && g_h_calls == 1 && g_h_ctx_seen == &ctxv);
    CHECK(g_ft.desc[1].count == 1 && g_ft.desc[1].handled == 1 && g_ft.desc[1].unhandled == 0);

    // Edge + handler NONE: counted as unhandled, never masked, even after many.
    g_h_ret = IRQ64_RET_NONE; log_reset();
    for (int i = 0; i < 200; i++) irq64_table_dispatch(&g_ft, 0x21);
    CHECK(g_ft.desc[1].unhandled == 200 && g_ft.desc[1].storm_masks == 0 && !g_ft.desc[1].storm_masked);
    int only_eh = 1;
    for (int i = 0; i < 400; i++) if (g_log[i] != (i % 2 ? 'H' : 'E')) only_eh = 0;
    CHECK(only_eh && g_log_n == 400);                                   // strictly E,H,E,H... no mask ever
    CHECK(g_ft.desc[1].handled == 1);

    // Unrequested legacy vector: chip EOI only, no handler, no crash.
    log_reset(); g_h_calls = 0;
    irq64_table_dispatch(&g_ft, 0x22);
    CHECK(log_is("E") && g_h_calls == 0 && g_ft.desc[2].count == 1 && g_ft.desc[2].unhandled == 1);

    // Orphan vector (no descriptor): orphan EOI hook, never a handler.
    log_reset();
    irq64_table_dispatch(&g_ft, 0x90);
    CHECK(log_is("O") && g_orphan_calls == 1 && g_orphan_vec == 0x90 && g_ft.orphan_vectors == 1);

    // Level: handler FIRST, EOI after.
    irq64_table_init(&g_ft, &fake_chip);
    g_ft.route = fake_route;
    g_route_trigger = IRQ64_TRIG_LEVEL; g_h_ret = IRQ64_RET_HANDLED;
    CHECK(irq64_table_request_legacy(&g_ft, 5, fake_handler, 0, "lvl") == 0);
    CHECK(g_ft.desc[5].trigger == IRQ64_TRIG_LEVEL);
    log_reset();
    irq64_table_dispatch(&g_ft, 0x25);
    CHECK(log_is("HE"));
    // Serviced level line never trips the storm guard however often it fires.
    for (int i = 0; i < 1000; i++) irq64_table_dispatch(&g_ft, 0x25);
    CHECK(g_ft.desc[5].storm_masks == 0 && !g_ft.desc[5].masked);

    // Synthetic level storm: 99 NONE -> not masked; the 100th -> masked BEFORE its EOI.
    g_h_ret = IRQ64_RET_NONE; log_reset();
    for (int i = 0; i < IRQ64_STORM_THRESHOLD - 1; i++) irq64_table_dispatch(&g_ft, 0x25);
    CHECK(g_ft.desc[5].storm_masks == 0 && !g_ft.desc[5].masked);
    log_reset();
    irq64_table_dispatch(&g_ft, 0x25);
    CHECK(log_is("HME"));
    CHECK(g_ft.desc[5].storm_masks == 1 && g_ft.desc[5].masked && g_ft.desc[5].storm_masked);

    // A HANDLED result resets the consecutive-NONE run.
    irq64_table_init(&g_ft, &fake_chip); g_ft.route = fake_route;
    CHECK(irq64_table_request_legacy(&g_ft, 5, fake_handler, 0, "lvl") == 0);
    g_h_ret = IRQ64_RET_NONE;
    for (int i = 0; i < IRQ64_STORM_THRESHOLD - 1; i++) irq64_table_dispatch(&g_ft, 0x25);
    g_h_ret = IRQ64_RET_HANDLED; irq64_table_dispatch(&g_ft, 0x25);
    g_h_ret = IRQ64_RET_NONE;
    for (int i = 0; i < IRQ64_STORM_THRESHOLD - 1; i++) irq64_table_dispatch(&g_ft, 0x25);
    CHECK(g_ft.desc[5].storm_masks == 0);

    // Level source with no handler at all is also storm-guarded.
    irq64_table_init(&g_ft, &fake_chip);
    g_ft.desc[6].trigger = IRQ64_TRIG_LEVEL;      // stray level line, nobody requested it
    log_reset();
    for (int i = 0; i < IRQ64_STORM_THRESHOLD; i++) irq64_table_dispatch(&g_ft, 0x26);
    CHECK(g_ft.desc[6].storm_masks == 1 && g_ft.desc[6].unhandled == (uint32_t)IRQ64_STORM_THRESHOLD);

    // Chip-level spurious: no handler, no EOI from the core (the chip did its own).
    irq64_table_init(&g_ft, &fake_spur_chip); g_ft.route = fake_route;
    g_route_trigger = IRQ64_TRIG_EDGE; g_h_ret = IRQ64_RET_HANDLED; g_h_calls = 0;
    CHECK(irq64_table_request_legacy(&g_ft, 7, fake_handler, 0, "lpt") == 0);
    g_chip_spur = 0; g_chip_spur_ret = 1; log_reset();
    irq64_table_dispatch(&g_ft, 0x27);
    CHECK(g_chip_spur == 1 && g_h_calls == 0 && g_ft.desc[7].spurious == 1 && g_ft.desc[7].handled == 0);
    CHECK(log_is(""));                                            // no E, no H
    g_chip_spur_ret = 0; log_reset();
    irq64_table_dispatch(&g_ft, 0x27);                            // real delivery on the same chip
    CHECK(log_is("EH") && g_h_calls == 1 && g_ft.desc[7].spurious == 1);

    // Handler-return semantics summary.
    CHECK(IRQ64_RET_NONE == 0 && IRQ64_RET_HANDLED == 1);
    group_end("dispatch flows: edge EOI-before-handler, level handler-then-EOI, storm guard, NONE/HANDLED, chip spurious, orphans, request/free");
}

// ═════════════════ 6. LAPIC 0xFF spurious: real IDT path ════════════
static void test_lapic_spurious(void) {
    group_begin();
    uint64_t sp0 = lapic64_spurious_count, eoi0 = lapic64_eoi_count;
    __asm__ volatile ("int $0xFF" ::: "memory");
    __asm__ volatile ("int $0xFF" ::: "memory");
    __asm__ volatile ("int $0xFF" ::: "memory");
    CHECK(lapic64_spurious_count == sp0 + 3);      // the real 0xFF gate ran three times
    CHECK(lapic64_eoi_count == eoi0);              // ...and sent NO EOI
    group_end("LAPIC spurious vector 0xFF (real IDT gate): counted, NO EOI");
}

// ═════════════════ 7. transactional PIC->APIC (fake CPU) ════════════
typedef struct {
    int      apic_hw;          // CPU physically has an APIC (CPUID flag also needs global enable)
    int      msr_ok;
    int      enable_refused;
    int      map_fail, io_fail, stuck_svr;
    uint64_t apic_base;
    uint32_t lapic[0x400 / 4];
    uint8_t  imr[2];
    // instrumentation
    int      rdmsr_calls, wrmsr_calls, map_calls, io_calls, bad_access;
    uint64_t mapped_phys;
} fakecpu_t;

static fakecpu_t F;

static int f_cpuid(void)   { return F.apic_hw && (F.apic_base & APIC_BASE_GLOBAL_EN); }
static int f_msr_ok(void)  { return F.msr_ok; }
static uint64_t f_rdmsr(uint32_t m) { F.rdmsr_calls++; return m == MSR_IA32_APIC_BASE ? F.apic_base : 0; }
static void f_wrmsr(uint32_t m, uint64_t v) {
    F.wrmsr_calls++;
    if (m != MSR_IA32_APIC_BASE || F.enable_refused) return;
    F.apic_base = v;
}
static int f_map(uint64_t p) { F.map_calls++; F.mapped_phys = p; return F.map_fail ? -1 : 0; }
static uint32_t f_lr(uint32_t r) {
    if (!(F.apic_base & APIC_BASE_GLOBAL_EN)) { F.bad_access++; return 0xFFFFFFFFu; }
    return F.lapic[r / 4];
}
static void f_lw(uint32_t r, uint32_t v) {
    if (!(F.apic_base & APIC_BASE_GLOBAL_EN)) { F.bad_access++; return; }
    if (r == LAPIC_REG_SVR && F.stuck_svr) return;
    F.lapic[r / 4] = v;
}
static uint8_t f_pg(int s) { return F.imr[s ? 1 : 0]; }
static void f_ps(int s, uint8_t m) { F.imr[s ? 1 : 0] = m; }
static int f_io(const madt64_info_t* m) { (void)m; F.io_calls++; return F.io_fail ? -1 : 0; }
static const irqmode_hw_t fake_hw = { f_cpuid, f_msr_ok, f_rdmsr, f_wrmsr, f_map, f_lr, f_lw, f_pg, f_ps, f_io };

static void fake_reset(void) {
    zero(&F, sizeof(F));
    F.apic_hw = 1; F.msr_ok = 1;
    F.apic_base = 0xFEE00000ull | APIC_BASE_BSP | APIC_BASE_GLOBAL_EN;
    // Distinctive, non-default "firmware" values so a wrong restore is visible.
    F.lapic[LAPIC_REG_ID / 4]         = 0;
    F.lapic[LAPIC_REG_TPR / 4]        = 0x20;
    F.lapic[LAPIC_REG_SVR / 4]        = 0x0FF;
    F.lapic[LAPIC_REG_LVT_TIMER / 4]  = 0x000200EF;
    F.lapic[LAPIC_REG_LVT_THERMAL / 4]= 0x000000FA;
    F.lapic[LAPIC_REG_LVT_PERF / 4]   = 0x000000FB;
    F.lapic[LAPIC_REG_LINT0 / 4]      = 0x00000700;
    F.lapic[LAPIC_REG_LINT1 / 4]      = 0x00000400;
    F.lapic[LAPIC_REG_LVT_ERROR / 4]  = 0x000000FE;
    F.imr[0] = 0xB8; F.imr[1] = 0xEF;
}

typedef struct { uint64_t apic_base; uint32_t lapic[0x400 / 4]; uint8_t imr[2]; } cpustate_t;
static void cap(cpustate_t* s) { s->apic_base = F.apic_base; for (int i = 0; i < 0x400 / 4; i++) s->lapic[i] = F.lapic[i]; s->imr[0] = F.imr[0]; s->imr[1] = F.imr[1]; }
static int same(const cpustate_t* s) {
    if (s->apic_base != F.apic_base || s->imr[0] != F.imr[0] || s->imr[1] != F.imr[1]) return 0;
    for (int i = 0; i < 0x400 / 4; i++) if (s->lapic[i] != F.lapic[i]) return 0;
    return 1;
}

static madt64_info_t g_tm;
static void tmadt(void) {
    zero(&g_tm, sizeof(g_tm));
    g_tm.lapic_phys = 0xFEE00000ull;
    g_tm.n_lapic = 1; g_tm.lapic[0].acpi_id = 0; g_tm.lapic[0].apic_id = 0; g_tm.lapic[0].flags = 1;
    g_tm.n_ioapic = 1; g_tm.ioapic[0].id = 1; g_tm.ioapic[0].addr = 0xFEC00000u; g_tm.ioapic[0].gsi_base = 0;
}

static void test_transaction(void) {
    group_begin();
    irqmode_result_t r;
    cpustate_t before;

    // ── success, firmware left the xAPIC enabled ──
    fake_reset(); tmadt(); cap(&before);
    irqmode64_commit(&fake_hw, &g_tm, 0, &r);
    CHECK(r.apic == 1 && r.reason == IRQMODE_REASON_OK && !r.rolled_back && !r.enabled_apic_ourselves);
    CHECK(F.imr[0] == 0xFF && F.imr[1] == 0xFF);
    CHECK((F.lapic[LAPIC_REG_LINT0 / 4] & LAPIC_LVT_MASKED) && (F.lapic[LAPIC_REG_LINT0 / 4] & 0x700) == 0x700);
    CHECK(F.lapic[LAPIC_REG_LINT1 / 4] == 0x400);
    CHECK(F.lapic[LAPIC_REG_LVT_TIMER / 4]   == (0x000200EFu | LAPIC_LVT_MASKED));
    CHECK(F.lapic[LAPIC_REG_LVT_THERMAL / 4] == (0xFAu | LAPIC_LVT_MASKED));
    CHECK(F.lapic[LAPIC_REG_LVT_PERF / 4]    == (0xFBu | LAPIC_LVT_MASKED));
    CHECK(F.lapic[LAPIC_REG_LVT_ERROR / 4]   == (0xFEu | LAPIC_LVT_MASKED));
    CHECK(F.lapic[LAPIC_REG_SVR / 4] == 0x1FF && F.lapic[LAPIC_REG_TPR / 4] == 0);
    CHECK(F.io_calls == 1 && F.map_calls == 1 && F.mapped_phys == 0xFEE00000ull && F.bad_access == 0);
    CHECK(r.bsp_apic_id == 0 && r.lapic_phys == 0xFEE00000ull && !r.madt_lapic_mismatch);

    // ── MSR base is authoritative over the MADT address ──
    fake_reset(); tmadt(); g_tm.lapic_phys = 0xFEE01000ull;
    irqmode64_commit(&fake_hw, &g_tm, 0, &r);
    CHECK(r.apic == 1 && r.madt_lapic_mismatch && F.mapped_phys == 0xFEE00000ull);

    // ── LAPIC NMI entries: matching processor, polarity/trigger honored ──
    fake_reset(); tmadt();
    g_tm.n_nmi = 1; g_tm.nmi[0].acpi_id = 0xFF; g_tm.nmi[0].flags = 0xF; g_tm.nmi[0].lint = 1;
    irqmode64_commit(&fake_hw, &g_tm, 0, &r);
    CHECK(r.apic && F.lapic[LAPIC_REG_LINT1 / 4] == (0x400u | (1u << 13) | (1u << 15)));
    CHECK(F.lapic[LAPIC_REG_LINT0 / 4] & LAPIC_LVT_MASKED);
    fake_reset(); tmadt();
    g_tm.n_nmi = 1; g_tm.nmi[0].acpi_id = 0; g_tm.nmi[0].flags = 0; g_tm.nmi[0].lint = 0;
    irqmode64_commit(&fake_hw, &g_tm, 0, &r);
    CHECK(r.apic && F.lapic[LAPIC_REG_LINT0 / 4] == 0x400 && (F.lapic[LAPIC_REG_LINT1 / 4] & LAPIC_LVT_MASKED));
    fake_reset(); tmadt();
    g_tm.n_nmi = 1; g_tm.nmi[0].acpi_id = 7; g_tm.nmi[0].flags = 0xF; g_tm.nmi[0].lint = 1;   // another CPU's
    irqmode64_commit(&fake_hw, &g_tm, 0, &r);
    CHECK(r.apic && F.lapic[LAPIC_REG_LINT1 / 4] == 0x400);

    // ── failure: LAPIC map fails -> snapshot restored exactly ──
    fake_reset(); tmadt(); F.map_fail = 1; cap(&before);
    irqmode64_commit(&fake_hw, &g_tm, 0, &r);
    CHECK(!r.apic && r.reason == IRQMODE_REASON_LAPIC_MAP_FAILED && r.rolled_back && same(&before) && F.io_calls == 0);

    // ── failure: BSP APIC id not in MADT ──
    fake_reset(); tmadt(); F.lapic[LAPIC_REG_ID / 4] = 5u << 24; cap(&before);
    irqmode64_commit(&fake_hw, &g_tm, 0, &r);
    CHECK(!r.apic && r.reason == IRQMODE_REASON_BSP_NOT_IN_MADT && r.rolled_back && same(&before) && F.io_calls == 0);

    // ── failure: readback verify (SVR stuck) -> every register restored ──
    fake_reset(); tmadt(); F.stuck_svr = 1; cap(&before);
    irqmode64_commit(&fake_hw, &g_tm, 0, &r);
    CHECK(!r.apic && r.reason == IRQMODE_REASON_VERIFY_FAILED && r.rolled_back && same(&before) && F.io_calls == 0);
    CHECK(F.imr[0] == 0xB8 && F.imr[1] == 0xEF);
    CHECK(F.lapic[LAPIC_REG_LINT0 / 4] == 0x700 && F.lapic[LAPIC_REG_LINT1 / 4] == 0x400);

    // ── failure: IOAPIC init (last step) -> full rollback incl. PIC IMRs, LINT0/LINT1, LVTs ──
    fake_reset(); tmadt(); F.io_fail = 1; cap(&before);
    irqmode64_commit(&fake_hw, &g_tm, 0, &r);
    CHECK(!r.apic && r.reason == IRQMODE_REASON_IOAPIC_INIT_FAILED && r.rolled_back && F.io_calls == 1);
    CHECK(same(&before));
    CHECK(F.imr[0] == 0xB8 && F.imr[1] == 0xEF);                          // actual previous IMRs, not 0x00/0xFF defaults
    CHECK(F.lapic[LAPIC_REG_LINT0 / 4] == 0x700 && F.lapic[LAPIC_REG_LINT1 / 4] == 0x400);
    CHECK(F.lapic[LAPIC_REG_LVT_TIMER / 4] == 0x000200EF && F.lapic[LAPIC_REG_LVT_THERMAL / 4] == 0xFA &&
          F.lapic[LAPIC_REG_LVT_PERF / 4] == 0xFB && F.lapic[LAPIC_REG_LVT_ERROR / 4] == 0xFE);
    CHECK(F.lapic[LAPIC_REG_SVR / 4] == 0x0FF && F.lapic[LAPIC_REG_TPR / 4] == 0x20);

    // ── firmware left APIC globally DISABLED (CPUID flag reads 0) but the CPU has one ──
    fake_reset(); tmadt(); F.apic_base &= ~APIC_BASE_GLOBAL_EN;
    CHECK(!f_cpuid() && f_msr_ok());
    irqmode64_commit(&fake_hw, &g_tm, 0, &r);
    CHECK(r.apic == 1 && r.enabled_apic_ourselves && (F.apic_base & APIC_BASE_GLOBAL_EN) && !(F.apic_base & APIC_BASE_X2APIC_EN));
    CHECK(F.bad_access == 0);                                              // LAPIC never touched before it was enabled
    // ...then a later failure must also DISABLE it again (actual previous MSR value).
    fake_reset(); tmadt(); F.apic_base &= ~APIC_BASE_GLOBAL_EN; F.io_fail = 1; cap(&before);
    irqmode64_commit(&fake_hw, &g_tm, 0, &r);
    CHECK(!r.apic && r.rolled_back && r.enabled_apic_ourselves && same(&before) && !(F.apic_base & APIC_BASE_GLOBAL_EN));
    fake_reset(); tmadt(); F.apic_base &= ~APIC_BASE_GLOBAL_EN; F.map_fail = 1; cap(&before);
    irqmode64_commit(&fake_hw, &g_tm, 0, &r);
    CHECK(!r.apic && r.rolled_back && same(&before));
    // Enable refused by hardware: fall back to PIC, MSR left as found.
    fake_reset(); tmadt(); F.apic_base &= ~APIC_BASE_GLOBAL_EN; F.enable_refused = 1; cap(&before);
    irqmode64_commit(&fake_hw, &g_tm, 0, &r);
    CHECK(!r.apic && r.reason == IRQMODE_REASON_ENABLE_REFUSED && same(&before) && F.io_calls == 0 && F.map_calls == 0);

    // ── genuinely no APIC in the CPU (no CPUID flag AND no MSR) ──
    fake_reset(); tmadt(); F.apic_hw = 0; F.msr_ok = 0; cap(&before);
    irqmode64_commit(&fake_hw, &g_tm, 0, &r);
    CHECK(!r.apic && r.reason == IRQMODE_REASON_CPU_NO_APIC && F.rdmsr_calls == 0 && F.wrmsr_calls == 0 && same(&before));

    // ── x2APIC already active: PIC fallback, hardware untouched, never switched back ──
    fake_reset(); tmadt(); F.apic_base |= APIC_BASE_X2APIC_EN; cap(&before);
    irqmode64_commit(&fake_hw, &g_tm, 0, &r);
    CHECK(!r.apic && r.reason == IRQMODE_REASON_X2APIC_ACTIVE && F.wrmsr_calls == 0 && F.map_calls == 0 && F.io_calls == 0 && same(&before) && !r.rolled_back);

    // ── no MADT / no IOAPIC: PIC, nothing touched ──
    fake_reset(); tmadt(); cap(&before);
    irqmode64_commit(&fake_hw, 0, 0, &r);
    CHECK(!r.apic && r.reason == IRQMODE_REASON_NO_MADT && F.rdmsr_calls == 0 && same(&before));
    g_tm.n_ioapic = 0;
    irqmode64_commit(&fake_hw, &g_tm, 0, &r);
    CHECK(!r.apic && r.reason == IRQMODE_REASON_NO_IOAPIC && F.rdmsr_calls == 0 && same(&before));

    // ── forced PIC (the -DIRQ64_FORCE_PIC decision): PIC, nothing touched ──
    fake_reset(); tmadt(); cap(&before);
    irqmode64_commit(&fake_hw, &g_tm, 1, &r);
    CHECK(!r.apic && r.reason == IRQMODE_REASON_FORCED_PIC && F.rdmsr_calls == 0 && F.wrmsr_calls == 0 &&
          F.map_calls == 0 && F.io_calls == 0 && same(&before));
#ifdef IRQ64_FORCE_PIC
    CHECK(irq64_mode() == IRQ64_MODE_PIC);          // this build really is the forced-PIC build
#endif
    group_end("transactional PIC->APIC: commit, MSR-authoritative base, NMI, rollback of every register/IMR/MSR, firmware-disabled xAPIC, x2APIC, no-APIC, forced PIC");
}

// ═════════════════ 8. 8042 OBF + AUXDATA routing ════════════════════
#define Q_MAX 64
static struct { uint8_t byte; uint8_t aux; } g_q[Q_MAX];
static int g_q_head, g_q_len, g_q_endless, g_data_reads, g_violation, g_status_only_aux;
static uint8_t g_kbd_got[Q_MAX], g_aux_got[Q_MAX];
static int g_kbd_n, g_aux_n;

static void q_reset(void) { g_q_head = g_q_len = g_q_endless = g_data_reads = g_violation = g_status_only_aux = g_kbd_n = g_aux_n = 0; }
static void q_push(uint8_t b, int aux) { g_q[g_q_len].byte = b; g_q[g_q_len].aux = (uint8_t)aux; g_q_len++; }
static uint8_t q_status(void) {
    if (g_status_only_aux) return PS2_STATUS_AUXDATA;              // AUXDATA garbage with OBF clear
    if (g_q_endless) return PS2_STATUS_OBF;
    if (g_q_head >= g_q_len) return 0;
    return (uint8_t)(PS2_STATUS_OBF | (g_q[g_q_head].aux ? PS2_STATUS_AUXDATA : 0));
}
static uint8_t q_data(void) {
    g_data_reads++;
    if (g_q_endless) return 0xAA;
    if (g_q_head >= g_q_len) { g_violation++; return 0x00; }        // read with OBF clear
    return g_q[g_q_head++].byte;
}
static void q_kbd(uint8_t b) { if (g_kbd_n < Q_MAX) g_kbd_got[g_kbd_n++] = b; }
static void q_aux(uint8_t b) { if (g_aux_n < Q_MAX) g_aux_got[g_aux_n++] = b; }
static const ps2_64_rx_t fake_rx = { q_status, q_data, q_kbd, q_aux };

static void test_ps2_routing(void) {
    group_begin();

    // Phantom IRQ (nothing in the output buffer): reads nothing, feeds nothing.
    q_reset();
    CHECK(ps2_64_rx_poll(&fake_rx) == 0);
    CHECK(g_data_reads == 0 && g_kbd_n == 0 && g_aux_n == 0 && g_violation == 0);
    // AUXDATA set but OBF clear is still "nothing there".
    q_reset(); g_status_only_aux = 1;
    CHECK(ps2_64_rx_poll(&fake_rx) == 0 && g_data_reads == 0 && g_aux_n == 0 && g_kbd_n == 0);

    // Keyboard byte -> keyboard parser only.
    q_reset(); q_push(0x1E, 0);
    CHECK(ps2_64_rx_poll(&fake_rx) == 1 && g_kbd_n == 1 && g_kbd_got[0] == 0x1E && g_aux_n == 0 && g_data_reads == 1);

    // AUX byte -> mouse parser only, even though (on real HW) IRQ1 may be the line that fired.
    q_reset(); q_push(0x08, 1);
    CHECK(ps2_64_rx_poll(&fake_rx) == 1 && g_aux_n == 1 && g_aux_got[0] == 0x08 && g_kbd_n == 0 && g_data_reads == 1);

    // Mixed stream: one byte per service, order preserved per sink, every
    // byte exactly once, no fabrication.
    q_reset();
    q_push(0x1E, 0); q_push(0x09, 1); q_push(0x02, 1); q_push(0x9E, 0); q_push(0x03, 1); q_push(0x30, 0);
    int total = 0, each_one = 1;
    for (int i = 0; i < 6; i++) { int n = ps2_64_rx_poll(&fake_rx); total += n; if (n != 1) each_one = 0; }
    CHECK(total == 6 && each_one);
    CHECK(g_kbd_n == 3 && g_kbd_got[0] == 0x1E && g_kbd_got[1] == 0x9E && g_kbd_got[2] == 0x30);
    CHECK(g_aux_n == 3 && g_aux_got[0] == 0x09 && g_aux_got[1] == 0x02 && g_aux_got[2] == 0x03);
    CHECK(g_data_reads == 6 && g_violation == 0);
    // Either IRQ line firing afterwards finds nothing: no double consumption.
    CHECK(ps2_64_rx_poll(&fake_rx) == 0 && ps2_64_rx_poll(&fake_rx) == 0);
    CHECK(g_data_reads == 6 && g_kbd_n == 3 && g_aux_n == 3);

    // A long interleaved stream split across many services: no loss/dup.
    q_reset();
    for (int i = 0; i < 40; i++) q_push((uint8_t)i, i & 1);
    int served = 0;
    for (int i = 0; i < 50; i++) served += ps2_64_rx_poll(&fake_rx);   // 10 of these are phantom IRQs
    CHECK(served == 40 && g_kbd_n + g_aux_n == 40 && g_data_reads == 40 && g_violation == 0);
    int ok = 1;
    for (int i = 0; i < g_kbd_n; i++) if (g_kbd_got[i] != (uint8_t)(2 * i)) ok = 0;
    for (int i = 0; i < g_aux_n; i++) if (g_aux_got[i] != (uint8_t)(2 * i + 1)) ok = 0;
    CHECK(ok);

    // A wedged controller (OBF stuck high) costs one byte per IRQ, never a livelock.
    q_reset(); g_q_endless = 1;
    CHECK(ps2_64_rx_poll(&fake_rx) == 1 && g_data_reads == 1);
    group_end("8042 shared receive: OBF gate, AUXDATA routing, exactly-once, one byte per service");
}

// ═════════════════ 9. live state sanity ═════════════════════════════
static void test_live(void) {
    group_begin();
    CHECK(irq64_mode() == IRQ64_MODE_PIC || irq64_mode() == IRQ64_MODE_APIC);
    const irq64_desc_t* t = irq64_get_desc(0);
    const irq64_desc_t* k = irq64_get_desc(1);
    CHECK(t && t->requested && t->vector == 0x20);
    CHECK(k && k->requested && k->vector == 0x21);
    if (irq64_mode() == IRQ64_MODE_APIC) {
        CHECK(t->trigger == IRQ64_TRIG_EDGE && k->trigger == IRQ64_TRIG_EDGE);
        uint64_t raw;
        CHECK(ioapic64_read_rte(t->hwirq, &raw) == 0);
        ioapic64_rte_t d; ioapic64_rte_decode(raw, &d);
        CHECK(d.vector == 0x20 && !d.masked && d.delivery == 0 && d.dest_mode == 0);
        CHECK(ioapic64_read_rte(k->hwirq, &raw) == 0);
        ioapic64_rte_decode(raw, &d);
        CHECK(d.vector == 0x21 && !d.masked);
        klog_hex("irq64: live: IRQ0 -> GSI ", t->hwirq);
        klog_hex("irq64: live: IRQ1 -> GSI ", k->hwirq);
    }
    group_end("live descriptor/route state");
}

void irq64_selftest_run(void) {
    g_pass = g_fail = 0;
    klog("irq64 selftest: begin\n");
    test_vectors();
    test_madt();
    test_acpi();
    test_ioapic_pure();
    test_pic_spurious();
    test_dispatch();
    test_lapic_spurious();
    test_transaction();
    test_ps2_routing();
    test_live();
    klog_hex("irq64 selftest: checks passed: ", (uint32_t)g_pass);
    klog_hex("irq64 selftest: checks failed: ", (uint32_t)g_fail);
    klog(g_fail ? "irq64 selftest: RESULT FAIL\n" : "irq64 selftest: RESULT PASS\n");
}
