// ToxenOS/user64/exec_test.c — Milestone 6 ring3 exec test program.
// Real compiled code, not a hardcoded stub: does a real loop/string-
// build, then proves it via sys_write before sys_exit -- the point is
// to show the kernel genuinely loaded and ran this binary, not just
// that more bytes sit in a page.
#include <stdint.h>
#include "tox64.h"

static int my_strlen(const char* s) {
    int i = 0;
    while (s[i]) i++;
    return i;
}

static int itoa10(int v, char* out) {
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    int j = 0;
    while (n > 0) out[j++] = tmp[--n];
    out[j] = 0;
    return j;
}

void _start(void) {
    int sum = 0;
    for (int i = 1; i <= 10; i++) sum += i;

    char msg[64] = "hello from a real ELF64 program, sum(1..10)=";
    int prefix_len = my_strlen(msg);
    itoa10(sum, msg + prefix_len);
    int total_len = my_strlen(msg);
    msg[total_len]     = '\n';
    msg[total_len + 1] = 0;

    sys_write(msg, (uint64_t)my_strlen(msg));
    sys_exit(0);

    for (;;) { }  // unreachable
}
