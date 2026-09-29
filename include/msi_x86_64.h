#ifndef MSI_X86_64_H
#define MSI_X86_64_H

// M+11B: the ONE place that knows the x86 MSI message format. PCI drivers
// and the PCI IRQ layer never compute addresses/data themselves.
//
// Address (low dword): bits 31:20 = 0xFEE, bits 19:12 = destination APIC
// ID, bit 3 RH = 0, bit 2 DM = 0 (physical), other bits 0; address_hi = 0.
// Data: bits 7:0 vector, bits 10:8 delivery mode 000 (Fixed), bit 15 trigger
// = 0 (edge), bit 14 level = 0. (Layout cross-checked against current
// Linux arch/x86/include/asm/msi.h and the Intel SDM interrupt-message
// description.)
//
// xAPIC only: an 8-bit destination ID. 0xFF is the physical-mode BROADCAST
// ID and is rejected, as is anything wider (no x2APIC / remapping here).
#include <stdint.h>

typedef struct {
    uint32_t address_lo;
    uint32_t address_hi;
    uint32_t data;
} msi_x86_msg_t;

#define MSI_X86_ERR_DEST    (-1)
#define MSI_X86_ERR_VECTOR  (-2)

// The vector must satisfy vector64_is_dynamic_device_vector() -- so 0x80
// (syscall), exceptions, the fixed legacy range, system/IPI vectors and
// 0xFF are all rejected. On failure *out is zeroed.
int msi_x86_compose(uint32_t dest_apic_id, int vector, msi_x86_msg_t* out);

#endif // MSI_X86_64_H
