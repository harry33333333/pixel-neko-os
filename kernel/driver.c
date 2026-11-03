#include "driver.h"

#define MAX_DRIVER_STATUS 16
#define MAX_DRIVER_CLASSES 4

static driver_status_t g_statuses[MAX_DRIVER_STATUS];
static int g_status_count = 0;
static driver_status_t g_active_by_class[MAX_DRIVER_CLASSES];

static driver_status_t* record_status(const char* name,
                                      driver_class_t dev_class,
                                      driver_state_t state,
                                      int rc,
                                      const char* detail) {
    driver_status_t* slot = 0;
    for (int i = 0; i < g_status_count; i++) {
        if (g_statuses[i].name == name &&
            g_statuses[i].dev_class == dev_class) {
            slot = &g_statuses[i];
            break;
        }
    }
    if (!slot && g_status_count < MAX_DRIVER_STATUS) {
        slot = &g_statuses[g_status_count++];
    }
    if (slot) {
        slot->name = name;
        slot->dev_class = dev_class;
        slot->state = state;
        slot->last_rc = rc;
        slot->detail = detail;
    }
    return slot;
}

int driver_register(const kernel_driver_t* drv) {
    if (!drv || !drv->name || !drv->probe) return -1;
    record_status(drv->name, drv->dev_class, DRIVER_STATE_UNPROBED, 0,
                  "Registered");
    return 0;
}

int driver_probe_and_select(driver_class_t dev_class,
                            const kernel_driver_t* const* candidates,
                            int count,
                            const char* fallback_detail) {
    if (dev_class < 0 || dev_class >= MAX_DRIVER_CLASSES) return -1;

    g_active_by_class[dev_class].name = "Fallback";
    g_active_by_class[dev_class].dev_class = dev_class;
    g_active_by_class[dev_class].state = DRIVER_STATE_FALLBACK;
    g_active_by_class[dev_class].last_rc = -1;
    g_active_by_class[dev_class].detail =
        fallback_detail ? fallback_detail : "Fallback active";

    if (!candidates || count <= 0) return -1;

    for (int i = 0; i < count; i++) {
        const kernel_driver_t* drv = candidates[i];
        if (!drv || !drv->name || !drv->probe) continue;
        driver_register(drv);
        int rc = drv->probe();
        if (rc == 0) {
            driver_status_t* st = record_status(
                drv->name, dev_class, DRIVER_STATE_ACTIVE, 0, "Active");
            if (st) g_active_by_class[dev_class] = *st;
            return i;
        }
        record_status(drv->name, dev_class, DRIVER_STATE_FAILED, rc,
                      "Probe skipped or failed");
    }

    return -1;
}

const driver_status_t* driver_get_active(driver_class_t dev_class) {
    if (dev_class < 0 || dev_class >= MAX_DRIVER_CLASSES) return 0;
    return &g_active_by_class[dev_class];
}

int driver_get_status_count(void) {
    return g_status_count;
}

const driver_status_t* driver_get_status(int index) {
    if (index < 0 || index >= g_status_count) return 0;
    return &g_statuses[index];
}
