#include "../tox.h"

static int str_match(const char* name, const char* pattern) {
    // Simple wildcard matching — supports * at start, end, or both
    // e.g. "*.elf", "ls*", "*ls*", or exact "ls.elf"
    int plen = tox_strlen(pattern);
    int nlen = tox_strlen(name);

    if (pattern[0] == '*' && pattern[plen-1] == '*') {
        // *substring* — check if name contains middle part
        char mid[128]; int mi = 0;
        for (int i = 1; i < plen-1; i++) mid[mi++] = pattern[i];
        mid[mi] = 0;
        int mlen = tox_strlen(mid);
        for (int i = 0; i <= nlen - mlen; i++) {
            int match = 1;
            for (int j = 0; j < mlen; j++)
                if (name[i+j] != mid[j]) { match = 0; break; }
            if (match) return 1;
        }
        return 0;
    } else if (pattern[0] == '*') {
        // *suffix — check end of name
        char* suffix = (char*)pattern + 1;
        int slen = tox_strlen(suffix);
        if (slen > nlen) return 0;
        for (int i = 0; i < slen; i++)
            if (name[nlen - slen + i] != suffix[i]) return 0;
        return 1;
    } else if (pattern[plen-1] == '*') {
        // prefix* — check start of name
        for (int i = 0; i < plen-1; i++)
            if (name[i] != pattern[i]) return 0;
        return 1;
    } else {
        // exact match
        if (nlen != plen) return 0;
        for (int i = 0; i < nlen; i++)
            if (name[i] != pattern[i]) return 0;
        return 1;
    }
}

static void do_find(const char* dir, const char* pattern, int* found) {
    char entry[256];
    uint32_t i = 0;

    while (tox_readdir(dir, entry, i) == 0) {
        char full[512];
        int l = 0;
        for (; dir[l]; l++) full[l] = dir[l];
        full[l++] = '/';
        for (int k = 0; entry[k]; k++) full[l++] = entry[k];
        full[l] = 0;

        if (tox_isdir(full) == 1) {
            // Recurse into directory
            if (str_match(entry, pattern)) {
                set_color(0x09); print(full); print("/\n");
                set_color(0x07);
                (*found)++;
            }
            do_find(full, pattern, found);
        } else {
            if (str_match(entry, pattern)) {
                set_color(0x07); print(full); print("\n");
                (*found)++;
            }
        }
        i++;
    }
}

void _start() {
    char args[512];
    get_args(args);

    // Parse: find <dir> <pattern>  or  find <pattern> (searches /C:)
    char dir[256], pattern[128];
    int i = 0, j = 0;

    // First arg
    while (args[i] && args[i] != ' ') dir[j++] = args[i++];
    dir[j] = 0;
    while (args[i] == ' ') i++;

    if (args[i]) {
        // Second arg — dir was first, pattern is second
        j = 0;
        while (args[i]) pattern[j++] = args[i++];
        pattern[j] = 0;
    } else {
        // Only one arg — it's the pattern, search from /C:
        tox_strcpy(pattern, dir);
        tox_strcpy(dir, "/C:");
    }

    if (!pattern[0]) {
        set_color(0x0C); print("usage: find [dir] <pattern>\n");
        set_color(0x07); tox_exit();
    }

    if (tox_isdir(dir) < 0) {
        set_color(0x0C); print("find: no such directory: "); print(dir); print("\n");
        set_color(0x07); tox_exit();
    }

    int found = 0;
    do_find(dir, pattern, &found);

    if (!found) {
        set_color(0x08); print("no matches found\n"); set_color(0x07);
    }

    tox_exit();
}
