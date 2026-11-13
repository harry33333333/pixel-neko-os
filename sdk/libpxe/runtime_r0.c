#include <pxe/sys.h>
#include <pxe/mem.h>
#include <pxe/str.h>
#include <pxe/gui.h>
#include <pxe/fs.h>
#include <pxe/audio.h>
#include <pxe/ring0.h>

const pxe_ring0_api_t* __pxe_r0_api = (const pxe_ring0_api_t*)0;

/* =========================================================================
 * System Runtime (<pxe/sys.h>)
 * ========================================================================= */

void pxe_exit(int status) {
    if (__pxe_r0_api && __pxe_r0_api->exit) {
        __pxe_r0_api->exit(status);
    }
    for (;;) {
        __asm__ volatile ("pause");
    }
}

int pxe_getpid(void) {
    return (__pxe_r0_api && __pxe_r0_api->getpid) ? __pxe_r0_api->getpid() : -1;
}

void pxe_yield(void) {
    if (__pxe_r0_api && __pxe_r0_api->yield) __pxe_r0_api->yield();
}

void pxe_sleep_ms(uint32_t ms) {
    if (__pxe_r0_api && __pxe_r0_api->sleep_ms) __pxe_r0_api->sleep_ms(ms);
}

uint64_t pxe_get_ticks(void) {
    return (__pxe_r0_api && __pxe_r0_api->get_ticks) ? __pxe_r0_api->get_ticks() : 0ULL;
}

int pxe_spawn(const char* path, int argc, const char* const* argv) {
    return (__pxe_r0_api && __pxe_r0_api->spawn)
        ? __pxe_r0_api->spawn(path, argc, argv) : -1;
}

int pxe_waitpid(int pid, int* out_status) {
    return (__pxe_r0_api && __pxe_r0_api->waitpid)
        ? __pxe_r0_api->waitpid(pid, out_status) : -1;
}

void pxe_debug_log(const char* msg) {
    if (__pxe_r0_api && __pxe_r0_api->debug_log) __pxe_r0_api->debug_log(msg);
}

int pxe_get_rtc_time(pxe_rtc_time_t* out) {
    return (__pxe_r0_api && __pxe_r0_api->get_rtc_time)
        ? __pxe_r0_api->get_rtc_time(out) : -1;
}

int pxe_sys_get_basic_info(pxe_basic_sysinfo_t* out) {
    return (__pxe_r0_api && __pxe_r0_api->get_basic_sysinfo)
        ? __pxe_r0_api->get_basic_sysinfo(out) : -1;
}

int pxe_display_set_mode(int width, int height, int bpp) {
    if (!__pxe_r0_api || !__pxe_r0_api->sys_config) return -1;
    int wh = (width & 0xFFFF) | ((height & 0xFFFF) << 16);
    return __pxe_r0_api->sys_config(PXE_CFG_SET_RESOLUTION, wh, bpp, (const char*)0);
}

int pxe_display_apply_mode(int width, int height, int bpp) {
    if (!__pxe_r0_api || !__pxe_r0_api->sys_config) return -1;
    int wh = (width & 0xFFFF) | ((height & 0xFFFF) << 16);
    return __pxe_r0_api->sys_config(PXE_CFG_APPLY_RESOLUTION, wh, bpp, (const char*)0);
}

int pxe_desktop_set_pattern(int pattern) {
    return (__pxe_r0_api && __pxe_r0_api->sys_config)
        ? __pxe_r0_api->sys_config(PXE_CFG_SET_PATTERN, pattern, 0, (const char*)0) : -1;
}

int pxe_desktop_set_wallpaper(const char* path, int mode) {
    return (__pxe_r0_api && __pxe_r0_api->sys_config)
        ? __pxe_r0_api->sys_config(PXE_CFG_SET_WALLPAPER, mode, 0, path) : -1;
}

int pxe_desktop_set_wallpaper_mode(int mode) {
    return (__pxe_r0_api && __pxe_r0_api->sys_config)
        ? __pxe_r0_api->sys_config(PXE_CFG_SET_WALLPAPER_MODE, mode, 0, (const char*)0) : -1;
}

int pxe_desktop_clear_wallpaper(void) {
    return (__pxe_r0_api && __pxe_r0_api->sys_config)
        ? __pxe_r0_api->sys_config(PXE_CFG_CLEAR_WALLPAPER, 0, 0, (const char*)0) : -1;
}

int pxe_mouse_set_speed(int speed) {
    return (__pxe_r0_api && __pxe_r0_api->sys_config)
        ? __pxe_r0_api->sys_config(PXE_CFG_SET_MOUSE_SPEED, speed, 0, (const char*)0) : -1;
}

int pxe_settings_save(void) {
    return (__pxe_r0_api && __pxe_r0_api->sys_config)
        ? __pxe_r0_api->sys_config(PXE_CFG_SAVE_SETTINGS, 0, 0, (const char*)0) : -1;
}

/* =========================================================================
 * Process-Tracked Kernel Heap (<pxe/mem.h>)
 * ========================================================================= */

void* pxe_sbrk(intptr_t delta) {
    (void)delta;
    return (void*)(intptr_t)-1;
}

void* malloc(size_t size) {
    return (__pxe_r0_api && __pxe_r0_api->r0_malloc)
        ? __pxe_r0_api->r0_malloc(size) : (void*)0;
}

void* calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) return malloc(16);
    if (nmemb > (~((size_t)0)) / size) return (void*)0;
    size_t total = nmemb * size;
    void* p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void* realloc(void* ptr, size_t size) {
    return (__pxe_r0_api && __pxe_r0_api->r0_realloc)
        ? __pxe_r0_api->r0_realloc(ptr, size) : (void*)0;
}

void free(void* ptr) {
    if (__pxe_r0_api && __pxe_r0_api->r0_free) __pxe_r0_api->r0_free(ptr);
}

/* =========================================================================
 * GUI & Windowing (<pxe/gui.h>)
 * ========================================================================= */

int pxe_win_create(const pxe_win_desc_t* desc, pxe_win_surface_t* out_surface) {
    return (__pxe_r0_api && __pxe_r0_api->win_create)
        ? __pxe_r0_api->win_create(desc, out_surface) : -1;
}

int pxe_win_destroy(int win_id) {
    return (__pxe_r0_api && __pxe_r0_api->win_destroy)
        ? __pxe_r0_api->win_destroy(win_id) : -1;
}

int pxe_win_set_title(int win_id, const char* title) {
    return (__pxe_r0_api && __pxe_r0_api->win_set_title)
        ? __pxe_r0_api->win_set_title(win_id, title) : -1;
}

int pxe_win_set_menu(int win_id, const pxe_menu_bar_t* menu_bar) {
    return (__pxe_r0_api && __pxe_r0_api->win_set_menu)
        ? __pxe_r0_api->win_set_menu(win_id, menu_bar) : -1;
}

int pxe_win_get_surface(int win_id, pxe_win_surface_t* out_surface) {
    return (__pxe_r0_api && __pxe_r0_api->win_get_surface)
        ? __pxe_r0_api->win_get_surface(win_id, out_surface) : -1;
}

int pxe_win_present(int win_id) {
    return (__pxe_r0_api && __pxe_r0_api->win_present)
        ? __pxe_r0_api->win_present(win_id, 0, 0, -1, -1) : -1;
}

int pxe_win_present_rect(int win_id, int x, int y, int w, int h) {
    return (__pxe_r0_api && __pxe_r0_api->win_present)
        ? __pxe_r0_api->win_present(win_id, x, y, w, h) : -1;
}

int pxe_win_poll_event(int win_id, pxe_event_t* out_ev) {
    return (__pxe_r0_api && __pxe_r0_api->win_poll_event)
        ? __pxe_r0_api->win_poll_event(win_id, out_ev, 0) : -1;
}

int pxe_win_wait_event(int win_id, pxe_event_t* out_ev) {
    return (__pxe_r0_api && __pxe_r0_api->win_poll_event)
        ? __pxe_r0_api->win_poll_event(win_id, out_ev, 1) : -1;
}

int pxe_clipboard_get(char* buf, size_t max_len) {
    return (__pxe_r0_api && __pxe_r0_api->clipboard_op)
        ? __pxe_r0_api->clipboard_op(0, buf, max_len) : -1;
}

int pxe_clipboard_set(const char* text) {
    return (__pxe_r0_api && __pxe_r0_api->clipboard_op)
        ? __pxe_r0_api->clipboard_op(1, (char*)(uintptr_t)text, 0) : -1;
}

void pxe_notify_banner(const char* message) {
    if (__pxe_r0_api && __pxe_r0_api->notify_banner) __pxe_r0_api->notify_banner(message);
}

void pxe_beep(void) {
    if (__pxe_r0_api && __pxe_r0_api->beep) __pxe_r0_api->beep();
}

/* =========================================================================
 * Filesystem (<pxe/fs.h>)
 * ========================================================================= */

int pxe_open(const char* path) {
    return (__pxe_r0_api && __pxe_r0_api->fs_open)
        ? __pxe_r0_api->fs_open(path) : -1;
}

int pxe_read(int fd, void* buf, size_t count) {
    return (__pxe_r0_api && __pxe_r0_api->fs_read)
        ? __pxe_r0_api->fs_read(fd, buf, count) : -1;
}

int pxe_seek(int fd, uint64_t offset) {
    return (__pxe_r0_api && __pxe_r0_api->fs_seek)
        ? __pxe_r0_api->fs_seek(fd, offset) : -1;
}

int pxe_close(int fd) {
    return (__pxe_r0_api && __pxe_r0_api->fs_close)
        ? __pxe_r0_api->fs_close(fd) : -1;
}

int pxe_stat(const char* path, pxe_dirent_t* out) {
    return (__pxe_r0_api && __pxe_r0_api->fs_stat)
        ? __pxe_r0_api->fs_stat(path, out) : -1;
}

int pxe_read_file(const char* path, void* buf, size_t max_size) {
    return (__pxe_r0_api && __pxe_r0_api->fs_read_file)
        ? __pxe_r0_api->fs_read_file(path, buf, max_size) : -1;
}

int pxe_write_file(const char* path, const void* buf, size_t size) {
    return (__pxe_r0_api && __pxe_r0_api->fs_write_file)
        ? __pxe_r0_api->fs_write_file(path, buf, size) : -1;
}

int pxe_copy_file(const char* src_path, const char* dst_path,
                  void (*progress_cb)(uint64_t copied, uint64_t total)) {
    if (!src_path || !dst_path) return -1;
    pxe_dirent_t st;
    if (pxe_stat(src_path, &st) != 0 || st.is_dir) return -1;

    if (st.size == 0) {
        int wr = pxe_write_file(dst_path, "", 0);
        if (wr >= 0 && progress_cb) progress_cb(0, 0);
        return (wr >= 0) ? 0 : -1;
    }

    int fd = pxe_open(src_path);
    if (fd < 0) return -1;

    uint8_t* buf = (uint8_t*)malloc((size_t)st.size);
    if (!buf) {
        pxe_close(fd);
        return -1;
    }

    uint64_t copied = 0;
    while (copied < st.size) {
        size_t chunk = (size_t)(st.size - copied);
        if (chunk > 32768u) chunk = 32768u;
        int n = pxe_read(fd, buf + copied, chunk);
        if (n <= 0) break;
        copied += (uint64_t)n;
        if (progress_cb) progress_cb(copied, st.size);
    }
    pxe_close(fd);

    if (copied != st.size) {
        free(buf);
        return -1;
    }

    int wr = pxe_write_file(dst_path, buf, (size_t)st.size);
    free(buf);
    return (wr >= 0) ? 0 : -1;
}

int pxe_listdir(const char* path, pxe_dirent_t* out_entries, int max_entries) {
    return (__pxe_r0_api && __pxe_r0_api->fs_listdir)
        ? __pxe_r0_api->fs_listdir(path, out_entries, max_entries) : -1;
}

int pxe_mkdir(const char* path) {
    return (__pxe_r0_api && __pxe_r0_api->fs_mkdir)
        ? __pxe_r0_api->fs_mkdir(path) : -1;
}

int pxe_unlink(const char* path) {
    return (__pxe_r0_api && __pxe_r0_api->fs_unlink)
        ? __pxe_r0_api->fs_unlink(path) : -1;
}

int pxe_rename(const char* old_path, const char* new_path) {
    return (__pxe_r0_api && __pxe_r0_api->fs_rename)
        ? __pxe_r0_api->fs_rename(old_path, new_path) : -1;
}

int pxe_get_drives(pxe_drive_info_t* out_drives, int max_drives) {
    return (__pxe_r0_api && __pxe_r0_api->fs_get_drives)
        ? __pxe_r0_api->fs_get_drives(out_drives, max_drives) : -1;
}

int pxe_eject_drive(char drive_letter) {
    return (__pxe_r0_api && __pxe_r0_api->fs_eject)
        ? __pxe_r0_api->fs_eject(drive_letter) : -1;
}

/* =========================================================================
 * Audio Engine (<pxe/audio.h>)
 * ========================================================================= */

int pxe_audio_play_file(const char* path) {
    return (__pxe_r0_api && __pxe_r0_api->audio_cmd)
        ? __pxe_r0_api->audio_cmd(PXE_AUD_PLAY_FILE, 0, path, (void*)0) : -1;
}

void pxe_audio_pause(void) {
    if (__pxe_r0_api && __pxe_r0_api->audio_cmd) {
        (void)__pxe_r0_api->audio_cmd(PXE_AUD_PAUSE, 0, (const char*)0, (void*)0);
    }
}

void pxe_audio_resume(void) {
    if (__pxe_r0_api && __pxe_r0_api->audio_cmd) {
        (void)__pxe_r0_api->audio_cmd(PXE_AUD_RESUME, 0, (const char*)0, (void*)0);
    }
}

void pxe_audio_stop(void) {
    if (__pxe_r0_api && __pxe_r0_api->audio_cmd) {
        (void)__pxe_r0_api->audio_cmd(PXE_AUD_STOP, 0, (const char*)0, (void*)0);
    }
}

void pxe_audio_seek(uint32_t seconds) {
    if (__pxe_r0_api && __pxe_r0_api->audio_cmd) {
        (void)__pxe_r0_api->audio_cmd(PXE_AUD_SEEK, (int)seconds, (const char*)0, (void*)0);
    }
}

void pxe_audio_set_volume(int volume) {
    if (__pxe_r0_api && __pxe_r0_api->audio_cmd) {
        (void)__pxe_r0_api->audio_cmd(PXE_AUD_SET_VOLUME, volume, (const char*)0, (void*)0);
    }
}

void pxe_audio_toggle_mute(void) {
    if (__pxe_r0_api && __pxe_r0_api->audio_cmd) {
        (void)__pxe_r0_api->audio_cmd(PXE_AUD_TOGGLE_MUTE, 0, (const char*)0, (void*)0);
    }
}

int pxe_audio_get_status(pxe_audio_status_t* out_status) {
    return (__pxe_r0_api && __pxe_r0_api->audio_cmd)
        ? __pxe_r0_api->audio_cmd(PXE_AUD_GET_STATUS, 0, (const char*)0, out_status) : -1;
}

int pxe_audio_feed_pcm(const void* samples, uint32_t sample_count,
                       int src_rate, int src_channels, int src_bits, int src_float) {
    if (!__pxe_r0_api || !__pxe_r0_api->audio_cmd) return -1;
    pxe_pcm_chunk_t chunk;
    chunk.samples      = samples;
    chunk.sample_count = sample_count;
    chunk.src_rate     = src_rate;
    chunk.src_channels = src_channels;
    chunk.src_bits     = src_bits;
    chunk.src_float    = src_float;
    return __pxe_r0_api->audio_cmd(PXE_AUD_FEED_PCM, 0, (const char*)0, &chunk);
}

/* =========================================================================
 * Privileged Ring 0 Introspection & Hardware Wrappers (<pxe/ring0.h>)
 * ========================================================================= */

int pxe_r0_enum_processes(pxe_r0_proc_info_t* out_list, int max_count) {
    return (__pxe_r0_api && __pxe_r0_api->enum_processes)
        ? __pxe_r0_api->enum_processes(out_list, max_count) : -1;
}

int pxe_r0_kill_process(uint32_t pid, int exit_code) {
    return (__pxe_r0_api && __pxe_r0_api->kill_process)
        ? __pxe_r0_api->kill_process(pid, exit_code) : -1;
}

int pxe_r0_focus_process_window(uint32_t pid) {
    return (__pxe_r0_api && __pxe_r0_api->focus_process_window)
        ? __pxe_r0_api->focus_process_window(pid) : -1;
}

void pxe_r0_get_memory_summary(pxe_r0_mem_info_t* out) {
    if (__pxe_r0_api && __pxe_r0_api->get_memory_summary) {
        __pxe_r0_api->get_memory_summary(out);
    }
}

int pxe_r0_enum_pci_devices(pxe_r0_pci_info_t* out, int max_count) {
    return (__pxe_r0_api && __pxe_r0_api->enum_pci_devices)
        ? __pxe_r0_api->enum_pci_devices(out, max_count) : -1;
}

int pxe_r0_enum_usb_devices(pxe_r0_usb_info_t* out, int max_count) {
    return (__pxe_r0_api && __pxe_r0_api->enum_usb_devices)
        ? __pxe_r0_api->enum_usb_devices(out, max_count) : -1;
}

int pxe_r0_enum_pdv_drivers(pxe_r0_pdv_info_t* out, int max_count) {
    return (__pxe_r0_api && __pxe_r0_api->enum_pdv_drivers)
        ? __pxe_r0_api->enum_pdv_drivers(out, max_count) : -1;
}

int pxe_r0_load_pdv_file(const char* pdv_path) {
    return (__pxe_r0_api && __pxe_r0_api->load_pdv_file)
        ? __pxe_r0_api->load_pdv_file(pdv_path) : -1;
}

int pxe_r0_rescan_pdevice_dir(void) {
    return (__pxe_r0_api && __pxe_r0_api->rescan_pdevice_dir)
        ? __pxe_r0_api->rescan_pdevice_dir() : -1;
}

int pxe_r0_enum_disks_and_parts(pxe_r0_part_info_t* out, int max_count) {
    return (__pxe_r0_api && __pxe_r0_api->enum_disks_and_parts)
        ? __pxe_r0_api->enum_disks_and_parts(out, max_count) : -1;
}

int pxe_r0_format_partition(int part_idx, int fs_type, const char* label) {
    return (__pxe_r0_api && __pxe_r0_api->format_partition)
        ? __pxe_r0_api->format_partition(part_idx, fs_type, label) : -1;
}

int pxe_r0_raw_disk_rw(int disk_id, uint64_t lba, uint32_t count, void* buf, bool is_write) {
    return (__pxe_r0_api && __pxe_r0_api->raw_disk_rw)
        ? __pxe_r0_api->raw_disk_rw(disk_id, lba, count, buf, is_write ? 1 : 0) : -1;
}

int pxe_r0_vfs_remount_all(void) {
    return (__pxe_r0_api && __pxe_r0_api->vfs_remount_all)
        ? __pxe_r0_api->vfs_remount_all() : -1;
}

void pxe_r0_power_control(int action) {
    if (__pxe_r0_api && __pxe_r0_api->power_control) {
        __pxe_r0_api->power_control(action);
    }
}

int pxe_r0_sync_filesystems(void) {
    return (__pxe_r0_api && __pxe_r0_api->sync_filesystems)
        ? __pxe_r0_api->sync_filesystems() : -1;
}

int pxe_r0_init_uninit_disk(void) {
    return (__pxe_r0_api && __pxe_r0_api->init_uninit_disk)
        ? __pxe_r0_api->init_uninit_disk() : -1;
}
