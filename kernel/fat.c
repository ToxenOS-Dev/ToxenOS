// ToxenOS/kernel/fat.c
// FAT12 / FAT16 / FAT32 driver
// Plugs into the VFS as a fs_driver_t — mount at any path via vfs_mount().
//
// Supports:
//   - FAT12, FAT16, FAT32 auto-detection from BPB
//   - Read files and directories
//   - Write / append to existing files
//   - Create files and directories
//   - Remove files
//   - Long File Names (LFN) for FAT32
//   - ATA sector I/O through ata_read / ata_write

#include <stdint.h>
#include "../include/fat.h"
#include "../include/ata.h"
#include "../include/mm.h"
#include "../include/vga.h"

// ── helpers ──────────────────────────────────────────────────────────────────

static int fat_strlen(const char* s) { int i=0; while(s[i]) i++; return i; }

static void fat_strcpy(char* d, const char* s, int max)
{
    int i=0;
    while (s[i] && i < max-1) { d[i]=s[i]; i++; }
    d[i]=0;
}

static int fat_strcmp(const char* a, const char* b)
{
    int i;
    for (i=0; a[i] && b[i]; i++)
        if (a[i] != b[i]) return 1;
    return a[i] != b[i];
}

// case-insensitive compare (FAT names are case-insensitive)
static int fat_strcmpi(const char* a, const char* b)
{
    int i;
    for (i=0; a[i] && b[i]; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 1;
    }
    return a[i] != b[i];
}

static void fat_memset(void* p, uint8_t v, uint32_t n)
{
    uint8_t* b = (uint8_t*)p;
    for (uint32_t i=0; i<n; i++) b[i]=v;
}

static void fat_memcpy(void* d, const void* s, uint32_t n)
{
    uint8_t* dd = (uint8_t*)d;
    const uint8_t* ss = (const uint8_t*)s;
    for (uint32_t i=0; i<n; i++) dd[i]=ss[i];
}

// ── BPB (BIOS Parameter Block) ───────────────────────────────────────────────

typedef struct {
    uint8_t  jump[3];
    uint8_t  oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  fat_count;
    uint16_t root_entry_count;    // FAT12/16 only (0 for FAT32)
    uint16_t total_sectors_16;
    uint8_t  media;
    uint16_t fat_size_16;         // FAT12/16 only (0 for FAT32)
    uint16_t sectors_per_track;
    uint16_t head_count;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    // FAT32 extended BPB
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;        // FAT32 root dir cluster
    uint16_t fs_info;
    uint16_t backup_boot;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];
} __attribute__((packed)) fat_bpb_t;

// ── Directory entry (8.3) ────────────────────────────────────────────────────

typedef struct {
    uint8_t  name[8];
    uint8_t  ext[3];
    uint8_t  attr;
    uint8_t  nt_res;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t acc_date;
    uint16_t cluster_hi;    // FAT32 high 16 bits of start cluster
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t cluster_lo;    // start cluster low 16 bits
    uint32_t file_size;
} __attribute__((packed)) fat_dirent_t;

// LFN entry
typedef struct {
    uint8_t  order;
    uint16_t name1[5];
    uint8_t  attr;          // always 0x0F
    uint8_t  type;
    uint8_t  checksum;
    uint16_t name2[6];
    uint16_t cluster;       // always 0
    uint16_t name3[2];
} __attribute__((packed)) fat_lfn_t;

#define FAT_ATTR_READONLY   0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LFN        0x0F

// ── Filesystem state ─────────────────────────────────────────────────────────

typedef enum { FAT_TYPE_12, FAT_TYPE_16, FAT_TYPE_32 } fat_type_t;

typedef struct {
    fat_type_t type;
    uint32_t   bytes_per_sector;
    uint32_t   sectors_per_cluster;
    uint32_t   bytes_per_cluster;
    uint32_t   fat_start;       // LBA of first FAT
    uint32_t   fat_size;        // sectors per FAT
    uint32_t   fat_count;
    uint32_t   root_start;      // LBA of root dir (FAT12/16 only)
    uint32_t   root_sectors;    // size of root dir in sectors (FAT12/16)
    uint32_t   data_start;      // LBA of first data cluster
    uint32_t   root_cluster;    // FAT32 root cluster
    uint32_t   total_clusters;
    char       mountpoint[64];
    int        mounted;
} fat_fs_t;

static fat_fs_t fat_fs;

// ── Sector I/O ───────────────────────────────────────────────────────────────

static uint8_t sector_buf[512];

#define FAT_LBA_OFFSET 1

static int fat_read_sector(uint32_t lba, uint8_t* buf)
{
    return ata_read_drive(ATA_DRIVE_SLAVE, lba + FAT_LBA_OFFSET, buf, 1);
}

static int fat_write_sector(uint32_t lba, const uint8_t* buf)
{
    return ata_write_drive(ATA_DRIVE_SLAVE, lba + FAT_LBA_OFFSET, buf, 1);
}

// Read a full cluster into buf (buf must be bytes_per_cluster bytes)
static int fat_read_cluster(uint32_t cluster, uint8_t* buf)
{
    uint32_t lba = fat_fs.data_start +
                   (cluster - 2) * fat_fs.sectors_per_cluster;
    for (uint32_t i = 0; i < fat_fs.sectors_per_cluster; i++) {
        if (ata_read_drive(ATA_DRIVE_SLAVE, lba + i + FAT_LBA_OFFSET, buf + i * fat_fs.bytes_per_sector, 1) < 0)
            return -1;
    }
    return 0;
}

static int fat_write_cluster(uint32_t cluster, const uint8_t* buf)
{
    uint32_t lba = fat_fs.data_start +
                   (cluster - 2) * fat_fs.sectors_per_cluster;
    for (uint32_t i = 0; i < fat_fs.sectors_per_cluster; i++) {
        if (ata_write_drive(ATA_DRIVE_SLAVE, lba + i + FAT_LBA_OFFSET, buf + i * fat_fs.bytes_per_sector, 1) < 0)
            return -1;
    }
    return 0;
}

// ── FAT table access ─────────────────────────────────────────────────────────

#define FAT32_EOC   0x0FFFFFF8
#define FAT16_EOC   0xFFF8
#define FAT12_EOC   0xFF8
#define FAT_FREE    0x00

static uint32_t fat_get_entry(uint32_t cluster)
{
    uint32_t fat_offset;
    if (fat_fs.type == FAT_TYPE_12)
        fat_offset = cluster + (cluster / 2);
    else if (fat_fs.type == FAT_TYPE_16)
        fat_offset = cluster * 2;
    else
        fat_offset = cluster * 4;

    uint32_t sector = fat_fs.fat_start + fat_offset / fat_fs.bytes_per_sector;
    uint32_t offset = fat_offset % fat_fs.bytes_per_sector;

    fat_read_sector(sector, sector_buf);

    if (fat_fs.type == FAT_TYPE_12) {
        uint16_t val;
        if (offset == fat_fs.bytes_per_sector - 1) {
            // entry spans two sectors
            val = sector_buf[offset];
            fat_read_sector(sector + 1, sector_buf);
            val |= (uint16_t)sector_buf[0] << 8;
        } else {
            val = *(uint16_t*)(sector_buf + offset);
        }
        if (cluster & 1) val >>= 4;
        else             val &= 0x0FFF;
        return val;
    } else if (fat_fs.type == FAT_TYPE_16) {
        return *(uint16_t*)(sector_buf + offset);
    } else {
        return (*(uint32_t*)(sector_buf + offset)) & 0x0FFFFFFF;
    }
}

static void fat_set_entry(uint32_t cluster, uint32_t value)
{
    uint32_t fat_offset;
    if (fat_fs.type == FAT_TYPE_12)
        fat_offset = cluster + (cluster / 2);
    else if (fat_fs.type == FAT_TYPE_16)
        fat_offset = cluster * 2;
    else
        fat_offset = cluster * 4;

    uint32_t sector = fat_fs.fat_start + fat_offset / fat_fs.bytes_per_sector;
    uint32_t offset = fat_offset % fat_fs.bytes_per_sector;

    fat_read_sector(sector, sector_buf);

    if (fat_fs.type == FAT_TYPE_12) {
        uint16_t cur;
        if (offset == fat_fs.bytes_per_sector - 1) {
            cur = sector_buf[offset] | ((uint16_t)sector_buf[0] << 8); // approx
        } else {
            cur = *(uint16_t*)(sector_buf + offset);
        }
        if (cluster & 1) {
            cur = (cur & 0x000F) | ((value & 0xFFF) << 4);
        } else {
            cur = (cur & 0xF000) | (value & 0x0FFF);
        }
        sector_buf[offset] = cur & 0xFF;
        if (offset + 1 < fat_fs.bytes_per_sector)
            sector_buf[offset+1] = (cur >> 8) & 0xFF;
        fat_write_sector(sector, sector_buf);
    } else if (fat_fs.type == FAT_TYPE_16) {
        *(uint16_t*)(sector_buf + offset) = (uint16_t)value;
        fat_write_sector(sector, sector_buf);
    } else {
        uint32_t cur = *(uint32_t*)(sector_buf + offset);
        cur = (cur & 0xF0000000) | (value & 0x0FFFFFFF);
        *(uint32_t*)(sector_buf + offset) = cur;
        fat_write_sector(sector, sector_buf);
        // also update second FAT if present
        if (fat_fs.fat_count > 1) {
            uint32_t sec2 = fat_fs.fat_start + fat_fs.fat_size + fat_offset / fat_fs.bytes_per_sector;
            fat_read_sector(sec2, sector_buf);
            cur = *(uint32_t*)(sector_buf + offset);
            cur = (cur & 0xF0000000) | (value & 0x0FFFFFFF);
            *(uint32_t*)(sector_buf + offset) = cur;
            fat_write_sector(sec2, sector_buf);
        }
    }
}

static int fat_is_eoc(uint32_t entry)
{
    if (fat_fs.type == FAT_TYPE_12) return entry >= FAT12_EOC;
    if (fat_fs.type == FAT_TYPE_16) return entry >= FAT16_EOC;
    return entry >= FAT32_EOC;
}

// Find a free cluster and allocate it (chain from prev if != 0)
static uint32_t fat_alloc_cluster(uint32_t prev)
{
    uint32_t eoc = (fat_fs.type == FAT_TYPE_12) ? 0xFF8 :
                   (fat_fs.type == FAT_TYPE_16) ? 0xFFF8 : 0x0FFFFFF8;

    for (uint32_t i = 2; i < fat_fs.total_clusters + 2; i++) {
        if (fat_get_entry(i) == 0) {
            fat_set_entry(i, eoc);  // mark as end of chain
            if (prev) fat_set_entry(prev, i);  // link previous
            // zero the cluster
            uint8_t zero[4096];
            fat_memset(zero, 0, fat_fs.bytes_per_cluster);
            fat_write_cluster(i, zero);
            return i;
        }
    }
    return 0;  // disk full
}

// Free an entire cluster chain starting at cluster
static void fat_free_chain(uint32_t cluster)
{
    while (cluster && !fat_is_eoc(cluster)) {
        uint32_t next = fat_get_entry(cluster);
        fat_set_entry(cluster, 0);
        cluster = next;
    }
}

// ── Name helpers ─────────────────────────────────────────────────────────────

// Convert 8.3 dir entry name to null-terminated string (trims spaces)
static void fat_parse_83(const fat_dirent_t* de, char* out)
{
    int i = 0, j = 0;
    // name
    for (i = 0; i < 8 && de->name[i] != ' '; i++) {
        char c = de->name[i];
        if (c >= 'A' && c <= 'Z') c += 32;  // lowercase
        out[j++] = c;
    }
    // extension
    if (de->ext[0] != ' ') {
        out[j++] = '.';
        for (i = 0; i < 3 && de->ext[i] != ' '; i++) {
            char c = de->ext[i];
            if (c >= 'A' && c <= 'Z') c += 32;
            out[j++] = c;
        }
    }
    out[j] = 0;
}

// Fill 8.3 name/ext fields from a filename string
static void fat_make_83(const char* name, uint8_t* name_out, uint8_t* ext_out)
{
    fat_memset(name_out, ' ', 8);
    fat_memset(ext_out,  ' ', 3);

    int i = 0, j = 0;
    while (name[i] && name[i] != '.' && j < 8) {
        char c = name[i++];
        if (c >= 'a' && c <= 'z') c -= 32;
        name_out[j++] = c;
    }
    if (name[i] == '.') {
        i++;
        j = 0;
        while (name[i] && j < 3) {
            char c = name[i++];
            if (c >= 'a' && c <= 'z') c -= 32;
            ext_out[j++] = c;
        }
    }
}

// Extract LFN characters from an LFN entry into buf at offset
static void fat_lfn_chars(const fat_lfn_t* lfn, char* buf, int offset)
{
    // Each LFN entry holds 13 UTF-16LE characters
    // We just take the low byte (ASCII range only)
    int pos = offset;
    for (int i = 0; i < 5; i++) {
        uint16_t ch = lfn->name1[i];
        if (ch == 0xFFFF || ch == 0) return;
        buf[pos++] = (char)(ch & 0xFF);
    }
    for (int i = 0; i < 6; i++) {
        uint16_t ch = lfn->name2[i];
        if (ch == 0xFFFF || ch == 0) return;
        buf[pos++] = (char)(ch & 0xFF);
    }
    for (int i = 0; i < 2; i++) {
        uint16_t ch = lfn->name3[i];
        if (ch == 0xFFFF || ch == 0) return;
        buf[pos++] = (char)(ch & 0xFF);
    }
    buf[pos] = 0;
}

// ── Path helpers ─────────────────────────────────────────────────────────────

static const char* fat_strip_mount(const char* path)
{
    int i = 0;
    while (fat_fs.mountpoint[i] && path[i] == fat_fs.mountpoint[i]) i++;
    if (!path[i]) return "/";
    return path + i;
}

static void fat_split_path(const char* path, char* parent, char* name)
{
    int len   = fat_strlen(path);
    int slash = -1;
    for (int i = len-1; i >= 0; i--) {
        if (path[i] == '/') { slash = i; break; }
    }
    if (slash <= 0) {
        fat_strcpy(parent, "/", 256);
        const char* n = path; if (*n == '/') n++;
        fat_strcpy(name, n, 256);
    } else {
        for (int i = 0; i < slash; i++) parent[i] = path[i];
        parent[slash] = 0;
        fat_strcpy(name, path + slash + 1, 256);
    }
}

// ── Directory iteration ───────────────────────────────────────────────────────

// Callback returns 1 to stop iteration (entry found), 0 to continue
typedef int (*fat_dir_cb)(fat_dirent_t* de, const char* lfn, void* ctx);

// Iterate over every valid (non-deleted, non-volume-label) entry in a dir.
// For FAT12/16 root: cluster=0. For everything else: cluster=start cluster.
static void fat_iter_dir(uint32_t cluster, fat_dir_cb cb, void* ctx)
{
    #define MAX_CLUSTER_SIZE 4096
    static uint8_t cbuf[MAX_CLUSTER_SIZE];
    char   lfn_buf[256];
    int    lfn_ready = 0;

    fat_memset(lfn_buf, 0, 256);

    // For FAT12/16 root directory, iterate fixed sectors
    if (cluster == 0 && fat_fs.type != FAT_TYPE_32) {
        for (uint32_t sec = 0; sec < fat_fs.root_sectors; sec++) {
            fat_read_sector(fat_fs.root_start + sec, cbuf);
            fat_dirent_t* entries = (fat_dirent_t*)cbuf;
            int per_sec = fat_fs.bytes_per_sector / sizeof(fat_dirent_t);
            for (int i = 0; i < per_sec; i++) {
                fat_dirent_t* de = &entries[i];
                if (de->name[0] == 0x00) return;  // end of dir
                if ((uint8_t)de->name[0] == 0xE5) { lfn_ready = 0; continue; } // deleted
                if (de->attr == FAT_ATTR_LFN) {
                    fat_lfn_t* lfn = (fat_lfn_t*)de;
                    int seq = (lfn->order & 0x3F) - 1;
                    fat_lfn_chars(lfn, lfn_buf, seq * 13);
                    lfn_ready = 1;
                    continue;
                }
                if (de->attr & FAT_ATTR_VOLUME_ID) { lfn_ready = 0; continue; }
                const char* name = lfn_ready ? lfn_buf : NULL;
                lfn_ready = 0;
                fat_memset(lfn_buf, 0, 256);
                if (cb(de, name, ctx)) return;
            }
        }
        return;
    }

    // Cluster chain
    while (cluster && !fat_is_eoc(cluster)) {
        fat_read_cluster(cluster, cbuf);
        fat_dirent_t* entries = (fat_dirent_t*)cbuf;
        int per_cluster = (int)(fat_fs.bytes_per_cluster / sizeof(fat_dirent_t));
        for (int i = 0; i < per_cluster; i++) {
            fat_dirent_t* de = &entries[i];
            if (de->name[0] == 0x00) return;
            if ((uint8_t)de->name[0] == 0xE5) { lfn_ready = 0; continue; }
            if (de->attr == FAT_ATTR_LFN) {
                fat_lfn_t* lfn = (fat_lfn_t*)de;
                int seq = (lfn->order & 0x3F) - 1;
                fat_lfn_chars(lfn, lfn_buf, seq * 13);
                lfn_ready = 1;
                continue;
            }
            if (de->attr & FAT_ATTR_VOLUME_ID) { lfn_ready = 0; continue; }
            const char* name = lfn_ready ? lfn_buf : NULL;
            lfn_ready = 0;
            fat_memset(lfn_buf, 0, 256);
            if (cb(de, name, ctx)) return;
        }
        cluster = fat_get_entry(cluster);
    }
}

// ── Lookup ────────────────────────────────────────────────────────────────────

typedef struct { const char* target; fat_dirent_t result; int found; } lookup_ctx_t;

static int lookup_cb(fat_dirent_t* de, const char* lfn, void* ctx)
{
    lookup_ctx_t* lc = (lookup_ctx_t*)ctx;
    char short_name[13];
    fat_parse_83(de, short_name);

    // try LFN match first, then 8.3
    int match = 0;
    if (lfn && !fat_strcmpi(lfn, lc->target)) match = 1;
    if (!match && !fat_strcmpi(short_name, lc->target)) match = 1;

    if (match) {
        lc->result = *de;
        lc->found  = 1;
        return 1;
    }
    return 0;
}

// Walk path and return the dirent of the final component. Returns 0 on success.
static int fat_lookup(const char* path, fat_dirent_t* out)
{
    if (!fat_strcmp(path, "/")) {
        // Synthesize a fake root dirent
        fat_memset(out, 0, sizeof(*out));
        out->attr       = FAT_ATTR_DIRECTORY;
        out->cluster_lo = (uint16_t)(fat_fs.root_cluster & 0xFFFF);
        out->cluster_hi = (uint16_t)(fat_fs.root_cluster >> 16);
        return 0;
    }

    const char* p = path;
    if (*p == '/') p++;

    // Start at root
    uint32_t cur_cluster = (fat_fs.type == FAT_TYPE_32) ? fat_fs.root_cluster : 0;
    fat_dirent_t de;

    while (*p) {
        char component[256];
        int len = 0;
        while (p[len] && p[len] != '/') len++;
        for (int i = 0; i < len; i++) component[i] = p[i];
        component[len] = 0;
        p += len;
        if (*p == '/') p++;

        lookup_ctx_t lc;
        lc.target = component;
        lc.found  = 0;
        fat_iter_dir(cur_cluster, lookup_cb, &lc);
        if (!lc.found) return -1;

        de = lc.result;
        cur_cluster = ((uint32_t)de.cluster_hi << 16) | de.cluster_lo;
    }

    *out = de;
    return 0;
}

// Get start cluster of a dirent
static uint32_t fat_dirent_cluster(const fat_dirent_t* de)
{
    return ((uint32_t)de->cluster_hi << 16) | de->cluster_lo;
}

// Get start cluster of a directory given its path
static uint32_t fat_dir_cluster(const char* path)
{
    if (!fat_strcmp(path, "/"))
        return (fat_fs.type == FAT_TYPE_32) ? fat_fs.root_cluster : 0;
    fat_dirent_t de;
    if (fat_lookup(path, &de) < 0) return 0;
    return fat_dirent_cluster(&de);
}

// ── Open file descriptors ─────────────────────────────────────────────────────

#define FAT_MAX_FDS 32

typedef struct {
    int      used;
    uint32_t start_cluster;
    uint32_t cur_cluster;
    uint32_t position;
    uint32_t size;
    uint32_t dir_cluster;    // cluster of parent directory
    uint32_t dir_entry_idx;  // index within parent dir (for size updates on write)
    uint8_t  attr;
} fat_fd_t;

static fat_fd_t fat_fds[FAT_MAX_FDS];

// ── Mount ─────────────────────────────────────────────────────────────────────

static int fat_mount_fn(const char* device)
{
    (void)device;

    fat_memset(&fat_fs, 0, sizeof(fat_fs));
    fat_strcpy(fat_fs.mountpoint, "/D:", 64);

    static uint8_t boot[512];
    // BPB is at LBA 1 (LBA 0 is a blank sector to work around QEMU slave bug)
    if (ata_read_drive(ATA_DRIVE_SLAVE, 1, boot, 1) < 0) return -1;

    fat_bpb_t* bpb = (fat_bpb_t*)boot;
    if (bpb->bytes_per_sector == 0 || bpb->sectors_per_cluster == 0) return -1;

    fat_fs.bytes_per_sector    = bpb->bytes_per_sector;
    fat_fs.sectors_per_cluster = bpb->sectors_per_cluster;
    fat_fs.bytes_per_cluster   = fat_fs.bytes_per_sector * fat_fs.sectors_per_cluster;
    fat_fs.fat_count           = bpb->fat_count;
    fat_fs.fat_start           = bpb->reserved_sectors;

    uint32_t fat_size   = bpb->fat_size_16 ? bpb->fat_size_16 : bpb->fat_size_32;
    uint32_t total_secs = bpb->total_sectors_16 ? bpb->total_sectors_16 : bpb->total_sectors_32;
    uint32_t root_secs  = ((bpb->root_entry_count * 32) + (bpb->bytes_per_sector - 1)) / bpb->bytes_per_sector;

    fat_fs.fat_size     = fat_size;
    fat_fs.root_start   = fat_fs.fat_start + fat_fs.fat_count * fat_size;
    fat_fs.root_sectors = root_secs;
    fat_fs.data_start   = fat_fs.root_start + root_secs;

    uint32_t data_secs    = total_secs - fat_fs.data_start;
    fat_fs.total_clusters = data_secs / fat_fs.sectors_per_cluster;

    if (fat_fs.total_clusters < 4085)
        fat_fs.type = FAT_TYPE_12;
    else if (fat_fs.total_clusters < 65525)
        fat_fs.type = FAT_TYPE_16;
    else
        fat_fs.type = FAT_TYPE_32;

    if (fat_fs.type == FAT_TYPE_32)
        fat_fs.root_cluster = bpb->root_cluster;

    fat_fs.mounted = 1;

    for (int i = 0; i < FAT_MAX_FDS; i++)
        fat_fds[i].used = 0;

    return 0;
}

// ── Open ──────────────────────────────────────────────────────────────────────

static int fat_open_fn(const char* path, int flags)
{
    const char* local = fat_strip_mount(path);

    fat_dirent_t de;
    int found = (fat_lookup(local, &de) == 0);

    if (!found) {
        if (!(flags & VFS_O_CREATE)) return -1;

        // Create a new file
        char parent_path[256], name[256];
        fat_split_path(local, parent_path, name);

        uint32_t dir_cluster = fat_dir_cluster(parent_path);

        // Alloc cluster for file
        uint32_t new_cluster = fat_alloc_cluster(0);
        if (!new_cluster) return -1;

        // Build dirent
        fat_dirent_t new_de;
        fat_memset(&new_de, 0, sizeof(new_de));
        fat_make_83(name, new_de.name, new_de.ext);
        new_de.attr       = FAT_ATTR_ARCHIVE;
        new_de.cluster_lo = (uint16_t)(new_cluster & 0xFFFF);
        new_de.cluster_hi = (uint16_t)(new_cluster >> 16);
        new_de.file_size  = 0;

        // Write dirent into parent directory
        // Find a free (0x00 or 0xE5) slot
        #define MAX_CS 4096
        static uint8_t dcbuf[MAX_CS];
        uint32_t dc = dir_cluster;
        uint32_t prev_dc = 0;
        int written = 0;

        while ((dc && !fat_is_eoc(dc)) || (dc == 0 && fat_fs.type != FAT_TYPE_32)) {
            if (dc == 0) {
                for (uint32_t s = 0; s < fat_fs.root_sectors; s++) {
                    fat_read_sector(fat_fs.root_start + s, dcbuf);
                    fat_dirent_t* entries = (fat_dirent_t*)dcbuf;
                    int per = fat_fs.bytes_per_sector / sizeof(fat_dirent_t);
                    for (int i = 0; i < per; i++) {
                        if (entries[i].name[0] == 0x00 || (uint8_t)entries[i].name[0] == 0xE5) {
                            entries[i] = new_de;
                            fat_write_sector(fat_fs.root_start + s, dcbuf);
                            written = 1; break;
                        }
                    }
                    if (written) break;
                }
                break;
            } else {
                fat_read_cluster(dc, dcbuf);
                fat_dirent_t* entries = (fat_dirent_t*)dcbuf;
                int per = (int)(fat_fs.bytes_per_cluster / sizeof(fat_dirent_t));
                for (int i = 0; i < per; i++) {
                    if (entries[i].name[0] == 0x00 || (uint8_t)entries[i].name[0] == 0xE5) {
                        entries[i] = new_de;
                        fat_write_cluster(dc, dcbuf);
                        written = 1; break;
                    }
                }
                if (written) break;
                prev_dc = dc;
                dc = fat_get_entry(dc);
                if (fat_is_eoc(dc)) {
                    dc = fat_alloc_cluster(prev_dc);
                    if (!dc) break;
                }
            }
        }

        if (!written) return -1;
        de = new_de;
    }

    // Find free fd
    for (int i = 0; i < FAT_MAX_FDS; i++) {
        if (!fat_fds[i].used) {
            fat_fds[i].used          = 1;
            fat_fds[i].start_cluster = fat_dirent_cluster(&de);
            fat_fds[i].cur_cluster   = fat_dirent_cluster(&de);
            fat_fds[i].position      = 0;
            fat_fds[i].size          = de.file_size;
            fat_fds[i].attr          = de.attr;
            return i;
        }
    }
    return -1;
}

// ── Close ─────────────────────────────────────────────────────────────────────

static int fat_close_fn(int fd)
{
    if (fd < 0 || fd >= FAT_MAX_FDS || !fat_fds[fd].used) return -1;
    fat_fds[fd].used = 0;
    return 0;
}

// ── Read ──────────────────────────────────────────────────────────────────────

static int fat_read_fn(int fd, uint8_t* buf, uint32_t size)
{
    if (fd < 0 || fd >= FAT_MAX_FDS || !fat_fds[fd].used) return -1;
    fat_fd_t* f = &fat_fds[fd];

    if (f->position >= f->size) return 0;
    if (f->position + size > f->size) size = f->size - f->position;

    static uint8_t cluster_buf[4096];
    uint32_t done = 0;

    while (done < size) {
        if (!f->cur_cluster || fat_is_eoc(f->cur_cluster)) break;

        uint32_t cluster_offset = f->position % fat_fs.bytes_per_cluster;
        uint32_t can_read = fat_fs.bytes_per_cluster - cluster_offset;
        if (can_read > size - done) can_read = size - done;

        fat_read_cluster(f->cur_cluster, cluster_buf);
        fat_memcpy(buf + done, cluster_buf + cluster_offset, can_read);
        done        += can_read;
        f->position += can_read;

        // Advance to next cluster if we've consumed this one
        if (f->position % fat_fs.bytes_per_cluster == 0)
            f->cur_cluster = fat_get_entry(f->cur_cluster);
    }

    return (int)done;
}

// ── Write ─────────────────────────────────────────────────────────────────────

static int fat_write_fn(int fd, const uint8_t* buf, uint32_t size)
{
    if (fd < 0 || fd >= FAT_MAX_FDS || !fat_fds[fd].used) return -1;
    fat_fd_t* f = &fat_fds[fd];

    static uint8_t cluster_buf[4096];
    uint32_t done = 0;

    while (done < size) {
        // Allocate a new cluster if needed
        if (!f->cur_cluster || fat_is_eoc(f->cur_cluster)) {
            uint32_t prev = f->start_cluster ? f->cur_cluster : 0;
            uint32_t nc   = fat_alloc_cluster(prev);
            if (!nc) break;
            if (!f->start_cluster) {
                f->start_cluster = nc;
            }
            f->cur_cluster = nc;
        }

        uint32_t cluster_offset = f->position % fat_fs.bytes_per_cluster;
        uint32_t can_write = fat_fs.bytes_per_cluster - cluster_offset;
        if (can_write > size - done) can_write = size - done;

        fat_read_cluster(f->cur_cluster, cluster_buf);
        fat_memcpy(cluster_buf + cluster_offset, buf + done, can_write);
        fat_write_cluster(f->cur_cluster, cluster_buf);

        done        += can_write;
        f->position += can_write;
        if (f->position > f->size) f->size = f->position;

        if (f->position % fat_fs.bytes_per_cluster == 0)
            f->cur_cluster = fat_get_entry(f->cur_cluster);
    }

    return (int)done;
}

// ── Readdir ───────────────────────────────────────────────────────────────────

typedef struct { uint32_t target; uint32_t count; char out[256]; int found; } readdir_ctx_t;

static int readdir_cb(fat_dirent_t* de, const char* lfn, void* ctx)
{
    readdir_ctx_t* rc = (readdir_ctx_t*)ctx;

    // skip . and .. entries
    if (de->name[0] == '.') return 0;
    // skip entries with no usable name
    if (!lfn && de->name[0] == ' ') return 0;

    char short_name[13];
    fat_parse_83(de, short_name);
    // skip if short name is empty
    if (!short_name[0]) return 0;

    if (rc->count == rc->target) {
        if (lfn && lfn[0])
            fat_strcpy(rc->out, lfn, 256);
        else
            fat_strcpy(rc->out, short_name, 256);
        rc->found = 1;
        return 1;
    }
    rc->count++;
    return 0;
}

static int fat_readdir_fn(const char* path, char* out, uint32_t index)
{
    const char* local = fat_strip_mount(path);
    uint32_t cluster  = fat_dir_cluster(local);

    readdir_ctx_t rc;
    rc.target = index;
    rc.count  = 0;
    rc.found  = 0;
    fat_memset(rc.out, 0, 256);

    fat_iter_dir(cluster, readdir_cb, &rc);

    if (!rc.found) return -1;
    fat_strcpy(out, rc.out, 256);
    return 0;
}

// ── Stat ──────────────────────────────────────────────────────────────────────

static int fat_stat_fn(const char* path, uint32_t* size)
{
    const char* local = fat_strip_mount(path);
    fat_dirent_t de;
    if (fat_lookup(local, &de) < 0) return -1;
    *size = de.file_size;
    return 0;
}

// ── Isdir ─────────────────────────────────────────────────────────────────────

static int fat_isdir_fn(const char* path)
{
    const char* local = fat_strip_mount(path);
    if (!fat_strcmp(local, "/")) return 1;
    fat_dirent_t de;
    if (fat_lookup(local, &de) < 0) return -1;
    return (de.attr & FAT_ATTR_DIRECTORY) ? 1 : 0;
}

// ── Mkdir ─────────────────────────────────────────────────────────────────────

static int fat_mkdir_fn(const char* path)
{
    const char* local = fat_strip_mount(path);

    char parent_path[256], name[256];
    fat_split_path(local, parent_path, name);

    uint32_t dir_cluster = fat_dir_cluster(parent_path);
    uint32_t new_cluster = fat_alloc_cluster(0);
    if (!new_cluster) return -1;

    // Write . and .. entries into the new directory cluster
    static uint8_t new_dir_buf[4096];
    fat_memset(new_dir_buf, 0, fat_fs.bytes_per_cluster);
    fat_dirent_t* dot    = (fat_dirent_t*)new_dir_buf;
    fat_dirent_t* dotdot = dot + 1;

    fat_memset(dot->name, ' ', 8); dot->name[0] = '.';
    fat_memset(dot->ext, ' ', 3);
    dot->attr       = FAT_ATTR_DIRECTORY;
    dot->cluster_lo = (uint16_t)(new_cluster & 0xFFFF);
    dot->cluster_hi = (uint16_t)(new_cluster >> 16);

    fat_memset(dotdot->name, ' ', 8);
    dotdot->name[0] = '.'; dotdot->name[1] = '.';
    fat_memset(dotdot->ext, ' ', 3);
    dotdot->attr       = FAT_ATTR_DIRECTORY;
    dotdot->cluster_lo = (uint16_t)(dir_cluster & 0xFFFF);
    dotdot->cluster_hi = (uint16_t)(dir_cluster >> 16);

    fat_write_cluster(new_cluster, new_dir_buf);

    // Add dirent in parent
    fat_dirent_t new_de;
    fat_memset(&new_de, 0, sizeof(new_de));
    fat_make_83(name, new_de.name, new_de.ext);
    new_de.attr       = FAT_ATTR_DIRECTORY;
    new_de.cluster_lo = (uint16_t)(new_cluster & 0xFFFF);
    new_de.cluster_hi = (uint16_t)(new_cluster >> 16);

    static uint8_t pbuf[4096];
    uint32_t dc = dir_cluster;
    int written = 0;

    if (dc == 0 && fat_fs.type != FAT_TYPE_32) {
        for (uint32_t s = 0; s < fat_fs.root_sectors; s++) {
            fat_read_sector(fat_fs.root_start + s, pbuf);
            fat_dirent_t* entries = (fat_dirent_t*)pbuf;
            int per = fat_fs.bytes_per_sector / sizeof(fat_dirent_t);
            for (int i = 0; i < per; i++) {
                if (entries[i].name[0] == 0x00 || (uint8_t)entries[i].name[0] == 0xE5) {
                    entries[i] = new_de;
                    fat_write_sector(fat_fs.root_start + s, pbuf);
                    written = 1; break;
                }
            }
            if (written) break;
        }
    } else {
        while (dc && !fat_is_eoc(dc)) {
            fat_read_cluster(dc, pbuf);
            fat_dirent_t* entries = (fat_dirent_t*)pbuf;
            int per = (int)(fat_fs.bytes_per_cluster / sizeof(fat_dirent_t));
            for (int i = 0; i < per; i++) {
                if (entries[i].name[0] == 0x00 || (uint8_t)entries[i].name[0] == 0xE5) {
                    entries[i] = new_de;
                    fat_write_cluster(dc, pbuf);
                    written = 1; break;
                }
            }
            if (written) break;
            uint32_t next = fat_get_entry(dc);
            if (fat_is_eoc(next)) next = fat_alloc_cluster(dc);
            dc = next;
        }
    }

    return written ? 0 : -1;
}

// ── Remove ────────────────────────────────────────────────────────────────────

static int fat_remove_fn(const char* path)
{
    const char* local = fat_strip_mount(path);
    fat_dirent_t de;
    if (fat_lookup(local, &de) < 0) return -1;

    // Free cluster chain
    fat_free_chain(fat_dirent_cluster(&de));

    // Mark dirent as deleted (0xE5) in parent directory
    char parent_path[256], name[256];
    fat_split_path(local, parent_path, name);
    uint32_t dir_cluster = fat_dir_cluster(parent_path);

    static uint8_t rbuf[4096];
    uint32_t dc = dir_cluster;

    if (dc == 0 && fat_fs.type != FAT_TYPE_32) {
        for (uint32_t s = 0; s < fat_fs.root_sectors; s++) {
            fat_read_sector(fat_fs.root_start + s, rbuf);
            fat_dirent_t* entries = (fat_dirent_t*)rbuf;
            int per = fat_fs.bytes_per_sector / sizeof(fat_dirent_t);
            for (int i = 0; i < per; i++) {
                char short_name[13];
                fat_parse_83(&entries[i], short_name);
                if (!fat_strcmpi(short_name, name)) {
                    entries[i].name[0] = 0xE5;
                    fat_write_sector(fat_fs.root_start + s, rbuf);
                    return 0;
                }
            }
        }
    } else {
        while (dc && !fat_is_eoc(dc)) {
            fat_read_cluster(dc, rbuf);
            fat_dirent_t* entries = (fat_dirent_t*)rbuf;
            int per = (int)(fat_fs.bytes_per_cluster / sizeof(fat_dirent_t));
            for (int i = 0; i < per; i++) {
                char short_name[13];
                fat_parse_83(&entries[i], short_name);
                if (!fat_strcmpi(short_name, name)) {
                    entries[i].name[0] = 0xE5;
                    fat_write_cluster(dc, rbuf);
                    return 0;
                }
            }
            dc = fat_get_entry(dc);
        }
    }

    return -1;
}

// ── Driver registration ───────────────────────────────────────────────────────

static fs_driver_t fat_driver = {
    .name    = "fat",
    .mount   = fat_mount_fn,
    .open    = fat_open_fn,
    .close   = fat_close_fn,
    .read    = fat_read_fn,
    .write   = fat_write_fn,
    .readdir = fat_readdir_fn,
    .stat    = fat_stat_fn,
    .mkdir   = fat_mkdir_fn,
    .remove  = fat_remove_fn,
    .isdir   = fat_isdir_fn,
};

fs_driver_t* fat_init()
{
    return &fat_driver;
}
