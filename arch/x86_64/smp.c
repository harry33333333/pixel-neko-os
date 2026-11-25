#include "smp.h"
#include "limine.h"
#include "cpu.h"
#include "gdt.h"
#include "idt.h"
#include "apic.h"
#include "vmm.h"
#include "fpu.h"
#include "syscall.h"
#include "process.h"
#include "scheduler.h"
#include "log.h"
#include "string.h"

extern volatile struct limine_smp_request smp_request;

static volatile uint32_t g_smp_online_cpus = 1;
static uint32_t g_smp_total_cpus = 1;
static bool g_smp_x2apic = false;

uint32_t smp_get_online_cpus(void) {
    return __atomic_load_n(&g_smp_online_cpus, __ATOMIC_ACQUIRE);
}

uint32_t smp_get_cpu_count(void) {
    return g_smp_total_cpus;
}

per_cpu_t* smp_get_cpu(uint32_t cpu_id) {
    if (cpu_id >= SMP_MAX_CPUS) return NULL;
    return &g_per_cpus[cpu_id];
}

void smp_detect_this_cpu_topology(void) {
    per_cpu_t* cpu = this_cpu();
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;

    uint32_t max_basic = 0;
    cpuid(0, 0, &max_basic, &ebx, &ecx, &edx);

    uint32_t apic_id = cpu->lapic_id;
    uint32_t smt_shift = 0;
    uint32_t core_shift = 0;
    bool topo_found = false;

    /* Prefer CPUID Leaf 0x1F (V2 Extended Topology) over Leaf 0x0B */
    uint32_t topo_leaf = 0;
    if (max_basic >= 0x1F) {
        cpuid(0x1F, 0, &eax, &ebx, &ecx, &edx);
        if ((ebx & 0xFFFFu) != 0) {
            topo_leaf = 0x1F;
        }
    }
    if (topo_leaf == 0 && max_basic >= 0x0B) {
        cpuid(0x0B, 0, &eax, &ebx, &ecx, &edx);
        if ((ebx & 0xFFFFu) != 0) {
            topo_leaf = 0x0B;
        }
    }

    if (topo_leaf != 0) {
        for (uint32_t sub = 0; sub < 8; sub++) {
            cpuid(topo_leaf, sub, &eax, &ebx, &ecx, &edx);
            uint32_t level_type = (ecx >> 8) & 0xFFu;
            if (level_type == 0) break;
            uint32_t shift = eax & 0x1Fu;
            apic_id = edx;
            if (level_type == 1) {
                smt_shift = shift;
            } else if (level_type == 2) {
                core_shift = shift;
            }
            topo_found = true;
        }
    }

    if (topo_found) {
        if (core_shift < smt_shift) core_shift = smt_shift;
        uint32_t smt_mask = (smt_shift < 32) ? ((1u << smt_shift) - 1u) : 0u;
        uint32_t core_bits = core_shift - smt_shift;
        uint32_t core_mask = (core_bits < 32) ? ((1u << core_bits) - 1u) : 0xFFFFFFFFu;
        cpu->lapic_id   = apic_id;
        cpu->smt_id     = apic_id & smt_mask;
        cpu->core_id    = (apic_id >> smt_shift) & core_mask;
        cpu->package_id = (core_shift < 32) ? (apic_id >> core_shift) : 0u;
    } else {
        /* Fallback for older CPUs / hypervisors without 0x0B/0x1F */
        cpu->package_id = 0;
        cpu->core_id    = cpu->cpu_id;
        cpu->smt_id     = 0;
    }

    /* Detect Intel Hybrid P-Core / E-Core via Leaf 0x07 + Leaf 0x1A */
    cpu->core_type = CPU_CORE_TYPE_GENERIC;
    if (max_basic >= 0x1A) {
        cpuid(0x07, 0, &eax, &ebx, &ecx, &edx);
        if (edx & (1u << 15)) {
            cpuid(0x1A, 0, &eax, &ebx, &ecx, &edx);
            uint32_t ctype = (eax >> 24) & 0xFFu;
            if (ctype == 0x40u) {
                cpu->core_type = CPU_CORE_TYPE_PERF;
            } else if (ctype == 0x20u) {
                cpu->core_type = CPU_CORE_TYPE_EFF;
            }
        }
    }
}

void smp_get_topology(smp_topology_t* out) {
    if (!out) return;
    uint32_t online = smp_get_online_cpus();
    if (online == 0) online = 1;

    uint32_t phys_keys[SMP_MAX_CPUS];
    uint32_t phys_count = 0;
    uint32_t pkg_keys[SMP_MAX_CPUS];
    uint32_t pkg_count = 0;

    for (uint32_t i = 0; i < g_smp_total_cpus && i < SMP_MAX_CPUS; i++) {
        if (!g_per_cpus[i].online) continue;

        uint32_t pkey = (g_per_cpus[i].package_id << 16) | (g_per_cpus[i].core_id & 0xFFFFu);
        bool seen_phys = false;
        for (uint32_t j = 0; j < phys_count; j++) {
            if (phys_keys[j] == pkey) { seen_phys = true; break; }
        }
        if (!seen_phys && phys_count < SMP_MAX_CPUS) {
            phys_keys[phys_count++] = pkey;
        }

        uint32_t skey = g_per_cpus[i].package_id;
        bool seen_pkg = false;
        for (uint32_t j = 0; j < pkg_count; j++) {
            if (pkg_keys[j] == skey) { seen_pkg = true; break; }
        }
        if (!seen_pkg && pkg_count < SMP_MAX_CPUS) {
            pkg_keys[pkg_count++] = skey;
        }
    }

    if (phys_count == 0) phys_count = online;
    if (pkg_count == 0) pkg_count = 1;

    out->logical_cpus   = online;
    out->physical_cores = phys_count;
    out->packages       = pkg_count;
    out->smt_enabled    = (online > phys_count);
    out->x2apic_active  = g_smp_x2apic;
}

static void smp_ap_entry(struct limine_smp_info* info) {
    __asm__ volatile ("cli" : : : "memory");

    uint32_t cpu_id = (uint32_t)info->extra_argument;
    if (cpu_id == 0 || cpu_id >= SMP_MAX_CPUS) {
        for (;;) __asm__ volatile ("cli; hlt");
    }

    /* 1. Switch to kernel PML4 and configure PAT WC MSR on this AP */
    vmm_switch_pml4(vmm_get_kernel_pml4());
    vmm_init_pat_ap();

    /* 2. Load per-AP GDT + TSS + IST1/IST2 stacks */
    gdt_init_ap(cpu_id);

    /* 3. Load shared 64-bit IDT */
    idt_load_ap();

    /* 4. Initialize this AP's per-CPU GS_BASE & KERNEL_GS_BASE */
    per_cpu_init_ap(cpu_id, info->lapic_id);

    /* 5. Configure SYSCALL/SYSRET MSRs on this AP */
    syscall_init_ap();

    /* 6. Enable FPU/SSE2 and XSAVE/AVX2 matching BSP */
    fpu_init_ap();

    /* 7. Detect this AP's physical core / SMT / hybrid topology */
    smp_detect_this_cpu_topology();

    /* 8. Enable Local APIC and 1000 Hz LAPIC timer on this AP */
    apic_init_ap();

    /* 9. Mark AP online */
    this_cpu()->online = 1;
    __atomic_fetch_add(&g_smp_online_cpus, 1u, __ATOMIC_RELEASE);

    /* 10. Wait for BSP to finish initializing scheduler, then enter scheduling */
    while (!scheduler_is_ready()) {
        __asm__ volatile ("pause" : : : "memory");
    }

    scheduler_start_ap();
}

void smp_init(void) {
    g_smp_online_cpus = 1;
    g_smp_total_cpus = 1;
    g_smp_x2apic = apic_is_x2apic();

    /* Populate BSP lapic_id and topology first */
    g_per_cpus[0].lapic_id = lapic_get_id();
    g_per_cpus[0].online = 1;
    smp_detect_this_cpu_topology();

    if (!smp_request.response || smp_request.response->cpu_count <= 1) {
        log_printf("[SMP] Single logical processor detected (LAPIC ID %u, %s).\n",
                   g_per_cpus[0].lapic_id, g_smp_x2apic ? "x2APIC" : "xAPIC");
        return;
    }

    struct limine_smp_response* resp = smp_request.response;
    uint64_t raw_count = resp->cpu_count;
    uint32_t bsp_lapic = resp->bsp_lapic_id;
    g_per_cpus[0].lapic_id = bsp_lapic;

    log_printf("[SMP] Limine reported %llu logical processors (BSP LAPIC=%u, flags=0x%x, %s).\n",
               (unsigned long long)raw_count, bsp_lapic, resp->flags,
               g_smp_x2apic ? "x2APIC" : "xAPIC");

    uint32_t next_id = 1;
    for (uint64_t i = 0; i < raw_count && next_id < SMP_MAX_CPUS; i++) {
        struct limine_smp_info* ci = resp->cpus[i];
        if (!ci || ci->lapic_id == bsp_lapic) continue;

        uint32_t cid = next_id++;
        g_per_cpus[cid].cpu_id = cid;
        g_per_cpus[cid].lapic_id = ci->lapic_id;

        /* Pre-allocate per-AP idle thread before waking the AP */
        if (!process_create_ap_idle(cid)) {
            log_printf("[SMP WARN] Failed to create idle thread for CPU %u (LAPIC %u); skipping.\n",
                       cid, ci->lapic_id);
            continue;
        }

        ci->extra_argument = (uint64_t)cid;
        __atomic_store_n(&ci->goto_address, smp_ap_entry, __ATOMIC_RELEASE);
    }

    g_smp_total_cpus = next_id;

    /* Wait up to ~200 ms for APs to come online */
    uint64_t start_ms = apic_get_ticks_ms();
    uint32_t spins = 0;
    while (smp_get_online_cpus() < g_smp_total_cpus) {
        if (apic_get_ticks_ms() - start_ms >= 200ULL) break;
        if (++spins > 50000000u) break;
        __asm__ volatile ("pause" : : : "memory");
    }

    smp_topology_t topo;
    smp_get_topology(&topo);
    log_printf("[SMP] Brought online %u / %u logical processors (%u physical cores, %u package(s), SMT=%s).\n",
               topo.logical_cpus, g_smp_total_cpus, topo.physical_cores,
               topo.packages, topo.smt_enabled ? "ON" : "OFF");
}
