#include "log.h"
#include "apic.h"
#include "fat.h"
#include "pxe.h"
#include "settings.h"
#include "string.h"
#include "vfs.h"

#define LOG_DISK_BATCH_NORMAL   4096u
#define LOG_DISK_BATCH_DEGRADED 1024u
#define LOG_DISK_FLUSH_TICKS    100u   /* 1.0s at 100 Hz */
#define LOG_DISK_SYNC_TICKS     200u   /* 2.0s at 100 Hz */
#define LOG_DISK_MAX_FILE_BYTES (16u * 1024u * 1024u)
#define LOG_DISK_MAX_ERRORS     3

static bool g_log_disk_enabled = false;
static bool g_log_flushing = false;
static char g_log_drive = 0;
static blkdev_t* g_log_dev = NULL;
static char g_log_path[64];
static vfs_stream_t g_log_stream;

static char g_date_prefix[10]; /* "YYYYMMDD-" */
static uint32_t g_scan_max_seq = 0;

static uint64_t g_last_flush_tick = 0;
static uint64_t g_last_sync_tick = 0;
static uint32_t g_flush_interval_ticks = LOG_DISK_FLUSH_TICKS;
static uint32_t g_sync_interval_ticks = LOG_DISK_SYNC_TICKS;
static uint32_t g_batch_limit = LOG_DISK_BATCH_NORMAL;
static uint32_t g_unsynced_bytes = 0;
static int g_consec_errors = 0;
static uint64_t g_backoff_until_tick = 0;

static char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static uint32_t append_uint_dec(char* dst, uint32_t pos, uint32_t cap,
                                uint32_t val, int min_digits) {
    char tmp[16];
    int n = 0;
    if (val == 0) tmp[n++] = '0';
    while (val > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (val % 10u));
        val /= 10u;
    }
    while (n < min_digits && n < (int)sizeof(tmp)) {
        tmp[n++] = '0';
    }
    while (n > 0 && pos + 1 < cap) {
        dst[pos++] = tmp[--n];
    }
    dst[pos] = '\0';
    return pos;
}

static void format_date_prefix(char out[10]) {
    pxe_rtc_time_t rtc;
    memset(&rtc, 0, sizeof(rtc));
    uint32_t year = 2026, month = 1, day = 1;
    if (pxe_kern_get_rtc(&rtc) == 0 &&
        rtc.year >= 2000 && rtc.year <= 2099 &&
        rtc.month >= 1 && rtc.month <= 12 &&
        rtc.day >= 1 && rtc.day <= 31) {
        year = rtc.year;
        month = rtc.month;
        day = rtc.day;
    }
    uint32_t pos = 0;
    pos = append_uint_dec(out, pos, 10, year, 4);
    pos = append_uint_dec(out, pos, 10, month, 2);
    pos = append_uint_dec(out, pos, 10, day, 2);
    if (pos + 1 < 10) {
        out[pos++] = '-';
        out[pos] = '\0';
    }
}

static void build_log_file_path(char* out, size_t cap, char drive,
                                const char prefix[10], uint32_t seq) {
    if (!out || cap < 24) return;
    uint32_t pos = 0;
    out[pos++] = drive;
    out[pos++] = ':';
    out[pos++] = '\\';
    out[pos++] = 'l';
    out[pos++] = 'o';
    out[pos++] = 'g';
    out[pos++] = '\\';
    for (int i = 0; prefix[i] && pos + 1 < cap; i++) {
        out[pos++] = prefix[i];
    }
    pos = append_uint_dec(out, pos, (uint32_t)cap, seq, 1);
    const char* ext = ".log";
    for (int i = 0; ext[i] && pos + 1 < cap; i++) {
        out[pos++] = ext[i];
    }
    out[pos] = '\0';
}

static void log_dir_scan_cb(const vfs_dirent_t* entry) {
    if (!entry || entry->is_dir) return;
    const char* name = entry->name;
    int i = 0;
    for (; i < 9; i++) {
        if (ascii_lower(name[i]) != ascii_lower(g_date_prefix[i])) return;
    }
    if (name[i] < '0' || name[i] > '9') return;
    uint32_t seq = 0;
    while (name[i] >= '0' && name[i] <= '9') {
        if (seq > 1000000u) return;
        seq = seq * 10u + (uint32_t)(name[i] - '0');
        i++;
    }
    if (ascii_lower(name[i]) != '.' ||
        ascii_lower(name[i + 1]) != 'l' ||
        ascii_lower(name[i + 2]) != 'o' ||
        ascii_lower(name[i + 3]) != 'g' ||
        name[i + 4] != '\0') {
        return;
    }
    if (seq > g_scan_max_seq) g_scan_max_seq = seq;
}

static bool log_drive_healthy(void) {
    if (!g_log_disk_enabled || !g_log_drive || !g_log_stream.is_open) {
        return false;
    }
    vfs_drive_t* drv = vfs_get_drive(g_log_drive);
    if (!drv || !drv->present || !drv->mounted || drv->is_readonly ||
        !drv->dev || drv->dev != g_log_dev) {
        return false;
    }
    return true;
}

static void log_disk_abort_local(const char* reason) {
    g_log_disk_enabled = false;
    g_log_stream.is_open = 0;
    if (reason) {
        log_printf("[LOG  ] Disk log disabled: %s\n", reason);
    }
}

static int log_write_one_batch(uint32_t max_bytes, bool* out_wrote_any) {
    static uint8_t scratch[LOG_DISK_BATCH_NORMAL];
    if (out_wrote_any) *out_wrote_any = false;
    if (max_bytes > sizeof(scratch)) max_bytes = sizeof(scratch);

    uint32_t head = 0;
    uint32_t count = log_ring_peek(scratch, max_bytes, &head, NULL);
    if (count == 0) return 0;

    if (g_log_stream.current_offset >= LOG_DISK_MAX_FILE_BYTES) {
        log_disk_abort_local("session size limit reached");
        return -1;
    }
    uint64_t room = LOG_DISK_MAX_FILE_BYTES - g_log_stream.current_offset;
    if ((uint64_t)count > room) count = (uint32_t)room;

    int written = vfs_stream_write(&g_log_stream, scratch, count);
    if (written <= 0) {
        return -1;
    }
    log_ring_consume(head, (uint32_t)written);
    g_unsynced_bytes += (uint32_t)written;
    if (out_wrote_any) *out_wrote_any = true;
    return written;
}

void log_disk_init(void) {
    g_log_disk_enabled = false;
    memset(&g_log_stream, 0, sizeof(g_log_stream));
    g_log_drive = 0;
    g_log_dev = NULL;
    g_log_path[0] = '\0';
    g_unsynced_bytes = 0;
    g_consec_errors = 0;
    g_backoff_until_tick = 0;
    g_flush_interval_ticks = LOG_DISK_FLUSH_TICKS;
    g_sync_interval_ticks = LOG_DISK_SYNC_TICKS;
    g_batch_limit = LOG_DISK_BATCH_NORMAL;

    if (settings_get_boot_mode() != SETTINGS_BOOT_LOCAL ||
        !settings_storage_available()) {
        return;
    }

    char drive = settings_get_system_drive();
    if (drive < 'C' || drive > 'Z') return;
    vfs_drive_t* drv = vfs_get_drive(drive);
    if (!drv || !drv->present || !drv->mounted || drv->is_readonly || !drv->dev) {
        return;
    }

    char dir_path[16];
    dir_path[0] = drive;
    dir_path[1] = ':';
    dir_path[2] = '\\';
    dir_path[3] = 'l';
    dir_path[4] = 'o';
    dir_path[5] = 'g';
    dir_path[6] = '\0';

    vfs_dirent_t ent;
    int find_rc = vfs_find_file(dir_path, &ent);
    if (find_rc == VFS_OK) {
        if (!ent.is_dir) {
            log_printf("[LOG  ] %s exists as a file; disk logging skipped\n",
                       dir_path);
            return;
        }
    } else if (vfs_mkdir(dir_path) != VFS_OK) {
        log_printf("[LOG  ] Failed to create %s; disk logging skipped\n",
                   dir_path);
        return;
    }

    format_date_prefix(g_date_prefix);
    g_scan_max_seq = 0;
    (void)vfs_list_dir(dir_path, log_dir_scan_cb);

    uint32_t seq = g_scan_max_seq + 1u;
    if (seq == 0) seq = 1;
    bool found_free = false;
    for (int tries = 0; tries < 64; tries++) {
        build_log_file_path(g_log_path, sizeof(g_log_path),
                            drive, g_date_prefix, seq);
        if (vfs_find_file(g_log_path, &ent) != VFS_OK) {
            found_free = true;
            break;
        }
        seq++;
    }
    if (!found_free) {
        log_printf("[LOG  ] Exhausted sequence numbers in %s\n", dir_path);
        return;
    }

    if (vfs_stream_create(g_log_path, &g_log_stream) != 0) {
        log_printf("[LOG  ] Failed to create %s\n", g_log_path);
        return;
    }

    g_log_drive = drive;
    g_log_dev = drv->dev;
    g_log_disk_enabled = true;
    g_last_flush_tick = apic_get_ticks();
    g_last_sync_tick = g_last_flush_tick;

    log_printf("[LOG  ] Local persistent log enabled: %s\n", g_log_path);
    log_disk_flush();
}

void log_disk_poll(void) {
    if (!g_log_disk_enabled || g_log_flushing) return;
    if (!log_drive_healthy()) {
        log_disk_abort_local("system drive unavailable or removed");
        return;
    }

    uint64_t now = apic_get_ticks();
    if (now < g_backoff_until_tick) return;

    uint32_t pending = log_ring_pending();
    if (pending == 0) {
        if (g_unsynced_bytes > 0 &&
            (now - g_last_sync_tick) >= g_sync_interval_ticks) {
            g_log_flushing = true;
            uint64_t t0 = apic_get_ticks();
            if (vfs_stream_sync(&g_log_stream) == 0) {
                g_unsynced_bytes = 0;
                g_last_sync_tick = apic_get_ticks();
                g_consec_errors = 0;
            } else {
                g_consec_errors++;
                if (g_consec_errors >= LOG_DISK_MAX_ERRORS ||
                    !log_drive_healthy()) {
                    g_log_flushing = false;
                    log_disk_abort_local("metadata sync failed");
                    return;
                }
                g_backoff_until_tick = now + 500u;
            }
            if ((apic_get_ticks() - t0) >= 5u) {
                g_flush_interval_ticks = 500u;
                g_sync_interval_ticks = 1500u;
                g_batch_limit = LOG_DISK_BATCH_DEGRADED;
            }
            g_log_flushing = false;
        }
        return;
    }

    bool enough_bytes = (pending >= (g_batch_limit / 2u));
    bool timer_due = (now - g_last_flush_tick) >= g_flush_interval_ticks;
    if (!enough_bytes && !timer_due) return;

    g_log_flushing = true;
    uint64_t t0 = apic_get_ticks();
    bool wrote_any = false;
    int wr = log_write_one_batch(g_batch_limit, &wrote_any);
    uint64_t dt = apic_get_ticks() - t0;

    if (wr < 0) {
        g_consec_errors++;
        if (g_consec_errors >= LOG_DISK_MAX_ERRORS || !log_drive_healthy()) {
            g_log_flushing = false;
            log_disk_abort_local("write error (disk dropped or I/O fault)");
            return;
        }
        g_backoff_until_tick = apic_get_ticks() + (uint64_t)g_consec_errors * 500u;
        g_log_flushing = false;
        return;
    }

    g_consec_errors = 0;
    g_last_flush_tick = apic_get_ticks();

    if (dt >= 5u) {
        /* Degraded/slow disk detected: throttle flushing to preserve UI & audio */
        g_flush_interval_ticks = 500u;
        g_sync_interval_ticks = 1500u;
        g_batch_limit = LOG_DISK_BATCH_DEGRADED;
    } else if (dt <= 1u && g_batch_limit < LOG_DISK_BATCH_NORMAL) {
        g_flush_interval_ticks = LOG_DISK_FLUSH_TICKS;
        g_sync_interval_ticks = LOG_DISK_SYNC_TICKS;
        g_batch_limit = LOG_DISK_BATCH_NORMAL;
    }

    if (g_unsynced_bytes >= 8192u ||
        (g_unsynced_bytes > 0 &&
         (g_last_flush_tick - g_last_sync_tick) >= g_sync_interval_ticks)) {
        if (vfs_stream_sync(&g_log_stream) == 0) {
            g_unsynced_bytes = 0;
            g_last_sync_tick = apic_get_ticks();
        } else {
            g_consec_errors++;
            if (g_consec_errors >= LOG_DISK_MAX_ERRORS || !log_drive_healthy()) {
                g_log_flushing = false;
                log_disk_abort_local("metadata sync failed");
                return;
            }
            g_backoff_until_tick = apic_get_ticks() + 500u;
        }
    }
    g_log_flushing = false;
}

void log_disk_flush(void) {
    if (!g_log_disk_enabled || g_log_flushing) return;
    if (!log_drive_healthy()) {
        log_disk_abort_local("system drive unavailable");
        return;
    }

    g_log_flushing = true;
    for (int iter = 0; iter < 40 && log_ring_pending() > 0; iter++) {
        bool wrote_any = false;
        if (log_write_one_batch(LOG_DISK_BATCH_NORMAL, &wrote_any) <= 0 ||
            !wrote_any) {
            break;
        }
    }
    if (vfs_stream_sync(&g_log_stream) == 0) {
        g_unsynced_bytes = 0;
        g_last_sync_tick = apic_get_ticks();
    }
    g_last_flush_tick = apic_get_ticks();
    g_log_flushing = false;
}

void log_disk_shutdown(void) {
    if (!g_log_disk_enabled) return;
    if (log_drive_healthy()) {
        log_disk_flush();
        if (g_log_stream.is_open) {
            (void)vfs_stream_sync(&g_log_stream);
            vfs_stream_close(&g_log_stream);
        }
        fat_flush_all();
    } else {
        g_log_stream.is_open = 0;
    }
    g_log_disk_enabled = false;
}
