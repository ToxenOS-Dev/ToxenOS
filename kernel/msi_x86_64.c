// kernel/msi_x86_64.c -- M+11B x86 MSI message composer. See the header.
#include "../include/msi_x86_64.h"
#include "../include/vector64.h"

int msi_x86_compose(uint32_t dest_apic_id, int vector, msi_x86_msg_t* out) {
    out->address_lo = out->address_hi = out->data = 0;
    if (dest_apic_id > 0xFE) return MSI_X86_ERR_DEST;
    if (!vector64_is_dynamic_device_vector(vector)) return MSI_X86_ERR_VECTOR;
    out->address_lo = 0xFEE00000u | (dest_apic_id << 12);   // RH=0, DM=0
    out->address_hi = 0;
    out->data = (uint32_t)vector;                            // Fixed, edge, level bit 0
    return 0;
}
