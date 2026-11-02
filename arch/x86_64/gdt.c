#include "gdt.h"
#include "log.h"
#include "string.h"

// Dedicated 16KB stack for Double Fault (#DF) handler via IST 1
__attribute__((aligned(16)))
static uint8_t df_ist1_stack[16384];
__attribute__((aligned(16)))
static uint8_t nmi_ist2_stack[16384];

// Global TSS instance
static tss64_t g_tss = {0};

// GDT entries: 0:Null, 1:KCode, 2:KData, 3:UData, 4:UCode, 5&6:TSS (16 bytes)
static uint64_t g_gdt[7] = {0};
static gdt_ptr_t g_gdt_ptr;

extern void gdt_flush(gdt_ptr_t* ptr);
extern void tss_flush(uint16_t sel);

static void set_gdt_gate(int num, uint64_t base, uint64_t limit, uint8_t access, uint8_t gran) {
    uint64_t descriptor = 0;
    descriptor |= (limit & 0xFFFFULL);
    descriptor |= (base & 0xFFFFFFULL) << 16;
    descriptor |= ((uint64_t)access) << 40;
    descriptor |= ((uint64_t)(limit >> 16) & 0x0FULL) << 48;
    descriptor |= ((uint64_t)(gran & 0xF0ULL)) << 48;
    descriptor |= ((uint64_t)(base >> 24) & 0xFFULL) << 56;
    g_gdt[num] = descriptor;
}

static void set_tss_gate(int num, uint64_t base, uint32_t limit) {
    // Low 8 bytes
    set_gdt_gate(num, base & 0xFFFFFFFFULL, limit, 0x89, 0x00);
    // High 8 bytes (base 32..63, reserved)
    g_gdt[num + 1] = (base >> 32) & 0xFFFFFFFFULL;
}

void tss_set_rsp0(uint64_t rsp0) {
    g_tss.rsp0 = rsp0;
}

void gdt_init(void) {
    memset(&g_tss, 0, sizeof(tss64_t));
    
    // Set up IST 1 for Double Fault (#DF) stack (top of stack is highest address, 16-byte aligned)
    uint64_t ist1_top = (uint64_t)df_ist1_stack + sizeof(df_ist1_stack);
    ist1_top &= ~0xFULL;
    g_tss.ist1 = ist1_top;
    uint64_t ist2_top = (uint64_t)nmi_ist2_stack + sizeof(nmi_ist2_stack);
    ist2_top &= ~0xFULL;
    g_tss.ist2 = ist2_top;
    g_tss.iomap_base = sizeof(tss64_t);

    // 0: Null Descriptor
    g_gdt[0] = 0;

    // 1: Kernel Code 64 (DPL 0, L=1, G=1, P=1, readable) -> Selector 0x08
    // Access: 0x9A (P=1, DPL=0, S=1, Type=10: Ex/Read)
    // Granularity/Flags: 0xA0 (G=1, L=1, D=0)
    set_gdt_gate(1, 0, 0xFFFFF, 0x9A, 0xA0);

    // 2: Kernel Data 64 (DPL 0, W=1, P=1, writable) -> Selector 0x10
    // Access: 0x92 (P=1, DPL=0, S=1, Type=2: Read/Write)
    // Granularity/Flags: 0xC0 (G=1, D=1, L=0 for a data segment)
    set_gdt_gate(2, 0, 0xFFFFF, 0x92, 0xC0);

    // 3: User Data 64 (DPL 3, W=1, P=1, writable) -> Selector 0x1B
    // Access: 0xF2 (P=1, DPL=3, S=1, Type=2: Read/Write)
    set_gdt_gate(3, 0, 0xFFFFF, 0xF2, 0xC0);

    // 4: User Code 64 (DPL 3, L=1, G=1, P=1, readable) -> Selector 0x23
    // Access: 0xFA (P=1, DPL=3, S=1, Type=10: Ex/Read)
    set_gdt_gate(4, 0, 0xFFFFF, 0xFA, 0xA0);

    // 5 & 6: TSS Descriptor (16 bytes in 64-bit AMD64 mode) -> Selector 0x28
    set_tss_gate(5, (uint64_t)&g_tss, sizeof(tss64_t) - 1);

    g_gdt_ptr.limit = sizeof(g_gdt) - 1;
    g_gdt_ptr.base = (uint64_t)&g_gdt;

    // Load GDT and refresh segments
    gdt_flush(&g_gdt_ptr);

    // Load Task Register
    tss_flush(GDT_TSS_SEL);

    log_printf("[GDT] 64-bit GDT installed (CS=0x%02x, DS=0x%02x, TSS=0x%02x).\n",
               GDT_KERNEL_CODE_SEL, GDT_KERNEL_DATA_SEL, GDT_TSS_SEL);
    log_printf("[GDT] Ring 3 descriptors: UDS=0x%016llx UCS=0x%016llx\n",
               (unsigned long long)g_gdt[GDT_USER_DATA_SEL / 8],
               (unsigned long long)g_gdt[GDT_USER_CODE_SEL / 8]);
    log_printf("[GDT] TSS active. Dedicated Double Fault IST1 Stack @ 0x%016llx\n", ist1_top);
    log_printf("[GDT] Paranoid NMI IST2 Stack @ 0x%016llx\n", ist2_top);
}
