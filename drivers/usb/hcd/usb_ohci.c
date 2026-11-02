#include "usb_ohci.h"
#include "usb_core.h"
#include "usb_compat.h"
#include "dma.h"
#include "log.h"
#include "string.h"

#define OHCI_MAX_CONTROLLERS 4
#define OHCI_MAX_INTR_SLOTS 6

typedef struct {
    volatile uint32_t control;
    volatile uint32_t tail;
    volatile uint32_t head;
    volatile uint32_t next;
} __attribute__((aligned(16))) ohci_ed_t;

typedef struct {
    volatile uint32_t control;
    volatile uint32_t current_buffer;
    volatile uint32_t next_td;
    volatile uint32_t buffer_end;
} __attribute__((aligned(16))) ohci_td_t;

typedef struct {
    bool used;
    usb_device_t* device;
    usb_endpoint_t* endpoint;
    ohci_ed_t ed;
    ohci_td_t td;
    ohci_td_t dummy;
    uint8_t buffer[64] __attribute__((aligned(16)));
} ohci_intr_t;

typedef struct {
    uint8_t hcca[256] __attribute__((aligned(256)));
    ohci_ed_t control_ed;
    ohci_td_t setup_td;
    ohci_td_t data_td;
    ohci_td_t status_td;
    ohci_td_t dummy_td;
    uint8_t setup_buffer[8] __attribute__((aligned(16)));
    uint8_t data_buffer[1024] __attribute__((aligned(16)));
    ohci_intr_t interrupts[OHCI_MAX_INTR_SLOTS];
} ohci_dma_t;

typedef struct {
    volatile uint8_t* mmio;
    const pci_device_t* pci;
    dma_buffer_t dma;
    ohci_dma_t* mem;
    usb_hci_t hci;
    uint16_t ports;
    uint32_t ports_seen;
} ohci_controller_t;

static ohci_controller_t g_ohci[OHCI_MAX_CONTROLLERS];
static uint32_t g_ohci_count = 0;

#define OHCI_REVISION       0x00
#define OHCI_CONTROL        0x04
#define OHCI_COMMAND_STATUS 0x08
#define OHCI_INTERRUPT_STATUS 0x0C
#define OHCI_INTERRUPT_ENABLE 0x10
#define OHCI_HCCA           0x18
#define OHCI_CONTROL_HEAD   0x20
#define OHCI_CONTROL_CURRENT 0x24
#define OHCI_DONE_HEAD      0x30
#define OHCI_FM_INTERVAL    0x34
#define OHCI_PERIODIC_START 0x40
#define OHCI_RH_DESCRIPTOR_A 0x48
#define OHCI_RH_STATUS      0x50
#define OHCI_RH_PORT_STATUS 0x54

#define OHCI_CONTROL_CLE    (1u << 4)
#define OHCI_CONTROL_BLE    (1u << 5)
#define OHCI_CONTROL_HCFS   (3u << 6)
#define OHCI_CONTROL_OPERATIONAL (2u << 6)
#define OHCI_COMMAND_CLF    (1u << 1)

#define OHCI_PORT_CONNECT   (1u << 0)
#define OHCI_PORT_ENABLE    (1u << 1)
#define OHCI_PORT_SUSPEND   (1u << 2)
#define OHCI_PORT_RESET     (1u << 4)
#define OHCI_PORT_POWER     (1u << 8)
#define OHCI_PORT_LOW       (1u << 9)
#define OHCI_PORT_CHANGES   ((1u << 16) | (1u << 17) | (1u << 20))

static uint32_t ohci_read(ohci_controller_t* c, uint32_t offset) {
    return *(volatile uint32_t*)(c->mmio + offset);
}

static void ohci_write(ohci_controller_t* c, uint32_t offset, uint32_t value) {
    *(volatile uint32_t*)(c->mmio + offset) = value;
}

static uint32_t ohci_phys(ohci_controller_t* c, const void* pointer) {
    return (uint32_t)(c->dma.phys_addr +
                      ((const uint8_t*)pointer - (uint8_t*)c->mem));
}

static void ohci_td_init(ohci_controller_t* c, ohci_td_t* td,
                         uint32_t pid, const void* buffer, int length,
                         bool toggle) {
    memset((void*)td, 0, sizeof(*td));
    td->control = (1u << 18) | (pid << 19) | ((toggle ? 1u : 0u) << 24) |
                  (3u << 26) | (0xFu << 28);
    if (length > 0 && buffer) {
        td->current_buffer = ohci_phys(c, buffer);
        td->buffer_end = td->current_buffer + (uint32_t)length - 1u;
    }
}

static uint32_t ohci_td_cc(const ohci_td_t* td) {
    return (td->control >> 28) & 0xF;
}

static void ohci_set_ed(ohci_controller_t* c, ohci_ed_t* ed,
                        usb_device_t* device, uint8_t endpoint,
                        uint8_t direction, uint16_t max_packet) {
    uint32_t low_speed = device->speed == USB_SPEED_LOW ? (1u << 13) : 0;
    ed->control = (device->address & 0x7F) |
                  ((uint32_t)(endpoint & 0xF) << 7) |
                  ((uint32_t)(direction & 3) << 11) |
                  low_speed |
                  ((uint32_t)(max_packet & 0x7FF) << 16);
    ed->head = 0;
    ed->tail = 0;
    ed->next = 0;
    (void)c;
}

static int ohci_run_transfer(ohci_controller_t* c, ohci_ed_t* ed,
                             ohci_td_t* first, ohci_td_t* last,
                             ohci_td_t* done_td, int timeout_ms) {
    ed->head = ohci_phys(c, first);
    ed->tail = ohci_phys(c, last);
    ed->next = 0;
    ohci_write(c, OHCI_CONTROL_HEAD, ohci_phys(c, ed));
    ohci_write(c, OHCI_COMMAND_STATUS, OHCI_COMMAND_CLF);
    dma_sync_for_device(&c->dma);

    int result = -1;
    for (int i = 0; i < timeout_ms; i++) {
        dma_sync_for_cpu(&c->dma);
        uint32_t code = ohci_td_cc(done_td);
        if (code != 0xF) {
            result = (code == 0 || code == 0xD) ? 0 : -1;
            break;
        }
        usb_delay_ms(1);
    }
    ohci_write(c, OHCI_CONTROL_HEAD, 0);
    ohci_write(c, OHCI_DONE_HEAD, 0);
    dma_sync_for_device(&c->dma);
    return result;
}

static int ohci_control(usb_hci_t* hci, usb_device_t* device,
                        const usb_setup_packet_t* setup, void* data,
                        int length, int direction) {
    ohci_controller_t* c = hci->priv;
    if (!c || !device || !setup || length < 0 || length > 1024) return -1;
    memcpy(c->mem->setup_buffer, setup, 8);
    if (length > 0 && direction == USB_DIR_OUT && data) {
        memcpy(c->mem->data_buffer, data, (size_t)length);
    }

    ohci_td_init(c, &c->mem->setup_td, 0, c->mem->setup_buffer, 8, false);
    if (length > 0) {
        uint32_t pid = direction == USB_DIR_IN ? 2u : 1u;
        ohci_td_init(c, &c->mem->data_td, pid, c->mem->data_buffer,
                     length, true);
        c->mem->setup_td.next_td = ohci_phys(c, &c->mem->data_td);
        c->mem->data_td.next_td = ohci_phys(c, &c->mem->status_td);
    } else {
        c->mem->setup_td.next_td = ohci_phys(c, &c->mem->status_td);
    }
    ohci_td_init(c, &c->mem->status_td,
                 direction == USB_DIR_IN ? 1u : 2u, NULL, 0, true);
    c->mem->status_td.next_td = ohci_phys(c, &c->mem->dummy_td);
    memset(&c->mem->dummy_td, 0, sizeof(c->mem->dummy_td));

    ohci_set_ed(c, &c->mem->control_ed, device, 0, 0,
                device->max_packet0);
    int result = ohci_run_transfer(c, &c->mem->control_ed,
                                   &c->mem->setup_td,
                                   &c->mem->dummy_td,
                                   &c->mem->status_td, 500);
    if (result != 0) {
        log_printf("[OHCI] control failed setup=%u data=%u status=%u\n",
                   ohci_td_cc(&c->mem->setup_td),
                   ohci_td_cc(&c->mem->data_td),
                   ohci_td_cc(&c->mem->status_td));
        return -1;
    }
    if (length > 0 && direction == USB_DIR_IN && data) {
        int actual = length;
        memcpy(data, c->mem->data_buffer, (size_t)actual);
        return actual;
    }
    return length;
}

static int ohci_intr_in(usb_hci_t* hci, usb_device_t* device,
                        usb_endpoint_t* endpoint, void* data, int length) {
    ohci_controller_t* c = hci->priv;
    if (!c || !device || !endpoint || !data || length <= 0) return -1;
    ohci_intr_t* slot = endpoint->hcd_priv;
    if (!slot || !slot->used) {
        for (int i = 0; i < OHCI_MAX_INTR_SLOTS; i++) {
            if (!c->mem->interrupts[i].used) {
                slot = &c->mem->interrupts[i];
                break;
            }
        }
        if (!slot) return -1;
        memset(slot, 0, sizeof(*slot));
        slot->used = true;
        slot->device = device;
        slot->endpoint = endpoint;
        endpoint->hcd_priv = slot;
    }

    int transfer_length = length > 64 ? 64 : length;
    ohci_td_init(c, &slot->td, 2, slot->buffer, transfer_length,
                 endpoint->toggle);
    slot->td.next_td = ohci_phys(c, &slot->dummy);
    memset(&slot->dummy, 0, sizeof(slot->dummy));
    ohci_set_ed(c, &slot->ed, device, endpoint->number, 2,
                endpoint->max_packet);
    int result = ohci_run_transfer(c, &slot->ed, &slot->td, &slot->dummy,
                                   &slot->td, 2);
    if (result != 0) return 0;
    endpoint->toggle ^= 1;
    memcpy(data, slot->buffer, (size_t)transfer_length);
    return transfer_length;
}

static void ohci_release(usb_hci_t* hci, usb_device_t* device) {
    ohci_controller_t* c = hci->priv;
    if (!c || !device) return;
    for (int i = 0; i < OHCI_MAX_INTR_SLOTS; i++) {
        if (c->mem->interrupts[i].used &&
            c->mem->interrupts[i].device == device) {
            memset(&c->mem->interrupts[i], 0,
                   sizeof(c->mem->interrupts[i]));
        }
    }
}

static int ohci_port_reset(usb_hci_t* hci, uint8_t port) {
    ohci_controller_t* c = hci->priv;
    if (!c || port == 0 || port > c->ports) return USB_SPEED_UNKNOWN;
    uint32_t reg = OHCI_RH_PORT_STATUS + 4u * (port - 1);
    uint32_t status = ohci_read(c, reg);
    if (!(status & OHCI_PORT_CONNECT)) return USB_SPEED_UNKNOWN;
    ohci_write(c, reg, OHCI_PORT_RESET);
    usb_delay_ms(20);
    for (int i = 0; i < 100; i++) {
        status = ohci_read(c, reg);
        if (!(status & OHCI_PORT_RESET)) break;
        usb_delay_ms(1);
    }
    usb_delay_ms(10);
    status = ohci_read(c, reg);
    if (status & OHCI_PORT_CHANGES) ohci_write(c, reg, status & OHCI_PORT_CHANGES);
    if (!(status & OHCI_PORT_CONNECT)) return USB_SPEED_UNKNOWN;
    return (status & OHCI_PORT_LOW) ? USB_SPEED_LOW : USB_SPEED_FULL;
}

static void ohci_poll(usb_hci_t* hci) {
    ohci_controller_t* c = hci->priv;
    if (!c) return;
    for (uint8_t port = 1; port <= c->ports; port++) {
        uint32_t reg = OHCI_RH_PORT_STATUS + 4u * (port - 1);
        uint32_t bit = 1u << (port - 1);
        uint32_t status = ohci_read(c, reg);
        if (status & OHCI_PORT_CHANGES) {
            ohci_write(c, reg, status & OHCI_PORT_CHANGES);
        }
        if (!(status & OHCI_PORT_CONNECT)) {
            if (c->ports_seen & bit) {
                usb_disconnect_port(hci, port);
                c->ports_seen &= ~bit;
            }
            continue;
        }
        if (c->ports_seen & bit) continue;
        uint8_t speed = (uint8_t)ohci_port_reset(hci, port);
        if (speed) {
            log_printf("[OHCI] root port %u %s connected\n",
                       port, usb_speed_name(speed));
            if (usb_enumerate(hci, port, speed) == 0) {
                c->ports_seen |= bit;
            }
        }
    }
}

int ohci_probe(const pci_device_t* device) {
    if (!device || g_ohci_count >= OHCI_MAX_CONTROLLERS) return -1;
    int bar = -1;
    for (int i = 0; i < PCI_BAR_COUNT; i++) {
        if (device->bar[i].valid && !device->bar[i].is_io) {
            bar = i;
            break;
        }
    }
    if (bar < 0 || device->bar[bar].address > UINT32_MAX) return -1;
    if (usb_legacy_handoff(device) != 0) return -1;

    ohci_controller_t* c = &g_ohci[g_ohci_count];
    memset(c, 0, sizeof(*c));
    c->pci = device;
    c->mmio = usb_map_mmio(device->bar[bar].address, 0x1000);
    if (!c->mmio) return -1;
    pci_enable_memory(device);
    pci_enable_bus_master(device);
    if (usb_dma_alloc_zero(sizeof(ohci_dma_t), UINT32_MAX, PAGE_SIZE,
                           &c->dma) != 0) {
        return -1;
    }
    c->mem = (ohci_dma_t*)c->dma.virt_addr;
    memset(c->mem, 0, sizeof(*c->mem));

    uint32_t revision = ohci_read(c, OHCI_REVISION) & 0xFF;
    if (revision < 0x10) {
        log_printf("[OHCI] Unsupported revision 0x%x\n", revision);
        return -1;
    }

    ohci_write(c, OHCI_CONTROL, 0);
    ohci_write(c, OHCI_INTERRUPT_ENABLE, 0);
    ohci_write(c, OHCI_INTERRUPT_STATUS, 0xFFFFFFFF);
    ohci_write(c, OHCI_HCCA, ohci_phys(c, c->mem->hcca));
    ohci_write(c, OHCI_FM_INTERVAL, 0x27782EDF);
    ohci_write(c, OHCI_PERIODIC_START, 0x00002EDF);
    ohci_write(c, OHCI_CONTROL,
               OHCI_CONTROL_CLE | OHCI_CONTROL_BLE |
               OHCI_CONTROL_OPERATIONAL);
    usb_delay_ms(20);

    uint32_t descriptor_a = ohci_read(c, OHCI_RH_DESCRIPTOR_A);
    c->ports = descriptor_a & 0xFF;
    if (c->ports == 0 || c->ports > 15) c->ports = 2;
    uint32_t no_power_switch = (descriptor_a >> 9) & 1u;
    if (no_power_switch) {
        ohci_write(c, OHCI_RH_STATUS, 1u << 16);
    } else {
        for (uint8_t port = 1; port <= c->ports; port++) {
            uint32_t reg = OHCI_RH_PORT_STATUS + 4u * (port - 1);
            ohci_write(c, reg, OHCI_PORT_POWER);
        }
    }
    usb_delay_ms(100);

    c->hci.name = "OHCI 1.1";
    c->hci.kind = USB_HCI_KIND_OHCI;
    c->hci.priv = c;
    c->hci.root_ports = (uint8_t)c->ports;
    c->hci.supports_hubs = true;
    c->hci.control = ohci_control;
    c->hci.intr_in = ohci_intr_in;
    c->hci.port_reset = ohci_port_reset;
    c->hci.device_release = ohci_release;
    c->hci.poll = ohci_poll;
    usb_register_hci(&c->hci);
    g_ohci_count++;
    ohci_poll(&c->hci);
    return 0;
}
