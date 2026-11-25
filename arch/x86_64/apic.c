#include "apic.h"
#include "acpi.h"
#include "audio.h"
#include "cpu.h"
#include "percpu.h"
#include "mm.h"
#include "vmm.h"
#include "port.h"
#include "log.h"
#include "idt.h"

static volatile uint8_t* g_lapic_virt = NULL;
static volatile uint8_t* g_ioapic_virt = NULL;
static volatile uint64_t g_lapic_ticks_100hz = 0;
static volatile uint64_t g_lapic_ticks_ms = 0;
static bool g_x2apic_enabled = false;
static bool g_use_ioapic_pit = false;
static uint32_t g_lapic_timer_initcnt_1khz = 0;

bool apic_is_x2apic(void) {
    return g_x2apic_enabled;
}

uint32_t lapic_read(uint32_t reg) {
    if (g_x2apic_enabled) {
        return (uint32_t)rdmsr(0x800u + (reg >> 4));
    }
    if (!g_lapic_virt) return 0;
    return *(volatile uint32_t*)(g_lapic_virt + reg);
}

void lapic_write(uint32_t reg, uint32_t val) {
    if (g_x2apic_enabled) {
        wrmsr(0x800u + (reg >> 4), (uint64_t)val);
        return;
    }
    if (!g_lapic_virt) return;
    *(volatile uint32_t*)(g_lapic_virt + reg) = val;
}

uint32_t lapic_get_id(void) {
    if (g_x2apic_enabled) {
        return lapic_read(LAPIC_ID);
    }
    return lapic_read(LAPIC_ID) >> 24;
}

void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

static void lapic_wait_icr_idle(void) {
    if (g_x2apic_enabled) return;
    uint32_t timeout = 100000u;
    while ((lapic_read(LAPIC_ICR_LOW) & (1u << 12)) && --timeout) {
        __asm__ volatile ("pause");
    }
}

void lapic_send_nmi_self(void) {
    // Delivery mode NMI (100b), level assert (bit 14), destination shorthand self (01b).
    if (g_x2apic_enabled) {
        wrmsr(0x830u, (1ULL << 14) | (4ULL << 8) | (1ULL << 18));
        return;
    }
    lapic_wait_icr_idle();
    lapic_write(LAPIC_ICR_LOW, (1u << 14) | (4u << 8) | (1u << 18));
}

void lapic_send_ipi(uint32_t dest_lapic_id, uint8_t vector) {
    if (g_x2apic_enabled) {
        uint64_t icr = ((uint64_t)dest_lapic_id << 32) | (1ULL << 14) | (uint64_t)vector;
        wrmsr(0x830u, icr);
        return;
    }
    if (!g_lapic_virt) return;
    lapic_wait_icr_idle();
    lapic_write(LAPIC_ICR_HIGH, (dest_lapic_id & 0xFFu) << 24);
    lapic_write(LAPIC_ICR_LOW, (1u << 14) | (uint32_t)vector);
}

void lapic_broadcast_ipi_others(uint8_t vector) {
    // Destination shorthand 11b (All Excluding Self), Level=Assert, Fixed delivery
    if (g_x2apic_enabled) {
        uint64_t icr = (3ULL << 18) | (1ULL << 14) | (uint64_t)vector;
        wrmsr(0x830u, icr);
        return;
    }
    if (!g_lapic_virt) return;
    lapic_wait_icr_idle();
    lapic_write(LAPIC_ICR_HIGH, 0);
    lapic_write(LAPIC_ICR_LOW, (3u << 18) | (1u << 14) | (uint32_t)vector);
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

    if (flags & 2) {
        low |= (1u << 13);
    }
    if (flags & 8) {
        low |= (1u << 15);
    }
    if (masked) {
        low |= (1u << 16);
    }

    uint32_t high = ((uint32_t)apic_id) << 24;

    ioapic_write(IOAPIC_REDTBL(gsi), low);
    ioapic_write(IOAPIC_REDTBL(gsi) + 1, high);
}

void apic_enable_pci_irq(uint8_t irq_line) {
    if (irq_line >= 16) return;

    uint32_t gsi = acpi_remap_irq(irq_line);
    uint16_t flags = acpi_get_irq_flags(irq_line);
    if (flags == 0) {
        flags = 0x000Au;
    }

    uint8_t bsp_id = (uint8_t)(g_per_cpus[0].lapic_id & 0xFFu);
    ioapic_set_entry((uint8_t)gsi, (uint8_t)(32u + irq_line), bsp_id, flags, false);
    log_printf("[APIC] Enabled PCI IRQ %u -> GSI %u, Vector %u (flags=0x%02x)\n",
               irq_line, gsi, 32u + irq_line, flags);
}

void apic_enable_pci_gsi(uint8_t gsi, uint16_t flags) {
    if (gsi >= 24u) return;
    if (flags == 0) {
        flags = 0x000Au;
    }

    uint8_t bsp_id = (uint8_t)(g_per_cpus[0].lapic_id & 0xFFu);
    ioapic_set_entry(gsi, (uint8_t)(32u + gsi), bsp_id, flags, false);
    log_printf("[APIC] Enabled PCI GSI %u -> Vector %u (flags=0x%02x)\n",
               gsi, 32u + gsi, flags);
}

static void apic_timer_handler(trap_frame_t* frame) {
    (void)frame;
    if (this_cpu_id() == 0) {
        if (g_use_ioapic_pit) {
            g_lapic_ticks_ms += 10ULL;
            g_lapic_ticks_100hz++;
        } else {
            uint64_t ms = g_lapic_ticks_ms + 1ULL;
            g_lapic_ticks_ms = ms;
            g_lapic_ticks_100hz = ms / 10ULL;
        }
        audio_service_dma_irq();
    }
}

uint64_t apic_get_ticks(void) {
    return g_lapic_ticks_100hz;
}

uint64_t apic_get_ticks_ms(void) {
    return g_lapic_ticks_ms;
}

static void pit_init_100hz(void) {
    uint16_t divisor = 11932;
    outb(0x43, 0x34);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

static uint32_t calibrate_lapic_timer_10ms(void) {
    uint16_t pit_ticks = 11932;
    uint8_t gate = inb(0x61);
    outb(0x61, (uint8_t)((gate & 0xFD) | 0x01));
    outb(0x43, 0xB0);
    outb(0x42, (uint8_t)(pit_ticks & 0xFF));
    outb(0x42, (uint8_t)((pit_ticks >> 8) & 0xFF));

    gate = inb(0x61) & 0xFE;
    outb(0x61, gate);
    outb(0x61, (uint8_t)(gate | 0x01));

    lapic_write(LAPIC_TIMER_DIV, 0x03);
    lapic_write(LAPIC_LVT_TIMER, 0x10000 | 32);
    lapic_write(LAPIC_TIMER_INITCNT, 0xFFFFFFFF);

    uint32_t spins = 0;
    while (!(inb(0x61) & 0x20) && spins < 5000000u) {
        spins++;
    }

    uint32_t curr = lapic_read(LAPIC_TIMER_CURRCNT);
    lapic_write(LAPIC_LVT_TIMER, 0x10000);
    outb(0x61, (uint8_t)(gate & 0xFC));

    uint32_t elapsed = 0xFFFFFFFFu - curr;
    if (elapsed < 10000u || spins >= 5000000u) {
        elapsed = 0;
    }
    return elapsed;
}

static void lapic_configure_local(void) {
    lapic_write(LAPIC_TPR, 0);
    if (!g_x2apic_enabled) {
        lapic_write(LAPIC_DFR, 0xFFFFFFFF);
        lapic_write(LAPIC_LDR, (lapic_read(LAPIC_LDR) & 0x00FFFFFF) | 1);
    }
    lapic_write(LAPIC_SVR, 0x1FF);
    lapic_write(LAPIC_LVT_LINT0, 0x10000);
    lapic_write(LAPIC_LVT_LINT1, 0x00400);
    lapic_write(LAPIC_LVT_ERROR, 0x10000);
}

void apic_init_ap(void) {
    uint64_t apic_base = rdmsr(MSR_IA32_APIC_BASE);
    if (!(apic_base & (1ULL << 11))) {
        apic_base |= (1ULL << 11);
        wrmsr(MSR_IA32_APIC_BASE, apic_base);
    }
    if (g_x2apic_enabled && !(apic_base & (1ULL << 10))) {
        apic_base |= (1ULL << 10);
        wrmsr(MSR_IA32_APIC_BASE, apic_base);
    }

    lapic_configure_local();

    if (g_lapic_timer_initcnt_1khz >= 1000u) {
        lapic_write(LAPIC_TIMER_DIV, 0x03);           // Divide by 16
        lapic_write(LAPIC_LVT_TIMER, (1u << 17) | 32); // Periodic mode, Vector 32
        lapic_write(LAPIC_TIMER_INITCNT, g_lapic_timer_initcnt_1khz);
    } else {
        lapic_write(LAPIC_LVT_TIMER, 0x10000);
    }
}

void apic_init(void) {
    // 0. Always program 8254 PIT Channel 0 to 100 Hz
    pit_init_100hz();

    // 1. Check IA32_APIC_BASE MSR (0x1B) for xAPIC / x2APIC mode
    uint64_t apic_base = rdmsr(MSR_IA32_APIC_BASE);
    if (!(apic_base & (1ULL << 11))) {
        apic_base |= (1ULL << 11);
        wrmsr(MSR_IA32_APIC_BASE, apic_base);
        log_printf("[APIC] LAPIC hardware globally enabled via IA32_APIC_BASE MSR.\n");
    }
    g_x2apic_enabled = (apic_base & (1ULL << 10)) != 0;

    // 2. Switch IMCR to APIC mode & mask Legacy 8259 PIC
    outb(0x22, 0x70);
    outb(0x23, 0x01);
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
    log_printf("[APIC] Legacy 8259 PIC disabled & masked (mode=%s).\n",
               g_x2apic_enabled ? "x2APIC" : "xAPIC");

    // 3. Map Local APIC via HHDM (needed for xAPIC MMIO)
    uint64_t lapic_phys = acpi_get_lapic_base();
    g_lapic_virt = (volatile uint8_t*)phys_to_virt(lapic_phys);
    vmm_map_page(vmm_get_kernel_pml4(), (virt_addr_t)g_lapic_virt, lapic_phys,
                 PTE_PRESENT | PTE_WRITABLE | PTE_PCD);
    log_printf("[APIC] LAPIC mapped at Virt %p (Phys 0x%08llx)\n", g_lapic_virt, lapic_phys);

    // 4. Initialize BSP LAPIC Registers
    lapic_configure_local();
    g_per_cpus[0].lapic_id = lapic_get_id();

    // 5. Calibrate and configure LAPIC Timer to 1000 Hz (1 ms period) on Vector 32
    uint32_t cnt_10ms = calibrate_lapic_timer_10ms();
    g_use_ioapic_pit = false;
    g_lapic_timer_initcnt_1khz = cnt_10ms / 10u;

    if (g_lapic_timer_initcnt_1khz >= 1000u) {
        lapic_write(LAPIC_TIMER_DIV, 0x03);           // Divide by 16
        lapic_write(LAPIC_LVT_TIMER, (1u << 17) | 32); // Periodic mode, Vector 32
        lapic_write(LAPIC_TIMER_INITCNT, g_lapic_timer_initcnt_1khz);
        log_printf("[APIC] LAPIC Timer calibrated to 1000 Hz (initcnt=%u, Vector 32)\n",
                   g_lapic_timer_initcnt_1khz);
    } else {
        lapic_write(LAPIC_LVT_TIMER, 0x10000);
        g_use_ioapic_pit = true;
        log_printf("[APIC] LAPIC Timer calibration fallback to 100 Hz IOAPIC PIT (Vector 32)\n");
    }

    irq_install_handler(0, apic_timer_handler);

    // 6. Map and configure I/O APIC via HHDM
    uint64_t ioapic_phys = acpi_get_ioapic_base();
    g_ioapic_virt = (volatile uint8_t*)phys_to_virt(ioapic_phys);
    vmm_map_page(vmm_get_kernel_pml4(), (virt_addr_t)g_ioapic_virt, ioapic_phys,
                 PTE_PRESENT | PTE_WRITABLE | PTE_PCD);
    log_printf("[APIC] I/O APIC mapped at Virt %p (Phys 0x%08llx)\n", g_ioapic_virt, ioapic_phys);

    uint32_t ver = ioapic_read(IOAPIC_REG_VER);
    uint32_t max_entries = ((ver >> 16) & 0xFF) + 1;
    log_printf("[APIC] I/O APIC Version: 0x%02x, Max Redirection Entries: %u\n", ver & 0xFF, max_entries);

    uint8_t bsp_id = (uint8_t)(g_per_cpus[0].lapic_id & 0xFFu);
    for (uint32_t i = 0; i < max_entries; i++) {
        ioapic_set_entry((uint8_t)i, (uint8_t)(32 + i), bsp_id, 0, true);
    }

    uint32_t timer_gsi = acpi_remap_irq(0);
    uint16_t timer_flags = acpi_get_irq_flags(0);
    ioapic_set_entry((uint8_t)timer_gsi, 32, bsp_id, timer_flags, !g_use_ioapic_pit);
    log_printf("[APIC] Routed IRQ 0 (Timer) to GSI %u, Vector 32 (%s)\n",
               timer_gsi, g_use_ioapic_pit ? "active" : "masked, using 1000 Hz LAPIC timer");

    uint32_t kbd_gsi = acpi_remap_irq(1);
    uint16_t kbd_flags = acpi_get_irq_flags(1);
    ioapic_set_entry((uint8_t)kbd_gsi, 33, bsp_id, kbd_flags, false);
    log_printf("[APIC] Routed IRQ 1 (Keyboard) to GSI %u, Vector 33\n", kbd_gsi);

    uint32_t mouse_gsi = acpi_remap_irq(12);
    uint16_t mouse_flags = acpi_get_irq_flags(12);
    ioapic_set_entry((uint8_t)mouse_gsi, 44, bsp_id, mouse_flags, false);
    log_printf("[APIC] Routed IRQ 12 (Mouse) to GSI %u, Vector 44\n", mouse_gsi);

    acpi_set_apic_mode();
    log_printf("[APIC] Modern Interrupt Subsystem (LAPIC/IOAPIC) initialized successfully.\n");
}
