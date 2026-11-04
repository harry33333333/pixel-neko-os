#include "usb_xhci.h"
#include "usb_core.h"
#include "usb_compat.h"
#include "dma.h"
#include "log.h"
#include "string.h"

#define XHCI_MAX_CONTROLLERS 2
#define XHCI_MAX_SLOTS 32
#define XHCI_MAX_PORTS 256
#define XHCI_RING_SIZE 256
#define XHCI_EVENT_RING_SIZE 4096
#define XHCI_ARENA_SIZE (8u * 1024u * 1024u)
#define XHCI_CONTROL_FAILURE_LIMIT 3
#define XHCI_PORT_FAILURE_LIMIT 5
#define XHCI_PORT_RETRY_POLLS 24

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
#define XHCI_PORT_PIC_MASK (3u << 14)
#define XHCI_PORT_LWS      (1u << 16)
#define XHCI_PORT_CSC      (1u << 17)
#define XHCI_PORT_PEC      (1u << 18)
#define XHCI_PORT_WRC      (1u << 19)
#define XHCI_PORT_OCC      (1u << 20)
#define XHCI_PORT_PRC      (1u << 21)
#define XHCI_PORT_PLC      (1u << 22)
#define XHCI_PORT_CEC      (1u << 23)
#define XHCI_PORT_WCE      (1u << 25)
#define XHCI_PORT_WDE      (1u << 26)
#define XHCI_PORT_WOE      (1u << 27)
#define XHCI_PORT_WPR      (1u << 31)
#define XHCI_PORT_CHANGES  (XHCI_PORT_CSC | XHCI_PORT_PEC | XHCI_PORT_WRC | \
                            XHCI_PORT_OCC | XHCI_PORT_PRC | XHCI_PORT_PLC | \
                            XHCI_PORT_CEC)
#define XHCI_PORT_RW_BITS  (XHCI_PORT_PP | XHCI_PORT_PIC_MASK | \
                            XHCI_PORT_WCE | XHCI_PORT_WDE | XHCI_PORT_WOE)
#define XHCI_PORT_PLS_GET(v) (((v) >> 5) & 0xFu)
#define XHCI_PORT_SPEED_GET(v) (((v) >> 10) & 0xFu)

#define XHCI_TRB_NORMAL             1u
#define XHCI_TRB_SETUP              2u
#define XHCI_TRB_DATA               3u
#define XHCI_TRB_STATUS             4u
#define XHCI_TRB_LINK               6u
#define XHCI_TRB_ENABLE_SLOT        9u
#define XHCI_TRB_DISABLE_SLOT       10u
#define XHCI_TRB_ADDRESS_DEVICE     11u
#define XHCI_TRB_CONFIGURE_ENDPOINT 12u
#define XHCI_TRB_EVALUATE_CONTEXT   13u
#define XHCI_TRB_STOP_ENDPOINT      15u
#define XHCI_TRB_TRANSFER_EVENT     32u
#define XHCI_TRB_COMMAND_EVENT      33u
#define XHCI_TRB_PORT_STATUS_EVENT  34u

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
    uint16_t ep0_max_packet;
    bool hub_configured;
    usb_device_t* device;
    uint8_t* output_context;
    uint64_t output_context_phys;
    xhci_ring_t ep0_ring;
    xhci_ring_t ep_rings[32];
    bool ep_configured[32];
    bool ep_pending[32];
    uint8_t* ep_buffers[32];
    uint64_t ep_buffers_phys[32];
    uint8_t* transfer_buffer;
    uint64_t transfer_buffer_phys;
    bool ep_complete[32];
    uint8_t ep_code[32];
    uint32_t ep_residual[32];
    uint64_t ep_event_parameter[32];
    uint16_t control_attempts;
    uint16_t control_failures;
    uint8_t consecutive_control_failures;
    uint8_t last_control_completion;
    bool control_fused;
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
    dma_buffer_t bulk_staging;
    xhci_slot_t slots[XHCI_MAX_SLOTS];
    uint8_t port_retry[XHCI_MAX_PORTS];
    uint8_t port_control_failures[XHCI_MAX_PORTS];
    uint8_t port_enum_failures[XHCI_MAX_PORTS];
    bool port_seen[XHCI_MAX_PORTS];
    bool usb3_port[XHCI_MAX_PORTS];
    bool port_fused[XHCI_MAX_PORTS];
    bool port_reset_skipped[XHCI_MAX_PORTS];
    uint32_t control_failures_total;
    uint32_t enumeration_failures_total;
} xhci_controller_t;

static xhci_controller_t g_xhci[XHCI_MAX_CONTROLLERS];
static uint32_t g_xhci_count = 0;

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
    if (!ring->virtual_base) {
        uint64_t physical = 0;
        ring->virtual_base = xhci_arena_alloc(
            c, sizeof(xhci_trb_t) * XHCI_RING_SIZE, 64, &physical);
        ring->physical_base = physical;
    }
    if (!ring->virtual_base) return;
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
    dma_sync_for_cpu(&c->arena);
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

static void xhci_route_transfer_event(xhci_controller_t* c,
                                      const xhci_trb_t* event) {
    uint8_t slot_id = (uint8_t)((event->control >> 24) & 0xFF);
    uint8_t dci = (uint8_t)((event->control >> 16) & 0x1F);
    if (slot_id == 0 || slot_id > c->max_slots || dci == 0 || dci >= 32) {
        return;
    }
    xhci_slot_t* slot = &c->slots[slot_id - 1];
    slot->ep_code[dci] = (uint8_t)(event->status >> 24);
    slot->ep_residual[dci] = event->status & 0xFFFFFF;
    slot->ep_event_parameter[dci] = event->parameter;
    slot->ep_complete[dci] = true;
}

static int xhci_wait_event(xhci_controller_t* c, uint8_t wanted_type,
                           xhci_trb_t* result, int timeout_ms) {
    for (int elapsed = 0; elapsed < timeout_ms; elapsed++) {
        xhci_trb_t event;
        while (xhci_event_next(c, &event)) {
            uint8_t type = (event.control >> 10) & 0x3F;
            if (type == XHCI_TRB_TRANSFER_EVENT) {
                xhci_route_transfer_event(c, &event);
                continue;
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

static void xhci_drain_events(xhci_controller_t* c) {
    xhci_trb_t event;
    while (xhci_event_next(c, &event)) {
        uint8_t type = (event.control >> 10) & 0x3F;
        if (type == XHCI_TRB_TRANSFER_EVENT) {
            xhci_route_transfer_event(c, &event);
        }
    }
}

static int xhci_wait_transfer(xhci_controller_t* c, xhci_slot_t* slot,
                              uint8_t dci, uint64_t expected_trb,
                              xhci_trb_t* result,
                              int timeout_ms) {
    bool short_packet_seen = false;
    uint32_t short_packet_residual = 0;
    for (int elapsed = 0; elapsed <= timeout_ms; elapsed++) {
        xhci_drain_events(c);
        if (slot->ep_complete[dci]) {
            uint8_t code = slot->ep_code[dci];
            if (expected_trb &&
                slot->ep_event_parameter[dci] != expected_trb &&
                (code == 1 || code == 13)) {
                /*
                 * IN data stages with ISP can report a short packet before
                 * the Status Stage.  Only the Status TRB retires the control
                 * transaction.
                 */
                if (code == 13) {
                    short_packet_seen = true;
                    short_packet_residual = slot->ep_residual[dci];
                }
                slot->ep_complete[dci] = false;
                continue;
            }
            if (result) {
                result->parameter = slot->ep_event_parameter[dci];
                result->status = (uint32_t)code << 24 |
                                 ((short_packet_seen ?
                                   short_packet_residual :
                                   slot->ep_residual[dci]) & 0xFFFFFF);
                result->control = 0;
            }
            slot->ep_complete[dci] = false;
            return 0;
        }
        if (elapsed < timeout_ms) usb_delay_ms(1);
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
    uint8_t code = (uint8_t)(event.status >> 24);
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

static uint16_t xhci_initial_ep0_max_packet(const usb_device_t* device) {
    if (!device) return 8;
    if (device->speed >= USB_SPEED_SUPER) return 512;

    uint16_t packet = device->max_packet0;
    if (device->speed == USB_SPEED_LOW) return 8;
    if (packet == 8 || packet == 16 || packet == 32 || packet == 64) {
        return packet;
    }
    return device->speed >= USB_SPEED_HIGH ? 64 : 8;
}

/*
 * FreeBSD _ref-xhci.c xhci_configure_endpoint:
 * Convert USB endpoint bInterval into xHCI Endpoint Context Interval exponent.
 */
static uint8_t xhci_compute_interval(uint8_t speed, uint8_t ep_type,
                                     uint8_t bInterval) {
    if (ep_type == USB_EP_TYPE_CONTROL || ep_type == USB_EP_TYPE_BULK) {
        return 0;
    }
    if (speed == USB_SPEED_LOW || speed == USB_SPEED_FULL) {
        /* Convert 1..255 ms (frames) into 2^exp microframes (125us), exp in 3..10 */
        uint32_t uframes = (uint32_t)(bInterval ? bInterval : 8) * 8u;
        uint8_t exp = 3;
        while (exp < 10 && (1u << (exp + 1)) <= uframes) {
            exp++;
        }
        return exp;
    }
    /* High-Speed / SuperSpeed: bInterval is 1..16 -> exponent is bInterval - 1 (0..15) */
    if (bInterval < 1) return 3;
    if (bInterval > 16) return 15;
    return (uint8_t)(bInterval - 1);
}

static xhci_slot_t* xhci_slot_for_device(xhci_controller_t* c,
                                         usb_device_t* device) {
    (void)c;
    if (!device) return NULL;
    if (device->hcd_priv) return (xhci_slot_t*)device->hcd_priv;
    return NULL;
}

static void xhci_discard_slot(xhci_controller_t* c, xhci_slot_t* slot) {
    if (!c || !slot || !slot->allocated) return;
    usb_device_t* device = slot->device;
    (void)xhci_command(
        c, 0, 0, (XHCI_TRB_DISABLE_SLOT << 10) |
        ((uint32_t)slot->slot_id << 24), NULL);
    if (slot->slot_id <= c->max_slots) {
        c->dcbaa[slot->slot_id] = 0;
        dma_sync_for_device(&c->arena);
    }
    slot->allocated = false;
    slot->device = NULL;
    slot->hub_configured = false;
    slot->ep0_max_packet = 0;
    for (int dci = 0; dci < 32; dci++) {
        slot->ep_configured[dci] = false;
        slot->ep_pending[dci] = false;
        slot->ep_complete[dci] = false;
    }
    if (device) device->hcd_priv = NULL;
}

static uint8_t xhci_find_root_port(usb_device_t* device) {
    usb_device_t* cur = device;
    for (int depth = 0; depth < 8 && cur; depth++) {
        if (cur->parent_hub_address == 0) return cur->port;
        cur = usb_find_device_by_address(cur->parent_hub_address);
    }
    return device ? device->port : 1;
}

static uint32_t xhci_slot_root_port(xhci_slot_t* slot) {
    return slot && slot->device ?
           (uint32_t)xhci_find_root_port(slot->device) : 0;
}

static bool xhci_port_is_fused(xhci_controller_t* c,
                               usb_device_t* device) {
    if (!c || !device) return true;
    uint8_t root_port = xhci_find_root_port(device);
    return root_port != 0 && c->port_fused[root_port - 1];
}

static void xhci_note_control_success(xhci_controller_t* c,
                                      xhci_slot_t* slot,
                                      uint8_t completion) {
    if (!c || !slot) return;
    slot->control_attempts++;
    slot->last_control_completion = completion;
    slot->consecutive_control_failures = 0;
    uint32_t port = xhci_slot_root_port(slot);
    if (port && port <= XHCI_MAX_PORTS) {
        c->port_control_failures[port - 1] = 0;
    }
}

static void xhci_note_control_failure(xhci_controller_t* c,
                                      xhci_slot_t* slot,
                                      uint8_t completion) {
    if (!c || !slot) return;
    slot->control_attempts++;
    slot->control_failures++;
    slot->last_control_completion = completion;
    if (slot->consecutive_control_failures < UINT8_MAX) {
        slot->consecutive_control_failures++;
    }
    c->control_failures_total++;

    uint32_t port = xhci_slot_root_port(slot);
    if (port && port <= XHCI_MAX_PORTS) {
        if (c->port_control_failures[port - 1] < UINT8_MAX) {
            c->port_control_failures[port - 1]++;
        }
        if (c->port_control_failures[port - 1] >=
            XHCI_CONTROL_FAILURE_LIMIT) {
            slot->control_fused = true;
            c->port_fused[port - 1] = true;
            log_printf("[xHCI] control breaker open: port=%u slot=%u "
                       "failures=%u last=%u total=%u\n",
                       port, slot->slot_id,
                       c->port_control_failures[port - 1],
                       completion, c->control_failures_total);
        }
    } else if (slot->consecutive_control_failures >=
               XHCI_CONTROL_FAILURE_LIMIT) {
        slot->control_fused = true;
    }
}

static int xhci_enable_slot(xhci_controller_t* c, uint8_t* slot_id) {
    xhci_trb_t event;
    if (xhci_command(c, 0, 0, XHCI_TRB_ENABLE_SLOT << 10, &event) != 0) {
        return -1;
    }
    uint8_t id = (uint8_t)((event.control >> 24) & 0xFF);
    if (id == 0 || id > c->max_slots) {
        log_printf("[xHCI] Enable Slot returned invalid slot %u\n", id);
        return -1;
    }
    xhci_slot_t* slot = &c->slots[id - 1];
    for (int index = 0; index < 32; index++) {
        slot->ep_configured[index] = false;
        slot->ep_pending[index] = false;
        slot->ep_complete[index] = false;
    }
    slot->allocated = true;
    slot->slot_id = id;
    slot->ep0_max_packet = 0;
    slot->hub_configured = false;
    slot->device = NULL;
    slot->control_attempts = 0;
    slot->control_failures = 0;
    slot->consecutive_control_failures = 0;
    slot->last_control_completion = 0xFF;
    slot->control_fused = false;
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
    uint8_t root_port = xhci_find_root_port(device);
    slot_dwords[1] = (uint32_t)(root_port & 0xFF) << 16;

    /*
     * FreeBSD xhci.c: Only populate TT Hub Slot ID and TT Port Number when
     * a Low/Full-Speed device is behind a High-Speed hub.
     */
    if (device->speed < USB_SPEED_HIGH && device->hs_hub_address != 0) {
        usb_device_t* hs_hub = usb_find_device_by_address(
            device->hs_hub_address);
        xhci_slot_t* hub_slot = xhci_slot_for_device(c, hs_hub);
        if (hub_slot) {
            slot_dwords[2] = ((uint32_t)hub_slot->slot_id << 0) |
                             ((uint32_t)device->hs_port_number << 8);
        }
    }

    uint16_t max_pkt0 = xhci_initial_ep0_max_packet(device);
    ep0_dwords[1] = (4u << 3) |
                    ((uint32_t)max_pkt0 << 16) |
                    (3u << 1);
    ep0_dwords[2] = (uint32_t)slot->ep0_ring.physical_base |
                    slot->ep0_ring.cycle;
    ep0_dwords[3] = (uint32_t)(slot->ep0_ring.physical_base >> 32);
    ep0_dwords[4] = 8u; /* Average TRB Length = 8 for Control EP0 */
}

static int xhci_address_device(xhci_controller_t* c, usb_device_t* device) {
    if (device->hcd_priv) return 0;
    uint8_t slot_id = 0;
    if (xhci_enable_slot(c, &slot_id) != 0) return -1;
    xhci_slot_t* slot = &c->slots[slot_id - 1];
    if (!slot->output_context) return -1;

    slot->device = device;
    slot->ep0_max_packet = xhci_initial_ep0_max_packet(device);
    xhci_ring_init(c, &slot->ep0_ring);
    xhci_build_address_context(c, slot, device);
    uint8_t* slot_context = c->input_context + c->context_size;
    uint32_t* slot_dwords = (uint32_t*)slot_context;
    slot_dwords[0] &= ~(0x1Fu << 27);
    slot_dwords[0] |= 1u << 27;
    c->dcbaa[slot->slot_id] = slot->output_context_phys;
    int result = xhci_command(
        c, c->input_context_phys, 0,
        (XHCI_TRB_ADDRESS_DEVICE << 10) |
        ((uint32_t)slot->slot_id << 24), NULL);
    if (result != 0) {
        xhci_discard_slot(c, slot);
        return -1;
    }
    device->hcd_priv = slot;
    return 0;
}

static void xhci_evaluate_ep0_if_needed(xhci_controller_t* c,
                                        xhci_slot_t* slot,
                                        usb_device_t* device) {
    uint16_t target_packet = xhci_initial_ep0_max_packet(device);
    if (target_packet == slot->ep0_max_packet) {
        return;
    }
    uint32_t size = c->context_size;
    memset(c->input_context, 0, size * 33);
    uint32_t* input_control = (uint32_t*)c->input_context;
    input_control[1] = 0x2u; /* Evaluate EP0 Context */
    uint8_t* ep0_context = c->input_context + size * 2;
    uint32_t* ep0_dwords = (uint32_t*)ep0_context;
    ep0_dwords[1] = (4u << 3) |
                    ((uint32_t)target_packet << 16) |
                    (3u << 1);
    ep0_dwords[4] = 8u;
    if (xhci_command(c, c->input_context_phys, 0,
                     (XHCI_TRB_EVALUATE_CONTEXT << 10) |
                     ((uint32_t)slot->slot_id << 24), NULL) == 0) {
        slot->ep0_max_packet = target_packet;
    }
}

static int xhci_submit_control(xhci_controller_t* c, xhci_slot_t* slot,
                               const uint8_t* setup, void* data, int length,
                               int direction) {
    if (!c || !slot || !setup || length < 0 || length > 1024 ||
        (length > 0 && !data) || slot->control_fused ||
        xhci_port_is_fused(c, slot->device)) {
        return -1;
    }

    /*
     * A late Transfer Event cannot be correlated with a new transaction.  Drop
     * the slot and let the next control request create a fresh EP0 ring.
     */
    xhci_drain_events(c);
    if (slot->ep_complete[1]) {
        slot->ep_complete[1] = false;
        xhci_note_control_failure(c, slot, 0xFD);
        log_printf("[xHCI] stale EP0 completion: slot=%u\n", slot->slot_id);
        xhci_discard_slot(c, slot);
        return -1;
    }

    uint64_t setup_parameter = 0;
    memcpy(&setup_parameter, setup, 8);
    uint64_t data_phys = 0;
    if (length > 0) {
        if (!slot->transfer_buffer) {
            uint64_t phys = 0;
            slot->transfer_buffer = xhci_arena_alloc(c, 1024, 64, &phys);
            slot->transfer_buffer_phys = phys;
        }
        if (!slot->transfer_buffer) {
            xhci_note_control_failure(c, slot, 0xFE);
            log_printf("[xHCI] EP0 staging allocation failed: slot=%u\n",
                       slot->slot_id);
            return -1;
        }
        memset(slot->transfer_buffer, 0, 1024);
        if (direction == USB_DIR_OUT && data) {
            memcpy(slot->transfer_buffer, data, (size_t)length);
        }
        data_phys = slot->transfer_buffer_phys;
    }

    uint32_t transfer_type = length > 0 ?
        (direction == USB_DIR_IN ? 3u : 2u) : 0u;

    /*
     * Control transfer stages are one implicit TD.  Linux queues Setup and
     * Data without TRB_CHAIN (bit 4); only the Status TRB carries IOC.  The
     * previous CHAIN-based layout is tolerated by QEMU but causes some xHCI
     * implementations to complete the Status stage without moving the IN data.
     */
    if (!xhci_ring_push(c, &slot->ep0_ring, setup_parameter, 8u,
                        (XHCI_TRB_SETUP << 10) | (transfer_type << 16) |
                        (1u << 6))) {
        xhci_note_control_failure(c, slot, 0xFE);
        xhci_discard_slot(c, slot);
        return -1;
    }
    if (length > 0) {
        uint32_t flags = 0;
        if (direction == USB_DIR_IN) flags |= (1u << 16) | (1u << 2);
        if (!xhci_ring_push(c, &slot->ep0_ring, data_phys,
                            (uint32_t)(length & 0x1FFFF),
                            (XHCI_TRB_DATA << 10) | flags)) {
            xhci_note_control_failure(c, slot, 0xFE);
            xhci_discard_slot(c, slot);
            return -1;
        }
    }
    uint32_t status_flags = (1u << 5);
    if (direction != USB_DIR_IN || length == 0) status_flags |= 1u << 16;
    xhci_trb_t* status_trb = xhci_ring_push(
        c, &slot->ep0_ring, 0, 0,
        (XHCI_TRB_STATUS << 10) | status_flags);
    if (!status_trb) {
        xhci_note_control_failure(c, slot, 0xFE);
        xhci_discard_slot(c, slot);
        return -1;
    }
    uint64_t status_trb_phys =
        slot->ep0_ring.physical_base +
        (uint64_t)((uint8_t*)status_trb -
                   (uint8_t*)slot->ep0_ring.virtual_base);
    slot->ep_complete[1] = false;
    slot->ep_code[1] = 0xFF;
    slot->ep_residual[1] = 0;
    volatile uint32_t* doorbell = (volatile uint32_t*)
        (c->doorbell + 4u * slot->slot_id);
    *doorbell = 1;

    xhci_trb_t event;
    if (xhci_wait_transfer(c, slot, 1, status_trb_phys,
                           &event, 500) != 0) {
        xhci_note_control_failure(c, slot, 0xFF);
        log_printf("[xHCI] control timeout slot=%u type=%02x req=%02x "
                   "value=%04x index=%04x len=%d\n",
                   slot->slot_id, setup[0], setup[1],
                   (unsigned)(setup[2] | ((uint16_t)setup[3] << 8)),
                   (unsigned)(setup[4] | ((uint16_t)setup[5] << 8)), length);
        xhci_discard_slot(c, slot);
        return -1;
    }
    uint8_t code = (uint8_t)(event.status >> 24);
    if (code != 1 && code != 13) {
        xhci_note_control_failure(c, slot, code);
        log_printf("[xHCI] control completion=%u slot=%u type=%02x "
                   "req=%02x len=%d residual=%u\n",
                   code, slot->slot_id, setup[0], setup[1], length,
                   (unsigned)(event.status & 0xFFFFFFu));
        xhci_discard_slot(c, slot);
        return -1;
    }
    xhci_note_control_success(c, slot, code);
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
    if (xhci_port_is_fused(c, device)) return -1;
    xhci_slot_t* slot = xhci_slot_for_device(c, device);
    if (!slot) {
        if (xhci_address_device(c, device) != 0) return -1;
        slot = device->hcd_priv;
        if (!slot) return -1;
    } else {
        xhci_evaluate_ep0_if_needed(c, slot, device);
    }
    return xhci_submit_control(c, slot, (const uint8_t*)setup,
                               data, length, direction);
}

static int xhci_configure_interrupt(xhci_controller_t* c, xhci_slot_t* slot,
                                    usb_device_t* device,
                                    usb_endpoint_t* endpoint, uint8_t dci) {
    if (slot->ep_configured[dci] && (!device->is_hub || slot->hub_configured)) {
        return 0;
    }
    if (!slot->ep_rings[dci].virtual_base) {
        xhci_ring_init(c, &slot->ep_rings[dci]);
        if (!slot->ep_rings[dci].virtual_base) return -1;
    } else {
        xhci_ring_init(c, &slot->ep_rings[dci]);
    }
    if (!slot->ep_buffers[dci]) {
        uint64_t phys = 0;
        slot->ep_buffers[dci] = xhci_arena_alloc(c, 64, 64, &phys);
        slot->ep_buffers_phys[dci] = phys;
        if (!slot->ep_buffers[dci]) return -1;
    }

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
    uint32_t last_dci = (slot_dwords[0] >> 27) & 0x1F;
    if (dci > last_dci) last_dci = dci;
    slot_dwords[0] &= ~(0x1Fu << 27);
    slot_dwords[0] |= last_dci << 27;

    /* FreeBSD xhci.c: Set HUB bit and Number of Ports when configuring a Hub */
    if (device->is_hub && device->hub_port_count > 0) {
        slot_dwords[0] |= (1u << 26);
        slot_dwords[1] &= ~(0xFFu << 24);
        slot_dwords[1] |= ((uint32_t)device->hub_port_count << 24);
        slot->hub_configured = true;
    }

    uint16_t max_pkt = endpoint->max_packet ? (endpoint->max_packet & 0x7FFu) : 8u;
    uint8_t max_burst = endpoint->max_burst & 0x0Fu;
    uint32_t payload = (uint32_t)max_pkt * ((uint32_t)max_burst + 1u);
    if (endpoint->bytes_per_interval) {
        payload = endpoint->bytes_per_interval;
    }
    if (payload > 0xFFFFu) payload = 0xFFFFu;
    uint8_t interval = xhci_compute_interval(device->speed,
                                             USB_EP_TYPE_INTERRUPT,
                                             endpoint->interval);
    ep_dwords[0] = (uint32_t)interval << 16;
    ep_dwords[1] = (3u << 1) |
                   ((uint32_t)max_burst << 8) |
                   ((uint32_t)max_pkt << 16) |
                   (7u << 3);
    ep_dwords[2] = (uint32_t)slot->ep_rings[dci].physical_base |
                   slot->ep_rings[dci].cycle;
    ep_dwords[3] = (uint32_t)(slot->ep_rings[dci].physical_base >> 32);
    ep_dwords[4] = (payload << 16) | max_pkt;

    int result = xhci_command(
        c, c->input_context_phys, 0,
        (XHCI_TRB_CONFIGURE_ENDPOINT << 10) |
        ((uint32_t)slot->slot_id << 24), NULL);
    slot->ep_complete[dci] = false;
    slot->ep_pending[dci] = false;
    if (result == 0) slot->ep_configured[dci] = true;

    uint32_t* oc  = (uint32_t*)slot->output_context;
    uint32_t* oep = (uint32_t*)(slot->output_context + (size_t)c->context_size * dci);
    log_printf("[xHCI] cfgEP slot=%u dci=%u res=%d slotState=%u epState=%u DW0=%08x DW1=%08x\n",
               slot->slot_id, dci, result,
               (oc[3] >> 27) & 0xF,
               oep[0] & 0x7,
               oep[0], oep[1]);
    return result;
}

static int xhci_intr_in(usb_hci_t* hci, usb_device_t* device,
                        usb_endpoint_t* endpoint, void* data, int length) {
    xhci_controller_t* c = hci->priv;
    if (!c || !device || !endpoint || !data || length <= 0) return -1;
    xhci_slot_t* slot = xhci_slot_for_device(c, device);
    if (!slot) return -1;
    uint8_t dci = (uint8_t)((endpoint->number * 2) + 1);
    if (endpoint->number == 0 || dci > 31) return -1;

    if (xhci_configure_interrupt(c, slot, device, endpoint, dci) != 0) {
        return -1;
    }

    int transfer_length = length > 64 ? 64 : length;
    if (endpoint->max_packet > 0 && transfer_length > (int)endpoint->max_packet) {
        transfer_length = (int)endpoint->max_packet;
    }

    if (slot->ep_pending[dci]) {
        xhci_trb_t event;
        if (xhci_wait_transfer(c, slot, dci, 0, &event, 0) == 0) {
            uint8_t code = (uint8_t)(event.status >> 24);
            slot->ep_pending[dci] = false;
            if (code != 1 && code != 13) {
                log_printf("[xHCI] intr_in completion code=%u for slot=%u dci=%u\n",
                           code, slot->slot_id, dci);
                xhci_command(c, 0, 0, (XHCI_TRB_STOP_ENDPOINT << 10) |
                               ((uint32_t)dci << 16) |
                               ((uint32_t)slot->slot_id << 24), NULL);
                slot->ep_configured[dci] = false;
                endpoint->hcd_priv = NULL;
                return -1;
            }
            int residual = (int)(event.status & 0xFFFFFF);
            int actual = transfer_length - residual;
            if (actual < 0) actual = 0;
            if (actual > length) actual = length;
            if (actual > 0) memcpy(data, slot->ep_buffers[dci], (size_t)actual);

            /* Immediately re-queue the next interrupt TRB so no reports are missed */
            memset(slot->ep_buffers[dci], 0, 64);
            slot->ep_complete[dci] = false;
            xhci_ring_push(c, &slot->ep_rings[dci], slot->ep_buffers_phys[dci],
                           (uint32_t)transfer_length,
                           (XHCI_TRB_NORMAL << 10) | (1u << 16) | (1u << 5));
            volatile uint32_t* doorbell = (volatile uint32_t*)
                (c->doorbell + 4u * slot->slot_id);
            *doorbell = dci;
            slot->ep_pending[dci] = true;
            return actual;
        }
        return 0;
    }

    memset(slot->ep_buffers[dci], 0, 64);
    slot->ep_complete[dci] = false;
    xhci_ring_push(c, &slot->ep_rings[dci], slot->ep_buffers_phys[dci],
                   (uint32_t)transfer_length,
                   (XHCI_TRB_NORMAL << 10) | (1u << 16) | (1u << 5));
    volatile uint32_t* doorbell = (volatile uint32_t*)
        (c->doorbell + 4u * slot->slot_id);
    *doorbell = dci;
    slot->ep_pending[dci] = true;
    return 0;
}

static int xhci_configure_bulk(xhci_controller_t* c, xhci_slot_t* slot,
                               usb_endpoint_t* endpoint, uint8_t dci) {
    if (slot->ep_configured[dci]) return 0;
    if (!slot->ep_rings[dci].virtual_base) {
        xhci_ring_init(c, &slot->ep_rings[dci]);
    } else {
        xhci_ring_init(c, &slot->ep_rings[dci]);
    }
    if (!slot->ep_rings[dci].virtual_base) return -1;

    memset(c->input_context, 0, c->context_size * 33);
    uint32_t* input_control = (uint32_t*)c->input_context;
    input_control[1] = 1u | (1u << dci);
    uint8_t* slot_context = c->input_context + c->context_size;
    uint8_t* ep_context = c->input_context + c->context_size * (dci + 1);
    uint32_t* slot_dwords = (uint32_t*)slot_context;
    uint32_t* ep_dwords = (uint32_t*)ep_context;
    memcpy(slot_context, slot->output_context + c->context_size,
           c->context_size);
    uint32_t last_dci = (slot_dwords[0] >> 27) & 0x1F;
    if (dci > last_dci) last_dci = dci;
    slot_dwords[0] &= ~(0x1Fu << 27);
    slot_dwords[0] |= last_dci << 27;
    uint16_t max_pkt = endpoint->max_packet ? (endpoint->max_packet & 0x7FFu) : 512u;
    uint8_t max_burst = endpoint->max_burst & 0x0Fu;
    uint32_t average = (uint32_t)max_pkt * ((uint32_t)max_burst + 1u);
    if (average > PAGE_SIZE) average = PAGE_SIZE;
    ep_dwords[0] = 0;
    ep_dwords[1] = (3u << 1) |
                   ((uint32_t)max_burst << 8) |
                   ((uint32_t)max_pkt << 16) |
                   ((endpoint->direction == USB_DIR_IN ? 6u : 2u) << 3);
    ep_dwords[2] = (uint32_t)slot->ep_rings[dci].physical_base |
                   slot->ep_rings[dci].cycle;
    ep_dwords[3] =
        (uint32_t)(slot->ep_rings[dci].physical_base >> 32);
    ep_dwords[4] = average;
    int result = xhci_command(
        c, c->input_context_phys, 0,
        (XHCI_TRB_CONFIGURE_ENDPOINT << 10) |
        ((uint32_t)slot->slot_id << 24), NULL);
    slot->ep_complete[dci] = false;
    if (result == 0) slot->ep_configured[dci] = true;

    uint32_t* oc  = (uint32_t*)slot->output_context;
    uint32_t* oep = (uint32_t*)(slot->output_context + (size_t)c->context_size * dci);
    log_printf("[xHCI] cfgEP slot=%u dci=%u res=%d slotState=%u epState=%u DW0=%08x DW1=%08x\n",
               slot->slot_id, dci, result,
               (oc[3] >> 27) & 0xF,
               oep[0] & 0x7,
               oep[0], oep[1]);
    return result;
}

static int xhci_bulk(usb_hci_t* hci, usb_device_t* device,
                     usb_endpoint_t* endpoint, void* data, int length,
                     int direction) {
    xhci_controller_t* c = hci->priv;
    if (!c || !device || !endpoint || !data || length <= 0 ||
        c->bulk_staging.size < (size_t)length) {
        return -1;
    }
    xhci_slot_t* slot = xhci_slot_for_device(c, device);
    if (!slot || endpoint->number == 0) return -1;
    uint8_t dci = (uint8_t)(endpoint->number * 2 +
                            (direction == USB_DIR_IN ? 1 : 0));
    if (dci >= 32 || xhci_configure_bulk(c, slot, endpoint, dci) != 0) {
        return -1;
    }

    memset((void*)c->bulk_staging.virt_addr, 0, (size_t)length);
    if (direction == USB_DIR_OUT) {
        memcpy((void*)c->bulk_staging.virt_addr, data, (size_t)length);
    }
    dma_sync_for_device(&c->bulk_staging);
    slot->ep_complete[dci] = false;
    xhci_ring_push(c, &slot->ep_rings[dci],
                   c->bulk_staging.phys_addr, (uint32_t)length,
                   (XHCI_TRB_NORMAL << 10) | (1u << 16) | (1u << 5));
    volatile uint32_t* doorbell =
        (volatile uint32_t*)(c->doorbell + 4u * slot->slot_id);
    *doorbell = dci;

    xhci_trb_t event;
    if (xhci_wait_transfer(c, slot, dci, 0, &event, 500) != 0) {
        log_printf("[xHCI] bulk timeout slot=%u dci=%u length=%d\n",
                   slot->slot_id, dci, length);
        return -1;
    }
    uint8_t code = (uint8_t)(event.status >> 24);
    if (code != 1 && code != 13) {
        log_printf("[xHCI] bulk completion=%u slot=%u dci=%u\n",
                   code, slot->slot_id, dci);
        return -1;
    }
    int residual = (int)(event.status & 0xFFFFFF);
    int actual = length - residual;
    if (actual < 0) actual = 0;
    if (actual > length) actual = length;
    dma_sync_for_cpu(&c->bulk_staging);
    if (direction == USB_DIR_IN && actual > 0) {
        memcpy(data, (void*)c->bulk_staging.virt_addr, (size_t)actual);
    }
    endpoint->toggle ^= 1;
    return actual;
}

static void xhci_release(usb_hci_t* hci, usb_device_t* device) {
    xhci_controller_t* c = hci->priv;
    if (!c || !device) return;
    xhci_slot_t* slot = xhci_slot_for_device(c, device);
    if (!slot) return;
    for (int dci = 2; dci < 32; dci++) {
        if (slot->ep_configured[dci]) {
            xhci_command(c, 0, 0, (XHCI_TRB_STOP_ENDPOINT << 10) |
                           ((uint32_t)dci << 16) |
                           ((uint32_t)slot->slot_id << 24), NULL);
            slot->ep_configured[dci] = false;
            slot->ep_pending[dci] = false;
        }
    }
    xhci_discard_slot(c, slot);
    memset(slot->output_context, 0, c->context_size * 32);
}

static uint8_t xhci_port_speed(uint32_t portsc) {
    uint8_t speed_id = (uint8_t)XHCI_PORT_SPEED_GET(portsc);
    switch (speed_id) {
        case 1: return USB_SPEED_FULL;
        case 2: return USB_SPEED_LOW;
        case 3: return USB_SPEED_HIGH;
        default:
            /*
             * IDs 4 and above are SuperSpeed-family links.  USB 3.1/3.2
             * controllers use additional PSI values for 10/20 Gb/s while the
             * generic stack only needs the canonical SuperSpeed class.
             */
            return speed_id >= 4 ? USB_SPEED_SUPER : USB_SPEED_UNKNOWN;
    }
}

static void xhci_port_clear_changes(xhci_controller_t* c, uint32_t reg,
                                    uint32_t value) {
    xhci_write32(c, reg,
                 (value & XHCI_PORT_RW_BITS) | XHCI_PORT_CHANGES);
}

static uint32_t xhci_port_reset_attempt(xhci_controller_t* c, uint32_t reg,
                                        bool warm_reset) {
    uint32_t value = xhci_read32(c, reg);
    if (!(value & XHCI_PORT_CCS)) return value;

    if (!(value & XHCI_PORT_PP)) {
        xhci_write32(c, reg, (value & XHCI_PORT_RW_BITS) | XHCI_PORT_PP);
        usb_delay_ms(100);
        value = xhci_read32(c, reg);
    }
    xhci_port_clear_changes(c, reg, value);

    /*
     * Wait for the link to leave RxDetect before starting reset.  This is
     * required by several controllers and avoids an early reset racing with
     * SuperSpeed receiver training.
     */
    usb_delay_ms(20);
    value = xhci_read32(c, reg);
    if (!(value & XHCI_PORT_CCS)) return value;

    uint32_t reset_bit = warm_reset ? XHCI_PORT_WPR : XHCI_PORT_PR;
    xhci_write32(c, reg,
                 (value & XHCI_PORT_RW_BITS) | XHCI_PORT_PP | reset_bit);
    usb_delay_ms(10);

    int stable = 0;
    for (int elapsed = 0; elapsed < 750; elapsed++) {
        value = xhci_read32(c, reg);
        if (!(value & reset_bit) && (value & XHCI_PORT_CCS) &&
            (value & XHCI_PORT_PED) && XHCI_PORT_PLS_GET(value) == 0) {
            if (++stable >= 2) {
                usb_delay_ms(10);
                return xhci_read32(c, reg);
            }
        } else {
            stable = 0;
        }
        usb_delay_ms(1);
    }
    return value;
}

static uint32_t xhci_port_wait_u0(xhci_controller_t* c, uint32_t reg,
                                  int timeout_ms) {
    uint32_t value = xhci_read32(c, reg);
    for (int elapsed = 0; elapsed < timeout_ms; elapsed++) {
        if ((value & XHCI_PORT_CCS) && (value & XHCI_PORT_PED) &&
            XHCI_PORT_PLS_GET(value) == 0) {
            return value;
        }
        usb_delay_ms(1);
        value = xhci_read32(c, reg);
    }
    return value;
}

static int xhci_port_reset(usb_hci_t* hci, uint8_t port) {
    xhci_controller_t* c = hci->priv;
    if (!c || port == 0 || port > c->max_ports) return USB_SPEED_UNKNOWN;
    uint32_t reg = c->operational + 0x400 + 0x10u * (port - 1);
    uint32_t value = xhci_read32(c, reg);
    if (!(value & XHCI_PORT_CCS)) return USB_SPEED_UNKNOWN;

    uint32_t before = value;
    bool usb3_port = c->usb3_port[port - 1];
    bool attempted = false;
    bool warm_attempted = false;
    bool previous_reset_skipped = c->port_reset_skipped[port - 1];
    c->port_reset_skipped[port - 1] = false;

    if (usb3_port) {
        value = xhci_port_wait_u0(c, reg, 100);
        if ((value & XHCI_PORT_PED) && XHCI_PORT_PLS_GET(value) == 0 &&
            !previous_reset_skipped) {
            xhci_port_clear_changes(c, reg, value);
            c->port_reset_skipped[port - 1] = true;
            log_printf("[xHCI] port %u USB3 link already in U0; "
                       "reset skipped\n", port);
            return xhci_port_speed(value);
        }

        uint8_t pls = (uint8_t)XHCI_PORT_PLS_GET(value);
        if ((value & XHCI_PORT_PED) && pls == 0 &&
            previous_reset_skipped) {
            log_printf("[xHCI] port %u USB3 U0 reset required after "
                       "enumeration failure\n", port);
            value = xhci_port_reset_attempt(c, reg, false);
            attempted = true;
        } else if (pls == 6 || pls == 8 || pls == 10) {
            log_printf("[xHCI] port %u USB3 link recovery warm reset "
                       "(PLS=%u PORTSC=%08x)\n", port, pls, value);
            value = xhci_port_reset_attempt(c, reg, true);
            attempted = true;
            warm_attempted = true;
        } else if (!(value & XHCI_PORT_PED) && (pls == 5 || pls == 7)) {
            /*
             * A newly attached USB3 device remains in RxDetect/Polling until
             * software initiates training.  This is the one USB3 case where
             * a regular Port Reset is required.
             */
            value = xhci_port_reset_attempt(c, reg, false);
            attempted = true;
        } else if (!(value & XHCI_PORT_PED)) {
            log_printf("[xHCI] port %u USB3 link not resettable "
                       "(PLS=%u PORTSC=%08x)\n", port, pls, value);
            return USB_SPEED_UNKNOWN;
        } else {
            log_printf("[xHCI] port %u USB3 link unstable, regular reset "
                       "suppressed (PLS=%u PORTSC=%08x)\n", port, pls, value);
            return USB_SPEED_UNKNOWN;
        }
    }

    if (!attempted && !usb3_port) {
        value = xhci_port_reset_attempt(c, reg, false);
        attempted = true;
    }

    if (!attempted && usb3_port && !(value & XHCI_PORT_PED)) {
        return USB_SPEED_UNKNOWN;
    }
    if (usb3_port && attempted && !warm_attempted &&
        !(value & XHCI_PORT_PED)) {
        uint8_t pls = (uint8_t)XHCI_PORT_PLS_GET(value);
        if (pls == 6 || pls == 8 || pls == 10) {
            log_printf("[xHCI] port %u USB3 reset recovery fallback "
                       "(PLS=%u PORTSC=%08x)\n", port, pls, value);
            value = xhci_port_reset_attempt(c, reg, true);
        }
    }
    xhci_port_clear_changes(c, reg, value);
    xhci_drain_events(c);

    if (!(value & XHCI_PORT_CCS)) return USB_SPEED_UNKNOWN;
    if (!(value & XHCI_PORT_PED)) {
        log_printf("[xHCI] port %u reset failed: before=%08x after=%08x\n",
                   port, before, value);
        return USB_SPEED_UNKNOWN;
    }
    return xhci_port_speed(value);
}

static void xhci_note_port_failure(xhci_controller_t* c, uint32_t port,
                                   const char* reason) {
    if (!c || port == 0 || port > XHCI_MAX_PORTS) return;
    uint32_t index = port - 1;
    if (c->port_enum_failures[index] < UINT8_MAX) {
        c->port_enum_failures[index]++;
    }
    c->enumeration_failures_total++;
    if (c->port_enum_failures[index] >= XHCI_PORT_FAILURE_LIMIT) {
        c->port_fused[index] = true;
        c->port_retry[index] = 0;
        log_printf("[xHCI] port %u breaker open after %u failures "
                   "(%s); waiting for disconnect/reconnect\n",
                   port, c->port_enum_failures[index], reason);
    } else {
        c->port_retry[index] = XHCI_PORT_RETRY_POLLS;
    }
}

static void xhci_poll(usb_hci_t* hci) {
    xhci_controller_t* c = hci->priv;
    if (!c) return;
    xhci_drain_events(c);
    for (uint32_t port = 1; port <= c->max_ports; port++) {
        uint32_t reg = c->operational + 0x400 + 0x10u * (port - 1);
        uint32_t value = xhci_read32(c, reg);
        uint32_t changes = value & XHCI_PORT_CHANGES;
        if (changes) {
            xhci_port_clear_changes(c, reg, value);
        }
        if (!(value & XHCI_PORT_CCS)) {
            if (c->port_seen[port - 1]) {
                usb_disconnect_port(hci, (uint8_t)port);
                c->port_seen[port - 1] = false;
            }
            c->port_retry[port - 1] = 0;
            c->port_control_failures[port - 1] = 0;
            c->port_enum_failures[port - 1] = 0;
            c->port_fused[port - 1] = false;
            c->port_reset_skipped[port - 1] = false;
            continue;
        }
        if (c->port_seen[port - 1]) continue;

        if (c->port_fused[port - 1]) {
            if (changes & XHCI_PORT_CSC) {
                c->port_fused[port - 1] = false;
                c->port_control_failures[port - 1] = 0;
                c->port_enum_failures[port - 1] = 0;
                c->port_reset_skipped[port - 1] = false;
            } else {
                continue;
            }
        }

        if (c->port_retry[port - 1] > 0) {
            c->port_retry[port - 1]--;
            continue;
        }

        uint8_t speed = (uint8_t)xhci_port_reset(hci, (uint8_t)port);
        if (speed == USB_SPEED_UNKNOWN) {
            xhci_note_port_failure(c, port, "reset");
            continue;
        }

        uint32_t reset_status = xhci_read32(c, reg);
        log_printf("[xHCI] root port %u %s connected "
                   "(PORTSC=%08x PLS=%u)\n", port,
                   usb_speed_name(speed), reset_status,
                   XHCI_PORT_PLS_GET(reset_status));

        int enum_res = usb_enumerate(hci, (uint8_t)port, speed);
        if (enum_res == 0) {
            c->port_seen[port - 1] = true;
            c->port_retry[port - 1] = 0;
            c->port_control_failures[port - 1] = 0;
            c->port_enum_failures[port - 1] = 0;
            c->port_fused[port - 1] = false;
            c->port_reset_skipped[port - 1] = false;
        } else {
            c->port_seen[port - 1] = false;
            xhci_note_port_failure(c, port, "enumerate");
            log_printf("[xHCI] port %u enumerate failed (%d), failures=%u\n",
                       port, enum_res, c->port_enum_failures[port - 1]);
        }
    }
}

/*
 * FreeBSD _ref-xhci.c xhci_halt_controller / xECP USBLEGSUP BIOS-to-OS handoff.
 */
static void xhci_bios_handoff(xhci_controller_t* c, uint32_t hccparams1) {
    uint32_t xecp = (hccparams1 >> 16) & 0xFFFFu;
    uint32_t offset = xecp << 2;
    uint32_t usb3_ports = 0;
    for (int i = 0; i < 64 && offset >= 0x10 && offset + 8 <= 0x10000; i++) {
        uint32_t cap = xhci_read32(c, offset);
        uint8_t cap_id = (uint8_t)(cap & 0xFFu);
        uint8_t next = (uint8_t)((cap >> 8) & 0xFFu);
        if (cap_id == 1u) { /* USB Legacy Support (USBLEGSUP) */
            xhci_write32(c, offset, cap | (1u << 24));
            bool released = false;
            for (int t = 0; t < 1000; t++) {
                cap = xhci_read32(c, offset);
                if (!(cap & (1u << 16))) {
                    released = true;
                    break;
                }
                usb_delay_ms(1);
            }
            if (!released) {
                log_printf("[xHCI] BIOS ownership timeout at xECP 0x%x "
                           "(continuing)\n", offset);
            }

            /* Mask legacy SMI sources and acknowledge their W1C status bits. */
            uint32_t ctlsts = xhci_read32(c, offset + 4);
            ctlsts &= ~0x0000E01Fu;
            ctlsts |= (7u << 29);
            xhci_write32(c, offset + 4, ctlsts);
            if (released) {
                log_printf("[xHCI] BIOS ownership hand-off complete at xECP "
                           "0x%x\n", offset);
            }
        } else if (cap_id == 2u && offset + 16 <= 0x10000) {
            /* Supported Protocol: identify USB3 root-port ranges. */
            uint32_t ports = xhci_read32(c, offset + 8);
            uint8_t major = (uint8_t)(cap >> 24);
            uint8_t first = (uint8_t)(ports & 0xFFu);
            uint8_t count = (uint8_t)((ports >> 8) & 0xFFu);
            if (major >= 3 && first && count) {
                uint32_t end = (uint32_t)first + count;
                if (end > c->max_ports + 1u) end = c->max_ports + 1u;
                for (uint32_t port = first; port < end; port++) {
                    c->usb3_port[port - 1] = true;
                    usb3_ports++;
                }
            }
        }
        if (next == 0) break;
        uint32_t next_offset = offset + ((uint32_t)next << 2);
        if (next_offset <= offset) break;
        offset = next_offset;
    }
    if (usb3_ports) {
        log_printf("[xHCI] Supported Protocol reports %u USB3 root port(s)\n",
                   usb3_ports);
    }

    /* Intel 7/8/9-series xHCI port routing (XUSB2PRM / USB3PRM). */
    if (c->pci && c->pci->vendor == 0x8086) {
        uint16_t did = c->pci->device;
        int legacy_routing = ((did >= 0x1E00 && did <= 0x1EFF) ||
                              (did >= 0x8C00 && did <= 0x9D00));
        if (legacy_routing) {
            uint32_t usb2prm = pci_config_read(c->pci->bus, c->pci->slot,
                                               c->pci->function, 0xD4);
            if (usb2prm) {
                uint32_t route = pci_config_read(
                    c->pci->bus, c->pci->slot, c->pci->function, 0xD0);
                uint32_t updated = route | usb2prm;
                if (updated != route) {
                    pci_config_write(c->pci->bus, c->pci->slot,
                                     c->pci->function, 0xD0, updated);
                }
            }
            uint32_t usb3prm = pci_config_read(c->pci->bus, c->pci->slot,
                                               c->pci->function, 0xDC);
            if (usb3prm) {
                uint32_t enable = pci_config_read(
                    c->pci->bus, c->pci->slot, c->pci->function, 0xD8);
                uint32_t updated = enable | usb3prm;
                if (updated != enable) {
                    pci_config_write(c->pci->bus, c->pci->slot,
                                     c->pci->function, 0xD8, updated);
                }
            }
            log_printf("[xHCI] Intel legacy port routing applied\n");
        }
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
    if (bar < 0) {
        log_printf("[xHCI] No valid MEM BAR found for device %02x:%02x.%x\n",
                   device->bus, device->slot, device->function);
        return -1;
    }
    if (usb_legacy_handoff(device) != 0) {
        log_printf("[xHCI] Generic legacy handoff deferred to xECP "
                   "handling\n");
    }

    xhci_controller_t* c = &g_xhci[g_xhci_count];
    memset(c, 0, sizeof(*c));
    c->pci = device;
    c->cap = usb_map_mmio(device->bar[bar].address, 0x10000);
    if (!c->cap) {
        log_printf("[xHCI] Failed to map MMIO BAR at 0x%lx\n",
                   (unsigned long)device->bar[bar].address);
        return -1;
    }
    pci_enable_memory(device);
    pci_enable_bus_master(device);
    c->cap_length = c->cap[0];
    c->operational = c->cap_length;
    c->runtime_offset = xhci_read32(c, 0x18) & ~0x1Fu;
    c->doorbell_offset = xhci_read32(c, 0x14) & ~0x3u;
    c->op = c->cap + c->operational;
    c->runtime = c->cap + c->runtime_offset;
    c->doorbell = c->cap + c->doorbell_offset;
    uint32_t hcsparams1 = xhci_read32(c, 0x04);
    uint32_t hcsparams2 = xhci_read32(c, 0x08);
    uint32_t hccparams1 = xhci_read32(c, 0x10);
    c->max_slots = hcsparams1 & 0xFF;
    if (c->max_slots > XHCI_MAX_SLOTS) c->max_slots = XHCI_MAX_SLOTS;
    c->max_ports = (hcsparams1 >> 24) & 0xFF;
    if (c->max_ports > XHCI_MAX_PORTS) c->max_ports = XHCI_MAX_PORTS;
    c->context_size = (hccparams1 & (1u << 2)) ? 64 : 32;

    uint32_t scratchpad_count =
        (((hcsparams2 >> 21) & 0x1F) << 5) |
        ((hcsparams2 >> 27) & 0x1F);

    log_printf("[xHCI] caplen=%u RTSOFF=%04x DB_OFF=%04x HCS1=%08x HCS2=%08x HCC1=%08x\n",
               c->cap_length, c->runtime_offset, c->doorbell_offset,
               hcsparams1, hcsparams2, hccparams1);
    log_printf("[xHCI] maxSlots=%u maxPorts=%u ctxSize=%u scratch=%u 64bitAddr=%d\n",
               c->max_slots, c->max_ports, c->context_size, scratchpad_count,
               (int)(hccparams1 & 1u));

    if (!c->max_slots || !c->max_ports) {
        log_printf("[xHCI] Invalid max_slots=%u or max_ports=%u\n", c->max_slots, c->max_ports);
        return -1;
    }

    xhci_bios_handoff(c, hccparams1);

    if (usb_dma_alloc_zero(XHCI_ARENA_SIZE,
                           (hccparams1 & 1u) ? UINT64_MAX : UINT32_MAX,
                           PAGE_SIZE, &c->arena) != 0) {
        log_printf("[xHCI] Failed to allocate arena\n");
        return -1;
    }
    if (usb_dma_alloc_zero(65536,
                           (hccparams1 & 1u) ? UINT64_MAX : UINT32_MAX,
                           64, &c->bulk_staging) != 0) {
        log_printf("[xHCI] Failed to allocate bulk staging\n");
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
        log_printf("[xHCI] Failed to allocate internal data structures\n");
        return -1;
    }
    for (uint32_t i = 0; i < XHCI_MAX_SLOTS; i++) {
        xhci_slot_t* slot = &c->slots[i];
        uint64_t physical = 0;
        slot->output_context = xhci_arena_alloc(
            c, (size_t)c->context_size * 32, 64, &physical);
        slot->output_context_phys = physical;
    }

    if (scratchpad_count) {
        if (scratchpad_count > 64) {
            log_printf("[xHCI] Scratchpad count %u exceeds 64\n", scratchpad_count);
            return -1;
        }
        uint64_t* pointer_array = xhci_arena_alloc(
            c, scratchpad_count * sizeof(uint64_t), 64, NULL);
        if (!pointer_array || dma_alloc(
                scratchpad_count * PAGE_SIZE,
                (hccparams1 & 1u) ? UINT64_MAX : UINT32_MAX,
                PAGE_SIZE, &c->scratchpad) != 0) {
            log_printf("[xHCI] Failed to allocate scratchpad buffers\n");
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
    int reset_ok = 0;
    for (int i = 0; i < 1000; i++) {
        if (!(xhci_read32(c, c->operational) & XHCI_CMD_RESET) &&
            !(xhci_read32(c, c->operational + 4) & XHCI_STS_CNR)) {
            reset_ok = 1;
            break;
        }
        usb_delay_ms(1);
    }
    if (!reset_ok) {
        log_printf("[xHCI] HCRST/CNR timeout: USBCMD=%08x USBSTS=%08x\n",
                   xhci_read32(c, c->operational),
                   xhci_read32(c, c->operational + 4));
        return -1;
    }

    xhci_write32(c, c->operational + 0x38, c->max_slots);
    xhci_write64(c, c->operational + 0x30, c->dcbaa_phys);
    xhci_write64(c, c->operational + 0x18,
                 c->command_ring.physical_base | 1u);

    /* Sync memory before hardware accesses ERST and rings */
    dma_sync_for_device(&c->arena);

    xhci_write32(c, c->runtime_offset + 0x20 + 0x08, 1);
    xhci_write64(c, c->runtime_offset + 0x20 + 0x10, erst_phys);
    xhci_write64(c, c->runtime_offset + 0x20 + 0x18,
                 c->event_ring_phys | (1u << 3));

    xhci_write32(c, c->operational, XHCI_CMD_RUN);
    int run_ok = 0;
    for (int i = 0; i < 200; i++) {
        uint32_t status = xhci_read32(c, c->operational + 4);
        if (!(status & (XHCI_STS_HALTED | XHCI_STS_HOSTERR |
                        XHCI_STS_CNR))) {
            run_ok = 1;
            break;
        }
        usb_delay_ms(1);
    }
    if (!run_ok) {
        log_printf("[xHCI] RUN start warning: USBSTS=%08x\n",
                   xhci_read32(c, c->operational + 4));
    }

    for (uint32_t port = 1; port <= c->max_ports; port++) {
        uint32_t reg = c->operational + 0x400 + 0x10u * (port - 1);
        uint32_t value = xhci_read32(c, reg);
        if (value & XHCI_PORT_CHANGES) {
            xhci_port_clear_changes(c, reg, value);
        }
        if (!(value & XHCI_PORT_PP)) {
            xhci_write32(c, reg,
                         (value & XHCI_PORT_RW_BITS) | XHCI_PORT_PP);
        }
    }
    usb_delay_ms(300);

    for (uint32_t p = 1; p <= c->max_ports; p++) {
        uint32_t v = xhci_read32(c, c->operational + 0x400 + 0x10 * (p - 1));
        if (v & 1) {
            log_printf("[xHCI] port%02u PORTSC=%08x CCS=%d PP=%d PED=%d "
                       "PLS=%u speed=%u\n",
                       p, v, v & 1, (v >> 9) & 1, (v >> 1) & 1,
                       XHCI_PORT_PLS_GET(v),
                       (unsigned)XHCI_PORT_SPEED_GET(v));
        }
    }

    c->hci.name = "xHCI 3.0";
    c->hci.kind = USB_HCI_KIND_XHCI;
    c->hci.priv = c;
    c->hci.root_ports = (uint8_t)c->max_ports;
    c->hci.supports_hubs = true;
    c->hci.skips_set_address = true;
    c->hci.control = xhci_control;
    c->hci.bulk = xhci_bulk;
    c->hci.intr_in = xhci_intr_in;
    c->hci.port_reset = xhci_port_reset;
    c->hci.device_release = xhci_release;
    c->hci.poll = xhci_poll;
    usb_register_hci(&c->hci);
    g_xhci_count++;
    xhci_poll(&c->hci);
    return 0;
}
