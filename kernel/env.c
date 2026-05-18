// kernel/env.c — Global environment variable store.
// Flat shared table: all processes read/write the same vars.
// Simple, safe, no per-process complexity, no inheritance needed.
#include <stdint.h>
#include "../include/env.h"

#define MAX_ENV   32
#define KEY_MAX   32
#define VAL_MAX   128

static char g_keys[MAX_ENV][KEY_MAX];   // 1024 bytes BSS
static char g_vals[MAX_ENV][VAL_MAX];   // 4096 bytes BSS
static int  g_count = 0;               //    4 bytes BSS
// Total: ~5KB — safe

static int keq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    return a[i] == 0 && b[i] == 0;
}

static void kcopy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

int env_get(const char* name, char* buf, uint32_t maxl) {
    if (!name || !buf || !maxl) return -1;
    for (int i = 0; i < g_count; i++) {
        if (keq(g_keys[i], name)) {
            kcopy(buf, g_vals[i], (int)maxl);
            int n = 0; while (g_vals[i][n]) n++;
            return n < (int)maxl ? n : (int)maxl - 1;
        }
    }
    return -1;
}

int env_set(const char* name, const char* val) {
    if (!name) return -1;
    // Update existing
    for (int i = 0; i < g_count; i++) {
        if (keq(g_keys[i], name)) {
            if (!val || !val[0]) {
                // Unset: shift table down
                for (int j = i; j < g_count - 1; j++) {
                    kcopy(g_keys[j], g_keys[j+1], KEY_MAX);
                    kcopy(g_vals[j], g_vals[j+1], VAL_MAX);
                }
                g_count--;
            } else {
                kcopy(g_vals[i], val, VAL_MAX);
            }
            return 0;
        }
    }
    // Add new
    if (!val || !val[0]) return 0;
    if (g_count >= MAX_ENV) return -1;
    kcopy(g_keys[g_count], name, KEY_MAX);
    kcopy(g_vals[g_count], val,  VAL_MAX);
    g_count++;
    return 0;
}
