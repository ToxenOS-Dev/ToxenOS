#ifndef PCI64_H
#define PCI64_H

#include <stdint.h>

// Milestone 28: PCI configuration-space enumeration for the 64-bit
// kernel. Ported from kernel/pci.c's design (mechanism #1, ports
// 0xCF8/0xCFC -- adequate for every QEMU machine type this kernel
// targets; PCIe ECAM/MCFG is real-hardware/PCIe-bridge territory this
// milestone deliberately doesn't need, see the Milestone 28 summary),
// but reworked rather than copied: devices are discovered into a
// kmalloc'd linked list (not a fixed 32-entry array), and BAR decoding
// is a first-class part of enumeration (the 32-bit driver left 64-bit
// BAR combination and prefetch/type decoding to each caller) so every
// storage driver added this milestone shares one correct
// implementation instead of three ad hoc ones.

#define PCI64_CONFIG_ADDRESS 0xCF8
#define PCI64_CONFIG_DATA    0xCFC

#define PCI64_CLASS_STORAGE       0x01
#define PCI64_SUBCLASS_IDE        0x01
#define PCI64_SUBCLASS_SATA       0x06 // AHCI
#define PCI64_SUBCLASS_NVME       0x08
#define PCI64_PROGIF_AHCI         0x01
#define PCI64_PROGIF_NVME         0x02

#define PCI64_VENDOR_VIRTIO       0x1AF4
#define PCI64_DEVICE_VIRTIO_BLK_LEGACY 0x1001 // also the transitional device ID QEMU uses by default

// M+2: the standard PCI "vendor-specific" capability ID (generic PCI,
// not itself a VirtIO concept) -- every modern VirtIO capability
// (COMMON_CFG/NOTIFY_CFG/ISR_CFG/DEVICE_CFG/PCI_CFG) is carried inside
// one of these. See pci64_find_capability()'s own header comment.
#define PCI64_CAP_ID_VENDOR_SPECIFIC 0x09

typedef enum {
    PCI64_BAR_NONE = 0, // unused slot, or the high dword of a 64-bit BAR (already folded into the preceding slot)
    PCI64_BAR_IO,
    PCI64_BAR_MEM32,
    PCI64_BAR_MEM64,
} pci64_bar_type_t;

typedef struct {
    pci64_bar_type_t type;
    uint64_t address;  // I/O port base, or physical MMIO base -- full 64 bits, never truncated
    uint64_t size;      // bytes (MMIO) or the I/O port range size
    int prefetchable;   // MEM32/MEM64 only
} pci64_bar_t;

typedef struct pci64_device_s {
    uint8_t bus, slot, func;
    uint16_t vendor_id, device_id;
    uint8_t class_code, subclass, prog_if, revision;
    uint8_t header_type;   // raw byte (bit 7 = multifunction), as read from the device
    uint8_t irq_line, irq_pin;
    pci64_bar_t bar[6];    // bar[i].type == PCI64_BAR_NONE for the high-dword slot of a 64-bit BAR
    // M+11B: opaque pci64_irq_set (kernel/pci_irq64.c) once ToxenOS owns this
    // device's MSI/MSI-X interrupts; NULL otherwise. Also a one-shot flag
    // for the malformed-capability-list log.
    void* irq_state;
    uint8_t cap_malformed_logged;
    struct pci64_device_s* next;
} pci64_device_t;

// Raw config-space accessors -- exposed for drivers that need a
// register this struct doesn't already decode (capability lists,
// command/status, controller-specific vendor registers).
uint32_t pci64_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci64_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint8_t  pci64_config_read8 (uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci64_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
void     pci64_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);
void     pci64_config_write8 (uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t value);

// M+11B: every accessor above is a single NATIVE-width access, executed
// with interrupts off (CF8 is shared mutable state); a misaligned offset
// (word not 2-aligned, dword not 4-aligned) is rejected -- reads return
// all-ones, writes are dropped, both counted -- never masked to a
// neighbouring field.
//
// pci64_cfg_update8/16/32: atomic read-modify-write of one control field at
// its OWN width: raw read, (old & ~clear) | set, raw write, all inside ONE
// interrupt-off critical section (a separate read then write would leave a
// window where another config access could change CF8). Return the
// pre-modification value.
uint8_t  pci64_cfg_update8 (uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t  clear, uint8_t  set);
uint16_t pci64_cfg_update16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t clear, uint16_t set);
uint32_t pci64_cfg_update32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t clear, uint32_t set);

// Raw backend (self-test hook). Ops perform NO locking and NO alignment
// checks -- the generic layer above provides both. Passing NULL restores the
// real CF8/CFC backend. Returns the previous backend.
typedef struct {
    uint8_t  (*read8) (uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
    uint16_t (*read16)(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
    uint32_t (*read32)(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
    void     (*write8) (uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint8_t  v);
    void     (*write16)(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint16_t v);
    void     (*write32)(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v);
} pci64_cfg_backend_t;
const pci64_cfg_backend_t* pci64_cfg_set_backend(const pci64_cfg_backend_t* be);
uint32_t pci64_cfg_misaligned_count(void);
// Number of config critical sections entered so far. A read-modify-write is
// ONE section: its raw read and raw write observe the same value.
uint32_t pci64_cfg_lock_generation(void);

// Enumerates every PCI bus (0-255)/slot (0-31)/function, correctly
// following the multifunction bit (header type 0x80) to scan functions
// 1-7 only when a device's function 0 sets it, and decodes every BAR
// (I/O vs 32-bit MMIO vs 64-bit MMIO, size, prefetchable) into a
// kmalloc'd device list. Safe to call once; subsequent calls are a
// silent no-op (returns the count from the first call) since this
// kernel's PCI topology never changes after boot. Must run before the
// first process64_spawn() if any discovered device's MMIO BAR is going
// to be mapped via physmem64_map_mmio() -- see that function's own
// ordering requirement.
int pci64_enumerate(void);

// Sets the I/O space, memory space, and bus-master enable bits in the
// PCI command register (offset 0x04) -- required before a driver can
// use a device's I/O or MMIO BARs or have it act as a DMA initiator.
void pci64_enable_device(pci64_device_t* dev);

// Registry iteration/lookup, valid only after pci64_enumerate().
pci64_device_t* pci64_iter(pci64_device_t* prev);
// Finds the first device (starting after `prev`, or from the beginning
// if NULL) matching class/subclass (subclass -1 = don't care) and, if
// prog_if >= 0, matching prog-if too. Used by each storage driver to
// find its own controllers without duplicating a bus/slot/func scan.
pci64_device_t* pci64_find_class(uint8_t class_code, int subclass, int prog_if, pci64_device_t* prev);
pci64_device_t* pci64_find_device(uint16_t vendor_id, uint16_t device_id, pci64_device_t* prev);

int pci64_device_count(void);

// M+2: safe traversal of the standard (offsets 0x00-0xFF -- this
// kernel's port-0xCF8/0xCFC mechanism #1 can never address PCIe extended
// config space anyway, see this header's own Milestone 28 comment)
// PCI capability list. Finds the first capability whose ID matches
// `cap_id`, starting either from the device's own capability-list head
// (`start_after == 0`) or resuming right after a previously-found
// capability at `start_after` (its own offset, as returned by a prior
// call) -- used to enumerate MULTIPLE capabilities of the same ID (every
// modern VirtIO device has several PCI64_CAP_ID_VENDOR_SPECIFIC entries,
// one per cfg_type). Returns the matching capability's own config-space
// offset (always >= 0x40), or -1 if none remain OR the list is malformed
// in any way this function can detect: the status register's
// capabilities-list bit isn't set, a pointer lands inside the fixed
// 0x00-0x3F header, a pointer leaves no room for even a 2-byte id/next
// pair before offset 0x100, the same offset is visited twice (a loop,
// self-referencing or otherwise), or the chain runs longer than any real
// device's capability list plausibly could (a generous fixed bound, not
// a real per-spec limit -- just a backstop against a pathological fake
// chain spinning this function forever). Never trusts a single pointer
// value without these checks -- see this milestone's own instruction not
// to trust PCI capability chains blindly.
int pci64_find_capability(pci64_device_t* dev, uint8_t cap_id, uint8_t start_after);
// M+11B: implemented over kernel/pci_cap64.c's strict walker -- a
// capability pointer with low bits set (misaligned) is now MALFORMED (-1)
// rather than silently masked; results for well-formed lists are identical.

// Logs every enumerated device (bus:slot.func, vendor:device,
// class/subclass/prog-if, every valid BAR's type/address/size,
// irq_line/pin) via klog(). Development use only.
void pci64_dump(void);

#endif // PCI64_H
