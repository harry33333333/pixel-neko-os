#ifndef _USB_HID_H
#define _USB_HID_H

#include "usb_core.h"

#define USB_HID_MAX_DEVICES 16

int usb_hid_attach(usb_device_t* device);
void usb_hid_detach(usb_device_t* device);
void usb_hid_poll(void);
int usb_hid_selftest(void);
int usb_hid_keyboard_count(void);
int usb_hid_mouse_count(void);
const char* usb_hid_controller_summary(void);

#endif
