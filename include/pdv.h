#ifndef _KERNEL_PDV_H
#define _KERNEL_PDV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pdv_format.h"
#include "../sdk/ddk/include/pdv/ddk.h"

#define PDV_MAX_LOADED 32

typedef struct {
    char     filename[48];
    char     driver_name[32];
    char     provider[32];
    char     version_str[16];
    uint16_t driver_class;
    uint64_t base_virt;
    size_t   rx_pages;
    size_t   ro_pages;
    size_t   rw_pages;
} pdv_module_info_t;

int pdv_load_file(const char* vfs_path, bool safe_mode, bool check_hw_match);
int pdv_autoload_boot_drivers(bool safe_mode, bool skip_all);
int pdv_get_loaded_count(void);
const pdv_module_info_t* pdv_get_loaded_info(int index);

/* Subsystem registration hooks implemented in gfx_accel.c and audio.c */
int  gfx_accel_register_pdv_driver(const pdv_display_driver_t* drv);
void gfx_accel_notify_ati_cap(void);
int  audio_register_pdv_driver(const pdv_audio_driver_t* drv);
void audio_set_probe_status(const char* status);

#endif /* _KERNEL_PDV_H */
