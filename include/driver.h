#ifndef DRIVER_H
#define DRIVER_H

typedef enum {
    DRIVER_CLASS_DISPLAY = 0,
    DRIVER_CLASS_AUDIO   = 1,
    DRIVER_CLASS_STORAGE = 2,
    DRIVER_CLASS_INPUT   = 3
} driver_class_t;

typedef enum {
    DRIVER_STATE_UNPROBED = 0,
    DRIVER_STATE_ACTIVE   = 1,
    DRIVER_STATE_FALLBACK = 2,
    DRIVER_STATE_FAILED   = 3
} driver_state_t;

typedef struct {
    const char* name;
    driver_class_t dev_class;
    int (*probe)(void);
} kernel_driver_t;

typedef struct {
    const char* name;
    driver_class_t dev_class;
    driver_state_t state;
    int last_rc;
    const char* detail;
} driver_status_t;

int driver_register(const kernel_driver_t* drv);
int driver_probe_and_select(driver_class_t dev_class,
                            const kernel_driver_t* const* candidates,
                            int count,
                            const char* fallback_detail);
const driver_status_t* driver_get_active(driver_class_t dev_class);
int driver_get_status_count(void);
const driver_status_t* driver_get_status(int index);

#endif
