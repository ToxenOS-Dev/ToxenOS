#ifndef PCI_H
#define PCI_H

#include <stdint.h>

// PCI config space access ports
#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

// PCI config space offsets
#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_REVISION_ID     0x08
#define PCI_PROG_IF         0x09
#define PCI_SUBCLASS        0x0A
#define PCI_CLASS           0x0B
#define PCI_CACHE_LINE      0x0C
#define PCI_LATENCY         0x0D
#define PCI_HEADER_TYPE     0x0E
#define PCI_BIST            0x0F
#define PCI_BAR0            0x10
#define PCI_BAR1            0x14
#define PCI_BAR2            0x18
#define PCI_BAR3            0x1C
#define PCI_BAR4            0x20
#define PCI_BAR5            0x24
#define PCI_INTERRUPT_LINE  0x3C
#define PCI_INTERRUPT_PIN   0x3D

// PCI command register bits
#define PCI_CMD_IO_SPACE    0x001  // enable I/O BAR access
#define PCI_CMD_MEM_SPACE   0x002  // enable MMIO BAR access
#define PCI_CMD_BUS_MASTER  0x004  // enable DMA

// Known vendor / device IDs
#define PCI_VENDOR_VIRTIO   0x1AF4
#define PCI_DEV_VIRTIO_NET  0x1000  // virtio legacy network card
#define PCI_DEV_VIRTIO_BLK  0x1001  // virtio legacy block device

typedef struct {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  irq_line;
    uint32_t bar[6];       // raw BAR values
    uint32_t bar_size[6];  // BAR sizes
} pci_device_t;

#define PCI_MAX_DEVICES 32
extern pci_device_t pci_devices[PCI_MAX_DEVICES];
extern int          pci_device_count;

void      pci_init();
uint32_t  pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void      pci_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);
pci_device_t* pci_find(uint16_t vendor, uint16_t device);

// Enable bus mastering + I/O + memory space for a device
void pci_enable(pci_device_t* dev);

// Get the I/O base address from BAR0 (for legacy virtio)
uint32_t pci_bar_io(pci_device_t* dev, int bar_idx);

#endif
