// tools/patch_diskboot.c
// Patches disk.img for hybrid BIOS+UEFI GPT boot.
//
// Disk layout produced:
//  LBA 0:        Protective MBR (boot.img code, type 0xEE entry)
//  LBA 0+0x44:   boot.img diskboot pointer → LBA 34
//  LBA 1:        GPT Primary Header
//  LBA 2-33:     GPT Partition Entry Array (128 × 128 bytes)
//  LBA 34-2047:  GRUB core.img (gap — up to 2014 sectors ≈ 1 MB)
//  LBA 2048:     EFI System Partition (FAT, 8192 sectors = 4 MB) ← gpt1
//  LBA 10240:    TxFS filesystem
//  LBA last-32:  Backup GPT Partition Entries
//  LBA last:     Backup GPT Header
//
// Usage: patch_diskboot <disk.img> <core_total_sectors> <total_disk_sectors>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ── CRC32 (IEEE 802.3 polynomial, required by GPT spec) ──────────────────────

static uint32_t crc32_table[256];

static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c >> 1) ^ (c & 1 ? 0xEDB88320u : 0u);
        crc32_table[i] = c;
    }
}

static uint32_t crc32_compute(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = (c >> 8) ^ crc32_table[(c ^ p[i]) & 0xFF];
    return c ^ 0xFFFFFFFFu;
}

// ── Structures ────────────────────────────────────────────────────────────────

#pragma pack(push, 1)

typedef struct {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_start;
    uint32_t lba_size;
} mbr_part_entry_t;

typedef struct {
    char     signature[8];       // "EFI PART"
    uint32_t revision;           // 0x00010000 = v1.0
    uint32_t header_size;        // 92
    uint32_t header_crc32;       // CRC of first 92 bytes (this field = 0 during calc)
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;   // 34
    uint64_t last_usable_lba;    // total_sectors - 34
    uint8_t  disk_guid[16];
    uint64_t partition_entry_lba;
    uint32_t num_partition_entries;   // 128
    uint32_t size_of_partition_entry; // 128
    uint32_t partition_entry_array_crc32;
} gpt_header_t;

typedef struct {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t start_lba;
    uint64_t end_lba;
    uint64_t attributes;
    uint16_t name[36]; // UTF-16LE, null-terminated
} gpt_partition_entry_t;

typedef struct {
    uint64_t start;
    uint16_t count;
    uint16_t segment;
} blocklist_entry_t;

#pragma pack(pop)

// EFI System Partition type GUID (mixed-endian as stored in GPT):
// C12A7328-F81F-11D2-BA4B-00A0C93EC93B
static const uint8_t ESP_TYPE_GUID[16] = {
    0x28, 0x73, 0x2A, 0xC1,  // C12A7328 bytes 0-3 (little-endian)
    0x1F, 0xF8,              // F81F bytes 4-5 (little-endian)
    0xD2, 0x11,              // 11D2 bytes 6-7 (big-endian as per spec)
    0xBA, 0x4B,
    0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

static const uint8_t DISK_GUID[16] = {
    0x54, 0x58, 0x45, 0x4E, 0x4F, 0x53, 0x31, 0x32,  // "TOXENOS12"
    0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30
};

static const uint8_t ESP_PART_GUID[16] = {
    0x45, 0x53, 0x50, 0x54, 0x58, 0x4E, 0x4F, 0x53,  // "ESPTxNOS"
    0x45, 0x46, 0x49, 0x30, 0x30, 0x30, 0x30, 0x31
};

static void write_utf16le(uint16_t* dst, const char* src, int max) {
    int i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = (uint16_t)(uint8_t)src[i];
    dst[i] = 0;
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <disk.img> <core_total_sectors> <total_disk_sectors>\n",
                argv[0]);
        return 1;
    }

    crc32_init();

    const char* disk_path    = argv[1];
    long        core_sectors = atol(argv[2]);
    long        total_sects  = atol(argv[3]);

    if (core_sectors < 2) {
        fprintf(stderr, "error: core_total_sectors must be >= 2\n"); return 1;
    }
    if (total_sects < 10240 + 16384 + 33) {
        fprintf(stderr, "error: disk too small for GPT+ESP+TxFS\n"); return 1;
    }

    FILE* f = fopen(disk_path, "r+b");
    if (!f) { perror("fopen"); return 1; }

    // ── 1. Patch boot.img offset 0x44: diskboot LBA = 34 ─────────────────────
    uint64_t diskboot_lba = 34;
    if (fseek(f, 0x44, SEEK_SET) != 0 ||
        fwrite(&diskboot_lba, 8, 1, f) != 1) {
        perror("patch boot.img 0x44"); fclose(f); return 1;
    }

    // ── 2. Protective MBR partition table at 0x1BE ────────────────────────────
    if (fseek(f, 0x1BE, SEEK_SET) != 0) { perror("fseek pmbr"); fclose(f); return 1; }
    mbr_part_entry_t pmbr;
    memset(&pmbr, 0, sizeof(pmbr));
    pmbr.status      = 0x00; // not active (protective MBR)
    pmbr.chs_first[0] = 0x00; pmbr.chs_first[1] = 0x02; pmbr.chs_first[2] = 0x00;
    pmbr.type        = 0xEE; // GPT protective
    pmbr.chs_last[0] = 0xFE; pmbr.chs_last[1] = 0xFF; pmbr.chs_last[2] = 0xFF;
    pmbr.lba_start   = 1;
    pmbr.lba_size    = (uint32_t)(total_sects > 0xFFFFFFFFu ? 0xFFFFFFFFu
                                                             : (uint32_t)total_sects - 1);
    if (fwrite(&pmbr, sizeof(pmbr), 1, f) != 1) {
        perror("write pmbr"); fclose(f); return 1;
    }
    // Zero the other 3 MBR partition slots
    uint8_t zero48[48] = {0};
    fwrite(zero48, 48, 1, f);

    // ── 3. Build GPT Partition Entry Array (128 × 128 = 16384 bytes) ─────────
    static gpt_partition_entry_t entries[128];
    memset(entries, 0, sizeof(entries));

    // Entry 0: EFI System Partition — LBA 2048 to 10239 (8192 sectors = 4 MB)
    memcpy(entries[0].type_guid,   ESP_TYPE_GUID, 16);
    memcpy(entries[0].unique_guid, ESP_PART_GUID,  16);
    entries[0].start_lba  = 2048;
    entries[0].end_lba    = 2048 + 8192 - 1; // 10239
    entries[0].attributes = 0;
    write_utf16le(entries[0].name, "EFI System Partition", 36);

    uint32_t entry_crc = crc32_compute(entries, sizeof(entries));

    // ── 4. GPT Primary Header at LBA 1 ───────────────────────────────────────
    gpt_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.signature, "EFI PART", 8);
    hdr.revision               = 0x00010000u;
    hdr.header_size            = 92;
    hdr.header_crc32           = 0;
    hdr.reserved               = 0;
    hdr.my_lba                 = 1;
    hdr.alternate_lba          = (uint64_t)(total_sects - 1);
    hdr.first_usable_lba       = 34;
    hdr.last_usable_lba        = (uint64_t)(total_sects - 34);
    memcpy(hdr.disk_guid, DISK_GUID, 16);
    hdr.partition_entry_lba    = 2;
    hdr.num_partition_entries  = 128;
    hdr.size_of_partition_entry = 128;
    hdr.partition_entry_array_crc32 = entry_crc;
    hdr.header_crc32           = crc32_compute(&hdr, 92);

    // Write primary header (LBA 1 = byte offset 512)
    uint8_t sector[512];
    memset(sector, 0, 512);
    memcpy(sector, &hdr, sizeof(hdr)); // 92 bytes, rest stays zero
    if (fseek(f, 512, SEEK_SET) != 0 || fwrite(sector, 512, 1, f) != 1) {
        perror("write gpt primary header"); fclose(f); return 1;
    }

    // ── 5. Write GPT Partition Entries at LBA 2-33 ───────────────────────────
    if (fseek(f, 512 * 2, SEEK_SET) != 0 ||
        fwrite(entries, sizeof(entries), 1, f) != 1) {
        perror("write gpt entries"); fclose(f); return 1;
    }

    // ── 6. Backup GPT Partition Entries at LBA (last-32) ─────────────────────
    long backup_entries_lba = total_sects - 33;
    if (fseek(f, 512LL * backup_entries_lba, SEEK_SET) != 0 ||
        fwrite(entries, sizeof(entries), 1, f) != 1) {
        perror("write backup gpt entries"); fclose(f); return 1;
    }

    // ── 7. Backup GPT Header at last LBA ─────────────────────────────────────
    gpt_header_t bhdr = hdr;
    bhdr.my_lba               = (uint64_t)(total_sects - 1);
    bhdr.alternate_lba        = 1;
    bhdr.partition_entry_lba  = (uint64_t)backup_entries_lba;
    bhdr.header_crc32         = 0;
    bhdr.header_crc32         = crc32_compute(&bhdr, 92);

    memset(sector, 0, 512);
    memcpy(sector, &bhdr, sizeof(bhdr));
    if (fseek(f, 512LL * (total_sects - 1), SEEK_SET) != 0 ||
        fwrite(sector, 512, 1, f) != 1) {
        perror("write backup gpt header"); fclose(f); return 1;
    }

    // ── 8. diskboot.img blocklist at LBA 34 offset 0x1F4 ─────────────────────
    // core.img: sector 0 at LBA 34 (diskboot), remaining at LBA 35..34+core_sectors-1
    if (fseek(f, 34LL * 512 + 0x1F4, SEEK_SET) != 0) {
        perror("fseek blocklist"); fclose(f); return 1;
    }
    blocklist_entry_t bl;
    bl.start   = 35;
    bl.count   = (uint16_t)(core_sectors - 1);
    bl.segment = 0x200;
    if (fwrite(&bl, sizeof(bl), 1, f) != 1) { perror("fwrite bl"); fclose(f); return 1; }
    memset(&bl, 0, sizeof(bl));
    if (fwrite(&bl, sizeof(bl), 1, f) != 1) { perror("fwrite bl term"); fclose(f); return 1; }

    fclose(f);

    printf("patch_diskboot: GPT layout written\n");
    printf("  Protective MBR: type 0xEE covering %ld sectors\n", total_sects);
    printf("  GPT primary at LBA 1, backup at LBA %ld\n", total_sects - 1);
    printf("  ESP (gpt1): LBA 2048-10239 (4 MB FAT)\n");
    printf("  GRUB BIOS: diskboot at LBA 34, core.img LBA 34-%ld (%ld sectors)\n",
           33 + core_sectors, core_sectors);
    printf("  TxFS: LBA 10240+\n");
    return 0;
}
