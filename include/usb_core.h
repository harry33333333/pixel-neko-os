#ifndef _USB_CORE_H
#define _USB_CORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "pci.h"

#define USB_MAX_HCIS       8
#define USB_MAX_DEVICES    32
#define USB_MAX_ENDPOINTS  8
#define USB_MAX_HUB_PORTS  15

#define USB_SPEED_UNKNOWN  0
#define USB_SPEED_LOW      1
#define USB_SPEED_FULL     2
#define USB_SPEED_HIGH     3
#define USB_SPEED_SUPER    4

#define USB_HCI_KIND_UHCI  (1U << 0)
#define USB_HCI_KIND_OHCI  (1U << 1)
#define USB_HCI_KIND_EHCI  (1U << 2)
#define USB_HCI_KIND_XHCI  (1U << 3)

#define USB_DIR_OUT 0x00U
#define USB_DIR_IN  0x80U

#define USB_RT_STANDARD  0x00U
#define USB_RT_CLASS     0x20U
#define USB_RT_VENDOR    0x40U
#define USB_RT_DEVICE    0x00U
#define USB_RT_INTERFACE 0x01U
#define USB_RT_ENDPOINT  0x02U
#define USB_RT_OTHER     0x03U

#define USB_REQ_GET_STATUS        0x00U
#define USB_REQ_CLEAR_FEATURE     0x01U
#define USB_REQ_SET_FEATURE       0x03U
#define USB_REQ_SET_ADDRESS       0x05U
#define USB_REQ_GET_DESCRIPTOR    0x06U
#define USB_REQ_SET_DESCRIPTOR    0x07U
#define USB_REQ_GET_CONFIGURATION 0x08U
#define USB_REQ_SET_CONFIGURATION 0x09U
#define USB_REQ_GET_INTERFACE     0x0AU
#define USB_REQ_SET_INTERFACE     0x0BU

#define USB_DT_DEVICE     0x01U
#define USB_DT_CONFIG     0x02U
#define USB_DT_STRING     0x03U
#define USB_DT_INTERFACE  0x04U
#define USB_DT_ENDPOINT   0x05U
#define USB_DT_BOS        0x0FU
#define USB_DT_HID        0x21U
#define USB_DT_REPORT     0x22U
#define USB_DT_HUB        0x29U
#define USB_DT_SS_HUB     0x2AU

#define USB_CLASS_HID          0x03U
#define USB_CLASS_MASS_STORAGE 0x08U
#define USB_CLASS_HUB          0x09U
#define USB_HID_SUBCLASS_BOOT  0x01U
#define USB_HID_PROTOCOL_KBD   0x01U
#define USB_HID_PROTOCOL_MOUSE 0x02U

#define USB_EP_TYPE_CONTROL     0x00U
#define USB_EP_TYPE_ISOCHRONOUS 0x01U
#define USB_EP_TYPE_BULK        0x02U
#define USB_EP_TYPE_INTERRUPT   0x03U

typedef struct {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed)) usb_setup_packet_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} __attribute__((packed)) usb_device_desc_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
} __attribute__((packed)) usb_config_desc_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} __attribute__((packed)) usb_interface_desc_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} __attribute__((packed)) usb_endpoint_desc_t;

typedef struct {
    uint8_t address;
    uint8_t number;
    uint8_t direction;
    uint8_t type;
    uint8_t interval;
    uint16_t max_packet;
    uint8_t toggle;
    uint8_t interface_number;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint16_t hid_report_desc_len;
    void* hcd_priv;
} usb_endpoint_t;

struct usb_hci;
typedef struct usb_device usb_device_t;

typedef struct usb_hci {
    const char* name;
    uint32_t kind;
    void* priv;
    uint8_t root_ports;
    bool supports_hubs;
    bool skips_set_address;
    int (*control)(struct usb_hci* hci, usb_device_t* dev,
                   const usb_setup_packet_t* setup,
                   void* data, int length, int direction);
    int (*intr_in)(struct usb_hci* hci, usb_device_t* dev,
                   usb_endpoint_t* endpoint, void* data, int length);
    int (*bulk)(struct usb_hci* hci, usb_device_t* dev,
                usb_endpoint_t* endpoint, void* data, int length,
                int direction);
    int (*port_reset)(struct usb_hci* hci, uint8_t port);
    void (*device_release)(struct usb_hci* hci, usb_device_t* dev);
    void (*poll)(struct usb_hci* hci);
} usb_hci_t;

struct usb_device {
    usb_hci_t* hci;
    uint8_t address;
    uint8_t speed;
    uint8_t port;
    uint8_t parent_hub_address;
    uint8_t parent_port;
    uint8_t hub_depth;
    uint8_t hs_hub_address;
    uint8_t hs_port_number;
    uint16_t max_packet0;
    uint8_t configuration;
    uint16_t vendor;
    uint16_t product;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol;
    uint8_t interface_number;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t endpoint_count;
    uint8_t configuration_value;
    uint16_t route;
    bool is_hub;
    uint8_t hub_port_count;
    uint16_t hub_ports_seen;
    uint8_t child_address[USB_MAX_HUB_PORTS + 1];
    usb_endpoint_t endpoints[USB_MAX_ENDPOINTS];
    void* hcd_priv;
};

void usb_init(void);
void usb_poll(void);
void usb_delay_ms(uint32_t milliseconds);

void usb_register_hci(usb_hci_t* hci);
usb_device_t* usb_alloc_device(void);
void usb_release_device(usb_device_t* dev);
usb_device_t* usb_find_device_by_address(uint8_t address);
usb_device_t* usb_find_child(usb_device_t* hub, uint8_t port);

int usb_control_msg(usb_device_t* dev, int direction, uint8_t request,
                    uint16_t value, uint16_t index, void* data, int length);
int usb_enumerate(usb_hci_t* hci, uint8_t port, uint8_t speed);
int usb_enumerate_hub(usb_hci_t* hci, usb_device_t* hub,
                      uint8_t port, uint8_t speed);
void usb_disconnect_port(usb_hci_t* hci, uint8_t root_port);
void usb_disconnect_child(usb_device_t* hub, uint8_t port);

uint32_t usb_get_hci_count(void);
uint32_t usb_get_device_count(void);
uint32_t usb_get_controller_kinds(void);
const char* usb_host_controller_summary(void);
const char* usb_speed_name(uint8_t speed);

void usb_memset(void* destination, uint8_t value, size_t length);
void usb_memcpy(void* destination, const void* source, size_t length);

#endif /* _USB_CORE_H */
