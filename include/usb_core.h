/*
 * Pixel Neko Next - USB Core Definitions and Subsystem Interface
 *
 * Derived from FreeBSD sys/dev/usb/usb_core.h
 * Copyright (c) 2008 Hans Petter Selasky <hselasky@FreeBSD.org>
 * Copyright (c) 1998, 2004 Lennart Augustsson <lennart@augustsson.net>
 * Copyright (c) 2026 Pixel Neko Project
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef USB_CORE_H
#define USB_CORE_H

#include "pci.h"

/* USB Device Speeds */
#define USB_SPEED_UNKNOWN 0
#define USB_SPEED_LOW     1  /* 1.5 Mbps */
#define USB_SPEED_FULL    2  /* 12 Mbps */
#define USB_SPEED_HIGH    3  /* 480 Mbps */

/* USB Descriptor Types */
#define USB_DT_DEVICE        0x01
#define USB_DT_CONFIG        0x02
#define USB_DT_STRING        0x03
#define USB_DT_INTERFACE     0x04
#define USB_DT_ENDPOINT      0x05
#define USB_DT_HID           0x21
#define USB_DT_REPORT        0x22
#define USB_DT_HUB           0x29

/* USB Request Types */
#define USB_DIR_OUT          0x00
#define USB_DIR_IN           0x80
#define USB_RT_STANDARD      0x00
#define USB_RT_CLASS         0x20
#define USB_RT_VENDOR        0x40
#define USB_RT_DEVICE        0x00
#define USB_RT_INTERFACE     0x01
#define USB_RT_ENDPOINT      0x02
#define USB_RT_OTHER         0x03

/* Standard USB Requests */
#define USB_REQ_GET_STATUS        0x00
#define USB_REQ_CLEAR_FEATURE     0x01
#define USB_REQ_SET_FEATURE       0x03
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_DESCRIPTOR    0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_GET_INTERFACE     0x0A
#define USB_REQ_SET_INTERFACE     0x0B

/* Endpoint Attributes */
#define USB_EP_TYPE_CONTROL     0x00
#define USB_EP_TYPE_ISOCHRONOUS 0x01
#define USB_EP_TYPE_BULK        0x02
#define USB_EP_TYPE_INTERRUPT   0x03

#pragma pack(push, 1)

typedef struct {
    unsigned char  bmRequestType;
    unsigned char  bRequest;
    unsigned short wValue;
    unsigned short wIndex;
    unsigned short wLength;
} usb_setup_packet_t;

typedef struct {
    unsigned char  bLength;
    unsigned char  bDescriptorType;
    unsigned short bcdUSB;
    unsigned char  bDeviceClass;
    unsigned char  bDeviceSubClass;
    unsigned char  bDeviceProtocol;
    unsigned char  bMaxPacketSize0;
    unsigned short idVendor;
    unsigned short idProduct;
    unsigned short bcdDevice;
    unsigned char  iManufacturer;
    unsigned char  iProduct;
    unsigned char  iSerialNumber;
    unsigned char  bNumConfigurations;
} usb_device_desc_t;

typedef struct {
    unsigned char  bLength;
    unsigned char  bDescriptorType;
    unsigned short wTotalLength;
    unsigned char  bNumInterfaces;
    unsigned char  bConfigurationValue;
    unsigned char  iConfiguration;
    unsigned char  bmAttributes;
    unsigned char  bMaxPower;
} usb_config_desc_t;

typedef struct {
    unsigned char bLength;
    unsigned char bDescriptorType;
    unsigned char bInterfaceNumber;
    unsigned char bAlternateSetting;
    unsigned char bNumEndpoints;
    unsigned char bInterfaceClass;
    unsigned char bInterfaceSubClass;
    unsigned char bInterfaceProtocol;
    unsigned char iInterface;
} usb_interface_desc_t;

typedef struct {
    unsigned char  bLength;
    unsigned char  bDescriptorType;
    unsigned char  bEndpointAddress;
    unsigned char  bmAttributes;
    unsigned short wMaxPacketSize;
    unsigned char  bInterval;
} usb_endpoint_desc_t;

#pragma pack(pop)

#define USB_MAX_ENDPOINTS 8
#define USB_MAX_DEVICES   16
#define USB_MAX_HCIS      8

#define USB_HCI_KIND_UHCI 1U
#define USB_HCI_KIND_EHCI 2U

typedef struct {
    unsigned char  address;
    unsigned char  direction; /* USB_DIR_IN or USB_DIR_OUT */
    unsigned char  type;      /* USB_EP_TYPE_* */
    unsigned char  interval;
    unsigned short max_packet;
    unsigned char  toggle;
    void*          hcd_priv;
} usb_endpoint_t;

struct usb_device;
struct usb_hci;

typedef struct usb_device {
    struct usb_hci*  hci;
    unsigned char    address;
    unsigned char    speed;        /* USB_SPEED_* */
    unsigned char    port;         /* Port on root or hub */
    unsigned char    parent_hub;   /* Address of parent hub (0 if root) */
    unsigned char    parent_port;  /* 1-based port on parent hub */
    unsigned char    hub_depth;
    unsigned char    max_packet0;
    unsigned char    config_value;
    unsigned short   vendor;
    unsigned short   product;
    unsigned char    class_code;
    unsigned char    subclass;
    unsigned char    protocol;
    unsigned char    interface_number;
    unsigned char    interface_protocol;
    int              endpoint_count;
    usb_endpoint_t   endpoints[USB_MAX_ENDPOINTS];
} usb_device_t;

typedef struct usb_hci {
    const char* name;
    unsigned int kind;
    void*       priv;
    int         supports_hubs;
    int  (*control)(struct usb_hci* hci, usb_device_t* dev,
                    const usb_setup_packet_t* setup, void* data, int len, int dir);
    int  (*intr_in)(struct usb_hci* hci, usb_device_t* dev, usb_endpoint_t* ep,
                    void* data, int len);
    int  (*bulk)(struct usb_hci* hci, usb_device_t* dev, usb_endpoint_t* ep,
                 void* data, int len, int dir);
    int  (*port_reset)(struct usb_hci* hci, int port);
    void (*device_release)(struct usb_hci* hci, usb_device_t* dev);
    void (*poll)(struct usb_hci* hci);
} usb_hci_t;

void usb_init(void);
void usb_poll(void);
int usb_firmware_legacy_active(void);
const char* usb_host_controller_summary(void);

void usb_register_hci(usb_hci_t* hci);
usb_device_t* usb_alloc_device(void);
void usb_release_device(usb_device_t* dev);

int usb_control_msg(usb_device_t* dev, int dir, unsigned char request,
                    unsigned short value, unsigned short index,
                    void* data, int len);

int usb_enumerate(usb_hci_t* hci, int port, int speed);
int usb_enumerate_hub(usb_hci_t* hci, usb_device_t* hub, int port, int speed);
void usb_disconnect_port(usb_hci_t* hci, int root_port);

void usb_memset(void* dst, unsigned char c, unsigned int n);
void usb_memcpy(void* dst, const void* src, unsigned int n);

#endif /* USB_CORE_H */
