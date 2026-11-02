#ifndef _PCI_H
#define _PCI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PCI_MAX_DEVICES 512
#define PCI_BAR_COUNT   6

typedef struct {
    uint64_t address;
    uint64_t size;
    uint8_t index;
    bool is_io;
    bool is_64bit;
    bool prefetchable;
    bool valid;
    bool resource_conflict;
} pci_bar_t;

typedef struct {
    uint16_t segment;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint16_t vendor;
    uint16_t device;
    uint16_t subsystem_vendor;
    uint16_t subsystem_device;
    uint8_t revision;
    uint8_t prog_if;
    uint8_t subclass;
    uint8_t class_code;
    uint8_t header_type;
    uint8_t irq_line;
    uint8_t irq_pin;
    uint16_t command;
    pci_bar_t bar[PCI_BAR_COUNT];
} pci_device_t;

typedef enum {
    PCI_CONFIG_IO_PORTS = 0,
    PCI_CONFIG_ECAM
} pci_config_backend_t;

void pci_scan_all(void);
bool pci_has_scanned(void);
int pci_get_device_count(void);
const pci_device_t* pci_get_device(int index);
const pci_device_t* pci_find_by_id(uint16_t vendor, uint16_t device);
const pci_device_t* pci_find_class(uint8_t class_code, uint8_t subclass);

uint32_t pci_config_read(uint8_t bus, uint8_t slot,
                         uint8_t function, uint8_t offset);
void pci_config_write(uint8_t bus, uint8_t slot,
                      uint8_t function, uint8_t offset, uint32_t value);

uint16_t pci_enable_io(const pci_device_t* dev);
uint16_t pci_enable_memory(const pci_device_t* dev);
uint16_t pci_enable_bus_master(const pci_device_t* dev);
uint64_t pci_get_bar_size(const pci_device_t* dev, int bar);
uint64_t pci_get_bar_address(const pci_device_t* dev, int bar);
uint8_t pci_find_capability(const pci_device_t* dev, uint8_t capability);
const char* pci_class_name(uint8_t class_code, uint8_t subclass);
const char* pci_get_backend_name(void);
bool pci_ecam_active(void);

#endif /* _PCI_H */
