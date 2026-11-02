#ifndef _USB_OHCI_H
#define _USB_OHCI_H

#include "pci.h"

int ohci_probe(const pci_device_t* device);

#endif
