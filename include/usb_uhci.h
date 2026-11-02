#ifndef _USB_UHCI_H
#define _USB_UHCI_H

#include "pci.h"

int uhci_probe(const pci_device_t* device);

#endif
