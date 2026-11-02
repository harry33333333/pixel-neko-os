#ifndef _APIC_H
#define _APIC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "idt.h"

// Local APIC Registers
#define LAPIC_ID            0x0020
#define LAPIC_VERSION       0x0030
#define LAPIC_TPR           0x0080
#define LAPIC_EOI           0x00B0
#define LAPIC_LDR           0x00D0
#define LAPIC_DFR           0x00E0
#define LAPIC_SVR           0x00F0
#define LAPIC_ESR           0x0280
#define LAPIC_ICR_LOW       0x0300
#define LAPIC_ICR_HIGH      0x0310
#define LAPIC_LVT_TIMER     0x0320
#define LAPIC_LVT_LINT0     0x0350
#define LAPIC_LVT_LINT1     0x0360
#define LAPIC_LVT_ERROR     0x0370
#define LAPIC_TIMER_INITCNT 0x0380
#define LAPIC_TIMER_CURRCNT 0x0390
#define LAPIC_TIMER_DIV     0x03E0

// I/O APIC Registers
#define IOAPIC_REG_ID       0x00
#define IOAPIC_REG_VER      0x01
#define IOAPIC_REG_ARB      0x02
#define IOAPIC_REDTBL(n)    (0x10 + 2 * (n))

void apic_init(void);
void lapic_eoi(void);
uint32_t lapic_read(uint32_t reg);
void lapic_write(uint32_t reg, uint32_t val);
uint32_t ioapic_read(uint8_t reg);
void ioapic_write(uint8_t reg, uint32_t val);
void ioapic_set_entry(uint8_t gsi, uint8_t vector, uint8_t apic_id, uint16_t flags, bool masked);
uint64_t apic_get_ticks(void);
uint8_t keyboard_get_scancode(void);

#endif /* _APIC_H */
