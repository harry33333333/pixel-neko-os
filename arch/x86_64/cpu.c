#include "cpu.h"
#include "log.h"
#include <stddef.h>

cpu_info_t g_cpu_info = {0};
char cpu_model_string[49] = "Unknown AMD64 CPU";

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

    // 3. Extended Leaf 0x80000000: Max Extended Leaf
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

    // 3. Extended Leaf 0x80000000: Max Extended Leaf
    cpuid(0x80000000, 0, &eax, &ebx, &ecx, &edx);
    g_cpu_info.max_ext_leaf = eax;

    // 4. Extended Leaf 0x80000001: 64-bit Extended Features
    if (g_cpu_info.max_ext_leaf >= 0x80000001) {
        cpuid(0x80000001, 0, &eax, &ebx, &ecx, &edx);
        g_cpu_info.ext_ecx = ecx;
        g_cpu_info.ext_edx = edx;

        g_cpu_info.has_syscall = (edx & CPUID_EXT_SYSCALL) != 0;
        g_cpu_info.has_nx      = (edx & CPUID_EXT_NX) != 0;
        g_cpu_info.has_lm      = (edx & CPUID_EXT_LM) != 0;
    }

    // 5. Extended Leaves 0x80000002 ~ 0x80000004: Brand String
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
        
        // Clean leading spaces in brand string
        char* p = g_cpu_info.brand;
        while (*p == ' ') p++;
        for (int i = 0; i < 48; i++) {
            cpu_model_string[i] = p[i];
            if (p[i] == '\0') break;
        }
        cpu_model_string[48] = '\0';
    } else {
        for (int i = 0; i < 12; i++) cpu_model_string[i] = g_cpu_info.vendor[i];
        cpu_model_string[12] = '\0';
    }

    log_printf("[CPU] Vendor: %s, Model: %s\n", g_cpu_info.vendor, cpu_model_string);
    log_printf("[CPU] Family: %u, Model: %u, Stepping: %u\n",
               g_cpu_info.family, g_cpu_info.model, g_cpu_info.stepping);
    log_printf("[CPU] Features: LM=%d, NX=%d, APIC=%d, SYSCALL=%d, SSE2=%d, SSE3=%d, CX16=%d\n",
               g_cpu_info.has_lm, g_cpu_info.has_nx, g_cpu_info.has_apic,
               g_cpu_info.has_syscall, g_cpu_info.has_sse2, g_cpu_info.has_sse3, g_cpu_info.has_cx16);

    // 6. Verify Mandatory Admission Features (Section 1.3)
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
