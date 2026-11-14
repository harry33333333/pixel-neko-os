#include "../../include/neko_abi.h"
#include <pxe/sys.h>
#include <pxe/mem.h>
#include <pxe/str.h>
#include <pxe/gui.h>
#include <pxe/fs.h>
#include <pxe/audio.h>

static inline long _pxe_syscall0(long num) {
    long ret = num;
    __asm__ volatile (
        "syscall"
        : "+a"(ret)
        :
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long _pxe_syscall1(long num, long a0) {
    long ret = num;
    __asm__ volatile (
        "syscall"
        : "+a"(ret)
        : "D"(a0)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long _pxe_syscall2(long num, long a0, long a1) {
    long ret = num;
    __asm__ volatile (
        "syscall"
        : "+a"(ret)
        : "D"(a0), "S"(a1)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long _pxe_syscall3(long num, long a0, long a1, long a2) {
    long ret = num;
    __asm__ volatile (
        "syscall"
        : "+a"(ret)
        : "D"(a0), "S"(a1), "d"(a2)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long _pxe_syscall4(long num, long a0, long a1, long a2, long a3) {
    register long r10 __asm__("r10") = a3;
    long ret = num;
    __asm__ volatile (
        "syscall"
        : "+a"(ret)
        : "D"(a0), "S"(a1), "d"(a2), "r"(r10)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long _pxe_syscall5(long num, long a0, long a1, long a2, long a3, long a4) {
    register long r10 __asm__("r10") = a3;
    register long r8 __asm__("r8") = a4;
    long ret = num;
    __asm__ volatile (
        "syscall"
        : "+a"(ret)
        : "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory"
    );
    return ret;
}

/* =========================================================================
 * System Runtime (<pxe/sys.h>)
 * ========================================================================= */

void pxe_exit(int status) {
    (void)_pxe_syscall1(NEKO_SYSCALL_EXIT, (long)status);
    for (;;) {
        __asm__ volatile ("pause");
    }
}

int pxe_getpid(void) {
    return (int)_pxe_syscall0(NEKO_SYSCALL_GETPID);
}

void pxe_yield(void) {
    (void)_pxe_syscall0(NEKO_SYSCALL_YIELD);
}

void pxe_sleep_ms(uint32_t ms) {
    uint64_t ticks = (ms + 9u) / 10u;
    if (ms > 0 && ticks == 0) ticks = 1;
    (void)_pxe_syscall1(NEKO_SYSCALL_SLEEP, (long)ticks);
}

uint64_t pxe_get_ticks(void) {
    return (uint64_t)_pxe_syscall0(NEKO_SYSCALL_GETTICKS);
}

int pxe_spawn(const char* path, int argc, const char* const* argv) {
    return (int)_pxe_syscall3(NEKO_SYSCALL_SPAWN, (long)path, (long)argc, (long)argv);
}

int pxe_waitpid(int pid, int* out_status) {
    return (int)_pxe_syscall2(NEKO_SYSCALL_WAITPID, (long)pid, (long)out_status);
}

void pxe_debug_log(const char* msg) {
    if (!msg) return;
    (void)_pxe_syscall3(NEKO_SYSCALL_WRITE, 1, (long)msg, (long)strlen(msg));
}

int pxe_get_rtc_time(pxe_rtc_time_t* out) {
    return (int)_pxe_syscall1(NEKO_SYSCALL_GET_RTC, (long)out);
}

int pxe_sys_get_basic_info(pxe_basic_sysinfo_t* out) {
    return (int)_pxe_syscall1(NEKO_SYSCALL_GET_SYSINFO, (long)out);
}

int pxe_display_get_caps(pxe_display_caps_t* out) {
    return (int)_pxe_syscall1(NEKO_SYSCALL_GET_DISP_CAPS, (long)out);
}

int pxe_display_set_mode(int width, int height, int bpp) {
    int wh = (width & 0xFFFF) | ((height & 0xFFFF) << 16);
    return (int)_pxe_syscall4(NEKO_SYSCALL_SYS_CONFIG, PXE_CFG_SET_RESOLUTION,
                              (long)wh, (long)bpp, 0);
}

int pxe_display_apply_mode(int width, int height, int bpp) {
    int wh = (width & 0xFFFF) | ((height & 0xFFFF) << 16);
    return (int)_pxe_syscall4(NEKO_SYSCALL_SYS_CONFIG, PXE_CFG_APPLY_RESOLUTION,
                              (long)wh, (long)bpp, 0);
}

int pxe_desktop_set_pattern(int pattern) {
    return (int)_pxe_syscall4(NEKO_SYSCALL_SYS_CONFIG, PXE_CFG_SET_PATTERN,
                              (long)pattern, 0, 0);
}

int pxe_desktop_set_wallpaper(const char* path, int mode) {
    return (int)_pxe_syscall4(NEKO_SYSCALL_SYS_CONFIG, PXE_CFG_SET_WALLPAPER,
                              (long)mode, 0, (long)path);
}

int pxe_desktop_set_wallpaper_mode(int mode) {
    return (int)_pxe_syscall4(NEKO_SYSCALL_SYS_CONFIG, PXE_CFG_SET_WALLPAPER_MODE,
                              (long)mode, 0, 0);
}

int pxe_desktop_clear_wallpaper(void) {
    return (int)_pxe_syscall4(NEKO_SYSCALL_SYS_CONFIG, PXE_CFG_CLEAR_WALLPAPER,
                              0, 0, 0);
}

int pxe_mouse_set_speed(int speed) {
    return (int)_pxe_syscall4(NEKO_SYSCALL_SYS_CONFIG, PXE_CFG_SET_MOUSE_SPEED,
                              (long)speed, 0, 0);
}

int pxe_settings_save(void) {
    return (int)_pxe_syscall4(NEKO_SYSCALL_SYS_CONFIG, PXE_CFG_SAVE_SETTINGS,
                              0, 0, 0);
}

/* =========================================================================
 * Userland Heap Allocator (<pxe/mem.h>) backed by NEKO_SYSCALL_SBRK
 * ========================================================================= */

#define PXE_HEAP_MAGIC 0x50584548u /* 'PXEH' */

typedef struct pxe_mblk {
    size_t size;
    uint32_t magic;
    uint32_t free;
    struct pxe_mblk* prev;
    struct pxe_mblk* next;
} __attribute__((aligned(16))) pxe_mblk_t;

static pxe_mblk_t* g_heap_head = (pxe_mblk_t*)0;
static pxe_mblk_t* g_heap_tail = (pxe_mblk_t*)0;

void* pxe_sbrk(intptr_t delta) {
    long ret = _pxe_syscall1(NEKO_SYSCALL_SBRK, (long)delta);
    if (ret < 0) return (void*)(intptr_t)-1;
    return (void*)(uintptr_t)ret;
}

static void heap_split(pxe_mblk_t* blk, size_t need) {
    if (blk->size >= need + sizeof(pxe_mblk_t) + 32u) {
        pxe_mblk_t* rem = (pxe_mblk_t*)((uint8_t*)(blk + 1) + need);
        rem->size  = blk->size - need - sizeof(pxe_mblk_t);
        rem->magic = PXE_HEAP_MAGIC;
        rem->free  = 1u;
        rem->prev  = blk;
        rem->next  = blk->next;
        if (blk->next) {
            blk->next->prev = rem;
        } else {
            g_heap_tail = rem;
        }
        blk->next = rem;
        blk->size = need;
    }
}

void* malloc(size_t size) {
    if (size == 0) size = 16;
    size = (size + 15u) & ~((size_t)15u);

    for (pxe_mblk_t* cur = g_heap_head; cur; cur = cur->next) {
        if (cur->free && cur->size >= size) {
            cur->free = 0u;
            heap_split(cur, size);
            return (void*)(cur + 1);
        }
    }

    size_t req = sizeof(pxe_mblk_t) + size;
    size_t chunk = (req + 65535u) & ~((size_t)65535u);
    void* mem = pxe_sbrk((intptr_t)chunk);
    if (mem == (void*)(intptr_t)-1) return (void*)0;

    pxe_mblk_t* blk = (pxe_mblk_t*)mem;
    blk->size  = chunk - sizeof(pxe_mblk_t);
    blk->magic = PXE_HEAP_MAGIC;
    blk->free  = 0u;
    blk->prev  = g_heap_tail;
    blk->next  = (pxe_mblk_t*)0;
    if (g_heap_tail) {
        g_heap_tail->next = blk;
    } else {
        g_heap_head = blk;
    }
    g_heap_tail = blk;

    heap_split(blk, size);
    return (void*)(blk + 1);
}

void free(void* ptr) {
    if (!ptr) return;
    pxe_mblk_t* blk = ((pxe_mblk_t*)ptr) - 1;
    if (blk->magic != PXE_HEAP_MAGIC || blk->free) return;
    blk->free = 1u;

    if (blk->next && blk->next->magic == PXE_HEAP_MAGIC && blk->next->free &&
        (uint8_t*)(blk + 1) + blk->size == (uint8_t*)blk->next) {
        pxe_mblk_t* nxt = blk->next;
        blk->size += sizeof(pxe_mblk_t) + nxt->size;
        blk->next = nxt->next;
        if (nxt->next) nxt->next->prev = blk;
        else g_heap_tail = blk;
    }

    if (blk->prev && blk->prev->magic == PXE_HEAP_MAGIC && blk->prev->free &&
        (uint8_t*)(blk->prev + 1) + blk->prev->size == (uint8_t*)blk) {
        pxe_mblk_t* prv = blk->prev;
        prv->size += sizeof(pxe_mblk_t) + blk->size;
        prv->next = blk->next;
        if (blk->next) blk->next->prev = prv;
        else g_heap_tail = prv;
    }
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
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return (void*)0;
    }
    pxe_mblk_t* blk = ((pxe_mblk_t*)ptr) - 1;
    if (blk->magic != PXE_HEAP_MAGIC) return (void*)0;
    size_t aligned = (size + 15u) & ~((size_t)15u);
    if (blk->size >= aligned) {
        heap_split(blk, aligned);
        return ptr;
    }
    void* next_ptr = malloc(aligned);
    if (!next_ptr) return (void*)0;
    memcpy(next_ptr, ptr, blk->size);
    free(ptr);
    return next_ptr;
}

/* =========================================================================
 * GUI & Windowing (<pxe/gui.h>)
 * ========================================================================= */

int pxe_win_create(const pxe_win_desc_t* desc, pxe_win_surface_t* out_surface) {
    return (int)_pxe_syscall2(NEKO_SYSCALL_WIN_CREATE, (long)desc, (long)out_surface);
}

int pxe_win_destroy(int win_id) {
    return (int)_pxe_syscall1(NEKO_SYSCALL_WIN_DESTROY, (long)win_id);
}

int pxe_win_set_title(int win_id, const char* title) {
    return (int)_pxe_syscall2(NEKO_SYSCALL_WIN_SET_TITLE, (long)win_id, (long)title);
}

int pxe_win_set_menu(int win_id, const pxe_menu_bar_t* menu_bar) {
    return (int)_pxe_syscall2(NEKO_SYSCALL_WIN_SET_MENU, (long)win_id, (long)menu_bar);
}

int pxe_win_get_surface(int win_id, pxe_win_surface_t* out_surface) {
    return (int)_pxe_syscall2(NEKO_SYSCALL_WIN_GET_SURF, (long)win_id, (long)out_surface);
}

int pxe_win_present(int win_id) {
    return (int)_pxe_syscall5(NEKO_SYSCALL_WIN_PRESENT, (long)win_id, 0, 0, -1, -1);
}

int pxe_win_present_rect(int win_id, int x, int y, int w, int h) {
    return (int)_pxe_syscall5(NEKO_SYSCALL_WIN_PRESENT, (long)win_id, (long)x, (long)y, (long)w, (long)h);
}

int pxe_win_poll_event(int win_id, pxe_event_t* out_ev) {
    return (int)_pxe_syscall3(NEKO_SYSCALL_WIN_POLL_EV, (long)win_id, (long)out_ev, 0);
}

int pxe_win_wait_event(int win_id, pxe_event_t* out_ev) {
    int rc;
    do {
        rc = (int)_pxe_syscall3(NEKO_SYSCALL_WIN_POLL_EV, (long)win_id, (long)out_ev, 1);
    } while (rc == 0);
    return rc;
}

int pxe_clipboard_get(char* buf, size_t max_len) {
    return (int)_pxe_syscall3(NEKO_SYSCALL_CLIPBOARD, 0, (long)buf, (long)max_len);
}

int pxe_clipboard_set(const char* text) {
    return (int)_pxe_syscall3(NEKO_SYSCALL_CLIPBOARD, 1, (long)text, 0);
}

void pxe_notify_banner(const char* message) {
    (void)_pxe_syscall1(NEKO_SYSCALL_NOTIFY_BANNER, (long)message);
}

void pxe_beep(void) {
    (void)_pxe_syscall0(NEKO_SYSCALL_BEEP);
}

/* =========================================================================
 * Filesystem (<pxe/fs.h>)
 * ========================================================================= */

int pxe_open(const char* path) {
    return (int)_pxe_syscall1(NEKO_SYSCALL_FS_OPEN, (long)path);
}

int pxe_create(const char* path) {
    return (int)_pxe_syscall1(NEKO_SYSCALL_FS_CREATE, (long)path);
}

int pxe_read(int fd, void* buf, size_t count) {
    return (int)_pxe_syscall3(NEKO_SYSCALL_FS_READ, (long)fd, (long)buf, (long)count);
}

int pxe_write(int fd, const void* buf, size_t count) {
    return (int)_pxe_syscall3(NEKO_SYSCALL_FS_WRITE, (long)fd, (long)buf, (long)count);
}

int pxe_seek(int fd, uint64_t offset) {
    return (int)_pxe_syscall2(NEKO_SYSCALL_FS_SEEK, (long)fd, (long)offset);
}

int pxe_close(int fd) {
    return (int)_pxe_syscall1(NEKO_SYSCALL_FS_CLOSE, (long)fd);
}

int pxe_stat(const char* path, pxe_dirent_t* out) {
    return (int)_pxe_syscall2(NEKO_SYSCALL_FS_STAT, (long)path, (long)out);
}

int pxe_read_file(const char* path, void* buf, size_t max_size) {
    return (int)_pxe_syscall3(NEKO_SYSCALL_FS_READ_FILE, (long)path, (long)buf, (long)max_size);
}

int pxe_write_file(const char* path, const void* buf, size_t size) {
    return (int)_pxe_syscall3(NEKO_SYSCALL_FS_WRITE_FILE, (long)path, (long)buf, (long)size);
}

int pxe_copy_file(const char* src_path, const char* dst_path,
                  void (*progress_cb)(uint64_t copied, uint64_t total)) {
    if (!src_path || !dst_path) return -1;
    pxe_dirent_t st;
    if (pxe_stat(src_path, &st) != 0 || st.is_dir) return -1;

    int src_fd = pxe_open(src_path);
    if (src_fd < 0) return -1;

    int dst_fd = pxe_create(dst_path);
    if (dst_fd < 0) {
        pxe_close(src_fd);
        return -1;
    }

    if (st.size == 0) {
        pxe_close(dst_fd);
        pxe_close(src_fd);
        if (progress_cb) progress_cb(0, 0);
        return 0;
    }

    const size_t chunk_sz = 65536u;
    uint8_t* buf = (uint8_t*)malloc(chunk_sz);
    if (!buf) {
        pxe_close(dst_fd);
        pxe_close(src_fd);
        pxe_unlink(dst_path);
        return -1;
    }

    uint64_t copied = 0;
    int ok = 1;
    while (copied < st.size) {
        uint64_t rem = st.size - copied;
        size_t want = (rem > (uint64_t)chunk_sz) ? chunk_sz : (size_t)rem;
        int n = pxe_read(src_fd, buf, want);
        if (n <= 0) {
            ok = 0;
            break;
        }
        int off = 0;
        while (off < n) {
            int w = pxe_write(dst_fd, buf + off, (size_t)(n - off));
            if (w <= 0) {
                ok = 0;
                break;
            }
            off += w;
        }
        if (!ok) break;
        copied += (uint64_t)n;
        if (progress_cb) progress_cb(copied, st.size);
    }

    free(buf);
    pxe_close(dst_fd);
    pxe_close(src_fd);

    if (!ok || copied != st.size) {
        pxe_unlink(dst_path);
        return -1;
    }
    return 0;
}

int pxe_listdir(const char* path, pxe_dirent_t* out_entries, int max_entries) {
    return (int)_pxe_syscall3(NEKO_SYSCALL_FS_LISTDIR, (long)path, (long)out_entries, (long)max_entries);
}

int pxe_mkdir(const char* path) {
    return (int)_pxe_syscall1(NEKO_SYSCALL_FS_MKDIR, (long)path);
}

int pxe_unlink(const char* path) {
    return (int)_pxe_syscall1(NEKO_SYSCALL_FS_UNLINK, (long)path);
}

int pxe_rename(const char* old_path, const char* new_path) {
    return (int)_pxe_syscall2(NEKO_SYSCALL_FS_RENAME, (long)old_path, (long)new_path);
}

int pxe_get_drives(pxe_drive_info_t* out_drives, int max_drives) {
    return (int)_pxe_syscall2(NEKO_SYSCALL_FS_GET_DRIVES, (long)out_drives, (long)max_drives);
}

int pxe_eject_drive(char drive_letter) {
    return (int)_pxe_syscall1(NEKO_SYSCALL_FS_EJECT, (long)(unsigned char)drive_letter);
}

/* =========================================================================
 * Audio Engine (<pxe/audio.h>)
 * ========================================================================= */

int pxe_audio_play_file(const char* path) {
    return (int)_pxe_syscall4(NEKO_SYSCALL_AUDIO_CMD, PXE_AUD_PLAY_FILE, 0, (long)path, 0);
}

void pxe_audio_pause(void) {
    (void)_pxe_syscall4(NEKO_SYSCALL_AUDIO_CMD, PXE_AUD_PAUSE, 0, 0, 0);
}

void pxe_audio_resume(void) {
    (void)_pxe_syscall4(NEKO_SYSCALL_AUDIO_CMD, PXE_AUD_RESUME, 0, 0, 0);
}

void pxe_audio_stop(void) {
    (void)_pxe_syscall4(NEKO_SYSCALL_AUDIO_CMD, PXE_AUD_STOP, 0, 0, 0);
}

void pxe_audio_seek(uint32_t seconds) {
    (void)_pxe_syscall4(NEKO_SYSCALL_AUDIO_CMD, PXE_AUD_SEEK, (long)seconds, 0, 0);
}

void pxe_audio_set_volume(int volume) {
    (void)_pxe_syscall4(NEKO_SYSCALL_AUDIO_CMD, PXE_AUD_SET_VOLUME, (long)volume, 0, 0);
}

void pxe_audio_toggle_mute(void) {
    (void)_pxe_syscall4(NEKO_SYSCALL_AUDIO_CMD, PXE_AUD_TOGGLE_MUTE, 0, 0, 0);
}

int pxe_audio_get_status(pxe_audio_status_t* out_status) {
    return (int)_pxe_syscall4(NEKO_SYSCALL_AUDIO_CMD, PXE_AUD_GET_STATUS, 0, 0, (long)out_status);
}

int pxe_audio_feed_pcm(const void* samples, uint32_t sample_count,
                       int src_rate, int src_channels, int src_bits, int src_float) {
    pxe_pcm_chunk_t chunk;
    chunk.samples      = samples;
    chunk.sample_count = sample_count;
    chunk.src_rate     = src_rate;
    chunk.src_channels = src_channels;
    chunk.src_bits     = src_bits;
    chunk.src_float    = src_float;
    return (int)_pxe_syscall4(NEKO_SYSCALL_AUDIO_CMD, PXE_AUD_FEED_PCM, 0, 0, (long)&chunk);
}
