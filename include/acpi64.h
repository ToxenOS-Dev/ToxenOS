#ifndef ACPI64_H
#define ACPI64_H

// M+11A: minimal ACPI table discovery (RSDP -> RSDT/XSDT -> table by
// signature), enough to find the MADT. Pure validation/lookup logic takes
// an explicit physical->virtual mapper callback so the self-tests can
// drive it over synthetic tables in ordinary memory; the real mapper
// wraps physmem64_map_mmio()/physmem64_to_virt().
//
// Discovery only -- nothing here programs any interrupt hardware.
#include <stdint.h>

typedef struct {
    char     sig[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_header_t;

typedef struct {
    char     sig[8];        // "RSD PTR "
    uint8_t  checksum;      // first 20 bytes
    char     oem_id[6];
    uint8_t  revision;      // 0 = ACPI 1.0 (RSDT only), >=2 = has XSDT
    uint32_t rsdt_addr;
    // ACPI 2.0+ extension:
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;  // whole structure
    uint8_t  reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

#define ACPI64_SDT_MAX_LEN  (4u * 1024u * 1024u)   // sanity bound for any one table

// Maps a physical range and returns a readable pointer, or NULL.
typedef const void* (*acpi64_map_fn)(uint64_t phys, uint64_t len);

// Sum of all bytes == 0 (mod 256).
int  acpi64_checksum_ok(const void* p, uint32_t len);
// Validates an RSDP candidate in `avail` bytes: signature, v1 checksum,
// and (revision >= 2) length + extended checksum. 1 = valid.
int  acpi64_rsdp_valid(const void* p, uint32_t avail);
// Validates an SDT header + body inside `avail` bytes: header fits,
// length sane and within avail, checksum. `sig` (4 chars) may be NULL.
int  acpi64_sdt_valid(const void* p, uint64_t avail, const char* sig);
// Scans a buffer at 16-byte steps for a valid RSDP. Returns its byte
// offset or -1.
int  acpi64_scan_rsdp(const uint8_t* buf, uint32_t len);
// Looks `sig` up under the root table at `root_phys` (XSDT if is_xsdt,
// 64-bit entries, else RSDT with 32-bit entries). Every table is
// re-validated (header/length/checksum) before being returned. Returns
// a mapped, validated pointer or NULL.
const acpi_sdt_header_t* acpi64_root_find(uint64_t root_phys, int is_xsdt,
                                          const char* sig, acpi64_map_fn map);

// Boot-time discovery: RSDP from Multiboot2 tags 15/14, falling back to
// the BIOS-area scan. Returns the validated MADT ("APIC") or NULL and
// records why in acpi64_last_status().
const acpi_sdt_header_t* acpi64_find_madt(uint64_t mb_info_addr);
const char* acpi64_last_status(void);

#endif // ACPI64_H
