// kernel/acpi.c — Minimal ACPI: S5 shutdown + power-button SCI
// Parses RSDP → RSDT/XSDT → FADT.  Scans DSDT for _S5_ SLP_TYP values.
// No AML interpreter; just enough for power-off and power-button detection.
#include <stdint.h>
#include "../include/acpi.h"
#include "../include/klog.h"
#include "../include/irq.h"
#include "../include/pic.h"
#include "../include/memmap.h"

// ── I/O helpers ──────────────────────────────────────────────────────────────
static inline uint16_t inw(uint16_t p) {
    uint16_t v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v;
}
static inline void outw(uint16_t p, uint16_t v) {
    __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p));
}
static inline void outb(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));
}

// ── ACPI table signatures ─────────────────────────────────────────────────────
#define SIG4(a,b,c,d) (((uint32_t)(a))|((uint32_t)(b)<<8)|((uint32_t)(c)<<16)|((uint32_t)(d)<<24))
#define SIG_FACP  SIG4('F','A','C','P')

// ── FADT fields we care about (offsets into the raw table) ───────────────────
// Byte offsets from start of FADT (ACPI 1.0 / 2.0+ compatible subset)
#define FADT_SMI_CMD        48   // u32: SMI command port
#define FADT_ACPI_ENABLE    52   // u8:  value to write to SMI_CMD to enable ACPI
#define FADT_PM1a_EVT_BLK   56   // u32: PM1a event block I/O port
#define FADT_PM1b_EVT_BLK   60   // u32: PM1b event block I/O port (may be 0)
#define FADT_PM1a_CNT_BLK   64   // u32: PM1a control block I/O port
#define FADT_PM1b_CNT_BLK   68   // u32: PM1b control block I/O port (may be 0)
#define FADT_PM1_EVT_LEN    88   // u8:  length of PM1 event block
#define FADT_SCI_INT        46   // u16: SCI interrupt (GSI / legacy IRQ)
#define FADT_DSDT           40   // u32: physical address of DSDT

// PM1 status/enable register layout
#define PM1_STS_PWRBTN  (1u << 8)   // power button status bit
#define PM1_EN_PWRBTN   (1u << 8)   // power button enable bit
#define PM1_CNT_SLP_EN  (1u << 13)  // sleep enable
#define PM1_CNT_SLP_TYP_SHIFT 10

// ── State ─────────────────────────────────────────────────────────────────────
static uint16_t g_pm1a_cnt  = 0;   // PM1a control block port
static uint16_t g_pm1b_cnt  = 0;   // PM1b control block port (0 if absent)
static uint16_t g_pm1a_evt  = 0;   // PM1a event block port (status + enable)
static uint8_t  g_slp_typ   = 5;   // SLP_TYP for S5 (default: 5 = works on most hw)
static int      g_acpi_ok   = 0;

// ── Table helpers ─────────────────────────────────────────────────────────────
static uint8_t table_sum(const uint8_t* p, uint32_t len) {
    uint8_t s = 0;
    for (uint32_t i = 0; i < len; i++) s += p[i];
    return s;
}

static uint32_t u32at(const uint8_t* p, uint32_t off) {
    return (uint32_t)p[off] | ((uint32_t)p[off+1]<<8) |
           ((uint32_t)p[off+2]<<16) | ((uint32_t)p[off+3]<<24);
}
static uint16_t u16at(const uint8_t* p, uint32_t off) {
    return (uint16_t)p[off] | ((uint16_t)p[off+1]<<8);
}

// ── Scan DSDT for _S5_ SLP_TYP values ────────────────────────────────────────
// AML pattern: 08 5F 53 35 5F 12 ... 0A <typa> 0A <typb>
// or:          5F 53 35 5F 12 ... 0A <typa> (NameSeg without leading 08)
static void parse_s5(const uint8_t* dsdt, uint32_t len) {
    // Search for ASCII "_S5_"
    for (uint32_t i = 0; i + 7 < len; i++) {
        if (dsdt[i] == '_' && dsdt[i+1] == 'S' && dsdt[i+2] == '5' && dsdt[i+3] == '_') {
            // Skip past "_S5_" + Package opcode byte(s)
            uint32_t j = i + 4;
            if (j < len && dsdt[j] == 0x12) j++;       // Package opcode
            if (j >= len) continue;
            // Skip package length (1–4 bytes with high 2 bits encoding size)
            uint8_t pkglen = dsdt[j++];
            if (pkglen > 0x3F) j += (pkglen >> 6);     // skip extra length bytes
            if (j + 3 >= len) continue;
            if (j < len && dsdt[j] == 0x0A) j++;       // BytePrefix for element 0
            g_slp_typ = dsdt[j++] & 0x07;              // SLP_TYPa (3 bits)
            klog_hex("ACPI: _S5_ SLP_TYP=", g_slp_typ);
            return;
        }
    }
    klog("ACPI: _S5_ not found, using default SLP_TYP=5\n");
}

// ── Parse FADT ────────────────────────────────────────────────────────────────
static void parse_fadt(uint32_t phys) {
    const uint8_t* f = (const uint8_t*)KPHYS_TO_VIRT(phys);
    uint32_t len = u32at(f, 4);
    if (len < 116 || table_sum(f, len) != 0) {
        klog("ACPI: FADT checksum fail\n");
        return;
    }

    g_pm1a_evt = (uint16_t)u32at(f, FADT_PM1a_EVT_BLK);
    g_pm1a_cnt = (uint16_t)u32at(f, FADT_PM1a_CNT_BLK);
    g_pm1b_cnt = (uint16_t)u32at(f, FADT_PM1b_CNT_BLK);

    uint16_t sci_int = u16at(f, FADT_SCI_INT);
    uint32_t smi_cmd = u32at(f, FADT_SMI_CMD);
    uint8_t  acpi_en = f[FADT_ACPI_ENABLE];

    klog_hex("ACPI: PM1a_CNT=", g_pm1a_cnt);
    klog_hex("ACPI: SCI_INT=", sci_int);

    // Enable ACPI mode if needed (SMI_CMD ≠ 0 and ACPI_ENABLE ≠ 0)
    if (smi_cmd && acpi_en) {
        outb((uint16_t)smi_cmd, acpi_en);
        // Wait for ACPI to come up (PM1_CNT SCI_EN bit)
        for (int i = 0; i < 300; i++) {
            if (inw(g_pm1a_cnt) & 0x0001) break;
            for (int j = 0; j < 10000; j++) __asm__ volatile("pause");
        }
    }

    // Enable power-button SCI in PM1a enable register (EVT_BLK + half-length)
    if (g_pm1a_evt) {
        uint8_t evtlen = (len >= 89) ? f[FADT_PM1_EVT_LEN] : 4;
        uint16_t en_port = g_pm1a_evt + (evtlen / 2);
        uint16_t cur = inw(en_port);
        outw(en_port, cur | PM1_EN_PWRBTN);
    }

    // Register SCI interrupt handler (IRQ is almost always 9)
    if (sci_int < 16) {
        extern void acpi_sci_handler(void);
        irq_register((uint8_t)sci_int, acpi_sci_handler);
        pic_unmask((uint8_t)sci_int);
    }

    // Parse DSDT for _S5_ SLP_TYP
    uint32_t dsdt_phys = u32at(f, FADT_DSDT);
    if (dsdt_phys) {
        const uint8_t* dsdt = (const uint8_t*)KPHYS_TO_VIRT(dsdt_phys);
        uint32_t dsdt_len = u32at(dsdt, 4);
        if (dsdt_len < 0x100000)    // sanity: <1MB
            parse_s5(dsdt + 36, dsdt_len - 36);
    }

    g_acpi_ok = 1;
}

// ── Walk RSDT/XSDT ───────────────────────────────────────────────────────────
static void parse_rsdt(uint32_t phys) {
    const uint8_t* r = (const uint8_t*)KPHYS_TO_VIRT(phys);
    uint32_t len = u32at(r, 4);
    if (len < 36 || table_sum(r, len) != 0) return;

    uint32_t entries = (len - 36) / 4;
    for (uint32_t i = 0; i < entries; i++) {
        uint32_t tphys = u32at(r, 36 + i*4);
        const uint8_t* t = (const uint8_t*)KPHYS_TO_VIRT(tphys);
        uint32_t sig = u32at(t, 0);
        if (sig == SIG_FACP) {
            parse_fadt(tphys);
            return;
        }
    }
    klog("ACPI: FADT not found in RSDT\n");
}

// Same for XSDT (64-bit pointers — upper 32 ignored on 32-bit OS)
static void parse_xsdt(uint32_t phys) {
    const uint8_t* r = (const uint8_t*)KPHYS_TO_VIRT(phys);
    uint32_t len = u32at(r, 4);
    if (len < 36 || table_sum(r, len) != 0) return;

    uint32_t entries = (len - 36) / 8;
    for (uint32_t i = 0; i < entries; i++) {
        uint32_t tphys = u32at(r, 36 + i*8);   // lower 32 bits only
        const uint8_t* t = (const uint8_t*)KPHYS_TO_VIRT(tphys);
        uint32_t sig = u32at(t, 0);
        if (sig == SIG_FACP) {
            parse_fadt(tphys);
            return;
        }
    }
    klog("ACPI: FADT not found in XSDT\n");
}

// ── Find RSDP ─────────────────────────────────────────────────────────────────
// "RSD PTR " (8 bytes) appears on a 16-byte boundary in:
//   EBDA (1st KB at *0x040E << 4) or BIOS ROM 0xE0000..0xFFFFF
static const uint8_t* find_rsdp_in(uint32_t base, uint32_t end) {
    for (uint32_t a = base; a < end; a += 16) {
        const uint8_t* p = (const uint8_t*)KPHYS_TO_VIRT(a);
        if (p[0]=='R' && p[1]=='S' && p[2]=='D' && p[3]==' ' &&
            p[4]=='P' && p[5]=='T' && p[6]=='R' && p[7]==' ') {
            // Checksum v1 (20 bytes)
            if (table_sum(p, 20) != 0) continue;
            return p;
        }
    }
    return 0;
}

// ── Public: init ─────────────────────────────────────────────────────────────
void acpi_init(void) {
    const uint8_t* rsdp = 0;

    // 1. Check EBDA (segment at 0x040E)
    uint16_t ebda_seg = *(volatile uint16_t*)KPHYS_TO_VIRT(0x040E);
    if (ebda_seg) rsdp = find_rsdp_in((uint32_t)ebda_seg << 4,
                                       ((uint32_t)ebda_seg << 4) + 1024);
    // 2. Check BIOS ROM area
    if (!rsdp) rsdp = find_rsdp_in(0xE0000, 0x100000);

    if (!rsdp) { klog("ACPI: RSDP not found\n"); return; }

    uint8_t rev = rsdp[15];
    if (rev >= 2) {
        // ACPI 2.0+: 8-byte XSDT pointer at offset 24 (lower 32 bits sufficient)
        uint32_t xsdp = u32at(rsdp, 24);
        if (xsdp) { parse_xsdt(xsdp); return; }
    }
    // ACPI 1.0: 4-byte RSDT pointer at offset 16
    parse_rsdt(u32at(rsdp, 16));
}

// ── SCI interrupt handler (power button) ─────────────────────────────────────
void acpi_sci_handler(void) {
    if (!g_pm1a_evt) return;

    // Read PM1a status (first half of EVT_BLK)
    uint16_t sts = inw(g_pm1a_evt);

    if (sts & PM1_STS_PWRBTN) {
        // Acknowledge the power-button event (write 1 to clear)
        outw(g_pm1a_evt, PM1_STS_PWRBTN);
        acpi_shutdown();
    }
    // Acknowledge any other SCI status bits
    outw(g_pm1a_evt, sts);
}

// ── Public: shutdown ─────────────────────────────────────────────────────────
void acpi_shutdown(void) {
    __asm__ volatile("cli");

    if (g_acpi_ok && g_pm1a_cnt) {
        uint16_t val = (uint16_t)(((uint16_t)g_slp_typ << PM1_CNT_SLP_TYP_SHIFT)
                                  | PM1_CNT_SLP_EN);
        outw(g_pm1a_cnt, val);
        if (g_pm1b_cnt) outw(g_pm1b_cnt, val);
        // Give hardware a moment to power off
        for (int i = 0; i < 1000000; i++) __asm__ volatile("pause");
    }

    // Fallback: keyboard controller reset (reboots instead of powering off,
    // but at least the machine doesn't freeze)
    outb(0x64, 0xFE);

    // Final fallback: triple-fault
    volatile struct { uint16_t limit; uint32_t base; } idt = {0, 0};
    __asm__ volatile("lidt (%0); int $3" :: "r"(&idt));
    while (1) __asm__ volatile("hlt");
}
