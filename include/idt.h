#ifndef _IDT_H
#define _IDT_H

#include <stdint.h>
#include <stddef.h>

// 64-bit IDT Gate Descriptor (16 bytes)
struct idt_entry {
    uint16_t offset_low;  // Offset bits 0..15
    uint16_t selector;    // Code segment selector (0x08)
    uint8_t  ist;         // Bits 0..2 = IST index (0 = none, 1 = IST1, etc.)
    uint8_t  type_attr;   // Gate type, DPL, and present bit
    uint16_t offset_mid;  // Offset bits 16..31
    uint32_t offset_high; // Offset bits 32..63
    uint32_t reserved;    // Reserved (0)
} __attribute__((packed));
typedef struct idt_entry idt_entry_t;

// IDT Pointer for LIDT
struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));
typedef struct idt_ptr idt_ptr_t;

// 64-bit Interrupt / Exception Trap Frame
struct trap_frame {
    // Saved general-purpose registers
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    
    // Interrupt details pushed by stub
    uint64_t int_no;
    uint64_t err_code;
    
    // Pushed automatically by CPU on interrupt entry
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed));
typedef struct trap_frame trap_frame_t;

typedef void (*irq_handler_t)(trap_frame_t* frame);

void idt_init(void);
void idt_set_gate(uint8_t num, uint64_t handler, uint16_t sel, uint8_t flags, uint8_t ist);
void irq_install_handler(int irq, irq_handler_t handler);
void irq_uninstall_handler(int irq);

#endif /* _IDT_H */
