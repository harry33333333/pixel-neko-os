#include "usb_core.h"
#include "usb_hid.h"
#include "usb_hub.h"
#include "usb_uhci.h"
#include "usb_ohci.h"
#include "usb_ehci.h"
#include "usb_xhci.h"
#include "device.h"
#include "port.h"
#include "log.h"
#include "string.h"

static usb_hci_t* g_hcis[USB_MAX_HCIS];
static uint32_t g_hci_count = 0;
static uint32_t g_controller_kinds = 0;
static usb_device_t g_devices[USB_MAX_DEVICES];
static uint32_t g_device_count = 0;
static uint8_t g_next_address = 1;
static uint64_t g_tsc_per_ms = 0;

#define PIT_INPUT_HZ 1193182ULL

static inline uint64_t read_tsc(void) {
    uint32_t low = 0;
    uint32_t high = 0;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

static bool calibrate_tsc_with_pit(void) {
    const uint32_t sample_ms = 10;
    uint16_t divisor =
        (uint16_t)((PIT_INPUT_HZ * sample_ms + 500) / 1000);
    uint8_t gate = inb(0x61);

    outb(0x61, (uint8_t)(gate & ~1u));
    outb(0x43, 0xB0); /* Channel 2, mode 0, binary count. */
    outb(0x42, (uint8_t)(divisor & 0xFF));
    outb(0x42, (uint8_t)(divisor >> 8));
    uint64_t start = read_tsc();
    outb(0x61, (uint8_t)(gate | 1u));

    bool elapsed = false;
    for (uint32_t spin = 0; spin < 5000000u; spin++) {
        if (inb(0x61) & 0x20u) {
            elapsed = true;
            break;
        }
        __asm__ volatile ("pause");
    }
    uint64_t end = read_tsc();
    outb(0x61, (uint8_t)(gate & ~1u));
    if (!elapsed || end <= start) return false;

    g_tsc_per_ms = (end - start) / sample_ms;
    return g_tsc_per_ms > 1000;
}

static void pit_delay_fallback(uint32_t milliseconds) {
    uint8_t gate = inb(0x61);
    while (milliseconds) {
        uint32_t chunk = milliseconds > 50 ? 50 : milliseconds;
        uint16_t divisor =
            (uint16_t)((PIT_INPUT_HZ * chunk + 500) / 1000);
        outb(0x61, (uint8_t)(gate & ~1u));
        outb(0x43, 0xB0);
        outb(0x42, (uint8_t)(divisor & 0xFF));
        outb(0x42, (uint8_t)(divisor >> 8));
        outb(0x61, (uint8_t)(gate | 1u));
        for (uint32_t spin = 0; spin < 5000000u; spin++) {
            if (inb(0x61) & 0x20u) break;
            __asm__ volatile ("pause");
        }
        outb(0x61, (uint8_t)(gate & ~1u));
        milliseconds -= chunk;
    }
}

void usb_memset(void* destination, uint8_t value, size_t length) {
    memset(destination, value, length);
}

void usb_memcpy(void* destination, const void* source, size_t length) {
    memcpy(destination, source, length);
}

void usb_delay_ms(uint32_t milliseconds) {
    if (!milliseconds) return;
    if (!g_tsc_per_ms && !calibrate_tsc_with_pit()) {
        pit_delay_fallback(milliseconds);
        return;
    }
    if (milliseconds > UINT64_MAX / g_tsc_per_ms) {
        pit_delay_fallback(milliseconds);
        return;
    }

    uint64_t target = read_tsc() + (uint64_t)milliseconds * g_tsc_per_ms;
    while ((int64_t)(read_tsc() - target) < 0) {
        __asm__ volatile ("pause");
    }
}

void usb_register_hci(usb_hci_t* hci) {
    if (!hci || g_hci_count >= USB_MAX_HCIS) return;
    g_hcis[g_hci_count++] = hci;
    g_controller_kinds |= hci->kind;
    log_printf("[USB ] Registered %s: root_ports=%u\n",
               hci->name ? hci->name : "HCI", hci->root_ports);
}

uint32_t usb_get_hci_count(void) {
    return g_hci_count;
}

uint32_t usb_get_device_count(void) {
    return g_device_count;
}

uint32_t usb_get_controller_kinds(void) {
    return g_controller_kinds;
}

void usb_release_device(usb_device_t* dev) {
    if (!dev) return;
    uint8_t parent_address = dev->address;
    usb_hci_t* hci = dev->hci;
    if (parent_address && hci) {
        bool removed;
        do {
            removed = false;
            for (uint32_t i = 0; i < USB_MAX_DEVICES; i++) {
                if (g_devices[i].hci == hci &&
                    g_devices[i].parent_hub_address == parent_address) {
                    usb_release_device(&g_devices[i]);
                    removed = true;
                    break;
                }
            }
        } while (removed);
    }
    if (dev->hci && dev->hci->device_release) {
        dev->hci->device_release(dev->hci, dev);
    }
    usb_hid_detach(dev);
    usb_hub_detach(dev);
    memset(dev, 0, sizeof(*dev));
    if (g_device_count) g_device_count--;
}

usb_device_t* usb_alloc_device(void) {
    for (uint32_t i = 0; i < USB_MAX_DEVICES; i++) {
        if (!g_devices[i].hci) {
            memset(&g_devices[i], 0, sizeof(g_devices[i]));
            g_device_count++;
            return &g_devices[i];
        }
    }
    return NULL;
}

usb_device_t* usb_find_device_by_address(uint8_t address) {
    if (address == 0) return NULL;
    for (uint32_t i = 0; i < USB_MAX_DEVICES; i++) {
        if (g_devices[i].hci && g_devices[i].address == address) {
            return &g_devices[i];
        }
    }
    return NULL;
}

usb_device_t* usb_find_child(usb_device_t* hub, uint8_t port) {
    if (!hub || !hub->address) return NULL;
    for (uint32_t i = 0; i < USB_MAX_DEVICES; i++) {
        if (g_devices[i].hci == hub->hci &&
            g_devices[i].parent_hub_address == hub->address &&
            g_devices[i].parent_port == port) {
            return &g_devices[i];
        }
    }
    return NULL;
}

static uint8_t allocate_address(void) {
    for (uint32_t attempts = 0; attempts < 126; attempts++) {
        uint8_t candidate = g_next_address++;
        if (g_next_address >= 127) g_next_address = 1;
        if (!usb_find_device_by_address(candidate)) return candidate;
    }
    return 0;
}

int usb_control_msg(usb_device_t* dev, int direction, uint8_t request,
                    uint16_t value, uint16_t index, void* data, int length) {
    if (!dev || !dev->hci || !dev->hci->control || length < 0) return -1;
    usb_setup_packet_t setup = {
        .bmRequestType = (uint8_t)direction,
        .bRequest = request,
        .wValue = value,
        .wIndex = index,
        .wLength = (uint16_t)length
    };
    return dev->hci->control(dev->hci, dev, &setup, data, length,
                             (direction & USB_DIR_IN) ? USB_DIR_IN : USB_DIR_OUT);
}

static void parse_configuration(usb_device_t* dev, const uint8_t* buffer,
                                int length) {
    int offset = 0;
    int selected_interface = -1;
    int selected_score = -1;
    uint8_t device_class = dev->class_code;
    uint8_t device_subclass = dev->subclass;
    uint8_t device_protocol = dev->protocol;
    while (offset + 2 <= length) {
        uint8_t item_length = buffer[offset];
        uint8_t item_type = buffer[offset + 1];
        if (item_length < 2 || offset + item_length > length) break;
        if (item_type == USB_DT_INTERFACE && item_length >= 9) {
            const usb_interface_desc_t* interface =
                (const usb_interface_desc_t*)(buffer + offset);
            uint8_t interface_class = interface->bInterfaceClass;
            uint8_t interface_subclass = interface->bInterfaceSubClass;
            uint8_t interface_protocol = interface->bInterfaceProtocol;
            if (interface_class == 0 && device_class) {
                interface_class = device_class;
                interface_subclass = device_subclass;
                interface_protocol = device_protocol;
            }
            int score = 0;
            if (interface_class == USB_CLASS_HID) score = 3;
            else if (interface_class == USB_CLASS_HUB) score = 2;
            else score = 1;
            if (score > selected_score) {
                selected_score = score;
                selected_interface = interface->bInterfaceNumber;
                dev->class_code = interface_class;
                dev->subclass = interface_subclass;
                dev->protocol = interface_protocol;
                dev->interface_number = interface->bInterfaceNumber;
                dev->interface_subclass = interface_subclass;
                dev->interface_protocol = interface_protocol;
            }
        }
        offset += item_length;
    }

    if (selected_interface < 0) return;
    int current_interface = -1;
    offset = 0;
    while (offset + 2 <= length && dev->endpoint_count < USB_MAX_ENDPOINTS) {
        uint8_t item_length = buffer[offset];
        uint8_t item_type = buffer[offset + 1];
        if (item_length < 2 || offset + item_length > length) break;
        if (item_type == USB_DT_INTERFACE && item_length >= 9) {
            current_interface = buffer[offset + 2];
        } else if (item_type == USB_DT_ENDPOINT && item_length >= 7 &&
                   current_interface == selected_interface) {
            const usb_endpoint_desc_t* descriptor =
                (const usb_endpoint_desc_t*)(buffer + offset);
            usb_endpoint_t* endpoint =
                &dev->endpoints[dev->endpoint_count++];
            memset(endpoint, 0, sizeof(*endpoint));
            endpoint->address = descriptor->bEndpointAddress;
            endpoint->number = descriptor->bEndpointAddress & 0x0F;
            endpoint->direction = (descriptor->bEndpointAddress & USB_DIR_IN)
                                      ? USB_DIR_IN
                                      : USB_DIR_OUT;
            endpoint->type = descriptor->bmAttributes & 0x03;
            endpoint->interval = descriptor->bInterval;
            endpoint->max_packet = descriptor->wMaxPacketSize & 0x07FFu;
        }
        offset += item_length;
    }
}

static void register_usb_device_model(usb_device_t* dev) {
    bus_t* usb_bus = bus_find(BUS_TYPE_USB);
    if (!usb_bus) return;
    char name[32];
    strcpy(name, "usb-");
    uint8_t address = dev->address;
    static const char hex[] = "0123456789abcdef";
    name[4] = hex[address >> 4];
    name[5] = hex[address & 0x0F];
    name[6] = '\0';
    device_t* model = device_register(usb_bus, name, dev->vendor, dev->product,
                                      dev->class_code, dev->subclass, dev);
    if (model) {
        model->irq = dev->hci ? (dev->hci->kind == USB_HCI_KIND_UHCI ? 1 : 0) : 0;
    }
}

static int enumerate_device(usb_hci_t* hci, usb_device_t* parent_hub,
                            uint8_t port, uint8_t speed) {
    usb_device_t* dev = usb_alloc_device();
    if (!dev) {
        log_printf("[USB ] Device table exhausted\n");
        return -1;
    }

    dev->hci = hci;
    dev->speed = speed;
    dev->port = port;
    dev->max_packet0 = speed >= USB_SPEED_HIGH ? 64 : 8;
    if (parent_hub) {
        dev->parent_hub_address = parent_hub->address;
        dev->parent_port = port;
        dev->hub_depth = (uint8_t)(parent_hub->hub_depth + 1);
        dev->route = (uint16_t)(parent_hub->route |
                     ((uint16_t)(port & 0x0F) << (parent_hub->hub_depth * 4)));
    }

    usb_device_desc_t descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    int result = usb_control_msg(
        dev,
        USB_DIR_IN | USB_RT_STANDARD | USB_RT_DEVICE,
        USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0,
        &descriptor, 8);
    if (result < 8) {
        log_printf("[USB ] Initial device descriptor failed on port %u (%d)\n",
                   port, result);
        usb_release_device(dev);
        return -1;
    }

    if (speed == USB_SPEED_SUPER && descriptor.bMaxPacketSize0 == 9) {
        dev->max_packet0 = 512;
    } else if (descriptor.bMaxPacketSize0 == 8 ||
               descriptor.bMaxPacketSize0 == 16 ||
               descriptor.bMaxPacketSize0 == 32 ||
               descriptor.bMaxPacketSize0 == 64) {
        dev->max_packet0 = descriptor.bMaxPacketSize0;
    } else {
        dev->max_packet0 = speed >= USB_SPEED_HIGH ? 64 : 8;
    }

    uint8_t address = allocate_address();
    if (!address) {
        usb_release_device(dev);
        return -1;
    }
    if (!hci->skips_set_address) {
        result = usb_control_msg(
            dev,
            USB_DIR_OUT | USB_RT_STANDARD | USB_RT_DEVICE,
            USB_REQ_SET_ADDRESS, address, 0, NULL, 0);
        if (result < 0) {
            log_printf("[USB ] SET_ADDRESS %u failed (%d)\n", address, result);
            usb_release_device(dev);
            return -1;
        }
        usb_delay_ms(5);
    }
    dev->address = address;

    memset(&descriptor, 0, sizeof(descriptor));
    result = usb_control_msg(
        dev,
        USB_DIR_IN | USB_RT_STANDARD | USB_RT_DEVICE,
        USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0,
        &descriptor, sizeof(descriptor));
    if (result >= (int)sizeof(descriptor) &&
        descriptor.bLength >= sizeof(descriptor)) {
        dev->vendor = descriptor.idVendor;
        dev->product = descriptor.idProduct;
        dev->class_code = descriptor.bDeviceClass;
        dev->subclass = descriptor.bDeviceSubClass;
        dev->protocol = descriptor.bDeviceProtocol;
    }

    uint8_t config_buffer[1024];
    memset(config_buffer, 0, sizeof(config_buffer));
    result = usb_control_msg(
        dev,
        USB_DIR_IN | USB_RT_STANDARD | USB_RT_DEVICE,
        USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0,
        config_buffer, 9);
    if (result >= 9) {
        usb_config_desc_t* config = (usb_config_desc_t*)config_buffer;
        uint16_t total = config->wTotalLength;
        if (total > sizeof(config_buffer)) total = sizeof(config_buffer);
        dev->configuration_value = config->bConfigurationValue;
        result = usb_control_msg(
            dev,
            USB_DIR_IN | USB_RT_STANDARD | USB_RT_DEVICE,
            USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0,
            config_buffer, total);
        if (result >= 9) {
            parse_configuration(dev, config_buffer, result);
        }
    }

    if (!dev->configuration_value) dev->configuration_value = 1;
    result = usb_control_msg(
        dev,
        USB_DIR_OUT | USB_RT_STANDARD | USB_RT_DEVICE,
        USB_REQ_SET_CONFIGURATION, dev->configuration_value, 0, NULL, 0);
    if (result < 0) {
        log_printf("[USB ] SET_CONFIGURATION failed for address %u\n",
                   dev->address);
        usb_release_device(dev);
        return -1;
    }
    usb_delay_ms(5);
    dev->configuration = dev->configuration_value;

    log_printf("[USB ] Device %u %s port=%u VID:PID=%04x:%04x class=%02x:%02x endpoints=%u\n",
               dev->address, usb_speed_name(dev->speed), port,
               dev->vendor, dev->product, dev->class_code, dev->subclass,
               dev->endpoint_count);
    register_usb_device_model(dev);

    if (dev->class_code == USB_CLASS_HUB) {
        dev->is_hub = true;
        if (usb_hub_attach(dev) != 0) {
            log_printf("[USB ] Hub attach failed for device %u\n", dev->address);
        }
    } else if (dev->class_code == USB_CLASS_HID) {
        if (usb_hid_attach(dev) != 0) {
            log_printf("[USB ] HID attach failed for device %u\n", dev->address);
        }
    }
    return 0;
}

int usb_enumerate(usb_hci_t* hci, uint8_t port, uint8_t speed) {
    return enumerate_device(hci, NULL, port, speed);
}

int usb_enumerate_hub(usb_hci_t* hci, usb_device_t* hub,
                      uint8_t port, uint8_t speed) {
    return enumerate_device(hci, hub, port, speed);
}

void usb_disconnect_child(usb_device_t* hub, uint8_t port) {
    if (!hub || port > USB_MAX_HUB_PORTS) return;
    uint8_t child_address = hub->child_address[port];
    usb_device_t* child = usb_find_device_by_address(child_address);
    if (child) {
        log_printf("[USB ] Hotplug disconnect hub=%u port=%u address=%u\n",
                   hub->address, port, child_address);
        usb_release_device(child);
    }
    hub->child_address[port] = 0;
    hub->hub_ports_seen &= (uint16_t)~(1u << port);
}

void usb_disconnect_port(usb_hci_t* hci, uint8_t root_port) {
    for (uint32_t i = 0; i < USB_MAX_DEVICES; i++) {
        usb_device_t* dev = &g_devices[i];
        if (!dev->hci || dev->hci != hci || dev->parent_hub_address != 0 ||
            dev->port != root_port) {
            continue;
        }
        uint8_t address = dev->address;
        log_printf("[USBHOT] Root port %u disconnected address %u\n",
                   root_port, address);
        usb_release_device(dev);
        for (uint32_t j = 0; j < USB_MAX_DEVICES; j++) {
            if (g_devices[j].hci == hci &&
                g_devices[j].parent_hub_address == address) {
                usb_release_device(&g_devices[j]);
            }
        }
    }
}

const char* usb_speed_name(uint8_t speed) {
    switch (speed) {
        case USB_SPEED_LOW: return "Low-Speed";
        case USB_SPEED_FULL: return "Full-Speed";
        case USB_SPEED_HIGH: return "High-Speed";
        case USB_SPEED_SUPER: return "SuperSpeed";
        default: return "Unknown-Speed";
    }
}

const char* usb_host_controller_summary(void) {
    if (!g_hci_count) return "No native USB controller";
    if ((g_controller_kinds & USB_HCI_KIND_XHCI) &&
        (g_controller_kinds & USB_HCI_KIND_EHCI)) {
        return "xHCI 3.0 + EHCI 2.0";
    }
    if (g_controller_kinds & USB_HCI_KIND_XHCI) return "xHCI 3.0";
    if (g_controller_kinds & USB_HCI_KIND_EHCI) return "EHCI 2.0";
    if ((g_controller_kinds & USB_HCI_KIND_UHCI) &&
        (g_controller_kinds & USB_HCI_KIND_OHCI)) {
        return "UHCI + OHCI 1.1";
    }
    if (g_controller_kinds & USB_HCI_KIND_UHCI) return "UHCI 1.1";
    if (g_controller_kinds & USB_HCI_KIND_OHCI) return "OHCI 1.1";
    return "USB controller active";
}

void usb_init(void) {
    g_hci_count = 0;
    g_controller_kinds = 0;
    g_device_count = 0;
    g_next_address = 1;
    memset(g_devices, 0, sizeof(g_devices));
    (void)usb_hid_selftest();

    if (!bus_find(BUS_TYPE_USB)) bus_register("usb", BUS_TYPE_USB);

    int pci_devices = pci_get_device_count();
    for (int pass = 0; pass < 4; pass++) {
        uint8_t wanted_prog_if = pass == 0 ? 0x30 :
                                 pass == 1 ? 0x20 :
                                 pass == 2 ? 0x10 : 0x00;
        for (int i = 0; i < pci_devices; i++) {
            const pci_device_t* pci = pci_get_device(i);
            if (!pci || pci->class_code != 0x0C ||
                pci->subclass != 0x03 || pci->prog_if != wanted_prog_if) {
                continue;
            }
            const char* type = pass == 0 ? "xHCI" :
                               pass == 1 ? "EHCI" :
                               pass == 2 ? "OHCI" : "UHCI";
            log_printf("[USB ] Probing %s controller %04x:%04x at %02x:%02x.%x\n",
                       type, pci->vendor, pci->device, pci->bus,
                       pci->slot, pci->function);
            if (pass == 0) xhci_probe(pci);
            else if (pass == 1) ehci_probe(pci);
            else if (pass == 2) ohci_probe(pci);
            else uhci_probe(pci);
        }
    }

    log_printf("[USB ] Initialization complete: controllers=%u devices=%u summary=%s\n",
               g_hci_count, g_device_count, usb_host_controller_summary());
}

void usb_poll(void) {
    for (uint32_t i = 0; i < g_hci_count; i++) {
        if (g_hcis[i] && g_hcis[i]->poll) g_hcis[i]->poll(g_hcis[i]);
    }
    usb_hub_poll();
    usb_hid_poll();
}
