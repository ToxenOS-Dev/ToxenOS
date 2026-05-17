// kernel/env.c — Per-process environment variable store.
// Kept separate from process_t to avoid inflating the static processes[] BSS.
#include <stdint.h>
#include "../include/env.h"
#include "../include/process.h"

#define ENV_TOTAL (MAX_PROCESSES * MAX_ENV_VARS)

static struct {
    uint32_t pid;
    char     key[ENV_KEY_MAX];
    char     val[ENV_VAL_MAX];
    int      used;
} env_table[ENV_TOTAL];

static int keycmp(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    return a[i] == b[i] ? 1 : 0;
}

static void kstrcpy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

int env_get(uint32_t pid, const char* name, char* buf, uint32_t maxl) {
    if (!name || !buf || !maxl) return -1;
    for (int i = 0; i < ENV_TOTAL; i++) {
        if (!env_table[i].used || env_table[i].pid != pid) continue;
        if (keycmp(env_table[i].key, name)) {
            kstrcpy(buf, env_table[i].val, (int)maxl);
            int n = 0; while (env_table[i].val[n]) n++;
            return n < (int)maxl ? n : (int)maxl - 1;
        }
    }
    return -1;
}

int env_set(uint32_t pid, const char* name, const char* val) {
    if (!name) return -1;
    // Find existing slot for this pid+key, or first free slot
    int slot = -1, free_slot = -1;
    for (int i = 0; i < ENV_TOTAL; i++) {
        if (!env_table[i].used) { if (free_slot < 0) free_slot = i; continue; }
        if (env_table[i].pid == pid && keycmp(env_table[i].key, name)) { slot = i; break; }
    }
    if (slot < 0) slot = free_slot;
    if (slot < 0) return -1;

    // Setting to empty string or null unsets the variable
    if (!val || !val[0]) { env_table[slot].used = 0; return 0; }

    env_table[slot].pid  = pid;
    env_table[slot].used = 1;
    kstrcpy(env_table[slot].key, name, ENV_KEY_MAX);
    kstrcpy(env_table[slot].val, val,  ENV_VAL_MAX);
    return 0;
}

