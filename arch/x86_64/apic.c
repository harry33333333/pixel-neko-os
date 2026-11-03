#include "apic.h"
#include "acpi.h"
#include "mm.h"
#include "vmm.h"
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

void lapic_send_nmi_self(void) {
    // Delivery mode NMI (100b), destination shorthand self (01b).
    lapic_write(LAPIC_ICR_LOW, (1u << 14) | (4u << 8) | (1u << 18));
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

static inline void rdmsr(uint32_t msr, uint32_t* lo, uint32_t* hi) {
    __asm__ volatile ("rdmsr" : "=a"(*lo), "=d"(*hi) : "c"(msr));
}

static inline void wrmsr(uint32_t msr, uint32_t lo, uint32_t hi) {
    __asm__ volatile ("wrmsr" : : "a"(lo), "d"(hi), "c"(msr));
}

static void apic_timer_handler(trap_frame_t* frame) {
    (void)frame;
    g_lapic_ticks++;
}

uint64_t apic_get_ticks(void) {
    return g_lapic_ticks;
}

static void pit_init_100hz(void) {
    // Program 8254 PIT Channel 0 to 100 Hz (1193182 / 100 = 11932)
    uint16_t divisor = 11932;
    outb(0x43, 0x34); // Channel 0, lo/hi byte, rate generator (mode 2), binary
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

static uint32_t calibrate_lapic_timer_100hz(void) {
    // Use 8254 PIT Channel 2 in one-shot mode to measure 10ms (1/100th sec = 11932 PIT ticks)
    uint16_t pit_ticks = 11932;
    uint8_t gate = inb(0x61);
    outb(0x61, (uint8_t)((gate & 0xFD) | 0x01)); // Disable speaker, enable PIT Ch2 gate
    outb(0x43, 0xB0);                             // Channel 2, lo/hi, mode 0 (interrupt on terminal count)
    outb(0x42, (uint8_t)(pit_ticks & 0xFF));
    outb(0x42, (uint8_t)((pit_ticks >> 8) & 0xFF));

    // Reset PIT Ch2 gate trigger
    gate = inb(0x61) & 0xFE;
    outb(0x61, gate);
    outb(0x61, (uint8_t)(gate | 0x01));

    // Start LAPIC timer in one-shot mode (masked) from 0xFFFFFFFF with divider 16
    lapic_write(LAPIC_TIMER_DIV, 0x03);
    lapic_write(LAPIC_LVT_TIMER, 0x10000 | 32);
    lapic_write(LAPIC_TIMER_INITCNT, 0xFFFFFFFF);

    // Wait for PIT Channel 2 output (bit 5 of port 0x61) to go high, with safety timeout
    uint32_t spins = 0;
    while (!(inb(0x61) & 0x20) && spins < 5000000u) {
        spins++;
    }

    uint32_t curr = lapic_read(LAPIC_TIMER_CURRCNT);
    lapic_write(LAPIC_LVT_TIMER, 0x10000);
    outb(0x61, (uint8_t)(gate & 0xFC));

    uint32_t elapsed = 0xFFFFFFFFu - curr;
    // Clamp to sane bounds if PIT Ch2 is not emulated or fast
    if (elapsed < 10000u || spins >= 5000000u) {
        elapsed = 0;
    }
    return elapsed;
}

void apic_init(void) {
    // 0. Always program 8254 PIT Channel 0 to 100 Hz (Legacy BIOS leaves it at 18.2 Hz)
    pit_init_100hz();

    // 1. Enable LAPIC globally in CPU via IA32_APIC_BASE MSR (0x1B)
    uint32_t msr_lo = 0, msr_hi = 0;
    rdmsr(0x1B, &msr_lo, &msr_hi);
    if (!(msr_lo & (1 << 11))) {
        msr_lo |= (1 << 11);
        wrmsr(0x1B, msr_lo, msr_hi);
        log_printf("[APIC] LAPIC hardware globally enabled via IA32_APIC_BASE MSR.\n");
    }

    // 2. Check and switch IMCR (Interrupt Mode Configuration Register) to APIC mode
    outb(0x22, 0x70);
    outb(0x23, 0x01);

    // 3. Completely mask Legacy 8259 PIC on both Master and Slave
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
    log_printf("[APIC] Legacy 8259 PIC disabled & masked.\n");

    // 4. Map Local APIC via HHDM
    uint64_t lapic_phys = acpi_get_lapic_base();
    g_lapic_virt = (volatile uint8_t*)phys_to_virt(lapic_phys);
    vmm_map_page(vmm_get_kernel_pml4(), (virt_addr_t)g_lapic_virt, lapic_phys, PTE_PRESENT | PTE_WRITABLE | PTE_PCD);
    log_printf("[APIC] LAPIC mapped at Virt %p (Phys 0x%08llx)\n", g_lapic_virt, lapic_phys);

    // 4. Initialize LAPIC Registers
    lapic_write(LAPIC_TPR, 0);          // Accept all interrupt priorities
    lapic_write(LAPIC_DFR, 0xFFFFFFFF); // Flat model
    lapic_write(LAPIC_LDR, (lapic_read(LAPIC_LDR) & 0x00FFFFFF) | 1);
    lapic_write(LAPIC_SVR, 0x1FF);      // Vector 0xFF, Bit 8 = 1 (APIC Software Enable)

    lapic_write(LAPIC_LVT_LINT0, 0x10000); // Mask LINT0
    lapic_write(LAPIC_LVT_LINT1, 0x00400); // LINT1 = NMI
    lapic_write(LAPIC_LVT_ERROR, 0x10000); // Mask error interrupt

    // 5. Calibrate and configure LAPIC Timer (Periodic mode at 100 Hz on Vector 32)
    uint32_t calibrated_cnt = calibrate_lapic_timer_100hz();
    bool use_ioapic_pit = false;
    if (calibrated_cnt >= 10000u) {
        lapic_write(LAPIC_TIMER_DIV, 0x03);           // Divide by 16
        lapic_write(LAPIC_LVT_TIMER, (1 << 17) | 32); // Periodic mode, Vector 32
        lapic_write(LAPIC_TIMER_INITCNT, calibrated_cnt);
        log_printf("[APIC] LAPIC Timer calibrated to 100 Hz (initcnt=%u, Vector 32)\n", calibrated_cnt);
    } else {
        lapic_write(LAPIC_LVT_TIMER, 0x10000); // Mask LAPIC timer, rely on 100 Hz PIT via IOAPIC
        use_ioapic_pit = true;
        log_printf("[APIC] LAPIC Timer calibration fallback to 100 Hz IOAPIC PIT (Vector 32)\n");
    }

    irq_install_handler(0, apic_timer_handler);

    // 6. Map and configure I/O APIC via HHDM
    uint64_t ioapic_phys = acpi_get_ioapic_base();
    g_ioapic_virt = (volatile uint8_t*)phys_to_virt(ioapic_phys);
    vmm_map_page(vmm_get_kernel_pml4(), (virt_addr_t)g_ioapic_virt, ioapic_phys, PTE_PRESENT | PTE_WRITABLE | PTE_PCD);
    log_printf("[APIC] I/O APIC mapped at Virt %p (Phys 0x%08llx)\n", g_ioapic_virt, ioapic_phys);

    uint32_t ver = ioapic_read(IOAPIC_REG_VER);
    uint32_t max_entries = ((ver >> 16) & 0xFF) + 1;
    log_printf("[APIC] I/O APIC Version: 0x%02x, Max Redirection Entries: %u\n", ver & 0xFF, max_entries);

    // Mask all redirection entries initially
    for (uint32_t i = 0; i < max_entries; i++) {
        ioapic_set_entry(i, 32 + i, 0, 0, true);
    }

    // Configure Timer IRQ 0 -> GSI routing via ACPI ISO table (unmasked only if LAPIC timer fallback)
    uint32_t timer_gsi = acpi_remap_irq(0);
    uint16_t timer_flags = acpi_get_irq_flags(0);
    ioapic_set_entry(timer_gsi, 32, 0, timer_flags, !use_ioapic_pit);
    log_printf("[APIC] Routed IRQ 0 (Timer) to GSI %u, Vector 32 (%s)\n",
               timer_gsi, use_ioapic_pit ? "active" : "masked, using calibrated LAPIC timer");

    // Configure PS/2 keyboard IRQ 1 and mouse IRQ 12 routing. Drivers install
    // their handlers after the unified input stack is initialized.
    uint32_t kbd_gsi = acpi_remap_irq(1);
    uint16_t kbd_flags = acpi_get_irq_flags(1);
    ioapic_set_entry(kbd_gsi, 33, 0, kbd_flags, false);
    log_printf("[APIC] Routed IRQ 1 (Keyboard) to GSI %u, Vector 33\n", kbd_gsi);

    uint32_t mouse_gsi = acpi_remap_irq(12);
    uint16_t mouse_flags = acpi_get_irq_flags(12);
    ioapic_set_entry(mouse_gsi, 44, 0, mouse_flags, false);
    log_printf("[APIC] Routed IRQ 12 (Mouse) to GSI %u, Vector 44\n", mouse_gsi);

    log_printf("[APIC] Modern Interrupt Subsystem (LAPIC/IOAPIC) initialized successfully.\n");
}
