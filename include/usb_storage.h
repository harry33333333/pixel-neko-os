#ifndef USB_STORAGE_H
#define USB_STORAGE_H

#include "usb_core.h"

int usb_storage_attach(usb_device_t* device);
void usb_storage_detach(usb_device_t* device);

#endif
