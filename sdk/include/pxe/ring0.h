#ifndef _PXE_RING0_H
#define _PXE_RING0_H

#include "abi.h"

/*
 * Pixel Neko OS Ring 0 Privileged Application API (<pxe/ring0.h>)
 * Available ONLY to PXE_RING_0 executables (e.g. sysinfo.pxe, install.pxe, taskmgr.pxe).
 */

#define PXE_R0_API_VERSION  0x0100u

/* Process states */
#define PXE_PROC_READY      0u
#define PXE_PROC_RUNNING    1u
#define PXE_PROC_BLOCKED    2u
#define PXE_PROC_ZOMBIE     3u

/* Power actions */
#define PXE_POWER_SHUTDOWN  1
#define PXE_POWER_REBOOT    2

typedef struct {
    uint32_t pid;
    uint32_t ppid;
    uint8_t  exec_ring;       /* 0 = Ring 0 kernel process, 3 = Ring 3 user process */
    uint8_t  state;           /* PXE_PROC_* */
    uint16_t window_count;    /* Number of open desktop windows owned by this PID */
    uint32_t reserved;
    uint64_t cpu_ticks_total; /* Accumulated timer ticks consumed */
    uint64_t mem_bytes_used;  /* Physical + tracked heap bytes */
    char     name[32];
    char     exe_path[128];
} pxe_r0_proc_info_t;

typedef struct {
    uint64_t total_pages;
    uint64_t free_pages;
    uint64_t used_pages;
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint64_t pdv_loaded_count;
    uint64_t process_count;
} pxe_r0_mem_info_t;

typedef struct {
    uint16_t segment;
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  function;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsys_vendor;
    uint16_t subsys_device;
    uint8_t  revision;
    uint8_t  irq_line;
    uint16_t reserved;
    uint64_t bar0_addr;
    uint64_t bar0_size;
    char     class_name[40];
} pxe_r0_pci_info_t;

typedef struct {
    uint8_t  address;
    uint8_t  speed;
    uint8_t  port;
    uint8_t  parent_hub;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  protocol;
    uint8_t  endpoint_count;
    uint16_t vendor_id;
    uint16_t product_id;
    char     hci_name[24];
    char     speed_name[16];
} pxe_r0_usb_info_t;

typedef struct {
    char     filename[48];
    char     driver_name[32];
    char     provider[32];
    char     version_str[16];
    uint16_t driver_class;
    uint16_t reserved;
    uint64_t base_virt;
    uint32_t rx_pages;
    uint32_t ro_pages;
    uint32_t rw_pages;
} pxe_r0_pdv_info_t;

typedef struct {
    int32_t  part_index;
    int32_t  disk_index;
    uint8_t  disk_type;
    uint8_t  sys_id;
    uint8_t  fs_type;
    uint8_t  is_removable;
    uint8_t  is_efi_system;
    char     mounted_letter;
    uint16_t reserved;
    uint64_t start_lba;
    uint64_t total_sectors;
    uint64_t disk_total_sectors;
    char     disk_name[32];
    char     fs_name[16];
    char     label[32];
} pxe_r0_part_info_t;

/* Kernel Direct Dispatch Table injected into crt0_r0 via RDX */
typedef struct pxe_ring0_api {
    uint32_t api_version;
    uint32_t struct_size;

    /* Universal Runtime Primitives (backing libpxe_r0.a) */
    void     (*exit)(int status);
    int      (*getpid)(void);
    void     (*yield)(void);
    void     (*sleep_ms)(uint32_t ms);
    uint64_t (*get_ticks)(void);
    int      (*spawn)(const char* path, int argc, const char* const* argv);
    int      (*waitpid)(int pid, int* out_status);
    void     (*debug_log)(const char* msg);
    int      (*get_rtc_time)(pxe_rtc_time_t* out);
    int      (*get_basic_sysinfo)(pxe_basic_sysinfo_t* out);
    int      (*sys_config)(int cmd, int arg0, int arg1, const char* str_arg);

    /* Process-Tracked Ring 0 Heap */
    void*    (*r0_malloc)(size_t size);
    void*    (*r0_realloc)(void* ptr, size_t size);
    void     (*r0_free)(void* ptr);

    /* Direct Zero-Copy Windowing & GUI */
    int      (*win_create)(const pxe_win_desc_t* desc, pxe_win_surface_t* out_surf);
    int      (*win_destroy)(int win_id);
    int      (*win_set_title)(int win_id, const char* title);
    int      (*win_set_menu)(int win_id, const pxe_menu_bar_t* menu);
    int      (*win_get_surface)(int win_id, pxe_win_surface_t* out_surf);
    int      (*win_present)(int win_id, int x, int y, int w, int h);
    int      (*win_poll_event)(int win_id, pxe_event_t* out_ev, int block);
    int      (*clipboard_op)(int is_set, char* buf, size_t max_len);
    void     (*notify_banner)(const char* msg);
    void     (*beep)(void);

    /* Direct VFS Operations */
    int      (*fs_open)(const char* path);
    int      (*fs_read)(int fd, void* buf, size_t count);
    int      (*fs_seek)(int fd, uint64_t offset);
    int      (*fs_close)(int fd);
    int      (*fs_stat)(const char* path, pxe_dirent_t* out);
    int      (*fs_read_file)(const char* path, void* buf, size_t max_size);
    int      (*fs_write_file)(const char* path, const void* buf, size_t size);
    int      (*fs_listdir)(const char* path, pxe_dirent_t* out, int max_entries);
    int      (*fs_mkdir)(const char* path);
    int      (*fs_unlink)(const char* path);
    int      (*fs_rename)(const char* old_path, const char* new_path);
    int      (*fs_get_drives)(pxe_drive_info_t* out, int max_drives);
    int      (*fs_eject)(char letter);

    /* Audio Engine */
    int      (*audio_cmd)(int cmd, int arg, const char* path, void* io_buf);

    /* Ring 0 Privileged Process, Hardware & Disk Operations */
    int      (*enum_processes)(pxe_r0_proc_info_t* out_list, int max_count);
    int      (*kill_process)(uint32_t pid, int exit_code);
    int      (*focus_process_window)(uint32_t pid);
    void     (*get_memory_summary)(pxe_r0_mem_info_t* out);

    int      (*enum_pci_devices)(pxe_r0_pci_info_t* out, int max_count);
    int      (*enum_usb_devices)(pxe_r0_usb_info_t* out, int max_count);
    int      (*enum_pdv_drivers)(pxe_r0_pdv_info_t* out, int max_count);
    int      (*load_pdv_file)(const char* pdv_path);
    int      (*rescan_pdevice_dir)(void);

    int      (*enum_disks_and_parts)(pxe_r0_part_info_t* out, int max_count);
    int      (*format_partition)(int part_idx, int fs_type, const char* label);
    int      (*raw_disk_rw)(int disk_id, uint64_t lba, uint32_t count, void* buf, int is_write);
    int      (*vfs_remount_all)(void);
    void     (*power_control)(int action);
} pxe_ring0_api_t;

/* Public Ring 0 C API Wrappers (implemented in libpxe_r0.a) */
int  pxe_r0_enum_processes(pxe_r0_proc_info_t* out_list, int max_count);
int  pxe_r0_kill_process(uint32_t pid, int exit_code);
int  pxe_r0_focus_process_window(uint32_t pid);
void pxe_r0_get_memory_summary(pxe_r0_mem_info_t* out);

int  pxe_r0_enum_pci_devices(pxe_r0_pci_info_t* out, int max_count);
int  pxe_r0_enum_usb_devices(pxe_r0_usb_info_t* out, int max_count);
int  pxe_r0_enum_pdv_drivers(pxe_r0_pdv_info_t* out, int max_count);
int  pxe_r0_load_pdv_file(const char* pdv_path);
int  pxe_r0_rescan_pdevice_dir(void);

int  pxe_r0_enum_disks_and_parts(pxe_r0_part_info_t* out, int max_count);
int  pxe_r0_format_partition(int part_idx, int fs_type, const char* label);
int  pxe_r0_raw_disk_rw(int disk_id, uint64_t lba, uint32_t count, void* buf, bool is_write);
int  pxe_r0_vfs_remount_all(void);
void pxe_r0_power_control(int action);

#endif /* _PXE_RING0_H */

