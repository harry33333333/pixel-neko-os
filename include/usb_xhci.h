#ifndef _USB_XHCI_H
#define _USB_XHCI_H

#include "pci.h"

int xhci_probe(const pci_device_t* device);

#endif
