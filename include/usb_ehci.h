#ifndef _USB_EHCI_H
#define _USB_EHCI_H

#include "pci.h"

int ehci_probe(const pci_device_t* device);

#endif
