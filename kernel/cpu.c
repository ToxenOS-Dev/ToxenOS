// ToxenOS/kernel/cpu.c — CPU detection via CPUID
#include <stdint.h>
#include "../include/klog.h"

static void cpuid(uint32_t leaf, uint32_t* eax, uint32_t* ebx,
                  uint32_t* ecx, uint32_t* edx) {
    __asm__ volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(0));
}

static void u32_to_str(uint32_t v, char* out) {
    out[0]=(char)(v      ); out[1]=(char)(v>> 8);
    out[2]=(char)(v>>16);  out[3]=(char)(v>>24);
}

void cpu_detect(void) {
    uint32_t eax, ebx, ecx, edx;

    // ── Vendor string ──────────────────────────────────────────────────────────
    cpuid(0, &eax, &ebx, &ecx, &edx);
    uint32_t max_leaf = eax;
    char vendor[13]; vendor[12] = 0;
    u32_to_str(ebx, vendor+0);
    u32_to_str(edx, vendor+4);
    u32_to_str(ecx, vendor+8);

    klog("CPU: "); klog(vendor); klog("\n");

    // ── Brand string (leaves 0x80000002-4) ────────────────────────────────────
    cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
    if (eax >= 0x80000004) {
        char brand[49]; brand[48] = 0;
        uint32_t* p = (uint32_t*)brand;
        cpuid(0x80000002, &p[0],&p[1],&p[2],&p[3]);
        cpuid(0x80000003, &p[4],&p[5],&p[6],&p[7]);
        cpuid(0x80000004, &p[8],&p[9],&p[10],&p[11]);
        // Trim leading spaces
        const char* b = brand;
        while (*b == ' ') b++;
        klog("CPU: "); klog(b); klog("\n");
    }

    // ── Family/Model/Stepping ──────────────────────────────────────────────────
    if (max_leaf >= 1) {
        cpuid(1, &eax, &ebx, &ecx, &edx);
        uint32_t stepping = eax & 0xF;
        uint32_t model    = (eax >> 4) & 0xF;
        uint32_t family   = (eax >> 8) & 0xF;
        // Extended model/family
        if (family == 0xF) family  += (eax >> 20) & 0xFF;
        if (family >= 0x6) model   += ((eax >> 16) & 0xF) << 4;

        klog("CPU: family=");
        char tmp[4]; tmp[3]=0;
        tmp[0]='0'+((family/10)%10); tmp[1]='0'+(family%10); tmp[2]=' '; klog(tmp);
        klog("model=");
        tmp[0]='0'+((model/10)%10); tmp[1]='0'+(model%10); tmp[2]=' '; klog(tmp);
        klog("step=");
        tmp[0]='0'+(stepping%10); tmp[1]='\n'; tmp[2]=0; klog(tmp);

        // ── Features ──────────────────────────────────────────────────────────
        klog("CPU: features:");
        if (edx & (1u<<0))  klog(" FPU");
        if (edx & (1u<<4))  klog(" TSC");
        if (edx & (1u<<15)) klog(" CMOV");
        if (edx & (1u<<23)) klog(" MMX");
        if (edx & (1u<<25)) klog(" SSE");
        if (edx & (1u<<26)) klog(" SSE2");
        if (ecx & (1u<<0))  klog(" SSE3");
        if (ecx & (1u<<9))  klog(" SSSE3");
        if (ecx & (1u<<19)) klog(" SSE4.1");
        if (ecx & (1u<<20)) klog(" SSE4.2");
        if (ecx & (1u<<28)) klog(" AVX");
        if (ecx & (1u<<5))  klog(" VMX");
        klog("\n");

        // ── Core count (logical) ───────────────────────────────────────────────
        uint32_t logical = (ebx >> 16) & 0xFF;
        if (logical > 0) {
            klog("CPU: logical cores=");
            tmp[0] = '0' + (char)(logical % 10);
            tmp[1] = '\n'; tmp[2] = 0; klog(tmp);
        }
    }

    // ── Physical address bits ──────────────────────────────────────────────────
    cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
    if (eax >= 0x80000008) {
        cpuid(0x80000008, &eax, &ebx, &ecx, &edx);
        uint32_t phys_bits = eax & 0xFF;
        uint32_t virt_bits = (eax >> 8) & 0xFF;
        klog("CPU: phys_bits=");
        char tmp[4]; tmp[3]=0;
        tmp[0]='0'+((phys_bits/10)%10); tmp[1]='0'+(phys_bits%10); tmp[2]=' '; klog(tmp);
        klog("virt_bits=");
        tmp[0]='0'+((virt_bits/10)%10); tmp[1]='0'+(virt_bits%10); tmp[2]='\n'; klog(tmp);
    }
}
