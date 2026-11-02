#include "idt.h"
#include "gdt.h"
#include "panic.h"
#include "log.h"
#include "string.h"
#include "syscall.h"
#include "scheduler.h"

// External assembly ISR declarations (0..31)
extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);

// External assembly IRQ declarations (32..47)
extern void irq0(void);
extern void irq1(void);
extern void irq2(void);
extern void irq3(void);
extern void irq4(void);
extern void irq5(void);
extern void irq6(void);
extern void irq7(void);
extern void irq8(void);
extern void irq9(void);
extern void irq10(void);
extern void irq11(void);
extern void irq12(void);
extern void irq13(void);
extern void irq14(void);
extern void irq15(void);
extern void isr_spurious(void);

// 256-entry 64-bit IDT
__attribute__((aligned(16)))
static idt_entry_t g_idt[256];
static idt_ptr_t g_idt_ptr;

static irq_handler_t g_irq_handlers[16] = {0};

// Forward declaration of LAPIC EOI
extern void lapic_eoi(void);

static const char* exception_names[32] = {
    "#DE: Divide-by-Zero Error",
    "#DB: Debug Exception",
    "NMI: Non-Maskable Interrupt",
    "#BP: Breakpoint Trap",
    "#OF: Overflow Trap",
    "#BR: BOUND Range Exceeded",
    "#UD: Invalid / Undefined Opcode",
    "#NM: Device Not Available (No Math)",
    "#DF: Double Fault Encountered",
    "Coprocessor Segment Overrun",
    "#TS: Invalid TSS Segment",
    "#NP: Segment Not Present",
    "#SS: Stack-Segment Fault",
    "#GP: General Protection Fault",
    "#PF: Page Fault Encountered",
    "Reserved Exception (#15)",
    "#MF: x87 FPU Floating-Point Error",
    "#AC: Alignment Check Fault",
    "#MC: Machine Check Exception",
    "#XM: SIMD Floating-Point Exception",
    "#VE: Virtualization Exception",
    "#CP: Control Protection Exception",
    "Reserved Exception (#22)",
    "Reserved Exception (#23)",
    "Reserved Exception (#24)",
    "Reserved Exception (#25)",
    "Reserved Exception (#26)",
    "Reserved Exception (#27)",
    "#HV: Hypervisor Injection Exception",
    "#VC: VMM Communication Exception",
    "#SX: Security Exception",
    "Reserved Exception (#31)"
};

void idt_set_gate(uint8_t num, uint64_t handler, uint16_t sel, uint8_t flags, uint8_t ist) {
    g_idt[num].offset_low  = (uint16_t)(handler & 0xFFFF);
    g_idt[num].selector    = sel;
    g_idt[num].ist         = ist & 0x07;
    g_idt[num].type_attr   = flags;
    g_idt[num].offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    g_idt[num].offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFF);
    g_idt[num].reserved    = 0;
}

void irq_install_handler(int irq, irq_handler_t handler) {
    if (irq >= 0 && irq < 16) {
        g_irq_handlers[irq] = handler;
    }
}

void irq_uninstall_handler(int irq) {
    if (irq >= 0 && irq < 16) {
        g_irq_handlers[irq] = NULL;
    }
}

// C-level exception dispatcher called from common_isr_stub
void c_exception_handler(trap_frame_t* frame) {
    const char* name = "Unknown Exception";
    if (frame && frame->int_no < 32) {
        name = exception_names[frame->int_no];
    }
    panic_show_exception(frame, name);
}

// C-level IRQ dispatcher called from common_irq_stub
trap_frame_t* c_irq_handler(trap_frame_t* frame) {
    if (!frame) return frame;
    int irq = (int)frame->int_no - 32;

    if (irq >= 0 && irq < 16 && g_irq_handlers[irq]) {
        g_irq_handlers[irq](frame);
    }

    // Acknowledge before a possible address-space/context switch.
    lapic_eoi();
    return scheduler_handle_irq(frame);
}

void idt_init(void) {
    memset(g_idt, 0, sizeof(g_idt));

    // Initialize all 256 gates with isr_spurious to safely handle spurious or unhandled vectors
    for (int i = 0; i < 256; i++) {
        idt_set_gate(i, (uint64_t)isr_spurious, GDT_KERNEL_CODE_SEL, 0x8E, 0);
    }

    // 0x8E = 64-bit Interrupt Gate, DPL 0, Present
    // Vector 8 (#DF) uses IST 1 (Dedicated Double Fault stack in TSS)!
    idt_set_gate(0,  (uint64_t)isr0,  GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(1,  (uint64_t)isr1,  GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(2,  (uint64_t)nmi_entry, GDT_KERNEL_CODE_SEL, 0x8E, 2);
    idt_set_gate(3,  (uint64_t)isr3,  GDT_KERNEL_CODE_SEL, 0xEE, 0); // User-callable breakpoint
    idt_set_gate(4,  (uint64_t)isr4,  GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(5,  (uint64_t)isr5,  GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(6,  (uint64_t)isr6,  GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(7,  (uint64_t)isr7,  GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(8,  (uint64_t)isr8,  GDT_KERNEL_CODE_SEL, 0x8E, 1); // IST 1 for #DF
    idt_set_gate(9,  (uint64_t)isr9,  GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(10, (uint64_t)isr10, GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(11, (uint64_t)isr11, GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(12, (uint64_t)isr12, GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(13, (uint64_t)isr13, GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(14, (uint64_t)isr14, GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(15, (uint64_t)isr15, GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(16, (uint64_t)isr16, GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(17, (uint64_t)isr17, GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(18, (uint64_t)isr18, GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(19, (uint64_t)isr19, GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(20, (uint64_t)isr20, GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(21, (uint64_t)isr21, GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(22, (uint64_t)isr22, GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(23, (uint64_t)isr23, GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(24, (uint64_t)isr24, GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(25, (uint64_t)isr25, GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(26, (uint64_t)isr26, GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(27, (uint64_t)isr27, GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(28, (uint64_t)isr28, GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(29, (uint64_t)isr29, GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(30, (uint64_t)isr30, GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(31, (uint64_t)isr31, GDT_KERNEL_CODE_SEL, 0x8E, 0);

    // Hardware IRQs (32..47)
    idt_set_gate(32, (uint64_t)irq0,  GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(33, (uint64_t)irq1,  GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(34, (uint64_t)irq2,  GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(35, (uint64_t)irq3,  GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(36, (uint64_t)irq4,  GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(37, (uint64_t)irq5,  GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(38, (uint64_t)irq6,  GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(39, (uint64_t)irq7,  GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(40, (uint64_t)irq8,  GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(41, (uint64_t)irq9,  GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(42, (uint64_t)irq10, GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(43, (uint64_t)irq11, GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(44, (uint64_t)irq12, GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(45, (uint64_t)irq13, GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(46, (uint64_t)irq14, GDT_KERNEL_CODE_SEL, 0x8E, 0);
    idt_set_gate(47, (uint64_t)irq15, GDT_KERNEL_CODE_SEL, 0x8E, 0);

    g_idt_ptr.limit = sizeof(g_idt) - 1;
    g_idt_ptr.base = (uint64_t)&g_idt;

    // Load IDT into CPU IDTR
    __asm__ volatile ("lidt %0" : : "m"(g_idt_ptr));

    log_printf("[IDT] 64-bit IDT loaded (256 vectors active, Double Fault on IST1).\n");
}
