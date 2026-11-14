#ifndef _KERNEL_PXE_H
#define _KERNEL_PXE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "process.h"
#include "pxe_format.h"
#include "../sdk/include/pxe/abi.h"
#include "../sdk/include/pxe/ring0.h"

void pxe_subsystem_init(void);

int  pxe_spawn_file(const char* path, int argc, const char* const* argv,
                    process_t* parent, bool bypass_r0_prompt);
int  pxe_ui_launch_from_path(const char* path, const char* arg1);
bool pxe_resolve_builtin_app_path(const char* filename, char* out_path, int out_size);
int  pxe_launch_builtin_app(const char* filename, const char* arg1, bool bypass_r0_prompt);
bool pxe_load_builtin_icon16(const char* filename, uint8_t out_icon[16][16]);
void pxe_release_r0_image(process_t* process);
bool pxe_is_trusted_apps_path(const char* path);

/* Shared Kernel Runtime Backend (used by both syscall.c and g_pxe_ring0_api) */
int  pxe_kern_get_rtc(pxe_rtc_time_t* out);
int  pxe_kern_get_basic_sysinfo(pxe_basic_sysinfo_t* out);
int  pxe_kern_get_display_caps(pxe_display_caps_t* out);
int  pxe_kern_sys_config(process_t* proc, int cmd, int arg0, int arg1, const char* str_arg);
int  pxe_kern_clipboard_op(int is_set, char* buf, size_t max_len);

int  pxe_kern_fs_open(process_t* proc, const char* path);
int  pxe_kern_fs_create(process_t* proc, const char* path);
int  pxe_kern_fs_read(process_t* proc, int fd, void* buf, size_t count);
int  pxe_kern_fs_write(process_t* proc, int fd, const void* buf, size_t count);
int  pxe_kern_fs_seek(process_t* proc, int fd, uint64_t offset);
int  pxe_kern_fs_close(process_t* proc, int fd);
int  pxe_kern_fs_stat(const char* path, pxe_dirent_t* out);
int  pxe_kern_fs_read_file(const char* path, void* buf, size_t max_size);
int  pxe_kern_fs_write_file(const char* path, const void* buf, size_t size);
int  pxe_kern_fs_listdir(const char* path, pxe_dirent_t* out, int max_entries);
int  pxe_kern_fs_mkdir(const char* path);
int  pxe_kern_fs_unlink(const char* path);
int  pxe_kern_fs_rename(const char* old_path, const char* new_path);
int  pxe_kern_fs_get_drives(pxe_drive_info_t* out, int max_drives);
int  pxe_kern_fs_eject(char letter);

int  pxe_kern_audio_get_status(pxe_audio_status_t* out);
int  pxe_kern_audio_cmd(int cmd, int arg, const char* path, void* io_buf);

#endif /* _KERNEL_PXE_H */

