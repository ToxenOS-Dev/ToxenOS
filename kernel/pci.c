// ToxenOS/kernel/pci.c
// PCI bus enumeration using config space mechanism #1 (I/O ports 0xCF8/0xCFC)
#include <stdint.h>
#include "../include/pci.h"
#include "../include/vga.h"
#include "../include/syscall.h"

pci_device_t pci_devices[PCI_MAX_DEVICES];
int          pci_device_count = 0;

static inline void outl(uint16_t port, uint32_t val)
    { __asm__ volatile("outl %0,%1"::"a"(val),"Nd"(port)); }
static inline uint32_t inl(uint16_t port)
    { uint32_t r; __asm__ volatile("inl %1,%0":"=a"(r):"Nd"(port)); return r; }

// Build the 32-bit address for a PCI config read/write
static uint32_t pci_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    return 0x80000000u
        | ((uint32_t)bus   << 16)
        | ((uint32_t)slot  << 11)
        | ((uint32_t)func  <<  8)
        | ((uint32_t)(offset & 0xFC));
}

uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    outl(PCI_CONFIG_ADDRESS, pci_addr(bus, slot, func, offset));
    uint32_t val = inl(PCI_CONFIG_DATA);
    // shift down for byte/word reads at non-dword-aligned offsets
    val >>= (offset & 3) * 8;
    return val;
}

void pci_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val)
{
    outl(PCI_CONFIG_ADDRESS, pci_addr(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, val);
}

// Read full 32-bit dword (no shift)
static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    outl(PCI_CONFIG_ADDRESS, pci_addr(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

static uint32_t pci_bar_size(uint8_t bus, uint8_t slot, uint8_t func, uint8_t bar_reg)
{
    uint32_t orig = pci_read32(bus, slot, func, bar_reg);
    pci_write(bus, slot, func, bar_reg, 0xFFFFFFFF);
    uint32_t mask = pci_read32(bus, slot, func, bar_reg);
    pci_write(bus, slot, func, bar_reg, orig);
    if (!mask || mask == 0xFFFFFFFF) return 0;
    // I/O BAR: mask low 2 bits; MMIO BAR: mask low 4 bits
    if (orig & 1) mask &= ~0x3u;
    else          mask &= ~0xFu;
    return (~mask) + 1;
}

static void pci_scan_device(uint8_t bus, uint8_t slot, uint8_t func)
{
    uint16_t vendor = (uint16_t)pci_read(bus, slot, func, PCI_VENDOR_ID);
    if (vendor == 0xFFFF) return;  // no device

    if (pci_device_count >= PCI_MAX_DEVICES) return;

    pci_device_t* d = &pci_devices[pci_device_count++];
    d->bus       = bus;
    d->slot      = slot;
    d->func      = func;
    d->vendor_id = vendor;
    d->device_id = (uint16_t)pci_read(bus, slot, func, PCI_DEVICE_ID);
    d->class_code= (uint8_t)pci_read(bus, slot, func, PCI_CLASS);
    d->subclass  = (uint8_t)pci_read(bus, slot, func, PCI_SUBCLASS);
    d->irq_line  = (uint8_t)pci_read(bus, slot, func, PCI_INTERRUPT_LINE);

    for (int i = 0; i < 6; i++) {
        uint8_t bar_reg = (uint8_t)(PCI_BAR0 + i * 4);
        d->bar[i]      = pci_read32(bus, slot, func, bar_reg);
        d->bar_size[i] = pci_bar_size(bus, slot, func, bar_reg);
    }
}

void pci_init()
{
    pci_device_count = 0;

    // Brute-force scan all buses, slots, functions
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint16_t vendor = (uint16_t)pci_read((uint8_t)bus, slot, 0, PCI_VENDOR_ID);
            if (vendor == 0xFFFF) continue;

            pci_scan_device((uint8_t)bus, slot, 0);

            // Check for multi-function device
            uint8_t htype = (uint8_t)pci_read((uint8_t)bus, slot, 0, PCI_HEADER_TYPE);
            if (htype & 0x80) {
                for (uint8_t func = 1; func < 8; func++)
                    pci_scan_device((uint8_t)bus, slot, func);
            }
        }
    }

    klog("PCI: found ");
    // log count as single digit (we won't have more than 9 in QEMU)
    char c[2] = {'0' + (char)pci_device_count, 0};
    klog(c);
    klog(" devices\n");
}

pci_device_t* pci_find(uint16_t vendor, uint16_t device)
{
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].vendor_id == vendor &&
            pci_devices[i].device_id == device)
            return &pci_devices[i];
    }
    return 0;
}

void pci_enable(pci_device_t* dev)
{
    uint32_t cmd = pci_read(dev->bus, dev->slot, dev->func, PCI_COMMAND);
    cmd |= PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER;
    pci_write(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);
}

uint32_t pci_bar_io(pci_device_t* dev, int bar_idx)
{
    // I/O BARs have bit 0 set; mask off the flag bits
    return dev->bar[bar_idx] & ~0x3u;
}
