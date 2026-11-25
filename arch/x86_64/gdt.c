#include "gdt.h"
#include "percpu.h"
#include "pmm.h"
#include "heap.h"
#include "mm.h"
#include "log.h"
#include "string.h"

// Dedicated 16KB stacks for BSP Double Fault (#DF) and NMI handlers
__attribute__((aligned(16)))
static uint8_t df_ist1_stack[16384];
__attribute__((aligned(16)))
static uint8_t nmi_ist2_stack[16384];

// BSP TSS and GDT
static tss64_t g_tss = {0};
static uint64_t g_gdt[7] = {0};
static gdt_ptr_t g_gdt_ptr;

typedef struct {
    uint64_t  gdt[7];
    tss64_t   tss;
    gdt_ptr_t ptr;
} __attribute__((aligned(16))) ap_gdt_ctx_t;

extern void gdt_flush(gdt_ptr_t* ptr);
extern void tss_flush(uint16_t sel);

static void set_gdt_gate_in(uint64_t* table, int num, uint64_t base, uint64_t limit, uint8_t access, uint8_t gran) {
    uint64_t descriptor = 0;
    descriptor |= (limit & 0xFFFFULL);
    descriptor |= (base & 0xFFFFFFULL) << 16;
    descriptor |= ((uint64_t)access) << 40;
    descriptor |= ((uint64_t)(limit >> 16) & 0x0FULL) << 48;
    descriptor |= ((uint64_t)(gran & 0xF0ULL)) << 48;
    descriptor |= ((uint64_t)(base >> 24) & 0xFFULL) << 56;
    table[num] = descriptor;
}

static void set_tss_gate_in(uint64_t* table, int num, uint64_t base, uint32_t limit) {
    set_gdt_gate_in(table, num, base & 0xFFFFFFFFULL, limit, 0x89, 0x00);
    table[num + 1] = (base >> 32) & 0xFFFFFFFFULL;
}

static void init_gdt_table(uint64_t* table, tss64_t* tss, gdt_ptr_t* ptr,
                           uint64_t ist1_top, uint64_t ist2_top) {
    memset(tss, 0, sizeof(tss64_t));
    tss->ist1 = ist1_top & ~0xFULL;
    tss->ist2 = ist2_top & ~0xFULL;
    tss->iomap_base = sizeof(tss64_t);

    table[0] = 0;
    set_gdt_gate_in(table, 1, 0, 0xFFFFF, 0x9A, 0xA0); // 0x08 Kernel Code 64
    set_gdt_gate_in(table, 2, 0, 0xFFFFF, 0x92, 0xC0); // 0x10 Kernel Data 64
    set_gdt_gate_in(table, 3, 0, 0xFFFFF, 0xF2, 0xC0); // 0x1B User Data 64
    set_gdt_gate_in(table, 4, 0, 0xFFFFF, 0xFA, 0xA0); // 0x23 User Code 64
    set_tss_gate_in(table, 5, (uint64_t)tss, sizeof(tss64_t) - 1);

    ptr->limit = (uint16_t)(7 * sizeof(uint64_t) - 1);
    ptr->base = (uint64_t)table;
}

void tss_set_rsp0(uint64_t rsp0) {
    per_cpu_t* cpu = this_cpu();
    tss64_t* tss = (cpu && cpu->tss_base) ? (tss64_t*)cpu->tss_base : &g_tss;
    tss->rsp0 = rsp0;
}

void gdt_init(void) {
    uint64_t ist1_top = (uint64_t)df_ist1_stack + sizeof(df_ist1_stack);
    uint64_t ist2_top = (uint64_t)nmi_ist2_stack + sizeof(nmi_ist2_stack);
    init_gdt_table(g_gdt, &g_tss, &g_gdt_ptr, ist1_top, ist2_top);

    g_per_cpus[0].tss_base = (uint64_t)&g_tss;

    gdt_flush(&g_gdt_ptr);
    tss_flush(GDT_TSS_SEL);

    log_printf("[GDT] 64-bit GDT installed (CS=0x%02x, DS=0x%02x, TSS=0x%02x).\n",
               GDT_KERNEL_CODE_SEL, GDT_KERNEL_DATA_SEL, GDT_TSS_SEL);
    log_printf("[GDT] Ring 3 descriptors: UDS=0x%016llx UCS=0x%016llx\n",
               (unsigned long long)g_gdt[GDT_USER_DATA_SEL / 8],
               (unsigned long long)g_gdt[GDT_USER_CODE_SEL / 8]);
    log_printf("[GDT] TSS active. Dedicated Double Fault IST1 Stack @ 0x%016llx\n", ist1_top & ~0xFULL);
    log_printf("[GDT] Paranoid NMI IST2 Stack @ 0x%016llx\n", ist2_top & ~0xFULL);
}

void gdt_init_ap(uint32_t cpu_id) {
    if (cpu_id == 0 || cpu_id >= SMP_MAX_CPUS) return;

    ap_gdt_ctx_t* ctx = (ap_gdt_ctx_t*)kzalloc(sizeof(ap_gdt_ctx_t));
    phys_addr_t ist1_phys = pmm_alloc_pages(4); /* 16 KB */
    phys_addr_t ist2_phys = pmm_alloc_pages(4); /* 16 KB */
    if (!ctx || !ist1_phys || !ist2_phys) {
        /* Fallback to shared stacks if OOM (should never happen) */
        gdt_flush(&g_gdt_ptr);
        return;
    }

    uint64_t ist1_top = (uint64_t)phys_to_virt(ist1_phys) + 4ULL * PAGE_SIZE;
    uint64_t ist2_top = (uint64_t)phys_to_virt(ist2_phys) + 4ULL * PAGE_SIZE;
    init_gdt_table(ctx->gdt, &ctx->tss, &ctx->ptr, ist1_top, ist2_top);

    g_per_cpus[cpu_id].tss_base = (uint64_t)&ctx->tss;

    gdt_flush(&ctx->ptr);
    tss_flush(GDT_TSS_SEL);
}
