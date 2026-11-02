#include "usb_xhci.h"
#include "usb_core.h"
#include "usb_compat.h"
#include "dma.h"
#include "log.h"
#include "string.h"

#define XHCI_MAX_CONTROLLERS 2
#define XHCI_MAX_SLOTS 16
#define XHCI_RING_SIZE 64
#define XHCI_EVENT_RING_SIZE 128
#define XHCI_ARENA_SIZE (256u * 1024u)

#define XHCI_CMD_RUN       (1u << 0)
#define XHCI_CMD_RESET     (1u << 1)
#define XHCI_CMD_INTE      (1u << 2)
#define XHCI_STS_HALTED    (1u << 0)
#define XHCI_STS_HOSTERR   (1u << 2)
#define XHCI_STS_CNR       (1u << 11)

#define XHCI_PORT_CCS      (1u << 0)
#define XHCI_PORT_PED      (1u << 1)
#define XHCI_PORT_OCA      (1u << 3)
#define XHCI_PORT_PR       (1u << 4)
#define XHCI_PORT_PP       (1u << 9)
#define XHCI_PORT_CSC      (1u << 17)
#define XHCI_PORT_PEC      (1u << 18)
#define XHCI_PORT_WRC      (1u << 19)
#define XHCI_PORT_PRC      (1u << 21)
#define XHCI_PORT_PLC      (1u << 22)
#define XHCI_PORT_CEC      (1u << 23)
#define XHCI_PORT_CHANGES  (XHCI_PORT_CSC | XHCI_PORT_PEC | XHCI_PORT_WRC | \
                            XHCI_PORT_PRC | XHCI_PORT_PLC | XHCI_PORT_CEC)

#define XHCI_TRB_NORMAL    1u
#define XHCI_TRB_SETUP     2u
#define XHCI_TRB_DATA      3u
#define XHCI_TRB_STATUS    4u
#define XHCI_TRB_LINK      6u
#define XHCI_TRB_ENABLE_SLOT 9u
#define XHCI_TRB_ADDRESS_DEVICE 11u
#define XHCI_TRB_CONFIGURE_ENDPOINT 12u
#define XHCI_TRB_DISABLE_SLOT 10u
#define XHCI_TRB_TRANSFER_EVENT 32u
#define XHCI_TRB_COMMAND_EVENT 33u

typedef struct {
    volatile uint64_t parameter;
    volatile uint32_t status;
    volatile uint32_t control;
} __attribute__((aligned(16))) xhci_trb_t;

typedef struct {
    void* virtual_base;
    uint64_t physical_base;
    uint32_t producer;
    uint8_t cycle;
} xhci_ring_t;

typedef struct {
    bool allocated;
    uint8_t slot_id;
    uint8_t interrupt_dci;
    bool interrupt_configured;
    bool interrupt_pending;
    usb_device_t* device;
    uint8_t* output_context;
    uint64_t output_context_phys;
    xhci_ring_t ep0_ring;
    xhci_ring_t interrupt_ring;
    uint8_t* report_buffer;
    uint64_t report_buffer_phys;
    uint8_t* transfer_buffer;
    uint64_t transfer_buffer_phys;
} xhci_slot_t;

typedef struct {
    volatile uint8_t* cap;
    volatile uint8_t* op;
    volatile uint8_t* runtime;
    volatile uint8_t* doorbell;
    const pci_device_t* pci;
    usb_hci_t hci;
    dma_buffer_t arena;
    uint8_t* memory;
    uint64_t memory_phys;
    size_t arena_cursor;
    uint32_t max_slots;
    uint32_t max_ports;
    uint8_t context_size;
    uint8_t cap_length;
    uint32_t operational;
    uint32_t runtime_offset;
    uint32_t doorbell_offset;
    xhci_ring_t command_ring;
    xhci_trb_t* event_ring;
    uint64_t event_ring_phys;
    uint32_t event_index;
    uint8_t event_cycle;
    uint64_t* dcbaa;
    uint64_t dcbaa_phys;
    uint8_t* input_context;
    uint64_t input_context_phys;
    dma_buffer_t scratchpad;
    xhci_slot_t slots[XHCI_MAX_SLOTS];
    uint32_t ports_seen;
} xhci_controller_t;

static xhci_controller_t g_xhci[XHCI_MAX_CONTROLLERS];
static uint32_t g_xhci_count = 0;
static uint32_t g_xhci_event_logs = 0;

static uint32_t xhci_read32(xhci_controller_t* c, uint32_t offset) {
    return *(volatile uint32_t*)(c->cap + offset);
}

static void xhci_write32(xhci_controller_t* c, uint32_t offset, uint32_t value) {
    *(volatile uint32_t*)(c->cap + offset) = value;
}

static void xhci_write64(xhci_controller_t* c, uint32_t offset, uint64_t value) {
    *(volatile uint64_t*)(c->cap + offset) = value;
}

static void* xhci_arena_alloc(xhci_controller_t* c, size_t size,
                              size_t alignment, uint64_t* physical) {
    if (!c || !size || !alignment) return NULL;
    size_t cursor = (c->arena_cursor + alignment - 1) & ~(alignment - 1);
    if (cursor > c->arena.size || size > c->arena.size - cursor) return NULL;
    c->arena_cursor = cursor + size;
    if (physical) *physical = c->memory_phys + cursor;
    return c->memory + cursor;
}

static void xhci_ring_init(xhci_controller_t* c, xhci_ring_t* ring) {
    uint64_t physical = 0;
    ring->virtual_base = xhci_arena_alloc(
        c, sizeof(xhci_trb_t) * XHCI_RING_SIZE, 64, &physical);
    ring->physical_base = physical;
    ring->producer = 0;
    ring->cycle = 1;
    memset(ring->virtual_base, 0, sizeof(xhci_trb_t) * XHCI_RING_SIZE);
}

static xhci_trb_t* xhci_ring_push(xhci_controller_t* c, xhci_ring_t* ring,
                                  uint64_t parameter, uint32_t status,
                                  uint32_t control) {
    if (!ring || !ring->virtual_base) return NULL;
    xhci_trb_t* trbs = (xhci_trb_t*)ring->virtual_base;
    if (ring->producer >= XHCI_RING_SIZE - 1) {
        xhci_trb_t* link = &trbs[XHCI_RING_SIZE - 1];
        memset((void*)link, 0, sizeof(*link));
        link->parameter = ring->physical_base;
        link->control = (XHCI_TRB_LINK << 10) |
                        (ring->cycle ? 1u : 0u) | (1u << 1);
        ring->cycle ^= 1;
        ring->producer = 0;
    }
    xhci_trb_t* trb = &trbs[ring->producer++];
    trb->parameter = parameter;
    trb->status = status;
    trb->control = control | (ring->cycle ? 1u : 0u);
    dma_sync_for_device(&c->arena);
    (void)c;
    return trb;
}

static bool xhci_event_next(xhci_controller_t* c, xhci_trb_t* event) {
    xhci_trb_t* trb = &c->event_ring[c->event_index];
    uint32_t control = trb->control;
    if ((control & 1u) != c->event_cycle) return false;
    *event = *trb;
    memset(trb, 0, sizeof(*trb));
    c->event_index++;
    if (c->event_index >= XHCI_EVENT_RING_SIZE) {
        c->event_index = 0;
        c->event_cycle ^= 1;
    }
    volatile uint64_t* erdp = (volatile uint64_t*)
        (c->runtime + 0x20 + 0x18);
    uint64_t next_phys = c->event_ring_phys +
                         (uint64_t)c->event_index * sizeof(xhci_trb_t);
    *erdp = (next_phys & ~0x7ULL) | (1u << 3);
    dma_sync_for_device(&c->arena);
    return true;
}

static int xhci_wait_event(xhci_controller_t* c, uint8_t wanted_type,
                           xhci_trb_t* result, int timeout_ms) {
    for (int elapsed = 0; elapsed < timeout_ms; elapsed++) {
        xhci_trb_t event;
        while (xhci_event_next(c, &event)) {
            uint8_t type = (event.control >> 10) & 0x3F;
            if (g_xhci_event_logs++ < 24) {
                log_printf("[xHCI] event type=%u status=0x%08x control=0x%08x code=%u slot=%u ep=%u\n",
                           type, event.status, event.control,
                           (event.control >> 24) & 0xFF,
                           event.control & 0xFF,
                           (event.control >> 16) & 0x1F);
            }
            if (type == wanted_type) {
                if (result) *result = event;
                return 0;
            }
        }
        usb_delay_ms(1);
    }
    return -1;
}

static int xhci_command(xhci_controller_t* c, uint64_t parameter,
                        uint32_t status, uint32_t control,
                        xhci_trb_t* completed_event) {
    xhci_ring_push(c, &c->command_ring, parameter, status, control);
    volatile uint32_t* doorbell = (volatile uint32_t*)
        (c->doorbell + 0);
    *doorbell = 0;
    xhci_trb_t event;
    if (xhci_wait_event(c, XHCI_TRB_COMMAND_EVENT, &event, 500) != 0) {
        log_printf("[xHCI] command completion timeout\n");
        return -1;
    }
    if (completed_event) *completed_event = event;
    uint8_t code = (uint8_t)(event.control >> 24);
    if (code != 1) {
        log_printf("[xHCI] command failed completion=%u type=%u\n",
                   code, (event.control >> 10) & 0x3F);
        return -1;
    }
    return 0;
}

static uint8_t xhci_speed_code(uint8_t speed) {
    switch (speed) {
        case USB_SPEED_LOW: return 2;
        case USB_SPEED_FULL: return 1;
        case USB_SPEED_HIGH: return 3;
        case USB_SPEED_SUPER: return 4;
        default: return 1;
    }
}

static xhci_slot_t* xhci_slot_for_device(xhci_controller_t* c,
                                         usb_device_t* device) {
    (void)c;
    if (!device) return NULL;
    if (device->hcd_priv) return (xhci_slot_t*)device->hcd_priv;
    return NULL;
}

static int xhci_enable_slot(xhci_controller_t* c, uint8_t* slot_id) {
    xhci_trb_t event;
    if (xhci_command(c, 0, 0, XHCI_TRB_ENABLE_SLOT << 10, &event) != 0) {
        return -1;
    }
    uint8_t id = (uint8_t)(event.control & 0xFF);
    if (id == 0 || id > c->max_slots) {
        log_printf("[xHCI] Enable Slot returned invalid slot %u\n", id);
        return -1;
    }
    xhci_slot_t* slot = &c->slots[id - 1];
    slot->allocated = true;
    slot->slot_id = id;
    slot->device = NULL;
    slot->interrupt_configured = false;
    slot->interrupt_pending = false;
    slot->interrupt_dci = 0;
    memset(slot->output_context, 0, c->context_size * 32);
    *slot_id = id;
    return 0;
}

static void xhci_build_address_context(xhci_controller_t* c,
                                       xhci_slot_t* slot,
                                       usb_device_t* device) {
    uint32_t size = c->context_size;
    memset(c->input_context, 0, size * 33);
    uint32_t* input_control = (uint32_t*)c->input_context;
    input_control[1] = 0x3; /* Add slot and EP0. */
    uint8_t* slot_context = c->input_context + size;
    uint8_t* ep0_context = c->input_context + size * 2;
    uint32_t* slot_dwords = (uint32_t*)slot_context;
    uint32_t* ep0_dwords = (uint32_t*)ep0_context;

    slot_dwords[0] = (device->route & 0xFFFFF) |
                     ((uint32_t)xhci_speed_code(device->speed) << 20) |
                     (1u << 27);
    if (device->parent_hub_address == 0) {
        slot_dwords[1] = (uint32_t)(device->port & 0xFF) << 16;
    } else {
        usb_device_t* hub = usb_find_device_by_address(
            device->parent_hub_address);
        xhci_slot_t* hub_slot = xhci_slot_for_device(c, hub);
        if (hub_slot) {
            slot_dwords[2] = ((uint32_t)hub_slot->slot_id << 16) |
                             ((uint32_t)device->parent_port << 24);
        }
    }
    ep0_dwords[1] = (4u << 3) |
                    ((uint32_t)device->max_packet0 << 16) |
                    (3u << 1);
    ep0_dwords[2] = (uint32_t)slot->ep0_ring.physical_base |
                    slot->ep0_ring.cycle;
    ep0_dwords[3] = (uint32_t)(slot->ep0_ring.physical_base >> 32);
}

static int xhci_address_device(xhci_controller_t* c, usb_device_t* device) {
    if (device->hcd_priv) return 0;
    uint8_t slot_id = 0;
    if (xhci_enable_slot(c, &slot_id) != 0) return -1;
    xhci_slot_t* slot = &c->slots[slot_id - 1];
    if (!slot->output_context) return -1;
    {
        slot->device = device;
        xhci_ring_init(c, &slot->ep0_ring);
        xhci_build_address_context(c, slot, device);
        uint32_t* input_control = (uint32_t*)c->input_context;
        uint8_t* slot_context = c->input_context + c->context_size;
        uint32_t* slot_dwords = (uint32_t*)slot_context;
        slot_dwords[0] &= ~(0x1Fu << 27);
        slot_dwords[0] |= 1u << 27;
        c->dcbaa[slot->slot_id] = slot->output_context_phys;
        int result = xhci_command(
            c, c->input_context_phys, 0,
            (XHCI_TRB_ADDRESS_DEVICE << 10) |
            ((uint32_t)slot->slot_id << 24), NULL);
        (void)input_control;
        if (result != 0) {
            slot->allocated = false;
            slot->device = NULL;
            return -1;
        }
        device->hcd_priv = slot;
        return 0;
    }
}

static int xhci_submit_control(xhci_controller_t* c, xhci_slot_t* slot,
                               const uint8_t* setup, void* data, int length,
                               int direction) {
    uint8_t* setup_buffer = xhci_arena_alloc(c, 8, 8, NULL);
    if (!setup_buffer) return -1;
    memcpy(setup_buffer, setup, 8);
    uint64_t setup_phys = (uint64_t)((uint8_t*)setup_buffer - c->memory) +
                          c->memory_phys;
    uint64_t data_phys = 0;
    if (length > 0) {
        uint8_t* staging = xhci_arena_alloc(c, 1024, 64, NULL);
        if (!staging) return -1;
        memset(staging, 0, 1024);
        if (direction == USB_DIR_OUT && data) {
            memcpy(staging, data, (size_t)length);
        }
        data_phys = (uint64_t)(staging - c->memory) + c->memory_phys;
        slot->transfer_buffer = staging;
        slot->transfer_buffer_phys = data_phys;
    }

    uint32_t transfer_type = length > 0 ?
        (direction == USB_DIR_IN ? 3u : 2u) : 0u;
    xhci_ring_push(c, &slot->ep0_ring, setup_phys, 8u,
                   (XHCI_TRB_SETUP << 10) | (transfer_type << 16) |
                   (1u << 6) | (1u << 4));
    if (length > 0) {
        uint32_t flags = (1u << 4);
        if (direction == USB_DIR_IN) flags |= 1u << 16;
        xhci_ring_push(c, &slot->ep0_ring, data_phys,
                       (uint32_t)(length & 0x1FFFF),
                       (XHCI_TRB_DATA << 10) | flags);
    }
    uint32_t status_flags = (1u << 5);
    if (direction != USB_DIR_IN || length == 0) status_flags |= 1u << 16;
    xhci_ring_push(c, &slot->ep0_ring, 0, 0,
                   (XHCI_TRB_STATUS << 10) | status_flags);
    log_printf("[xHCI] submit slot=%u len=%d dir=%d next=%u cycle=%u\n",
               slot->slot_id, length, direction,
               slot->ep0_ring.producer, slot->ep0_ring.cycle);
    volatile uint32_t* doorbell = (volatile uint32_t*)
        (c->doorbell + 4u * slot->slot_id);
    *doorbell = 1;

    xhci_trb_t event;
    if (xhci_wait_event(c, XHCI_TRB_TRANSFER_EVENT, &event, 500) != 0) {
        log_printf("[xHCI] control transfer timeout slot=%u\n", slot->slot_id);
        return -1;
    }
    uint8_t code = (uint8_t)(event.control >> 24);
    if (code != 1 && code != 13) {
        log_printf("[xHCI] control completion=%u slot=%u\n",
                   code, slot->slot_id);
        return -1;
    }
    if (length > 0 && direction == USB_DIR_IN && data) {
        int residual = (int)(event.status & 0xFFFFFF);
        int actual = length - residual;
        if (actual < 0) actual = 0;
        if (actual > length) actual = length;
        memcpy(data, slot->transfer_buffer, (size_t)actual);
        return actual;
    }
    return length;
}

static int xhci_control(usb_hci_t* hci, usb_device_t* device,
                        const usb_setup_packet_t* setup, void* data,
                        int length, int direction) {
    xhci_controller_t* c = hci->priv;
    if (!c || !device || !setup || length < 0) return -1;
    xhci_slot_t* slot = xhci_slot_for_device(c, device);
    if (!slot) {
        if (xhci_address_device(c, device) != 0) return -1;
        slot = device->hcd_priv;
    }
    return xhci_submit_control(c, slot, (const uint8_t*)setup,
                               data, length, direction);
}

static int xhci_configure_interrupt(xhci_controller_t* c, xhci_slot_t* slot,
                                    usb_endpoint_t* endpoint) {
    if (slot->interrupt_configured || !endpoint) return 0;
    uint8_t dci = (uint8_t)((endpoint->number * 2) + 1);
    if (endpoint->number == 0 || dci > 31) return -1;
    slot->interrupt_dci = dci;

    memset(c->input_context, 0, c->context_size * 33);
    uint32_t* input_control = (uint32_t*)c->input_context;
    input_control[1] = 1u | (1u << dci);
    uint8_t* slot_context = c->input_context + c->context_size;
    uint8_t* ep_context = c->input_context +
                          c->context_size * (dci + 1);
    uint32_t* slot_dwords = (uint32_t*)slot_context;
    uint32_t* ep_dwords = (uint32_t*)ep_context;
    memcpy(slot_context, slot->output_context + c->context_size,
           c->context_size);
    slot_dwords[0] &= ~(0x1Fu << 27);
    slot_dwords[0] |= (uint32_t)dci << 27;

    uint8_t interval = endpoint->interval ? endpoint->interval : 8;
    ep_dwords[0] = (uint32_t)interval << 16;
    ep_dwords[1] = (7u << 3) |
                   ((uint32_t)(endpoint->max_packet & 0x7FF) << 16) |
                   (3u << 1);
    ep_dwords[2] = (uint32_t)slot->interrupt_ring.physical_base |
                   slot->interrupt_ring.cycle;
    ep_dwords[3] = (uint32_t)(slot->interrupt_ring.physical_base >> 32);
    int result = xhci_command(
        c, c->input_context_phys, 0,
        (XHCI_TRB_CONFIGURE_ENDPOINT << 10) |
        ((uint32_t)slot->slot_id << 24), NULL);
    if (result == 0) slot->interrupt_configured = true;
    return result;
}

static int xhci_intr_in(usb_hci_t* hci, usb_device_t* device,
                        usb_endpoint_t* endpoint, void* data, int length) {
    xhci_controller_t* c = hci->priv;
    if (!c || !device || !endpoint || !data || length <= 0) return -1;
    xhci_slot_t* slot = xhci_slot_for_device(c, device);
    if (!slot) return -1;
    if (!slot->interrupt_ring.virtual_base) {
        xhci_ring_init(c, &slot->interrupt_ring);
    }
    if (xhci_configure_interrupt(c, slot, endpoint) != 0) return -1;

    if (slot->interrupt_pending) {
        xhci_trb_t event;
        if (xhci_wait_event(c, XHCI_TRB_TRANSFER_EVENT, &event, 1) == 0) {
            uint8_t code = (uint8_t)(event.control >> 24);
            slot->interrupt_pending = false;
            if (code != 1 && code != 13) return -1;
            int transfer_length = length > 64 ? 64 : length;
            int residual = (int)(event.status & 0xFFFFFF);
            int actual = transfer_length - residual;
            if (actual < 0) actual = 0;
            if (actual > length) actual = length;
            if (actual > 0) memcpy(data, slot->report_buffer, (size_t)actual);
            return actual;
        }
        return 0;
    }

    int transfer_length = length > 64 ? 64 : length;
    memset(slot->report_buffer, 0, 64);
    xhci_ring_push(c, &slot->interrupt_ring, slot->report_buffer_phys,
                   (uint32_t)transfer_length,
                   (XHCI_TRB_NORMAL << 10) | (1u << 16) | (1u << 5));
    volatile uint32_t* doorbell = (volatile uint32_t*)
        (c->doorbell + 4u * slot->slot_id);
    *doorbell = slot->interrupt_dci;
    slot->interrupt_pending = true;
    return 0;
}

static void xhci_release(usb_hci_t* hci, usb_device_t* device) {
    xhci_controller_t* c = hci->priv;
    if (!c || !device) return;
    xhci_slot_t* slot = xhci_slot_for_device(c, device);
    if (!slot) return;
    xhci_command(c, 0, 0, (XHCI_TRB_DISABLE_SLOT << 10) |
                           ((uint32_t)slot->slot_id << 24), NULL);
    c->dcbaa[slot->slot_id] = 0;
    memset(slot->output_context, 0, c->context_size * 32);
    slot->allocated = false;
    slot->device = NULL;
    slot->interrupt_configured = false;
    slot->interrupt_pending = false;
    device->hcd_priv = NULL;
}

static uint8_t xhci_port_speed(uint32_t portsc) {
    uint8_t speed_id = (uint8_t)((portsc >> 10) & 0xF);
    switch (speed_id) {
        case 1: return USB_SPEED_FULL;
        case 2: return USB_SPEED_LOW;
        case 3: return USB_SPEED_HIGH;
        case 4: return USB_SPEED_SUPER;
        default: return USB_SPEED_UNKNOWN;
    }
}

static int xhci_port_reset(usb_hci_t* hci, uint8_t port) {
    xhci_controller_t* c = hci->priv;
    if (!c || port == 0 || port > c->max_ports) return USB_SPEED_UNKNOWN;
    uint32_t reg = c->operational + 0x400 + 0x10u * (port - 1);
    uint32_t value = xhci_read32(c, reg);
    if (!(value & XHCI_PORT_CCS)) return USB_SPEED_UNKNOWN;
    xhci_write32(c, reg, (value & ~XHCI_PORT_CHANGES) |
                 (value & XHCI_PORT_CHANGES));
    value = xhci_read32(c, reg);
    xhci_write32(c, reg, (value & ~XHCI_PORT_CHANGES) | XHCI_PORT_PP |
                 XHCI_PORT_PR);
    usb_delay_ms(20);
    for (int i = 0; i < 100; i++) {
        value = xhci_read32(c, reg);
        if (!(value & XHCI_PORT_PR)) break;
        usb_delay_ms(1);
    }
    usb_delay_ms(10);
    value = xhci_read32(c, reg);
    xhci_write32(c, reg, (value & ~XHCI_PORT_CHANGES) |
                 (value & XHCI_PORT_CHANGES));
    if (!(value & XHCI_PORT_PED)) return USB_SPEED_UNKNOWN;
    return xhci_port_speed(value);
}

static void xhci_poll(usb_hci_t* hci) {
    xhci_controller_t* c = hci->priv;
    if (!c) return;
    for (uint8_t port = 1; port <= c->max_ports; port++) {
        uint32_t reg = c->operational + 0x400 + 0x10u * (port - 1);
        uint32_t bit = 1u << (port - 1);
        uint32_t value = xhci_read32(c, reg);
        if (value & XHCI_PORT_CHANGES) {
            xhci_write32(c, reg, (value & ~XHCI_PORT_CHANGES) |
                         (value & XHCI_PORT_CHANGES));
        }
        if (!(value & XHCI_PORT_CCS)) {
            if (c->ports_seen & bit) {
                usb_disconnect_port(hci, port);
                c->ports_seen &= ~bit;
            }
            continue;
        }
        if (c->ports_seen & bit) continue;
        uint8_t speed = (uint8_t)xhci_port_reset(hci, port);
        if (speed == USB_SPEED_UNKNOWN) continue;
        log_printf("[xHCI] root port %u %s connected\n", port,
                   usb_speed_name(speed));
        if (usb_enumerate(hci, port, speed) == 0) c->ports_seen |= bit;
    }
}

int xhci_probe(const pci_device_t* device) {
    if (!device || g_xhci_count >= XHCI_MAX_CONTROLLERS) return -1;
    int bar = -1;
    for (int i = 0; i < PCI_BAR_COUNT; i++) {
        if (device->bar[i].valid && !device->bar[i].is_io) {
            bar = i;
            break;
        }
    }
    if (bar < 0) return -1;
    if (usb_legacy_handoff(device) != 0) return -1;

    xhci_controller_t* c = &g_xhci[g_xhci_count];
    memset(c, 0, sizeof(*c));
    c->pci = device;
    c->cap = usb_map_mmio(device->bar[bar].address, 0x10000);
    if (!c->cap) return -1;
    pci_enable_memory(device);
    pci_enable_bus_master(device);
    c->cap_length = c->cap[0];
    c->operational = c->cap_length;
    c->runtime_offset = xhci_read32(c, 0x18);
    c->doorbell_offset = xhci_read32(c, 0x14);
    c->op = c->cap + c->operational;
    c->runtime = c->cap + c->runtime_offset;
    c->doorbell = c->cap + c->doorbell_offset;
    uint32_t hcsparams1 = xhci_read32(c, 0x04);
    uint32_t hcsparams2 = xhci_read32(c, 0x08);
    uint32_t hccparams1 = xhci_read32(c, 0x10);
    c->max_slots = hcsparams1 & 0xFF;
    if (c->max_slots > XHCI_MAX_SLOTS) c->max_slots = XHCI_MAX_SLOTS;
    c->max_ports = (hcsparams1 >> 24) & 0xFF;
    c->context_size = (hccparams1 & (1u << 2)) ? 64 : 32;
    if (!c->max_slots || !c->max_ports) return -1;

    if (usb_dma_alloc_zero(XHCI_ARENA_SIZE,
                           (hccparams1 & 1u) ? UINT64_MAX : UINT32_MAX,
                           PAGE_SIZE, &c->arena) != 0) {
        return -1;
    }
    c->memory = (uint8_t*)c->arena.virt_addr;
    c->memory_phys = c->arena.phys_addr;
    c->arena_cursor = 0;
    memset(c->memory, 0, c->arena.size);

    uint64_t command_phys = 0;
    c->command_ring.virtual_base = xhci_arena_alloc(
        c, sizeof(xhci_trb_t) * XHCI_RING_SIZE, 64, &command_phys);
    c->command_ring.physical_base = command_phys;
    c->command_ring.producer = 0;
    c->command_ring.cycle = 1;
    c->event_ring = xhci_arena_alloc(
        c, sizeof(xhci_trb_t) * XHCI_EVENT_RING_SIZE, 64,
        &c->event_ring_phys);
    c->event_index = 0;
    c->event_cycle = 1;
    uint64_t* erst = xhci_arena_alloc(c, 16, 64, NULL);
    uint64_t erst_phys = (uint64_t)((uint8_t*)erst - c->memory) +
                         c->memory_phys;
    erst[0] = c->event_ring_phys;
    erst[1] = XHCI_EVENT_RING_SIZE;
    c->dcbaa = xhci_arena_alloc(
        c, sizeof(uint64_t) * (XHCI_MAX_SLOTS + 1), 64, &c->dcbaa_phys);
    c->input_context = xhci_arena_alloc(c, c->context_size * 33, 64,
                                         &c->input_context_phys);
    if (!c->command_ring.virtual_base || !c->event_ring || !erst ||
        !c->dcbaa || !c->input_context) {
        return -1;
    }
    for (uint32_t i = 0; i < XHCI_MAX_SLOTS; i++) {
        xhci_slot_t* slot = &c->slots[i];
        uint64_t physical = 0;
        slot->output_context = xhci_arena_alloc(
            c, (size_t)c->context_size * 32, 64, &physical);
        slot->output_context_phys = physical;
        slot->report_buffer = xhci_arena_alloc(c, 64, 64, &physical);
        slot->report_buffer_phys = physical;
    }

    uint32_t scratchpad_count =
        (((hcsparams2 >> 21) & 0x1F) << 5) |
        ((hcsparams2 >> 27) & 0x1F);
    if (scratchpad_count) {
        if (scratchpad_count > 64) return -1;
        uint64_t* pointer_array = xhci_arena_alloc(
            c, scratchpad_count * sizeof(uint64_t), 64, NULL);
        if (!pointer_array || dma_alloc(
                scratchpad_count * PAGE_SIZE,
                (hccparams1 & 1u) ? UINT64_MAX : UINT32_MAX,
                PAGE_SIZE, &c->scratchpad) != 0) {
            return -1;
        }
        for (uint32_t i = 0; i < scratchpad_count; i++) {
            pointer_array[i] = c->scratchpad.phys_addr + i * PAGE_SIZE;
        }
        c->dcbaa[0] = (uint64_t)((uint8_t*)pointer_array - c->memory) +
                      c->memory_phys;
    }

    xhci_write32(c, c->operational, 0);
    for (int i = 0; i < 100 && !(xhci_read32(c, c->operational + 4) &
                                 XHCI_STS_HALTED); i++) {
        usb_delay_ms(1);
    }
    xhci_write32(c, c->operational, XHCI_CMD_RESET);
    for (int i = 0; i < 100; i++) {
        if (!(xhci_read32(c, c->operational) & XHCI_CMD_RESET) &&
            !(xhci_read32(c, c->operational + 4) & XHCI_STS_CNR)) {
            break;
        }
        usb_delay_ms(1);
    }
    xhci_write32(c, c->operational + 0x38, c->max_slots);
    xhci_write64(c, c->operational + 0x30, c->dcbaa_phys);
    xhci_write64(c, c->operational + 0x18,
                 c->command_ring.physical_base | 1u);
    xhci_write32(c, c->runtime_offset + 0x20 + 0x08, 1);
    xhci_write64(c, c->runtime_offset + 0x20 + 0x10, erst_phys);
    xhci_write64(c, c->runtime_offset + 0x20 + 0x18,
                 c->event_ring_phys | (1u << 3));
    xhci_write32(c, c->operational, XHCI_CMD_RUN);
    for (int i = 0; i < 100; i++) {
        uint32_t status = xhci_read32(c, c->operational + 4);
        if (!(status & (XHCI_STS_HALTED | XHCI_STS_HOSTERR |
                        XHCI_STS_CNR))) {
            break;
        }
        usb_delay_ms(1);
    }

    for (uint32_t port = 1; port <= c->max_ports; port++) {
        uint32_t reg = c->operational + 0x400 + 0x10u * (port - 1);
        uint32_t value = xhci_read32(c, reg);
        xhci_write32(c, reg, (value & ~XHCI_PORT_CHANGES) | XHCI_PORT_PP);
    }
    usb_delay_ms(100);

    c->hci.name = "xHCI 3.0";
    c->hci.kind = USB_HCI_KIND_XHCI;
    c->hci.priv = c;
    c->hci.root_ports = (uint8_t)c->max_ports;
    c->hci.supports_hubs = true;
    c->hci.skips_set_address = true;
    c->hci.control = xhci_control;
    c->hci.intr_in = xhci_intr_in;
    c->hci.port_reset = xhci_port_reset;
    c->hci.device_release = xhci_release;
    c->hci.poll = xhci_poll;
    usb_register_hci(&c->hci);
    g_xhci_count++;
    xhci_poll(&c->hci);
    return 0;
}
