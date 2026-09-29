// kernel/madt64.c -- M+11A MADT parser. See include/madt64.h.
#include "../include/madt64.h"
#include "../include/acpi64.h"

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

int madt64_decode_flags(uint16_t flags, int* polarity, int* trigger) {
    unsigned pol = flags & 3u, trg = (flags >> 2) & 3u;
    if (pol == 2 || trg == 2) return -1;
    if (polarity) *polarity = (pol == 3) ? 1 : 0;   // 00/01 -> high
    if (trigger)  *trigger  = (trg == 3) ? 1 : 0;   // 00/01 -> edge
    return 0;
}

int madt64_parse(const void* table, uint64_t avail, madt64_info_t* out) {
    if (!table || !out) return MADT_ERR_NULL;
    if (avail < 44) return MADT_ERR_TOO_SHORT;
    const uint8_t* t = (const uint8_t*)table;
    if (t[0] != 'A' || t[1] != 'P' || t[2] != 'I' || t[3] != 'C') return MADT_ERR_SIGNATURE;
    uint32_t len = rd32(t + 4);
    if (len < 44 || len > avail || len > ACPI64_SDT_MAX_LEN) return MADT_ERR_LENGTH;
    if (!acpi64_checksum_ok(t, len)) return MADT_ERR_CHECKSUM;

    madt64_info_t* m = out;
    m->lapic_phys = rd32(t + 36);
    m->flags = rd32(t + 40);
    m->lapic_addr_overridden = 0;
    m->n_lapic = m->n_ioapic = m->n_iso = m->n_nmi = 0;
    m->truncated = m->unknown_entries = 0;

    uint32_t off = 44;
    while (off < len) {
        if (len - off < 2) return MADT_ERR_ENTRY_LEN;
        uint8_t type = t[off], elen = t[off + 1];
        if (elen < 2 || elen > len - off) return MADT_ERR_ENTRY_LEN;
        const uint8_t* e = t + off;
        switch (type) {
        case 0: // Processor Local APIC
            if (elen < 8) return MADT_ERR_ENTRY_SHORT;
            if (m->n_lapic < MADT64_MAX_LAPIC) {
                madt_lapic_t* l = &m->lapic[m->n_lapic++];
                l->acpi_id = e[2]; l->apic_id = e[3]; l->flags = rd32(e + 4);
            } else m->truncated++;
            break;
        case 1: // I/O APIC
            if (elen < 12) return MADT_ERR_ENTRY_SHORT;
            if (m->n_ioapic < MADT64_MAX_IOAPIC) {
                madt_ioapic_t* io = &m->ioapic[m->n_ioapic++];
                io->id = e[2]; io->addr = rd32(e + 4); io->gsi_base = rd32(e + 8);
            } else m->truncated++;
            break;
        case 2: { // Interrupt Source Override
            if (elen < 10) return MADT_ERR_ENTRY_SHORT;
            uint16_t fl = rd16(e + 8);
            if (madt64_decode_flags(fl, 0, 0) != 0) return MADT_ERR_ISO_FLAGS;
            if (m->n_iso < MADT64_MAX_ISO) {
                madt_iso_t* o = &m->iso[m->n_iso++];
                o->bus = e[2]; o->source = e[3]; o->gsi = rd32(e + 4); o->flags = fl;
            } else m->truncated++;
            break;
        }
        case 4: // Local APIC NMI
            if (elen < 6) return MADT_ERR_ENTRY_SHORT;
            if (m->n_nmi < MADT64_MAX_NMI) {
                madt_nmi_t* n = &m->nmi[m->n_nmi++];
                n->acpi_id = e[2]; n->flags = rd16(e + 3); n->lint = e[5];
            } else m->truncated++;
            break;
        case 5: // Local APIC Address Override
            if (elen < 12) return MADT_ERR_ENTRY_SHORT;
            m->lapic_phys = rd64(e + 4);
            m->lapic_addr_overridden = 1;
            break;
        default:
            m->unknown_entries++;
            break;
        }
        off += elen;
    }
    return 0;
}

int madt64_resolve_isa(const madt64_info_t* m, int isa_irq, uint32_t* gsi, int* polarity, int* trigger) {
    if (isa_irq < 0 || isa_irq > 15) return -1;
    uint32_t g = (uint32_t)isa_irq;
    int pol = 0, trg = 0;
    for (int i = 0; i < m->n_iso; i++) {
        const madt_iso_t* o = &m->iso[i];
        if (o->bus == 0 && o->source == isa_irq) {
            g = o->gsi;
            madt64_decode_flags(o->flags, &pol, &trg);
            break;
        }
    }
    if (gsi) *gsi = g;
    if (polarity) *polarity = pol;
    if (trigger) *trigger = trg;
    return 0;
}

const madt_lapic_t* madt64_find_lapic(const madt64_info_t* m, uint32_t apic_id) {
    for (int i = 0; i < m->n_lapic; i++)
        if (m->lapic[i].apic_id == apic_id && (m->lapic[i].flags & (MADT_LAPIC_ENABLED | MADT_LAPIC_ONLINE_CAPABLE)))
            return &m->lapic[i];
    return 0;
}
