#include "device.h"
#include "log.h"
#include "string.h"

typedef struct device_reservation {
    bool used;
    bool shared;
    device_resource_type_t type;
    uint64_t start;
    uint64_t length;
    device_t* owner;
} device_reservation_t;

static bus_t* g_buses = NULL;
static device_t* g_devices = NULL;
static device_driver_t* g_drivers = NULL;
static device_reservation_t g_reservations[1024];
static uint32_t g_bus_count = 0;
static uint32_t g_device_count = 0;
static uint32_t g_driver_count = 0;

static bool ranges_overlap(uint64_t a_start, uint64_t a_length,
                           uint64_t b_start, uint64_t b_length) {
    if (a_length == 0 || b_length == 0) return false;
    if (a_start > UINT64_MAX - a_length ||
        b_start > UINT64_MAX - b_length) {
        return true;
    }
    return a_start < b_start + b_length && b_start < a_start + a_length;
}

void device_model_init(void) {
    g_buses = NULL;
    g_devices = NULL;
    g_drivers = NULL;
    g_bus_count = 0;
    g_device_count = 0;
    g_driver_count = 0;
    memset(g_reservations, 0, sizeof(g_reservations));
}

bus_t* bus_register(const char* name, bus_type_t type) {
    if (g_bus_count >= BUS_MAX_COUNT || !name) return NULL;

    bus_t* existing = bus_find(type);
    if (existing) return existing;

    static bus_t bus_pool[BUS_MAX_COUNT];
    bus_t* bus = &bus_pool[g_bus_count];
    memset(bus, 0, sizeof(*bus));
    bus->id = g_bus_count;
    bus->type = type;
    strncpy(bus->name, name, sizeof(bus->name) - 1);
    bus->next = g_buses;
    g_buses = bus;
    g_bus_count++;
    return bus;
}

bus_t* bus_find(bus_type_t type) {
    for (bus_t* bus = g_buses; bus; bus = bus->next) {
        if (bus->type == type) return bus;
    }
    return NULL;
}

bus_t* bus_get_by_id(uint32_t id) {
    for (bus_t* bus = g_buses; bus; bus = bus->next) {
        if (bus->id == id) return bus;
    }
    return NULL;
}

uint32_t bus_get_count(void) {
    return g_bus_count;
}

bool device_region_is_free(device_resource_type_t type,
                           uint64_t start, uint64_t length) {
    if (length == 0) return false;
    for (size_t i = 0; i < sizeof(g_reservations) / sizeof(g_reservations[0]); i++) {
        device_reservation_t* reservation = &g_reservations[i];
        if (!reservation->used || reservation->type != type ||
            reservation->shared) {
            continue;
        }
        if (ranges_overlap(start, length,
                           reservation->start, reservation->length)) {
            return false;
        }
    }
    return true;
}

static bool reserve_resource(device_t* device, device_resource_type_t type,
                             uint64_t start, uint64_t length, bool shared) {
    if (type != DEVICE_RESOURCE_IRQ && !shared &&
        !device_region_is_free(type, start, length)) {
        return false;
    }

    for (size_t i = 0; i < sizeof(g_reservations) / sizeof(g_reservations[0]); i++) {
        device_reservation_t* reservation = &g_reservations[i];
        if (!reservation->used) {
            reservation->used = true;
            reservation->shared = shared;
            reservation->type = type;
            reservation->start = start;
            reservation->length = length;
            reservation->owner = device;
            return true;
        }
    }
    return false;
}

bool device_add_resource(device_t* device, device_resource_type_t type,
                         uint64_t start, uint64_t length, bool shared) {
    if (!device || length == 0 ||
        device->resource_count >= DEVICE_MAX_RESOURCES) {
        return false;
    }
    if (!reserve_resource(device, type, start, length, shared)) {
        log_printf("[BUS ] Resource conflict: %s %s [0x%llx..0x%llx]\n",
                   device->name,
                   type == DEVICE_RESOURCE_MEMORY ? "MEM" :
                   (type == DEVICE_RESOURCE_IO ? "I/O" : "IRQ"),
                   (unsigned long long)start,
                   (unsigned long long)(start + length - 1));
        return false;
    }

    device_resource_t* resource = &device->resources[device->resource_count++];
    resource->type = type;
    resource->start = start;
    resource->length = length;
    resource->shared = shared;
    return true;
}

device_t* device_register(bus_t* bus, const char* name,
                          uint16_t vendor_id, uint16_t device_id,
                          uint8_t class_code, uint8_t subclass,
                          void* private_data) {
    static device_t device_pool[DEVICE_MAX_COUNT];
    if (!bus || !name || g_device_count >= DEVICE_MAX_COUNT) return NULL;

    device_t* device = &device_pool[g_device_count];
    memset(device, 0, sizeof(*device));
    device->id = g_device_count;
    device->bus = bus;
    device->vendor_id = vendor_id;
    device->device_id = device_id;
    device->class_code = class_code;
    device->subclass = subclass;
    device->state = DEVICE_STATE_REGISTERED;
    device->private_data = private_data;
    strncpy(device->name, name, sizeof(device->name) - 1);

    device->next = g_devices;
    g_devices = device;
    bus->device_count++;
    g_device_count++;
    return device;
}

device_t* device_get_by_id(uint32_t id) {
    for (device_t* device = g_devices; device; device = device->next) {
        if (device->id == id) return device;
    }
    return NULL;
}

uint32_t device_get_count(void) {
    return g_device_count;
}

device_driver_t* driver_register(device_driver_t* driver) {
    if (!driver || !driver->name || !driver->match ||
        g_driver_count >= DRIVER_MAX_COUNT) {
        return NULL;
    }
    driver->registered = true;
    driver->bind_count = 0;
    driver->next = g_drivers;
    g_drivers = driver;
    g_driver_count++;
    return driver;
}

int driver_probe_all(void) {
    int bound = 0;
    for (device_driver_t* driver = g_drivers; driver; driver = driver->next) {
        for (device_t* device = g_devices; device; device = device->next) {
            if (device->driver || device->bus->type != driver->bus_type ||
                !driver->match(device)) {
                continue;
            }

            device->driver = driver;
            device->state = DEVICE_STATE_BOUND;
            int result = driver->probe ? driver->probe(device) : 0;
            if (result < 0) {
                device->driver = NULL;
                device->state = DEVICE_STATE_FAILED;
                log_printf("[DRV ] %s failed to bind %s (%d)\n",
                           driver->name, device->name, result);
                continue;
            }
            driver->bind_count++;
            bound++;
            log_printf("[DRV ] Bound %s -> %s\n", driver->name, device->name);
        }
    }
    return bound;
}

int driver_get_bind_count(void) {
    int bound = 0;
    for (device_driver_t* driver = g_drivers; driver; driver = driver->next) {
        bound += (int)driver->bind_count;
    }
    return bound;
}

void device_tree_dump(void) {
    log_printf("[BUS ] Device tree: %u bus(es), %u device(s), %d bound driver(s)\n",
               g_bus_count, g_device_count, driver_get_bind_count());
    for (bus_t* bus = g_buses; bus; bus = bus->next) {
        log_printf("[BUS ] %s (%s): %u device(s)\n", bus->name,
                   bus->type == BUS_TYPE_PCI ? "PCIe/PCI" :
                   (bus->type == BUS_TYPE_ISA ? "ISA" : "ROOT"),
                   bus->device_count);
        for (device_t* device = g_devices; device; device = device->next) {
            if (device->bus != bus) continue;
            log_printf("[DEV ] %s vendor=%04x device=%04x class=%02x:%02x resource=%u driver=%s\n",
                       device->name, device->vendor_id, device->device_id,
                       device->class_code, device->subclass,
                       device->resource_count,
                       device->driver ? device->driver->name : "unbound");
        }
    }
}
