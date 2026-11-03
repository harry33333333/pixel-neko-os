#include "cpu.h"
#include "log.h"
#include "port.h"
#include <stddef.h>

cpu_info_t g_cpu_info = {0};
char cpu_model_string[49] = "Unknown AMD64 CPU";

static inline uint64_t cpu_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void clean_brand_string(char* str) {
    char* src = str;
    char* dst = str;
    while (*src == ' ') src++;
    int prev_space = 0;
    while (*src) {
        if (*src == ' ') {
            if (!prev_space) {
                *dst++ = ' ';
                prev_space = 1;
            }
        } else {
            *dst++ = *src;
            prev_space = 0;
        }
        src++;
    }
    if (dst > str && *(dst - 1) == ' ') dst--;
    *dst = '\0';
}

static uint32_t parse_brand_mhz(const char* brand) {
    if (!brand) return 0;
    for (int i = 0; brand[i]; i++) {
        if (brand[i] == '@' && brand[i + 1] == ' ') {
            const char* p = &brand[i + 2];
            while (*p == ' ') p++;
            uint32_t whole = 0;
            uint32_t frac = 0;
            uint32_t frac_div = 1;
            while (*p >= '0' && *p <= '9') {
                whole = whole * 10 + (uint32_t)(*p - '0');
                p++;
            }
            if (*p == '.') {
                p++;
                while (*p >= '0' && *p <= '9') {
                    frac = frac * 10 + (uint32_t)(*p - '0');
                    frac_div *= 10;
                    p++;
                }
            }
            while (*p == ' ') p++;
            if (p[0] == 'G' && p[1] == 'H' && (p[2] == 'z' || p[2] == 'Z')) {
                return whole * 1000u + (frac * 1000u) / frac_div;
            }
            if (p[0] == 'M' && p[1] == 'H' && (p[2] == 'z' || p[2] == 'Z')) {
                return whole;
            }
        }
    }
    return 0;
}

static uint32_t measure_cpu_mhz(void) {
    // 1. Calibrate TSC over a 10ms window using PIT Channel 2 (11932 ticks @ 1.193182 MHz)
    uint8_t gate = inb(0x61);
    outb(0x61, (uint8_t)(gate & 0xFCu)); // Disable Gate 2 & Speaker first
    outb(0x43, 0xB0);                    // Channel 2, lobyte/hibyte, Mode 0 (one-shot), binary
    outb(0x42, (uint8_t)(11932 & 0xFF));
    outb(0x42, (uint8_t)((11932 >> 8) & 0xFF));
    outb(0x61, (uint8_t)((gate & 0xFDu) | 0x01u)); // Rising edge on Gate 2 starts countdown

    uint64_t start_tsc = cpu_rdtsc();
    uint32_t timeout = 50000000u;
    while (!(inb(0x61) & 0x20u) && --timeout) {
        __asm__ volatile ("pause");
    }
    uint64_t end_tsc = cpu_rdtsc();
    outb(0x61, gate);

    if (timeout > 0 && end_tsc > start_tsc) {
        uint64_t delta = end_tsc - start_tsc;
        uint32_t mhz = (uint32_t)(delta / 10000ULL);
        if (mhz >= 50 && mhz <= 10000) {
            return mhz;
        }
    }

    // 2. Fallback: PIT Channel 0 counter latch (~5ms window = 5966 ticks)
    outb(0x43, 0x34); // Channel 0, lobyte/hibyte, rate generator
    outb(0x40, 0xFF);
    outb(0x40, 0xFF);
    outb(0x43, 0x00);
    uint16_t start_cnt = (uint16_t)inb(0x40);
    start_cnt |= (uint16_t)inb(0x40) << 8;
    start_tsc = cpu_rdtsc();

    uint32_t elapsed = 0;
    uint16_t prev_cnt = start_cnt;
    timeout = 20000000u;
    while (elapsed < 5966u && --timeout) {
        outb(0x43, 0x00);
        uint16_t cur_cnt = (uint16_t)inb(0x40);
        cur_cnt |= (uint16_t)inb(0x40) << 8;
        if (cur_cnt <= prev_cnt) {
            elapsed += (uint32_t)(prev_cnt - cur_cnt);
        } else {
            elapsed += (uint32_t)(0x10000u - cur_cnt + prev_cnt);
        }
        prev_cnt = cur_cnt;
    }
    end_tsc = cpu_rdtsc();
    if (elapsed >= 5966u && end_tsc > start_tsc) {
        uint64_t delta = end_tsc - start_tsc;
        uint32_t mhz = (uint32_t)((delta * 1193182ULL) / ((uint64_t)elapsed * 1000000ULL));
        if (mhz >= 50 && mhz <= 10000) {
            return mhz;
        }
    }

    return 0;
}

bool cpu_detect_and_verify(void) {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;

    // 1. Basic Leaf 0: Vendor String & Max Basic Leaf
    cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    g_cpu_info.max_basic_leaf = eax;

    // Vendor is EBX, EDX, ECX
    ((uint32_t*)g_cpu_info.vendor)[0] = ebx;
    ((uint32_t*)g_cpu_info.vendor)[1] = edx;
    ((uint32_t*)g_cpu_info.vendor)[2] = ecx;
    g_cpu_info.vendor[12] = '\0';

    // 2. Basic Leaf 1: Family, Model, Stepping, Basic Features
    if (g_cpu_info.max_basic_leaf >= 1) {
        cpuid(1, 0, &eax, &ebx, &ecx, &edx);
        g_cpu_info.stepping = eax & 0x0F;
        g_cpu_info.model = (eax >> 4) & 0x0F;
        g_cpu_info.family = (eax >> 8) & 0x0F;
        if (g_cpu_info.family == 0x0F) {
            g_cpu_info.family += (eax >> 20) & 0xFF;
        }
        if (g_cpu_info.family == 0x06 || g_cpu_info.family == 0x0F) {
            g_cpu_info.model += ((eax >> 16) & 0x0F) << 4;
        }
        g_cpu_info.basic_ecx = ecx;
        g_cpu_info.basic_edx = edx;

        g_cpu_info.has_apic = (edx & CPUID_FEAT_APIC) != 0;
        g_cpu_info.has_sse2 = (edx & CPUID_FEAT_SSE2) != 0;
        g_cpu_info.has_sse3 = (ecx & CPUID_FEAT_SSE3) != 0;
        g_cpu_info.has_cx16 = (ecx & CPUID_FEAT_CX16) != 0;
    }

    // 3. Frequency Leaf 0x16 / 0x15
    if (g_cpu_info.max_basic_leaf >= 0x16) {
        cpuid(0x16, 0, &eax, &ebx, &ecx, &edx);
        g_cpu_info.mhz = eax & 0xFFFF;
    }
    if (!g_cpu_info.mhz && g_cpu_info.max_basic_leaf >= 0x15) {
        cpuid(0x15, 0, &eax, &ebx, &ecx, &edx);
        if (eax && ebx && ecx) {
            g_cpu_info.mhz = (uint32_t)((uint64_t)ecx * ebx / eax / 1000000u);
        }
    }

    // 4. Extended Leaf 0x80000000: Max Extended Leaf
    cpuid(0x80000000, 0, &eax, &ebx, &ecx, &edx);
    g_cpu_info.max_ext_leaf = eax;

    // 5. Extended Leaf 0x80000001: 64-bit Extended Features
    if (g_cpu_info.max_ext_leaf >= 0x80000001) {
        cpuid(0x80000001, 0, &eax, &ebx, &ecx, &edx);
        g_cpu_info.ext_ecx = ecx;
        g_cpu_info.ext_edx = edx;

        g_cpu_info.has_syscall = (edx & CPUID_EXT_SYSCALL) != 0;
        g_cpu_info.has_nx      = (edx & CPUID_EXT_NX) != 0;
        g_cpu_info.has_lm      = (edx & CPUID_EXT_LM) != 0;
    }

    // 6. Extended Leaves 0x80000002 ~ 0x80000004: Brand String
    if (g_cpu_info.max_ext_leaf >= 0x80000004) {
        uint32_t* brand_ptr = (uint32_t*)g_cpu_info.brand;
        for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
            cpuid(leaf, 0, &eax, &ebx, &ecx, &edx);
            *brand_ptr++ = eax;
            *brand_ptr++ = ebx;
            *brand_ptr++ = ecx;
            *brand_ptr++ = edx;
        }
        g_cpu_info.brand[48] = '\0';
        for (int i = 0; i < 48; i++) {
            cpu_model_string[i] = g_cpu_info.brand[i];
            if (g_cpu_info.brand[i] == '\0') break;
        }
        cpu_model_string[48] = '\0';
        clean_brand_string(cpu_model_string);
    } else {
        for (int i = 0; i < 12; i++) cpu_model_string[i] = g_cpu_info.vendor[i];
        cpu_model_string[12] = '\0';
    }

    // 7. Measure CPU MHz via PIT hardware calibration or brand string if CPUID 0x16/0x15 unavailable
    if (g_cpu_info.mhz == 0) {
        g_cpu_info.mhz = measure_cpu_mhz();
    }
    if (g_cpu_info.mhz == 0) {
        g_cpu_info.mhz = parse_brand_mhz(cpu_model_string);
    }

    log_printf("[CPU] Vendor: %s, Model: %s\n", g_cpu_info.vendor, cpu_model_string);
    log_printf("[CPU] Family: %u, Model: %u, Stepping: %u, Freq: %u MHz\n",
               g_cpu_info.family, g_cpu_info.model, g_cpu_info.stepping, g_cpu_info.mhz);
    log_printf("[CPU] Features: LM=%d, NX=%d, APIC=%d, SYSCALL=%d, SSE2=%d, SSE3=%d, CX16=%d\n",
               g_cpu_info.has_lm, g_cpu_info.has_nx, g_cpu_info.has_apic,
               g_cpu_info.has_syscall, g_cpu_info.has_sse2, g_cpu_info.has_sse3, g_cpu_info.has_cx16);

    // 8. Verify Mandatory Admission Features (Section 1.3)
    bool pass = true;
    if (!g_cpu_info.has_lm) {
        log_printf("[CPU CRITICAL] CPU does not support 64-bit Long Mode (LM)!\n");
        pass = false;
    }
    if (!g_cpu_info.has_nx) {
        log_printf("[CPU CRITICAL] CPU does not support No-Execute bit (NX)!\n");
        pass = false;
    }
    if (!g_cpu_info.has_apic) {
        log_printf("[CPU CRITICAL] CPU does not have on-chip Local APIC!\n");
        pass = false;
    }
    if (!g_cpu_info.has_syscall) {
        log_printf("[CPU CRITICAL] CPU does not support fast SYSCALL/SYSRET (0x80000001:EDX[11])!\n");
        pass = false;
    }
    if (!g_cpu_info.has_sse2) {
        log_printf("[CPU CRITICAL] CPU does not support SSE2!\n");
        pass = false;
    }

    return pass;
}
