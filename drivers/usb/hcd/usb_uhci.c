#include "usb_uhci.h"
#include "usb_core.h"
#include "usb_compat.h"
#include "dma.h"
#include "log.h"
#include "port.h"
#include "string.h"

#define UHCI_MAX_CONTROLLERS 4
#define UHCI_MAX_INTR_SLOTS  12
#define UHCI_MAX_DATA_TDS    64

#define UHCI_CMD_RUN      0x0001
#define UHCI_CMD_RESET    0x0002
#define UHCI_CMD_GRESET   0x0004
#define UHCI_CMD_MAXPACK  0x0080
#define UHCI_STS_HALTED   0x0020

#define UHCI_PORT_CONNECT  0x0001
#define UHCI_PORT_CONN_CHG 0x0002
#define UHCI_PORT_ENABLED  0x0004
#define UHCI_PORT_EN_CHG   0x0008
#define UHCI_PORT_LOW      0x0100
#define UHCI_PORT_RESET    0x0200
#define UHCI_PORT_SUSPEND  0x1000
#define UHCI_PORT_CHANGES  (UHCI_PORT_CONN_CHG | UHCI_PORT_EN_CHG)

#define UHCI_TD_ACTIVE  (1u << 23)
#define UHCI_TD_STALLED (1u << 22)
#define UHCI_TD_DBUFFER (1u << 21)
#define UHCI_TD_BABBLE  (1u << 20)
#define UHCI_TD_NAK     (1u << 19)
#define UHCI_TD_CRCTO   (1u << 18)
#define UHCI_TD_BITSTUFF (1u << 17)
#define UHCI_TD_ERROR   (UHCI_TD_STALLED | UHCI_TD_DBUFFER | UHCI_TD_BABBLE | UHCI_TD_CRCTO | UHCI_TD_BITSTUFF)

typedef struct {
    volatile uint32_t link;
    volatile uint32_t control;
    volatile uint32_t token;
    volatile uint32_t buffer;
    uint32_t reserved[4];
} __attribute__((aligned(16))) uhci_td_t;

typedef struct {
    volatile uint32_t head;
    volatile uint32_t element;
    uint32_t reserved[6];
} __attribute__((aligned(16))) uhci_qh_t;

typedef struct {
    bool used;
    usb_device_t* device;
    usb_endpoint_t* endpoint;
    int req_length;
    uhci_qh_t qh;
    uhci_td_t td;
    uint8_t buffer[64] __attribute__((aligned(16)));
} uhci_intr_t;

typedef struct {
    uint32_t frame_list[1024] __attribute__((aligned(4096)));
    uhci_qh_t control_qh;
    uhci_td_t setup_td;
    uhci_td_t data_tds[UHCI_MAX_DATA_TDS];
    uhci_td_t status_td;
    uint8_t setup_buffer[8] __attribute__((aligned(16)));
    uint8_t data_buffer[1024] __attribute__((aligned(16)));
    uhci_intr_t interrupts[UHCI_MAX_INTR_SLOTS];
} uhci_dma_t;

typedef struct {
    usb_hci_t hci;
    const pci_device_t* pci;
    uint16_t io_base;
    dma_buffer_t dma;
    uhci_dma_t* mem;
    uint32_t ports_seen;
} uhci_controller_t;

static uhci_controller_t g_uhci[UHCI_MAX_CONTROLLERS];
static uint32_t g_uhci_count = 0;

static void uhci_release_intr_slot(uhci_controller_t* controller,
                                   uhci_intr_t* slot);

static uint32_t uhci_phys(uhci_controller_t* c, const void* pointer) {
    return (uint32_t)(c->dma.phys_addr +
                      ((const uint8_t*)pointer - (uint8_t*)c->mem));
}

static uint32_t uhci_link_qh(uhci_controller_t* c, uhci_qh_t* qh) {
    return (uhci_phys(c, qh) & ~0x0Fu) | 2u;
}

static uint32_t uhci_length(int length) {
    if (length <= 0) return 0x7FFu;
    return (uint32_t)(length - 1);
}

static bool uhci_stop(uhci_controller_t* c, uint16_t* saved_command) {
    uint16_t command = inw(c->io_base);
    *saved_command = command;
    if (!(command & UHCI_CMD_RUN)) return false;
    outw(c->io_base, (uint16_t)(command & ~UHCI_CMD_RUN));
    for (int timeout = 0; timeout < 20; timeout++) {
        if (inw(c->io_base + 0x02) & UHCI_STS_HALTED) break;
        usb_delay_ms(1);
    }
    return true;
}

static void uhci_rebuild(uhci_controller_t* c) {
    uint16_t command = 0;
    bool was_running = uhci_stop(c, &command);
    uhci_qh_t* first = NULL;
    uhci_qh_t* previous = NULL;
    for (int i = 0; i < UHCI_MAX_INTR_SLOTS; i++) {
        uhci_intr_t* slot = &c->mem->interrupts[i];
        if (!slot->used) continue;
        slot->qh.head = 1;
        if (previous) previous->head = uhci_link_qh(c, &slot->qh);
        else first = &slot->qh;
        previous = &slot->qh;
    }
    c->mem->control_qh.head = first ? uhci_link_qh(c, first) : 1;
    for (int i = 0; i < 1024; i++) {
        c->mem->frame_list[i] = uhci_link_qh(c, &c->mem->control_qh);
    }
    if (was_running) outw(c->io_base, (uint16_t)(command | UHCI_CMD_RUN));
}

static void uhci_program_td(uhci_controller_t* c, uhci_td_t* td,
                            uint8_t pid, uint8_t address, uint8_t endpoint,
                            bool toggle, bool low_speed, bool spd,
                            uint32_t data_phys, int length) {
    memset((void*)td, 0, sizeof(*td));
    td->control = UHCI_TD_ACTIVE | (3u << 27) |
                  (spd ? (1u << 29) : 0u) |
                  (low_speed ? (1u << 26) : 0u);
    td->token = pid | ((uint32_t)(address & 0x7F) << 8) |
                ((uint32_t)(endpoint & 0x0F) << 15) |
                ((toggle ? 1u : 0u) << 19) |
                (uhci_length(length) << 21);
    td->buffer = data_phys;
    (void)c;
}

static void uhci_release_intr_slot(uhci_controller_t* controller,
                                   uhci_intr_t* slot) {
    if (!controller || !slot || !slot->used) return;
    if (slot->endpoint) slot->endpoint->hcd_priv = NULL;
    uint16_t command = 0;
    bool was_running = uhci_stop(controller, &command);
    memset(slot, 0, sizeof(*slot));
    dma_sync_for_device(&controller->dma);
    uhci_rebuild(controller);
    if (was_running) outw(controller->io_base, (uint16_t)(command | UHCI_CMD_RUN));
    dma_sync_for_device(&controller->dma);
}

static int uhci_control(usb_hci_t* hci, usb_device_t* device,
                        const usb_setup_packet_t* setup, void* data,
                        int length, int direction) {
    uhci_controller_t* c = hci->priv;
    if (!c || !device || !setup || length < 0 || length > 1024) return -1;
    dma_sync_for_device(&c->dma);

    memcpy(c->mem->setup_buffer, setup, 8);
    if (length > 0 && data && direction == USB_DIR_OUT) {
        memcpy(c->mem->data_buffer, data, (size_t)length);
    }

    bool low = device->speed == USB_SPEED_LOW;
    int max_pkt = device->max_packet0 ? (int)device->max_packet0 : (low ? 8 : 64);
    if (max_pkt < 8) max_pkt = 8;
    if (max_pkt > 64) max_pkt = 64;

    uhci_program_td(c, &c->mem->setup_td, 0x2D, device->address, 0,
                    false, low, false, uhci_phys(c, c->mem->setup_buffer), 8);

    int num_data_tds = 0;
    if (length > 0) {
        uint8_t pid = direction == USB_DIR_IN ? 0x69 : 0xE1;
        bool toggle = true;
        int offset = 0;
        while (offset < length && num_data_tds < UHCI_MAX_DATA_TDS) {
            int chunk = length - offset;
            if (chunk > max_pkt) chunk = max_pkt;
            uhci_td_t* td = &c->mem->data_tds[num_data_tds];
            uhci_program_td(c, td, pid, device->address, 0,
                            toggle, low, direction == USB_DIR_IN,
                            uhci_phys(c, c->mem->data_buffer + offset), chunk);
            if (num_data_tds > 0) {
                c->mem->data_tds[num_data_tds - 1].link =
                    (uhci_phys(c, td) & ~0x0Fu) | 4u;
            }
            toggle = !toggle;
            offset += chunk;
            num_data_tds++;
        }
        c->mem->setup_td.link = (uhci_phys(c, &c->mem->data_tds[0]) & ~0x0Fu) | 4u;
        c->mem->data_tds[num_data_tds - 1].link =
            (uhci_phys(c, &c->mem->status_td) & ~0x0Fu) | 4u;
    } else {
        c->mem->setup_td.link = (uhci_phys(c, &c->mem->status_td) & ~0x0Fu) | 4u;
    }

    uhci_program_td(c, &c->mem->status_td,
                    (length > 0 && direction == USB_DIR_IN) ? 0xE1 : 0x69,
                    device->address, 0, true, low, false, 0, 0);
    c->mem->status_td.link = 1;
    c->mem->control_qh.element = uhci_phys(c, &c->mem->setup_td) & ~0x0Fu;
    dma_sync_for_device(&c->dma);

    outw(c->io_base, (uint16_t)(inw(c->io_base) | UHCI_CMD_RUN));
    int result = -1;
    bool failed = false;
    for (int timeout = 0; timeout < 500; timeout++) {
        dma_sync_for_cpu(&c->dma);
        if (c->mem->setup_td.control & UHCI_TD_ERROR) {
            failed = true;
            break;
        }
        for (int k = 0; k < num_data_tds; k++) {
            uint32_t ctrl = c->mem->data_tds[k].control;
            if (ctrl & UHCI_TD_ERROR) {
                failed = true;
                break;
            }
            if (!(ctrl & UHCI_TD_ACTIVE)) {
                int act = (int)((ctrl + 1u) & 0x7FFu);
                int expected = (k == num_data_tds - 1)
                                   ? (length - k * max_pkt)
                                   : max_pkt;
                if (act < expected &&
                    (c->mem->status_td.control & UHCI_TD_ACTIVE)) {
                    /* Short packet: jump QH directly to status stage */
                    c->mem->control_qh.element =
                        uhci_phys(c, &c->mem->status_td) & ~0x0Fu;
                    dma_sync_for_device(&c->dma);
                    break;
                }
            } else {
                break;
            }
        }
        if (failed) break;
        if (c->mem->status_td.control & UHCI_TD_ERROR) {
            failed = true;
            break;
        }
        if (!(c->mem->status_td.control & UHCI_TD_ACTIVE)) {
            result = 0;
            break;
        }
        usb_delay_ms(1);
    }
    c->mem->control_qh.element = 1;
    dma_sync_for_cpu(&c->dma);
    if (result < 0) {
        uint32_t d_ctrl = num_data_tds > 0 ? c->mem->data_tds[0].control : 0;
        for (int k = 0; k < num_data_tds; k++) {
            if (c->mem->data_tds[k].control & UHCI_TD_ERROR) {
                d_ctrl = c->mem->data_tds[k].control;
                break;
            }
        }
        log_printf("[UHCI] control failed setup=0x%08x data=0x%08x status=0x%08x\n",
                   c->mem->setup_td.control, d_ctrl,
                   c->mem->status_td.control);
        return -1;
    }

    if (length > 0 && direction == USB_DIR_IN && data) {
        int total_actual = 0;
        for (int k = 0; k < num_data_tds; k++) {
            uint32_t ctrl = c->mem->data_tds[k].control;
            if (ctrl & UHCI_TD_ACTIVE) break;
            int act = (int)((ctrl + 1u) & 0x7FFu);
            int expected = (k == num_data_tds - 1)
                               ? (length - k * max_pkt)
                               : max_pkt;
            if (act > expected) act = expected;
            total_actual += act;
            if (act < expected) break;
        }
        if (total_actual > length) total_actual = length;
        memcpy(data, c->mem->data_buffer, (size_t)total_actual);
        result = total_actual;
    } else {
        result = length;
    }
    return result;
}

static int uhci_intr_in(usb_hci_t* hci, usb_device_t* device,
                        usb_endpoint_t* endpoint, void* data, int length) {
    uhci_controller_t* c = hci->priv;
    if (!c || !device || !endpoint || !data || length <= 0) return -1;
    int xfer_len = length > 64 ? 64 : length;
    if (endpoint->max_packet > 0 && xfer_len > (int)endpoint->max_packet) {
        xfer_len = (int)endpoint->max_packet;
    }

    uhci_intr_t* slot = endpoint->hcd_priv;
    if (!slot || !slot->used) {
        for (int i = 0; i < UHCI_MAX_INTR_SLOTS; i++) {
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
        slot->req_length = xfer_len;
        endpoint->hcd_priv = slot;
        uint32_t phys = uhci_phys(c, slot->buffer);
        uhci_program_td(c, &slot->td, 0x69, device->address,
                        endpoint->number, endpoint->toggle,
                        device->speed == USB_SPEED_LOW, true, phys,
                        xfer_len);
        slot->td.link = 1;
        slot->qh.element = uhci_phys(c, &slot->td) & ~0x0Fu;
        uhci_rebuild(c);
        dma_sync_for_device(&c->dma);
    }

    dma_sync_for_cpu(&c->dma);
    uint32_t status = slot->td.control;
    if (status & UHCI_TD_ACTIVE) return 0;
    if (status & UHCI_TD_ERROR) {
        if (status & UHCI_TD_STALLED) {
            log_printf("[UHCI] interrupt endpoint 0x%02x stalled\n",
                       endpoint->address);
            uhci_release_intr_slot(c, slot);
            return -1;
        }
        /* Recover from transient CRC/bitstuff error */
        uint32_t phys = uhci_phys(c, slot->buffer);
        uhci_program_td(c, &slot->td, 0x69, device->address,
                        endpoint->number, endpoint->toggle,
                        device->speed == USB_SPEED_LOW, true, phys,
                        slot->req_length);
        slot->td.link = 1;
        slot->qh.element = uhci_phys(c, &slot->td) & ~0x0Fu;
        dma_sync_for_device(&c->dma);
        return 0;
    }

    int actual = (int)((status + 1u) & 0x7FFu);
    if (actual > length) actual = length;
    if (actual > 64) actual = 64;
    if (actual > 0) memcpy(data, slot->buffer, (size_t)actual);
    endpoint->toggle ^= 1;
    uint32_t phys = uhci_phys(c, slot->buffer);
    uhci_program_td(c, &slot->td, 0x69, device->address,
                    endpoint->number, endpoint->toggle,
                    device->speed == USB_SPEED_LOW, true, phys,
                    slot->req_length);
    slot->td.link = 1;
    slot->qh.element = uhci_phys(c, &slot->td) & ~0x0Fu;
    dma_sync_for_device(&c->dma);
    return actual;
}

static int uhci_bulk(usb_hci_t* hci, usb_device_t* device,
                     usb_endpoint_t* endpoint, void* data, int length,
                     int direction) {
    uhci_controller_t* c = hci->priv;
    if (!c || !device || !endpoint || !data || length <= 0 ||
        length > (int)sizeof(c->mem->data_buffer)) {
        return -1;
    }
    if (direction == USB_DIR_OUT) {
        memcpy(c->mem->data_buffer, data, (size_t)length);
    }
    int max_pkt = endpoint->max_packet ? (int)endpoint->max_packet : 64;
    if (max_pkt < 8) max_pkt = 8;
    if (max_pkt > 64) max_pkt = 64;

    uint8_t pid = direction == USB_DIR_IN ? 0x69 : 0xE1;
    int num_tds = 0;
    int offset = 0;
    uint8_t toggle = endpoint->toggle;
    while (offset < length && num_tds < UHCI_MAX_DATA_TDS) {
        int chunk = length - offset;
        if (chunk > max_pkt) chunk = max_pkt;
        uhci_td_t* td = &c->mem->data_tds[num_tds];
        uhci_program_td(c, td, pid, device->address, endpoint->number,
                        toggle != 0, device->speed == USB_SPEED_LOW,
                        direction == USB_DIR_IN,
                        uhci_phys(c, c->mem->data_buffer + offset), chunk);
        if (num_tds > 0) {
            c->mem->data_tds[num_tds - 1].link =
                (uhci_phys(c, td) & ~0x0Fu) | 4u;
        }
        toggle ^= 1;
        offset += chunk;
        num_tds++;
    }
    c->mem->data_tds[num_tds - 1].link = 1;
    c->mem->control_qh.element =
        uhci_phys(c, &c->mem->data_tds[0]) & ~0x0Fu;
    dma_sync_for_device(&c->dma);
    outw(c->io_base, (uint16_t)(inw(c->io_base) | UHCI_CMD_RUN));

    int result = -1;
    bool failed = false;
    for (int timeout = 0; timeout < 500; timeout++) {
        dma_sync_for_cpu(&c->dma);
        bool all_done = true;
        for (int k = 0; k < num_tds; k++) {
            uint32_t ctrl = c->mem->data_tds[k].control;
            if (ctrl & UHCI_TD_ERROR) {
                failed = true;
                break;
            }
            if (ctrl & UHCI_TD_ACTIVE) {
                all_done = false;
                break;
            }
            int act = (int)((ctrl + 1u) & 0x7FFu);
            int expected = (k == num_tds - 1) ? (length - k * max_pkt) : max_pkt;
            if (act < expected) {
                all_done = true;
                break;
            }
        }
        if (failed) break;
        if (all_done) {
            result = 0;
            break;
        }
        usb_delay_ms(1);
    }
    c->mem->control_qh.element = 1;
    dma_sync_for_cpu(&c->dma);
    if (result != 0) return -1;

    int total_actual = 0;
    for (int k = 0; k < num_tds; k++) {
        uint32_t ctrl = c->mem->data_tds[k].control;
        if (ctrl & UHCI_TD_ACTIVE) break;
        endpoint->toggle ^= 1;
        int act = (int)((ctrl + 1u) & 0x7FFu);
        int expected = (k == num_tds - 1) ? (length - k * max_pkt) : max_pkt;
        if (act > expected) act = expected;
        total_actual += act;
        if (act < expected) break;
    }
    if (direction == USB_DIR_IN) {
        if (total_actual > length) total_actual = length;
        memcpy(data, c->mem->data_buffer, (size_t)total_actual);
        return total_actual;
    }
    return length;
}

static void uhci_device_release(usb_hci_t* hci, usb_device_t* device) {
    uhci_controller_t* c = hci->priv;
    if (!c || !device) return;
    uint16_t command = 0;
    bool was_running = uhci_stop(c, &command);
    for (int i = 0; i < UHCI_MAX_INTR_SLOTS; i++) {
        if (c->mem->interrupts[i].used &&
            c->mem->interrupts[i].device == device) {
            if (c->mem->interrupts[i].endpoint) {
                c->mem->interrupts[i].endpoint->hcd_priv = NULL;
            }
            memset(&c->mem->interrupts[i], 0,
                   sizeof(c->mem->interrupts[i]));
        }
    }
    dma_sync_for_device(&c->dma);
    uhci_rebuild(c);
    if (was_running) outw(c->io_base, (uint16_t)(command | UHCI_CMD_RUN));
    dma_sync_for_device(&c->dma);
}

static int uhci_port_reset(usb_hci_t* hci, uint8_t port) {
    uhci_controller_t* c = hci->priv;
    if (!c || port < 1 || port > 2) return USB_SPEED_UNKNOWN;
    uint16_t reg = c->io_base + (port == 1 ? 0x10 : 0x12);
    uint16_t status = inw(reg);
    if (!(status & UHCI_PORT_CONNECT)) return USB_SPEED_UNKNOWN;

    /* FreeBSD uhci.c port reset sequence (RWC bits masked properly) */
    outw(reg, (uint16_t)(UHCI_PORT_RESET));
    usb_delay_ms(50);
    outw(reg, (uint16_t)0);
    usb_delay_ms(10);

    for (int i = 0; i < 10; i++) {
        outw(reg, (uint16_t)(UHCI_PORT_ENABLED | UHCI_PORT_CHANGES));
        usb_delay_ms(5);
        status = inw(reg);
        if (!(status & UHCI_PORT_CONNECT)) return USB_SPEED_UNKNOWN;
        if (status & UHCI_PORT_ENABLED) break;
    }
    if (status & UHCI_PORT_CHANGES) {
        outw(reg, (uint16_t)((status & ~UHCI_PORT_SUSPEND) | UHCI_PORT_CHANGES));
    }
    usb_delay_ms(10);
    status = inw(reg);
    if (!(status & UHCI_PORT_CONNECT) || !(status & UHCI_PORT_ENABLED)) {
        return USB_SPEED_UNKNOWN;
    }
    return (status & UHCI_PORT_LOW) ? USB_SPEED_LOW : USB_SPEED_FULL;
}

static void uhci_poll(usb_hci_t* hci) {
    uhci_controller_t* c = hci->priv;
    if (!c) return;
    for (uint8_t port = 1; port <= 2; port++) {
        uint16_t reg = c->io_base + (port == 1 ? 0x10 : 0x12);
        uint32_t bit = 1u << (port - 1);
        uint16_t status = inw(reg);
        if (status & UHCI_PORT_CHANGES) {
            outw(reg, (uint16_t)((status & ~UHCI_PORT_SUSPEND) | UHCI_PORT_CHANGES));
        }
        if (!(status & UHCI_PORT_CONNECT)) {
            if (c->ports_seen & bit) {
                usb_disconnect_port(hci, port);
                c->ports_seen &= ~bit;
            }
            continue;
        }
        if (c->ports_seen & bit) continue;
        c->ports_seen |= bit;
        uint8_t speed = (uint8_t)uhci_port_reset(hci, port);
        if (speed) {
            log_printf("[UHCI] root port %u %s connected\n",
                       port, usb_speed_name(speed));
            (void)usb_enumerate(hci, port, speed);
        }
    }
}

int uhci_probe(const pci_device_t* device) {
    if (!device || g_uhci_count >= UHCI_MAX_CONTROLLERS) return -1;
    int io_bar = -1;
    for (int i = 0; i < PCI_BAR_COUNT; i++) {
        if (device->bar[i].valid && device->bar[i].is_io) {
            io_bar = i;
            break;
        }
    }
    if (io_bar < 0 || device->bar[io_bar].address > 0xFFFF) return -1;
    if (usb_legacy_handoff(device) != 0) {
        log_printf("[UHCI] Legacy ownership not released; PS/2 fallback remains active\n");
        return -1;
    }

    /* FreeBSD uhci_pci.c: Disable UHCI Legacy Support SMI at PCI reg 0xC0 */
    uint32_t legsup = pci_config_read(device->bus, device->slot,
                                      device->function, 0xC0);
    pci_config_write(device->bus, device->slot, device->function,
                     0xC0, (legsup & 0xFFFF0000u) | 0x8F00u);

    uhci_controller_t* c = &g_uhci[g_uhci_count];
    memset(c, 0, sizeof(*c));
    c->pci = device;
    c->io_base = (uint16_t)(device->bar[io_bar].address & ~0x1Fu);
    if (!c->io_base) return -1;
    if (usb_dma_alloc_zero(sizeof(uhci_dma_t), UINT32_MAX, PAGE_SIZE,
                           &c->dma) != 0) {
        return -1;
    }
    c->mem = (uhci_dma_t*)c->dma.virt_addr;
    pci_enable_io(device);
    pci_enable_bus_master(device);

    /* FreeBSD uhci_init: Global reset followed by Host Controller Reset */
    outw(c->io_base, UHCI_CMD_GRESET);
    usb_delay_ms(20);
    outw(c->io_base, 0);
    usb_delay_ms(10);

    outw(c->io_base, UHCI_CMD_RESET);
    for (int i = 0; i < 100 && (inw(c->io_base) & UHCI_CMD_RESET); i++) {
        usb_delay_ms(1);
    }
    outw(c->io_base, 0);
    outw(c->io_base + 0x02, 0x003F);
    outw(c->io_base + 0x04, 0);
    outb(c->io_base + 0x0C, 0x40);

    memset(c->mem, 0, sizeof(*c->mem));
    c->mem->control_qh.head = 1;
    c->mem->control_qh.element = 1;
    uhci_rebuild(c);
    uint32_t frame_phys = uhci_phys(c, c->mem->frame_list);
    if (frame_phys & 0xFFF) {
        log_printf("[UHCI] Frame list is not 4KB aligned\n");
        return -1;
    }
    outl(c->io_base + 0x08, frame_phys);
    outw(c->io_base + 0x06, 0);
    outw(c->io_base, UHCI_CMD_RUN | UHCI_CMD_MAXPACK);
    usb_delay_ms(10);
    if (inw(c->io_base + 0x02) & UHCI_STS_HALTED) {
        log_printf("[UHCI] Controller remained halted\n");
        return -1;
    }

    c->hci.name = "UHCI 1.1";
    c->hci.kind = USB_HCI_KIND_UHCI;
    c->hci.priv = c;
    c->hci.root_ports = 2;
    c->hci.supports_hubs = true;
    c->hci.control = uhci_control;
    c->hci.bulk = uhci_bulk;
    c->hci.intr_in = uhci_intr_in;
    c->hci.port_reset = uhci_port_reset;
    c->hci.device_release = uhci_device_release;
    c->hci.poll = uhci_poll;
    usb_register_hci(&c->hci);
    g_uhci_count++;
    uhci_poll(&c->hci);
    return 0;
}
