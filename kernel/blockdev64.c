// kernel/blockdev64.c — Milestone 28: generic block-device registry.
// See include/blockdev64.h for the design rationale.
#include <stdint.h>
#include "../include/blockdev64.h"
#include "../include/heap64.h"
#include "../include/klog.h"

static blockdev64_t* g_head = 0;

static int str_eq_local(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

int blockdev64_register(blockdev64_t* dev) {
    if (blockdev64_find(dev->name)) {
        klog("blockdev64: register: duplicate name '");
        klog(dev->name);
        klog("' -- refused\n");
        return -1;
    }
    dev->next = g_head;
    g_head = dev;

    klog("blockdev64: registered '");
    klog(dev->name);
    klog("'\n");
    return 0;
}

blockdev64_t* blockdev64_find(const char* name) {
    for (blockdev64_t* d = g_head; d; d = d->next) {
        if (str_eq_local(d->name, name)) return d;
    }
    return 0;
}

blockdev64_t* blockdev64_iter(blockdev64_t* prev) {
    return prev ? prev->next : g_head;
}

int blockdev64_count(void) {
    int n = 0;
    for (blockdev64_t* d = g_head; d; d = d->next) n++;
    return n;
}

static const char* type_name(blockdev64_type_t t) {
    switch (t) {
    case BLOCKDEV64_TYPE_ATA:    return "ATA/PIO";
    case BLOCKDEV64_TYPE_AHCI:   return "AHCI";
    case BLOCKDEV64_TYPE_NVME:   return "NVMe";
    case BLOCKDEV64_TYPE_VIRTIO: return "VirtIO-blk";
    }
    return "?";
}

static void dec_to_str_local(uint64_t val, char* out) {
    char tmp[24];
    int n = 0;
    if (val == 0) tmp[n++] = '0';
    while (val > 0 && n < 24) { tmp[n++] = (char)('0' + (val % 10)); val /= 10; }
    int j = 0;
    while (n > 0) out[j++] = tmp[--n];
    out[j] = 0;
}

void blockdev64_dump(void) {
    klog("blockdev64: dump ---\n");
    for (blockdev64_t* d = g_head; d; d = d->next) {
        char buf[24];
        klog("  "); klog(d->name);
        klog(" type="); klog(type_name(d->type));
        klog(" logical_block_size="); dec_to_str_local(d->logical_block_size, buf); klog(buf);
        klog(" block_count(512B)="); dec_to_str_local(d->block_count, buf); klog(buf);
        klog("\n");
    }
    klog("blockdev64: dump end ---\n");
}
