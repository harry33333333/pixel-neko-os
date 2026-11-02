#ifndef _GDT_H
#define _GDT_H

#include <stdint.h>
#include <stddef.h>

#define GDT_KERNEL_CODE_SEL 0x08
#define GDT_KERNEL_DATA_SEL 0x10
#define GDT_USER_DATA_SEL   0x1B
#define GDT_USER_CODE_SEL   0x23
#define GDT_TSS_SEL         0x28

// 64-bit Task State Segment (TSS)
struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1; // Dedicated Double Fault (#DF) stack
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));
typedef struct tss64 tss64_t;

// GDT Pointer structure for LGDT
struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));
typedef struct gdt_ptr gdt_ptr_t;

void gdt_init(void);
void tss_set_rsp0(uint64_t rsp0);

#endif /* _GDT_H */
