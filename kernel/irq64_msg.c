// kernel/irq64_msg.c -- M+11B: message-signalled interrupt descriptors for
// the irq64 core (logical IRQ + one dynamic CPU vector each), and the MSI /
// MSI-X chips. MSI arrives DIRECTLY at the LAPIC: EOI is a LAPIC EOI, the
// flow is edge, and the IOAPIC is never involved. See include/irq64.h.
#include "../include/irq64.h"
#include "../include/lapic64.h"

static void (*g_msg_eoi)(void) = 0;
void irq64_msg_set_eoi_hook(void (*fn)(void)) { g_msg_eoi = fn; }

static void msg_eoi(irq64_desc_t* d) {
    (void)d;
    if (g_msg_eoi) g_msg_eoi(); else lapic64_eoi();
}

static int msg_ctl(irq64_desc_t* d, int enable) {
    irq64_msg_owner_t* o = (irq64_msg_owner_t*)d->chip_data;
    if (!o || !o->ctl) return IRQ64_E_NOTSUP;
    return o->ctl(o->owner, d->dev_index, enable);
}
static void msg_mask(irq64_desc_t* d)   { (void)msg_ctl(d, 0); }
static void msg_unmask(irq64_desc_t* d) { (void)msg_ctl(d, 1); }

static const irq64_chip_t msi_chip  = { "MSI",   msg_mask, msg_unmask, msg_eoi, 0, IRQ64_SRC_MSI,  msg_ctl };
static const irq64_chip_t msix_chip = { "MSI-X", msg_mask, msg_unmask, msg_eoi, 0, IRQ64_SRC_MSIX, msg_ctl };

int irq64_table_msg_alloc(irq64_table_t* t, irq64_src_t src, uint16_t bdf, uint16_t dev_index,
                          int can_mask, irq64_msg_owner_t* owner, int* out_irq) {
    if (!t->apic_mode) return IRQ64_E_NODEV;              // forced PIC / no LAPIC: no MSI
    if (src != IRQ64_SRC_MSI && src != IRQ64_SRC_MSIX) return IRQ64_E_INVAL;
    int irq = -1;
    for (int i = 16; i < IRQ64_MAX; i++) if (!t->desc[i].allocated) { irq = i; break; }
    if (irq < 0) return IRQ64_E_NOSPC;
    int vec = irq_vector_alloc(&t->vmap, irq);
    if (vec < 0) return IRQ64_E_NOSPC;

    irq64_desc_t* d = &t->desc[irq];
    d->chip = (src == IRQ64_SRC_MSIX) ? &msix_chip : &msi_chip;
    d->src = (uint8_t)src;
    d->handler = 0; d->ctx = 0; d->name = 0;
    d->hwirq = dev_index;
    d->vector = (int16_t)vec;
    d->trigger = IRQ64_TRIG_EDGE; d->polarity = IRQ64_POL_HIGH;
    d->requested = 0;
    d->can_mask = (uint8_t)(can_mask ? 1 : 0);
    // A maskable message starts hardware-masked. A non-maskable MSI has no
    // mask to be "in"; its silence before go-live comes from MSI Enable=0
    // (a set-level property), so the descriptor reads as enabled.
    d->masked = d->can_mask ? 1 : 0;
    d->storm_masked = 0;
    d->sealed = 0; d->allocated = 1;
    d->bdf = bdf; d->dev_index = dev_index; d->dest_apic = (uint8_t)t->dest_apic;
    d->chip_data = owner;
    d->count = d->handled = d->unhandled = d->spurious = 0;
    d->consecutive_unhandled = d->storm_masks = 0; d->eoi_count = 0;
    t->vec_to_irq[vec] = (int16_t)irq;
    *out_irq = irq;
    return 0;
}

static irq64_desc_t* msg_desc(irq64_table_t* t, int irq) {
    if (irq < 16 || irq >= IRQ64_MAX) return 0;
    irq64_desc_t* d = &t->desc[irq];
    return d->allocated ? d : 0;
}

int irq64_table_msg_bind(irq64_table_t* t, int irq, irq64_handler_t h, void* ctx, const char* name) {
    irq64_desc_t* d = msg_desc(t, irq);
    if (!d || !h) return IRQ64_E_INVAL;
    if (d->sealed || d->requested) return IRQ64_E_BUSY;   // bound exactly once; pinned after go-live
    d->handler = h; d->ctx = ctx; d->name = name;
    d->requested = 1;
    return 0;
}

int irq64_table_msg_seal(irq64_table_t* t, int irq) {
    irq64_desc_t* d = msg_desc(t, irq);
    if (!d) return IRQ64_E_INVAL;
    d->sealed = 1;
    return 0;
}

int irq64_table_msg_free(irq64_table_t* t, int irq) {
    irq64_desc_t* d = msg_desc(t, irq);
    if (!d) return IRQ64_E_INVAL;
    if (d->sealed) return IRQ64_E_BUSY;                   // live/quiesced/faulted: owned until reboot
    int vec = d->vector;
    t->vec_to_irq[vec] = -1;
    irq_vector_free(&t->vmap, vec);
    d->chip = 0; d->handler = 0; d->ctx = 0; d->name = 0;
    d->vector = -1; d->requested = 0; d->allocated = 0; d->masked = 1;
    d->src = IRQ64_SRC_NONE; d->chip_data = 0; d->can_mask = 1;
    return 0;
}

int irq64_table_msg_vector(const irq64_table_t* t, int irq) {
    if (irq < 16 || irq >= IRQ64_MAX || !t->desc[irq].allocated) return -1;
    return t->desc[irq].vector;
}

int irq64_table_ctl(irq64_table_t* t, int irq, int enable) {
    if (irq < 0 || irq >= IRQ64_MAX) return IRQ64_E_INVAL;
    irq64_desc_t* d = &t->desc[irq];
    if (!d->chip || !d->requested) return IRQ64_E_INVAL;
    if (!d->can_mask) return IRQ64_E_NOTSUP;              // never claim a disable the hardware can't honour
    if (d->chip->ctl) {
        int rc = d->chip->ctl(d, enable);
        if (rc == 0) d->masked = enable ? 0 : 1;
        return rc;
    }
    if (enable) d->chip->unmask(d); else d->chip->mask(d);
    d->masked = enable ? 0 : 1;
    return 0;
}
