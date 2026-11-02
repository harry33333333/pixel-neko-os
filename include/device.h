#ifndef _DEVICE_H
#define _DEVICE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define BUS_MAX_COUNT       16
#define DEVICE_MAX_COUNT    512
#define DRIVER_MAX_COUNT    32
#define DEVICE_MAX_RESOURCES 10

typedef enum {
    BUS_TYPE_ROOT = 0,
    BUS_TYPE_PCI,
    BUS_TYPE_ISA,
    BUS_TYPE_USB
} bus_type_t;

typedef enum {
    DEVICE_RESOURCE_MEMORY = 0,
    DEVICE_RESOURCE_IO,
    DEVICE_RESOURCE_IRQ
} device_resource_type_t;

typedef enum {
    DEVICE_STATE_REGISTERED = 0,
    DEVICE_STATE_BOUND,
    DEVICE_STATE_FAILED
} device_state_t;

typedef struct device device_t;
typedef struct bus bus_t;
typedef struct device_driver device_driver_t;

typedef struct {
    device_resource_type_t type;
    uint64_t start;
    uint64_t length;
    bool shared;
} device_resource_t;

struct bus {
    uint32_t id;
    char name[32];
    bus_type_t type;
    device_t* devices;
    uint32_t device_count;
    bus_t* next;
};

struct device {
    uint32_t id;
    char name[48];
    bus_t* bus;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;
    uint8_t bus_number;
    uint8_t slot;
    uint8_t function;
    uint8_t irq;
    device_state_t state;
    device_driver_t* driver;
    device_resource_t resources[DEVICE_MAX_RESOURCES];
    uint8_t resource_count;
    void* private_data;
    device_t* next;
};

struct device_driver {
    const char* name;
    bus_type_t bus_type;
    bool (*match)(const device_t* device);
    int (*probe)(device_t* device);
    void (*remove)(device_t* device);
    bool registered;
    uint32_t bind_count;
    device_driver_t* next;
};

void device_model_init(void);
bus_t* bus_register(const char* name, bus_type_t type);
bus_t* bus_find(bus_type_t type);
bus_t* bus_get_by_id(uint32_t id);
uint32_t bus_get_count(void);

device_t* device_register(bus_t* bus, const char* name,
                          uint16_t vendor_id, uint16_t device_id,
                          uint8_t class_code, uint8_t subclass,
                          void* private_data);
device_t* device_get_by_id(uint32_t id);
uint32_t device_get_count(void);
bool device_add_resource(device_t* device, device_resource_type_t type,
                         uint64_t start, uint64_t length, bool shared);
bool device_region_is_free(device_resource_type_t type,
                           uint64_t start, uint64_t length);

device_driver_t* driver_register(device_driver_t* driver);
int driver_probe_all(void);
int driver_get_bind_count(void);
void device_tree_dump(void);

#endif /* _DEVICE_H */
