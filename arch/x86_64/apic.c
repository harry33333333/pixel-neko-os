#include "apic.h"
#include "acpi.h"
#include "mm.h"
#include "port.h"
#include "log.h"
#include "idt.h"

static volatile uint8_t* g_lapic_virt = NULL;
static volatile uint8_t* g_ioapic_virt = NULL;
static volatile uint64_t g_lapic_ticks = 0;

uint32_t lapic_read(uint32_t reg) {
    if (!g_lapic_virt) return 0;
    return *(volatile uint32_t*)(g_lapic_virt + reg);
}

void lapic_write(uint32_t reg, uint32_t val) {
    if (!g_lapic_virt) return;
    *(volatile uint32_t*)(g_lapic_virt + reg) = val;
}

void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

uint32_t ioapic_read(uint8_t reg) {
    if (!g_ioapic_virt) return 0;
    *(volatile uint32_t*)(g_ioapic_virt + 0x00) = reg;
    return *(volatile uint32_t*)(g_ioapic_virt + 0x10);
}

void ioapic_write(uint8_t reg, uint32_t val) {
    if (!g_ioapic_virt) return;
    *(volatile uint32_t*)(g_ioapic_virt + 0x00) = reg;
    *(volatile uint32_t*)(g_ioapic_virt + 0x10) = val;
}

void ioapic_set_entry(uint8_t gsi, uint8_t vector, uint8_t apic_id, uint16_t flags, bool masked) {
    uint32_t low = vector;

    // Delivery Mode: 000b (Fixed)
    // Destination Mode: 0 (Physical)

    // Polarity: Bit 1 of flags (1 = Active Low, 0 = Active High)
    if (flags & 2) {
        low |= (1 << 13);
    }

    // Trigger Mode: Bit 3 of flags (1 = Level Triggered, 0 = Edge Triggered)
    if (flags & 8) {
        low |= (1 << 15);
    }

    if (masked) {
        low |= (1 << 16);
    }

    uint32_t high = ((uint32_t)apic_id) << 24;

    ioapic_write(IOAPIC_REDTBL(gsi), low);
    ioapic_write(IOAPIC_REDTBL(gsi) + 1, high);
}

static void apic_timer_handler(trap_frame_t* frame) {
    (void)frame;
    g_lapic_ticks++;
}

static void apic_keyboard_handler(trap_frame_t* frame) {
    (void)frame;
    // Read 8042 scancode to clear output buffer
    (void)inb(0x60);
}

uint64_t apic_get_ticks(void) {
    return g_lapic_ticks;
}

void apic_init(void) {
    // 1. Check and switch IMCR (Interrupt Mode Configuration Register) to APIC mode
    outb(0x22, 0x70);
    outb(0x23, 0x01);

    // 2. Completely mask Legacy 8259 PIC on both Master and Slave
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
    log_printf("[APIC] Legacy 8259 PIC disabled & masked.\n");

    // 3. Map Local APIC via HHDM
    uint64_t lapic_phys = acpi_get_lapic_base();
    g_lapic_virt = (volatile uint8_t*)phys_to_virt(lapic_phys);
    log_printf("[APIC] LAPIC mapped at Virt %p (Phys 0x%08llx)\n", g_lapic_virt, lapic_phys);

    // 4. Initialize LAPIC Registers
    lapic_write(LAPIC_TPR, 0);          // Accept all interrupt priorities
    lapic_write(LAPIC_DFR, 0xFFFFFFFF); // Flat model
    lapic_write(LAPIC_LDR, (lapic_read(LAPIC_LDR) & 0x00FFFFFF) | 1);
    lapic_write(LAPIC_SVR, 0x1FF);      // Vector 0xFF, Bit 8 = 1 (APIC Software Enable)

    lapic_write(LAPIC_LVT_LINT0, 0x10000); // Mask LINT0
    lapic_write(LAPIC_LVT_LINT1, 0x00400); // LINT1 = NMI
    lapic_write(LAPIC_LVT_ERROR, 0x10000); // Mask error interrupt

    // 5. Configure LAPIC Timer (Periodic mode at Vector 32)
    lapic_write(LAPIC_TIMER_DIV, 0x03);           // Divide by 16
    lapic_write(LAPIC_LVT_TIMER, (1 << 17) | 32); // Periodic mode, Vector 32
    lapic_write(LAPIC_TIMER_INITCNT, 0x200000);   // Periodic countdown interval

    irq_install_handler(0, apic_timer_handler);
    log_printf("[APIC] LAPIC Timer configured in periodic mode (Vector 32)\n");

    // 6. Map and configure I/O APIC via HHDM
    uint64_t ioapic_phys = acpi_get_ioapic_base();
    g_ioapic_virt = (volatile uint8_t*)phys_to_virt(ioapic_phys);
    log_printf("[APIC] I/O APIC mapped at Virt %p (Phys 0x%08llx)\n", g_ioapic_virt, ioapic_phys);

    uint32_t ver = ioapic_read(IOAPIC_REG_VER);
    uint32_t max_entries = ((ver >> 16) & 0xFF) + 1;
    log_printf("[APIC] I/O APIC Version: 0x%02x, Max Redirection Entries: %u\n", ver & 0xFF, max_entries);

    // Mask all redirection entries initially
    for (uint32_t i = 0; i < max_entries; i++) {
        ioapic_set_entry(i, 32 + i, 0, 0, true);
    }

    // Configure Timer IRQ 0 -> GSI routing via ACPI ISO table
    uint32_t timer_gsi = acpi_remap_irq(0);
    uint16_t timer_flags = acpi_get_irq_flags(0);
    ioapic_set_entry(timer_gsi, 32, 0, timer_flags, false);
    log_printf("[APIC] Routed IRQ 0 (Timer) to GSI %u, Vector 32\n", timer_gsi);

    // Configure Keyboard IRQ 1 -> GSI routing
    uint32_t kbd_gsi = acpi_remap_irq(1);
    uint16_t kbd_flags = acpi_get_irq_flags(1);
    irq_install_handler(1, apic_keyboard_handler);
    ioapic_set_entry(kbd_gsi, 33, 0, kbd_flags, false);
    log_printf("[APIC] Routed IRQ 1 (Keyboard) to GSI %u, Vector 33\n", kbd_gsi);

    log_printf("[APIC] Modern Interrupt Subsystem (LAPIC/IOAPIC) initialized successfully.\n");
}
