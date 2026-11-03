#include "usb_ehci.h"
#include "usb_core.h"
#include "usb_compat.h"
#include "dma.h"
#include "log.h"
#include "string.h"

#define EHCI_MAX_CONTROLLERS 4
#define EHCI_MAX_INTR_SLOTS  12

/*
 * Per Intel EHCI Specification 1.0 Appendix B (64-bit Data Structures):
 * When HCCPARAMS.AC64 == 1 (e.g. Intel 5 Series / H55 Express 8086:3b34 & 3b3c),
 * hardware unconditionally accesses the 5 extended buffer pointer dwords
 * (offsets 0x20..0x33 in qTD and offsets 0x30..0x43 in QH).
 * Including ext_buffer[5] and aligning to 64 bytes is valid for both 32-bit
 * (AC64 == 0) and 64-bit (AC64 == 1) EHCI controllers and prevents setup_qtd's
 * extended pointers from overlapping data_qtd!
 */
typedef struct {
    volatile uint32_t next;
    volatile uint32_t alt_next;
    volatile uint32_t token;
    volatile uint32_t buffer[5];
    volatile uint32_t ext_buffer[5];
    uint32_t pad[3];
} __attribute__((aligned(64))) ehci_qtd_t;

typedef struct {
    volatile uint32_t head;
    volatile uint32_t epchar;
    volatile uint32_t epcap;
    volatile uint32_t current_qtd;
    volatile uint32_t next_qtd;
    volatile uint32_t alt_qtd;
    volatile uint32_t token;
    volatile uint32_t buffer[5];
    volatile uint32_t ext_buffer[5];
    uint32_t pad[5];
} __attribute__((aligned(64))) ehci_qh_t;

typedef struct {
    bool used;
    usb_device_t* device;
    usb_endpoint_t* endpoint;
    int transfer_length;
    ehci_qh_t qh;
    ehci_qtd_t qtd;
    ehci_qtd_t dummy;
    uint8_t buffer[64] __attribute__((aligned(32)));
} ehci_intr_t;

typedef struct {
    uint32_t periodic[1024] __attribute__((aligned(4096)));
    ehci_qh_t async_qh;
    ehci_qh_t control_qh;
    ehci_qtd_t setup_qtd;
    ehci_qtd_t data_qtd;
    ehci_qtd_t status_qtd;
    ehci_qh_t bulk_qh;
    ehci_qtd_t bulk_qtd;
    ehci_qtd_t bulk_dummy;
    uint8_t setup_buffer[8] __attribute__((aligned(32)));
    uint8_t data_buffer[1024] __attribute__((aligned(32)));
    ehci_intr_t interrupts[EHCI_MAX_INTR_SLOTS];
} ehci_dma_t;

typedef struct {
    volatile uint8_t* mmio;
    const pci_device_t* pci;
    dma_buffer_t dma;
    ehci_dma_t* mem;
    usb_hci_t hci;
    uint32_t operational;
    uint8_t ports;
    uint32_t ports_seen;
    bool companion_present;
} ehci_controller_t;

static ehci_controller_t g_ehci[EHCI_MAX_CONTROLLERS];
static uint32_t g_ehci_count = 0;

static void ehci_release(usb_hci_t* hci, usb_device_t* device);

#define EHCI_CMD_RUN       (1u << 0)
#define EHCI_CMD_RESET     (1u << 1)
#define EHCI_CMD_PERIODIC  (1u << 4)
#define EHCI_CMD_ASYNC     (1u << 5)
#define EHCI_CMD_IAAD      (1u << 6)
#define EHCI_STS_HALTED    (1u << 12)
#define EHCI_STS_IAAD      (1u << 5)

#define EHCI_PORT_CONNECT   (1u << 0)
#define EHCI_PORT_CSC       (1u << 1)
#define EHCI_PORT_ENABLED   (1u << 2)
#define EHCI_PORT_PEC       (1u << 3)
#define EHCI_PORT_OVER      (1u << 5)
#define EHCI_PORT_RESET     (1u << 8)
#define EHCI_PORT_LINE_MASK (3u << 10)
#define EHCI_PORT_LINE_LOW  (1u << 10)
#define EHCI_PORT_POWER     (1u << 12)
#define EHCI_PORT_OWNER     (1u << 13)
#define EHCI_PORT_RWC       (EHCI_PORT_CSC | EHCI_PORT_PEC | EHCI_PORT_OVER)

#define EHCI_QTD_ACTIVE     (1u << 7)
#define EHCI_QTD_HALTED     (1u << 6)
#define EHCI_QTD_PID_OUT    0u
#define EHCI_QTD_PID_IN     1u
#define EHCI_QTD_PID_SETUP  2u

static uint32_t ehci_reg_read(ehci_controller_t* c, uint32_t offset) {
    return *(volatile uint32_t*)(c->mmio + offset);
}

static void ehci_reg_write(ehci_controller_t* c, uint32_t offset, uint32_t value) {
    *(volatile uint32_t*)(c->mmio + offset) = value;
}

static uint32_t ehci_phys(ehci_controller_t* c, const void* pointer) {
    return (uint32_t)(c->dma.phys_addr +
                      ((const uint8_t*)pointer - (uint8_t*)c->mem));
}

static uint32_t ehci_qh_link(ehci_controller_t* c, ehci_qh_t* qh) {
    return ehci_phys(c, qh) | 2u;
}

static void ehci_qtd_init(ehci_controller_t* c, ehci_qtd_t* qtd,
                          uint32_t pid, const void* buffer, int length,
                          bool toggle) {
    memset((void*)qtd, 0, sizeof(*qtd));
    qtd->next = 1;
    qtd->alt_next = 1;
    qtd->token = EHCI_QTD_ACTIVE | (3u << 10) | (pid << 8) |
                 ((uint32_t)length << 16) |
                 (toggle ? (1u << 31) : 0);
    if (length > 0 && buffer) {
        uint32_t first = ehci_phys(c, buffer);
        uint32_t last = first + (uint32_t)length - 1u;
        qtd->buffer[0] = first;
        if ((first & 0xFFFFF000u) != (last & 0xFFFFF000u)) {
            qtd->buffer[1] = last & 0xFFFFF000u;
        }
    }
}

static int ehci_qtd_complete(const ehci_qtd_t* qtd) {
    return !(qtd->token & EHCI_QTD_ACTIVE) &&
           !(qtd->token & EHCI_QTD_HALTED);
}

static void ehci_program_qh_caps(ehci_qh_t* qh, usb_device_t* device,
                                 uint8_t ep_num, uint16_t max_pkt,
                                 bool is_control, bool is_intr) {
    uint32_t speed = device->speed == USB_SPEED_HIGH ? 2u :
                     (device->speed == USB_SPEED_LOW ? 1u : 0u);
    uint32_t c_flag = (device->speed != USB_SPEED_HIGH && is_control)
                          ? (1u << 27)
                          : 0u;
    uint32_t rl = is_intr ? 0u : (8u << 28);
    /* Bit 14 (DTC): use data toggle from qTD */
    qh->epchar = (device->address & 0x7Fu) |
                 ((uint32_t)(ep_num & 0x0Fu) << 8) |
                 (speed << 12) |
                 (1u << 14) |
                 ((uint32_t)(max_pkt & 0x7FFu) << 16) |
                 c_flag | rl;

    if (device->speed == USB_SPEED_HIGH) {
        qh->epcap = (1u << 30) | (is_intr ? 0x01u : 0x00u);
    } else {
        uint8_t hub_addr = device->hs_hub_address ? device->hs_hub_address
                                                  : device->parent_hub_address;
        uint8_t port_num = device->hs_port_number ? device->hs_port_number
                                                  : device->parent_port;
        uint32_t split = ((uint32_t)(hub_addr & 0x7Fu) << 16) |
                         ((uint32_t)(port_num & 0x7Fu) << 23);
        if (is_intr) {
            qh->epcap = (1u << 30) | split | (0x1Cu << 8) | 0x01u;
        } else {
            qh->epcap = (1u << 30) | split;
        }
    }
}

static void ehci_rebuild_periodic(ehci_controller_t* c) {
    ehci_qh_t* first = NULL;
    ehci_qh_t* previous = NULL;
    for (int i = 0; i < EHCI_MAX_INTR_SLOTS; i++) {
        ehci_intr_t* slot = &c->mem->interrupts[i];
        if (!slot->used) continue;
        slot->qh.head = 1;
        if (previous) previous->head = ehci_qh_link(c, &slot->qh);
        else first = &slot->qh;
        previous = &slot->qh;
    }
    uint32_t head = first ? ehci_qh_link(c, first) : 1;
    for (int i = 0; i < 1024; i++) c->mem->periodic[i] = head;
    dma_sync_for_device(&c->dma);
}

static void ehci_doorbell_wait(ehci_controller_t* c) {
    uint32_t cmd = ehci_reg_read(c, c->operational);
    ehci_reg_write(c, c->operational, cmd | EHCI_CMD_IAAD);
    for (int timeout = 0; timeout < 20; timeout++) {
        if (ehci_reg_read(c, c->operational + 0x04) & EHCI_STS_IAAD) {
            ehci_reg_write(c, c->operational + 0x04, EHCI_STS_IAAD);
            break;
        }
        usb_delay_ms(1);
    }
}

static int ehci_control(usb_hci_t* hci, usb_device_t* device,
                        const usb_setup_packet_t* setup, void* data,
                        int length, int direction) {
    ehci_controller_t* c = hci->priv;
    if (!c || !device || !setup || length < 0 || length > 1024) return -1;
    memcpy(c->mem->setup_buffer, setup, 8);
    if (length && direction == USB_DIR_OUT && data) {
        memcpy(c->mem->data_buffer, data, (size_t)length);
    }

    ehci_qtd_init(c, &c->mem->setup_qtd, EHCI_QTD_PID_SETUP,
                  c->mem->setup_buffer, 8, false);
    if (length > 0) {
        ehci_qtd_init(c, &c->mem->data_qtd,
                      direction == USB_DIR_IN ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT,
                      c->mem->data_buffer, length, true);
        c->mem->setup_qtd.next = ehci_phys(c, &c->mem->data_qtd);
        c->mem->data_qtd.next = ehci_phys(c, &c->mem->status_qtd);
        c->mem->data_qtd.alt_next = ehci_phys(c, &c->mem->status_qtd);
    } else {
        c->mem->setup_qtd.next = ehci_phys(c, &c->mem->status_qtd);
    }
    ehci_qtd_init(c, &c->mem->status_qtd,
                  (length > 0 && direction == USB_DIR_IN) ? EHCI_QTD_PID_OUT : EHCI_QTD_PID_IN,
                  NULL, 0, true);

    ehci_qh_t* qh = &c->mem->control_qh;
    memset(qh, 0, sizeof(*qh));
    ehci_program_qh_caps(qh, device, 0,
                         device->max_packet0 ? device->max_packet0 : 8,
                         true, false);
    qh->next_qtd = ehci_phys(c, &c->mem->setup_qtd);
    qh->alt_qtd = 1;
    qh->token = 0;
    qh->head = c->mem->async_qh.head;
    c->mem->async_qh.head = ehci_qh_link(c, qh);
    dma_sync_for_device(&c->dma);

    int result = -1;
    for (int timeout = 0; timeout < 500; timeout++) {
        dma_sync_for_cpu(&c->dma);
        if ((c->mem->setup_qtd.token & EHCI_QTD_HALTED) ||
            (length > 0 && (c->mem->data_qtd.token & EHCI_QTD_HALTED)) ||
            (c->mem->status_qtd.token & EHCI_QTD_HALTED)) {
            break;
        }
        if (ehci_qtd_complete(&c->mem->status_qtd)) {
            result = 0;
            break;
        }
        usb_delay_ms(1);
    }

    c->mem->async_qh.head = ehci_qh_link(c, &c->mem->async_qh);
    dma_sync_for_device(&c->dma);
    ehci_doorbell_wait(c);

    if (result != 0) {
        log_printf("[EHCI] control failed setup=0x%08x data=0x%08x status=0x%08x\n",
                   c->mem->setup_qtd.token, c->mem->data_qtd.token,
                   c->mem->status_qtd.token);
        return -1;
    }
    if (length > 0 && direction == USB_DIR_IN && data) {
        int remaining =
            (int)((c->mem->data_qtd.token >> 16) & 0x7FFF);
        int actual = length - remaining;
        if (actual < 0 || actual > length) actual = length;
        memset(data, 0, (size_t)length);
        memcpy(data, c->mem->data_buffer, (size_t)actual);
        return actual;
    }
    return length;
}

static int ehci_intr_in(usb_hci_t* hci, usb_device_t* device,
                        usb_endpoint_t* endpoint, void* data, int length) {
    ehci_controller_t* c = hci->priv;
    if (!c || !device || !endpoint || !data || length <= 0) return -1;
    int transfer_length = length > 64 ? 64 : length;
    if (endpoint->max_packet > 0 && transfer_length > (int)endpoint->max_packet) {
        transfer_length = (int)endpoint->max_packet;
    }

    ehci_intr_t* slot = endpoint->hcd_priv;
    if (!slot || !slot->used) {
        for (int i = 0; i < EHCI_MAX_INTR_SLOTS; i++) {
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
        slot->transfer_length = transfer_length;
        endpoint->hcd_priv = slot;

        ehci_program_qh_caps(&slot->qh, device, endpoint->number,
                             endpoint->max_packet ? endpoint->max_packet : 8,
                             false, true);
        memset(&slot->dummy, 0, sizeof(slot->dummy));
        slot->dummy.next = 1;
        slot->dummy.alt_next = 1;
        ehci_qtd_init(c, &slot->qtd, EHCI_QTD_PID_IN, slot->buffer,
                      transfer_length, endpoint->toggle != 0);
        slot->qtd.next = ehci_phys(c, &slot->dummy);
        slot->qh.next_qtd = ehci_phys(c, &slot->qtd);
        slot->qh.alt_qtd = 1;
        slot->qh.token = 0;
        ehci_rebuild_periodic(c);
        return 0;
    }

    dma_sync_for_cpu(&c->dma);
    if (slot->qtd.token & EHCI_QTD_ACTIVE) return 0;
    if (slot->qtd.token & EHCI_QTD_HALTED) {
        endpoint->hcd_priv = NULL;
        slot->used = false;
        ehci_rebuild_periodic(c);
        return -1;
    }
    int remaining = (int)((slot->qtd.token >> 16) & 0x7FFF);
    int actual = slot->transfer_length - remaining;
    if (actual < 0) actual = 0;
    if (actual > length) actual = length;
    if (actual > 64) actual = 64;
    if (actual > 0) memcpy(data, slot->buffer, (size_t)actual);
    endpoint->toggle ^= 1;

    memset(&slot->dummy, 0, sizeof(slot->dummy));
    slot->dummy.next = 1;
    slot->dummy.alt_next = 1;
    ehci_qtd_init(c, &slot->qtd, EHCI_QTD_PID_IN, slot->buffer,
                  slot->transfer_length, endpoint->toggle != 0);
    slot->qtd.next = ehci_phys(c, &slot->dummy);
    slot->qh.next_qtd = ehci_phys(c, &slot->qtd);
    slot->qh.alt_qtd = 1;
    slot->qh.token = 0;
    dma_sync_for_device(&c->dma);
    return actual;
}

static int ehci_bulk(usb_hci_t* hci, usb_device_t* device,
                     usb_endpoint_t* endpoint, void* data, int length,
                     int direction) {
    ehci_controller_t* c = hci->priv;
    if (!c || !device || !endpoint || !data || length <= 0 ||
        length > (int)sizeof(c->mem->data_buffer)) {
        return -1;
    }
    if (direction == USB_DIR_OUT) {
        memcpy(c->mem->data_buffer, data, (size_t)length);
    }
    ehci_qtd_init(c, &c->mem->bulk_qtd,
                  direction == USB_DIR_IN ? EHCI_QTD_PID_IN :
                                            EHCI_QTD_PID_OUT,
                  c->mem->data_buffer, length, endpoint->toggle != 0);
    memset(&c->mem->bulk_dummy, 0, sizeof(c->mem->bulk_dummy));
    c->mem->bulk_dummy.next = 1;
    c->mem->bulk_dummy.alt_next = 1;
    c->mem->bulk_qtd.next = ehci_phys(c, &c->mem->bulk_dummy);

    ehci_qh_t* qh = &c->mem->bulk_qh;
    memset(qh, 0, sizeof(*qh));
    ehci_program_qh_caps(qh, device, endpoint->number,
                         endpoint->max_packet ? endpoint->max_packet : 64,
                         false, false);
    qh->next_qtd = ehci_phys(c, &c->mem->bulk_qtd);
    qh->alt_qtd = 1;
    qh->head = c->mem->async_qh.head;
    c->mem->async_qh.head = ehci_qh_link(c, qh);
    dma_sync_for_device(&c->dma);

    int result = -1;
    for (int timeout = 0; timeout < 500; timeout++) {
        dma_sync_for_cpu(&c->dma);
        if (c->mem->bulk_qtd.token & EHCI_QTD_HALTED) break;
        if (ehci_qtd_complete(&c->mem->bulk_qtd)) {
            result = 0;
            break;
        }
        usb_delay_ms(1);
    }
    c->mem->async_qh.head = ehci_qh_link(c, &c->mem->async_qh);
    dma_sync_for_device(&c->dma);
    ehci_doorbell_wait(c);

    if (result != 0) return -1;
    endpoint->toggle ^= 1;
    if (direction == USB_DIR_IN) {
        int remaining = (int)((c->mem->bulk_qtd.token >> 16) & 0x7FFF);
        int actual = length - remaining;
        if (actual < 0 || actual > length) actual = length;
        memcpy(data, c->mem->data_buffer, (size_t)actual);
        return actual;
    }
    return length;
}

static void ehci_release(usb_hci_t* hci, usb_device_t* device) {
    ehci_controller_t* c = hci->priv;
    if (!c || !device) return;
    for (int i = 0; i < EHCI_MAX_INTR_SLOTS; i++) {
        if (c->mem->interrupts[i].used &&
            c->mem->interrupts[i].device == device) {
            if (c->mem->interrupts[i].endpoint) {
                c->mem->interrupts[i].endpoint->hcd_priv = NULL;
            }
            memset(&c->mem->interrupts[i], 0,
                   sizeof(c->mem->interrupts[i]));
        }
    }
    ehci_rebuild_periodic(c);
    ehci_doorbell_wait(c);
}

static void ehci_write_portsc(ehci_controller_t* c, uint32_t reg,
                              uint32_t value, uint32_t clear_changes) {
    uint32_t write_value = (value & ~EHCI_PORT_RWC) | clear_changes;
    ehci_reg_write(c, reg, write_value);
}

static int ehci_port_reset(usb_hci_t* hci, uint8_t port) {
    ehci_controller_t* c = hci->priv;
    if (!c || port == 0 || port > c->ports) return USB_SPEED_UNKNOWN;
    uint32_t reg = c->operational + 0x44 + 4u * (port - 1);
    uint32_t value = ehci_reg_read(c, reg);
    if (!(value & EHCI_PORT_CONNECT) || (value & EHCI_PORT_OWNER)) {
        return USB_SPEED_UNKNOWN;
    }
    /* FreeBSD ehci.c: Low-Speed K-state check before reset */
    if ((value & EHCI_PORT_LINE_MASK) == EHCI_PORT_LINE_LOW &&
        c->companion_present) {
        return USB_SPEED_UNKNOWN;
    }
    ehci_write_portsc(c, reg,
                      (value & ~EHCI_PORT_ENABLED) | EHCI_PORT_POWER | EHCI_PORT_RESET,
                      value & EHCI_PORT_RWC);
    usb_delay_ms(50);
    value = ehci_reg_read(c, reg);
    ehci_write_portsc(c, reg, value & ~EHCI_PORT_RESET,
                      value & EHCI_PORT_RWC);
    for (int i = 0; i < 100; i++) {
        value = ehci_reg_read(c, reg);
        if (!(value & EHCI_PORT_RESET)) break;
        usb_delay_ms(1);
    }
    usb_delay_ms(20);
    value = ehci_reg_read(c, reg);
    ehci_write_portsc(c, reg, value, value & EHCI_PORT_RWC);
    return (value & EHCI_PORT_ENABLED) ? USB_SPEED_HIGH : USB_SPEED_UNKNOWN;
}

static void ehci_poll(usb_hci_t* hci) {
    ehci_controller_t* c = hci->priv;
    if (!c) return;
    for (uint8_t port = 1; port <= c->ports; port++) {
        uint32_t reg = c->operational + 0x44 + 4u * (port - 1);
        uint32_t bit = 1u << (port - 1);
        uint32_t value = ehci_reg_read(c, reg);
        if (value & EHCI_PORT_RWC) {
            ehci_write_portsc(c, reg, value, value & EHCI_PORT_RWC);
        }
        bool connected = (value & EHCI_PORT_CONNECT) != 0;
        if (!connected || (value & EHCI_PORT_OWNER)) {
            if ((c->ports_seen & bit) && !connected) {
                usb_disconnect_port(hci, port);
                c->ports_seen &= ~bit;
            }
            continue;
        }
        if (c->ports_seen & bit) continue;
        c->ports_seen |= bit;
        uint8_t speed = (uint8_t)ehci_port_reset(hci, port);
        if (speed == USB_SPEED_HIGH) {
            log_printf("[EHCI] root port %u High-Speed connected\n", port);
            (void)usb_enumerate(hci, port, speed);
        } else if (c->companion_present) {
            value = ehci_reg_read(c, reg);
            if (value & EHCI_PORT_CONNECT) {
                ehci_write_portsc(c, reg, value | EHCI_PORT_POWER |
                                  EHCI_PORT_OWNER,
                                  value & EHCI_PORT_RWC);
                log_printf("[EHCI] root port %u routed to physical companion\n",
                           port);
            }
        }
    }
}

static bool ehci_companion_exists(void) {
    int count = pci_get_device_count();
    for (int i = 0; i < count; i++) {
        const pci_device_t* pci = pci_get_device(i);
        if (pci && pci->class_code == 0x0C && pci->subclass == 0x03 &&
            (pci->prog_if == 0x00 || pci->prog_if == 0x10)) {
            return true;
        }
    }
    return false;
}

int ehci_probe(const pci_device_t* device) {
    if (!device || g_ehci_count >= EHCI_MAX_CONTROLLERS) return -1;
    int bar = -1;
    for (int i = 0; i < PCI_BAR_COUNT; i++) {
        if (device->bar[i].valid && !device->bar[i].is_io) {
            bar = i;
            break;
        }
    }
    if (bar < 0 || device->bar[bar].address > UINT32_MAX) return -1;
    if (usb_legacy_handoff(device) != 0) {
        log_printf("[EHCI] BIOS ownership still active; controller skipped\n");
        return -1;
    }

    ehci_controller_t* c = &g_ehci[g_ehci_count];
    memset(c, 0, sizeof(*c));
    c->pci = device;
    c->mmio = usb_map_mmio(device->bar[bar].address, 0x1000);
    if (!c->mmio) return -1;
    pci_enable_memory(device);
    pci_enable_bus_master(device);
    uint8_t cap_length = c->mmio[0];
    c->operational = cap_length;
    uint32_t hcsparams = ehci_reg_read(c, 0x04);
    c->ports = hcsparams & 0x0F;
    if (!c->ports) return -1;
    /* Check HCSPARAMS N_CC (bits 15:12): Intel H55 RMH has N_CC == 0 */
    uint32_t n_cc = (hcsparams >> 12) & 0x0Fu;
    c->companion_present = (n_cc > 0) && ehci_companion_exists();
    if (usb_dma_alloc_zero(sizeof(ehci_dma_t), UINT32_MAX, PAGE_SIZE,
                           &c->dma) != 0) {
        return -1;
    }
    c->mem = (ehci_dma_t*)c->dma.virt_addr;
    memset(c->mem, 0, sizeof(*c->mem));

    ehci_reg_write(c, c->operational + 0x08, 0);
    uint32_t command = ehci_reg_read(c, c->operational);
    ehci_reg_write(c, c->operational, command & ~EHCI_CMD_RUN);
    for (int i = 0; i < 20; i++) {
        if (ehci_reg_read(c, c->operational + 0x04) & EHCI_STS_HALTED) break;
        usb_delay_ms(1);
    }
    ehci_reg_write(c, c->operational, EHCI_CMD_RESET);
    for (int i = 0; i < 100; i++) {
        if (!(ehci_reg_read(c, c->operational) & EHCI_CMD_RESET)) break;
        usb_delay_ms(1);
    }
    if (ehci_reg_read(c, c->operational) & EHCI_CMD_RESET) return -1;
    ehci_reg_write(c, c->operational + 0x10, 0); /* CTRLDSSEGMENT = 0 */
    ehci_reg_write(c, c->operational + 0x04, 0x3F);
    ehci_reg_write(c, c->operational + 0x08, 0);
    ehci_reg_write(c, c->operational + 0x0C, 0);

    uint32_t periodic_phys = ehci_phys(c, c->mem->periodic);
    if (periodic_phys & 0xFFF) return -1;
    for (int i = 0; i < 1024; i++) c->mem->periodic[i] = 1;
    ehci_reg_write(c, c->operational + 0x14, periodic_phys);

    memset(&c->mem->async_qh, 0, sizeof(c->mem->async_qh));
    c->mem->async_qh.head =
        ehci_qh_link(c, &c->mem->async_qh);
    c->mem->async_qh.epchar = (1u << 15);
    c->mem->async_qh.next_qtd = 1;
    c->mem->async_qh.alt_qtd = 1;
    c->mem->async_qh.token = EHCI_QTD_HALTED;
    dma_sync_for_device(&c->dma);

    ehci_reg_write(c, c->operational + 0x18,
                   ehci_phys(c, &c->mem->async_qh));
    ehci_reg_write(c, c->operational + 0x40, 1);
    ehci_reg_write(c, c->operational,
                   (8u << 16) | EHCI_CMD_RUN | EHCI_CMD_PERIODIC | EHCI_CMD_ASYNC);
    usb_delay_ms(20);

    for (uint8_t port = 1; port <= c->ports; port++) {
        uint32_t reg = c->operational + 0x44 + 4u * (port - 1);
        uint32_t value = ehci_reg_read(c, reg);
        ehci_write_portsc(c, reg, (value & ~EHCI_PORT_OWNER) | EHCI_PORT_POWER,
                          value & EHCI_PORT_RWC);
    }
    /* Wait 150ms for root ports and integrated Rate Matching Hub (RMH) to power up */
    usb_delay_ms(150);

    c->hci.name = "EHCI 2.0";
    c->hci.kind = USB_HCI_KIND_EHCI;
    c->hci.priv = c;
    c->hci.root_ports = c->ports;
    c->hci.supports_hubs = true;
    c->hci.control = ehci_control;
    c->hci.bulk = ehci_bulk;
    c->hci.intr_in = ehci_intr_in;
    c->hci.port_reset = ehci_port_reset;
    c->hci.device_release = ehci_release;
    c->hci.poll = ehci_poll;
    usb_register_hci(&c->hci);
    g_ehci_count++;
    ehci_poll(&c->hci);
    return 0;
}
