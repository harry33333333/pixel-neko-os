#include "desktop.h"
#include "heap.h"
#include "log.h"
#include "pxe.h"
#include "settings.h"
#include "string.h"
#include "vfs.h"
#include "wm.h"
#include <stdarg.h>

static char lower_ascii(char value);

static int desktop_format(char* output, size_t capacity,
                          const char* format, ...) {
    if (!output || !capacity || !format) return 0;
    size_t used = 0;
    va_list arguments;
    va_start(arguments, format);
    while (*format && used + 1 < capacity) {
        if (*format != '%') {
            output[used++] = *format++;
            continue;
        }
        format++;
        if (*format == '%') {
            output[used++] = '%';
            format++;
            continue;
        }
        char temporary[24];
        size_t length = 0;
        if (*format == 's') {
            const char* value = va_arg(arguments, const char*);
            if (!value) value = "";
            while (*value && used + 1 < capacity) {
                output[used++] = *value++;
            }
            format++;
            continue;
        }
        bool negative = false;
        unsigned int value = 0;
        if (*format == 'd') {
            int number = va_arg(arguments, int);
            negative = number < 0;
            value = negative ? (unsigned int)(-number) : (unsigned int)number;
            format++;
        } else if (*format == 'u') {
            value = va_arg(arguments, unsigned int);
            format++;
        } else if (*format == 'c') {
            value = (unsigned int)va_arg(arguments, int);
            format++;
        } else {
            output[used++] = '%';
            continue;
        }
        do {
            temporary[length++] = (char)('0' + value % 10u);
            value /= 10u;
        } while (value && length < sizeof(temporary));
        if (negative && used + 1 < capacity) output[used++] = '-';
        while (length && used + 1 < capacity) {
            output[used++] = temporary[--length];
        }
    }
    va_end(arguments);
    output[used] = '\0';
    return (int)used;
}

static char* desktop_strrchr(char* text, char value) {
    char* result = NULL;
    if (!text) return NULL;
    while (*text) {
        if (*text == value) result = text;
        text++;
    }
    return value == '\0' ? text : result;
}

static int desktop_strcasecmp(const char* left, const char* right) {
    while (*left && *right) {
        char a = lower_ascii(*left++);
        char b = lower_ascii(*right++);
        if (a != b) return (unsigned char)a - (unsigned char)b;
    }
    return (unsigned char)*left - (unsigned char)*right;
}

#define snprintf desktop_format
#define strrchr desktop_strrchr
#define strcasecmp desktop_strcasecmp

#define DESKTOP_LAYOUT_NAME ".desktop-layout"
#define DESKTOP_LAYOUT_MAX 16384
#define DESKTOP_LAYOUT_HEADER "PNK_LAYOUT2"
#define DESKTOP_RIGHT_MARGIN 72
#define DESKTOP_ROW_STEP 72
#define DESKTOP_COLUMN_STEP 96

typedef struct {
    char name[128];
    int x;
    int y;
    int right_offset;
    bool anchored_right;
    bool used;
} desktop_layout_entry_t;

typedef struct {
    const char* name;
    const char* title;
    const char* target_suffix;
    desktop_target_kind_t target_kind;
    bool locked;
} desktop_default_t;

static const desktop_default_t g_defaults[] = {
    { "File Management.pnk", "File Management", "\\APPS\\files.pxe",
      DESKTOP_TARGET_APP, true },
    { "About This Machine.pnk", "About This Machine", "\\APPS\\about.pxe",
      DESKTOP_TARGET_APP, true },
    { "Clock.pnk", "Clock", "\\APPS\\clock.pxe",
      DESKTOP_TARGET_APP, false },
    { "Calculator.pnk", "Calculator", "\\APPS\\calc.pxe",
      DESKTOP_TARGET_APP, false },
    { "Control Panel.pnk", "Control Panel", "\\APPS\\cpanel.pxe",
      DESKTOP_TARGET_APP, false },
    { "System Information.pnk", "System Information", "\\APPS\\sysinfo.pxe",
      DESKTOP_TARGET_APP, false },
    { "Pixel CPU Detect.pnk", "Pixel CPU Detect", "\\APPS\\cpuz.pxe",
      DESKTOP_TARGET_APP, false },
    { "Notepad.pnk", "Notepad", "\\APPS\\notepad.pxe",
      DESKTOP_TARGET_APP, false },
    { "Media Player.pnk", "Media Player", "\\APPS\\player.pxe",
      DESKTOP_TARGET_APP, false },
    { "Image Viewer.pnk", "Image Viewer", "\\APPS\\viewer.pxe",
      DESKTOP_TARGET_APP, false },
    { "GAME.pnk", "GAME", "\\GAME", DESKTOP_TARGET_FOLDER, false }
};

static desktop_item_t g_items[DESKTOP_MAX_ITEMS];
static int g_item_count;
static desktop_layout_entry_t g_layout[DESKTOP_MAX_ITEMS];
static int g_layout_count;
static char g_desktop_path[16];
static char g_desktop_dir[40];
static char g_game_dir[40];
static char g_layout_path[64];
static bool g_mutable;
static bool g_initialized;
static volatile bool g_refresh_requested;

static char lower_ascii(char value) {
    return value >= 'A' && value <= 'Z' ? (char)(value + 32) : value;
}

static bool text_equal_ci(const char* left, const char* right) {
    if (!left || !right) return left == right;
    while (*left && *right) {
        if (lower_ascii(*left) != lower_ascii(*right)) return false;
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static bool text_starts_with_ci(const char* text, const char* prefix) {
    if (!text || !prefix) return false;
    while (*prefix) {
        if (!*text || lower_ascii(*text) != lower_ascii(*prefix)) return false;
        text++;
        prefix++;
    }
    return true;
}

void desktop_notify_path_changed(const char* path) {
    if (!g_initialized || !g_mutable || !path || !path[0]) return;
    if (path[1] == ':' && g_desktop_path[0] &&
        lower_ascii(path[0]) != lower_ascii(g_desktop_path[0])) {
        return;
    }

    const char* cursor = path;
    while (*cursor) {
        while (*cursor == '\\' || *cursor == '/') cursor++;
        const char* component = cursor;
        while (*cursor && *cursor != '\\' && *cursor != '/') cursor++;
        size_t length = (size_t)(cursor - component);
        if (length == 7) {
            bool match = true;
            static const char k_desktop[] = "desktop";
            for (size_t index = 0; index < length; index++) {
                if (lower_ascii(component[index]) != k_desktop[index]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                g_refresh_requested = true;
                return;
            }
        }
    }
}

static bool ends_with_ci(const char* text, const char* suffix) {
    if (!text || !suffix) return false;
    size_t text_length = strlen(text);
    size_t suffix_length = strlen(suffix);
    if (suffix_length > text_length) return false;
    return text_equal_ci(text + text_length - suffix_length, suffix);
}

static void copy_text(char* target, size_t capacity, const char* source) {
    if (!target || !capacity) return;
    size_t index = 0;
    while (source && source[index] && index + 1 < capacity) {
        target[index] = source[index];
        index++;
    }
    target[index] = '\0';
}

static void join_path(char* target, size_t capacity, const char* base,
                      const char* name) {
    if (!target || !capacity) return;
    target[0] = '\0';
    copy_text(target, capacity, base);
    size_t used = strlen(target);
    if (used && used + 1 < capacity &&
        target[used - 1] != '\\' && target[used - 1] != '/') {
        target[used++] = '\\';
        target[used] = '\0';
    }
    copy_text(target + used, capacity - used, name);
}

static void make_drive_path(char* target, size_t capacity, char drive,
                            const char* suffix) {
    if (!target || capacity < 3) return;
    target[0] = drive ? drive : '?';
    target[1] = ':';
    copy_text(target + 2, capacity - 2, suffix);
}

static bool path_exists_assume(const char* path) {
    vfs_dirent_t entry;
    return path && vfs_find_file(path, &entry) == VFS_OK;
}

static char find_boot_media_drive(void) {
    char live_drive = vfs_get_boot_cdrom_letter();
    if (live_drive) return live_drive;
    for (char drive = 'C'; drive <= 'Z'; drive++) {
        vfs_drive_t* volume = vfs_get_drive(drive);
        if (!volume || !volume->mounted ||
            (volume->drv_type != VFS_DRV_CDROM &&
             volume->drv_type != VFS_DRV_USB)) {
            continue;
        }
        char probe[48];
        make_drive_path(probe, sizeof(probe), drive, "\\APPS\\files.pxe");
        if (path_exists_assume(probe)) return drive;
    }
    return 0;
}

static char find_local_drive(void) {
    char drive = settings_get_system_drive();
    if (drive) return drive;
    for (drive = 'C'; drive <= 'Z'; drive++) {
        vfs_drive_t* volume = vfs_get_drive(drive);
        if (!volume || !volume->mounted || volume->is_readonly ||
            !volume->is_supported) {
            continue;
        }
        char probe[48];
        make_drive_path(probe, sizeof(probe), drive, "\\boot\\kernel.elf");
        if (!path_exists_assume(probe)) continue;
        make_drive_path(probe, sizeof(probe), drive, "\\APPS\\files.pxe");
        if (path_exists_assume(probe)) return drive;
    }
    return 0;
}

static void build_shortcut(uint8_t output[512], size_t* output_size,
                           const char* title, const char* target,
                           desktop_target_kind_t target_kind, bool locked) {
    const char* kind = target_kind == DESKTOP_TARGET_FOLDER ? "folder" :
                       target_kind == DESKTOP_TARGET_FILE ? "file" : "app";
    int length = snprintf((char*)output, 512,
                          "PNK1\r\nkind=%s\r\ntarget=%s\r\n"
                          "title=%s\r\nlocked=%d\r\n",
                          kind, target, title, locked ? 1 : 0);
    if (length < 0) length = 0;
    if (length >= 512) length = 511;
    *output_size = (size_t)length;
}

static bool ensure_directory(const char* path) {
    if (path_exists_assume(path)) return true;
    return vfs_mkdir(path) == VFS_OK || path_exists_assume(path);
}

static bool write_default_shortcut(const desktop_default_t* item,
                                   char drive, char* error,
                                   size_t error_capacity) {
    char filename[48];
    char path[96];
    char target[256];
    copy_text(filename, sizeof(filename), item->name);
    char* extension = strrchr(filename, '.');
    if (extension) *extension = '\0';
    copy_text(filename + strlen(filename), sizeof(filename) - strlen(filename),
              ".pnk");
    join_path(path, sizeof(path), g_desktop_dir, filename);
    if (path_exists_assume(path)) return true;
    make_drive_path(target, sizeof(target), drive, item->target_suffix);
    uint8_t data[512];
    size_t data_size = 0;
    build_shortcut(data, &data_size, item->title, target, item->target_kind,
                   item->locked);
    if (vfs_write_file(path, data, (unsigned int)data_size) != VFS_OK) {
        if (error && error_capacity) {
            snprintf(error, error_capacity, "Could not create %s", filename);
        }
        return false;
    }
    return true;
}

static void seed_defaults(char drive, bool seed_all) {
    if (!g_mutable) return;
    char error[96] = { 0 };
    (void)ensure_directory(g_game_dir);
    (void)ensure_directory(g_desktop_dir);
    for (size_t index = 0; index < sizeof(g_defaults) / sizeof(g_defaults[0]);
         index++) {
        bool game_shortcut = text_equal_ci(g_defaults[index].name, "GAME.pnk");
        if (!seed_all && !g_defaults[index].locked && !game_shortcut) continue;
        (void)write_default_shortcut(&g_defaults[index], drive, error,
                                     sizeof(error));
    }
    if (error[0]) log_printf("[DESK] %s\n", error);
}

static bool install_missing_locked_defaults(char drive) {
    if (!g_mutable) return false;
    bool changed = false;
    for (size_t index = 0; index < sizeof(g_defaults) / sizeof(g_defaults[0]);
         index++) {
        if (!g_defaults[index].locked) continue;
        char filename[48];
        char path[96];
        copy_text(filename, sizeof(filename), g_defaults[index].name);
        char* extension = strrchr(filename, '.');
        if (extension) *extension = '\0';
        copy_text(filename + strlen(filename),
                  sizeof(filename) - strlen(filename), ".pnk");
        join_path(path, sizeof(path), g_desktop_dir, filename);
        if (path_exists_assume(path)) continue;
        if (write_default_shortcut(&g_defaults[index], drive, NULL, 0)) {
            changed = true;
        }
    }
    return changed;
}

static void parse_shortcut(desktop_item_t* item) {
    uint8_t data[512];
    memset(data, 0, sizeof(data));
    int length = vfs_read_file(item->path, data, sizeof(data) - 1);
    if (length <= 0) return;
    data[length] = '\0';
    const char* cursor = (const char*)data;
    while (*cursor) {
        const char* line = cursor;
        const char* end = line;
        while (*end && *end != '\r' && *end != '\n') end++;
        if (end - line > 3) {
            if (line[0] == 'k' && line[1] == 'i' && line[2] == 'n' &&
                line[3] == 'd' && line[4] == '=') {
                const char* value = line + 5;
                char kind[12];
                size_t count = (size_t)(end - value);
                if (count >= sizeof(kind)) count = sizeof(kind) - 1;
                memcpy(kind, value, count);
                kind[count] = '\0';
                if (text_equal_ci(kind, "folder")) {
                    item->target_kind = DESKTOP_TARGET_FOLDER;
                } else if (text_equal_ci(kind, "file")) {
                    item->target_kind = DESKTOP_TARGET_FILE;
                } else {
                    item->target_kind = DESKTOP_TARGET_APP;
                }
            } else if (line[0] == 't' && line[1] == 'a' && line[2] == 'r' &&
                       line[3] == 'g' && line[4] == 'e' && line[5] == 't' &&
                       line[6] == '=') {
                size_t count = (size_t)(end - (line + 7));
                if (count >= sizeof(item->target)) {
                    count = sizeof(item->target) - 1;
                }
                memcpy(item->target, line + 7, count);
                item->target[count] = '\0';
            } else if (line[0] == 't' && line[1] == 'i' && line[2] == 't' &&
                       line[3] == 'l' && line[4] == 'e' && line[5] == '=') {
                size_t count = (size_t)(end - (line + 6));
                if (count >= sizeof(item->display_name)) {
                    count = sizeof(item->display_name) - 1;
                }
                memcpy(item->display_name, line + 6, count);
                item->display_name[count] = '\0';
            } else if (line[0] == 'l' && line[1] == 'o' && line[2] == 'c' &&
                       line[3] == 'k' && line[4] == 'e' && line[5] == 'd' &&
                       line[6] == '=') {
                item->locked = line[7] == '1';
            }
        }
        cursor = *end ? end + 1 : end;
        while (*cursor == '\r' || *cursor == '\n') cursor++;
    }
}

static void display_name_from_file(const char* name, char* output,
                                   size_t capacity) {
    copy_text(output, capacity, name);
    char* extension = strrchr(output, '.');
    if (extension && text_equal_ci(extension, ".pnk")) *extension = '\0';
}

static bool is_locked_shortcut(const char* name) {
    return text_equal_ci(name, "File Management.pnk") ||
           text_equal_ci(name, "About This Machine.pnk");
}

static desktop_layout_entry_t* layout_entry(const char* name) {
    for (int index = 0; index < g_layout_count; index++) {
        if (g_layout[index].used && text_equal_ci(g_layout[index].name, name)) {
            return &g_layout[index];
        }
    }
    return NULL;
}

static desktop_item_t* old_item(const char* name) {
    for (int index = 0; index < g_item_count; index++) {
        if (text_equal_ci(g_items[index].name, name)) return &g_items[index];
    }
    return NULL;
}

static void clamp_position(int* x, int* y);

static void default_position(int ordinal, int* x, int* y) {
    int rows = (screen_h - 40) / DESKTOP_ROW_STEP;
    if (rows < 2) rows = 2;
    int column = ordinal / rows;
    int row = ordinal % rows;
    *x = screen_w - DESKTOP_RIGHT_MARGIN - column * DESKTOP_COLUMN_STEP;
    *y = 30 + row * DESKTOP_ROW_STEP;
    if (*x < 4) *x = 4;
    if (*y > screen_h - 70) *y = screen_h - 70;
}

static void apply_anchored_position(desktop_item_t* item, int ordinal) {
    int rows = (screen_h - 40) / DESKTOP_ROW_STEP;
    if (rows < 2) rows = 2;
    int column = ordinal / rows;
    int row = ordinal % rows;
    if (item->right_offset <= 0) {
        item->right_offset = DESKTOP_RIGHT_MARGIN;
    }
    item->x = screen_w - item->right_offset -
              column * DESKTOP_COLUMN_STEP;
    item->y = 30 + row * DESKTOP_ROW_STEP;
    clamp_position(&item->x, &item->y);
}

static void clamp_position(int* x, int* y) {
    if (*x < 4) *x = 4;
    if (*x + 36 > screen_w) *x = screen_w - 36;
    if (*y < 24) *y = 24;
    if (*y + 70 > screen_h) *y = screen_h - 70;
    if (*x < 4) *x = 4;
    if (*y < 24) *y = 24;
}

static const char* parse_decimal(const char* text, int* value) {
    bool negative = false;
    if (!text || !value) return text;
    if (*text == '-') {
        negative = true;
        text++;
    }
    if (*text < '0' || *text > '9') return NULL;
    int result = 0;
    while (*text >= '0' && *text <= '9') {
        if (result < 100000) result = result * 10 + (*text - '0');
        text++;
    }
    *value = negative ? -result : result;
    return text;
}

static void parse_layout(const char* text, size_t length) {
    g_layout_count = 0;
    size_t offset = 0;
    while (offset < length && g_layout_count < DESKTOP_MAX_ITEMS) {
        size_t end = offset;
        while (end < length && text[end] != '\r' && text[end] != '\n') end++;
        if (end > offset) {
            const char* line = text + offset;
            char line_copy[160];
            size_t line_length = end - offset;
            if (line_length >= sizeof(line_copy)) {
                line_length = sizeof(line_copy) - 1;
            }
            memcpy(line_copy, line, line_length);
            line_copy[line_length] = '\0';
            bool anchored_right = line_copy[0] == 'r' &&
                                  line_copy[1] == ',';
            bool absolute = line_copy[0] == 'a' &&
                            line_copy[1] == ',';
            const char* field = line_copy +
                                (anchored_right || absolute ? 2 : 0);
            int first = 0;
            int y = 0;
            const char* cursor = parse_decimal(field, &first);
            if (cursor && *cursor == ',') {
                cursor = parse_decimal(cursor + 1, &y);
            } else {
                cursor = NULL;
            }
            if (cursor && *cursor == ',') {
                cursor++;
                desktop_layout_entry_t* entry =
                    &g_layout[g_layout_count++];
                memset(entry, 0, sizeof(*entry));
                entry->used = true;
                entry->anchored_right = anchored_right;
                entry->right_offset = anchored_right ?
                    first : DESKTOP_RIGHT_MARGIN;
                entry->x = anchored_right ? 0 : first;
                entry->y = y;
                copy_text(entry->name, sizeof(entry->name),
                          cursor);
                size_t name_length = strlen(entry->name);
                while (name_length &&
                       (entry->name[name_length - 1] == '\r' ||
                        entry->name[name_length - 1] == '\n')) {
                    entry->name[--name_length] = '\0';
                }
            }
        }
        offset = end;
        while (offset < length &&
               (text[offset] == '\r' || text[offset] == '\n')) {
            offset++;
        }
    }
}

static void load_layout(void) {
    g_layout_count = 0;
    if (!g_mutable && !path_exists_assume(g_layout_path)) return;
    uint8_t* text = (uint8_t*)kmalloc(DESKTOP_LAYOUT_MAX);
    if (!text) return;
    memset(text, 0, DESKTOP_LAYOUT_MAX);
    int length = vfs_read_file(g_layout_path, text, DESKTOP_LAYOUT_MAX - 1);
    if (length > 0) {
        text[length] = '\0';
        size_t header_length = strlen(DESKTOP_LAYOUT_HEADER);
        if ((size_t)length >= header_length &&
            strncmp((const char*)text, DESKTOP_LAYOUT_HEADER,
                    header_length) == 0) {
            parse_layout((const char*)text, (size_t)length);
        } else {
            /*
             * PNK_LAYOUT1 used absolute coordinates for the previous
             * 800x600 auto layout.  Reflow it once into the new top-right
             * anchored format instead of leaving icons in the middle of a
             * larger display.
             */
            g_layout_count = 0;
            log_printf("[DESK] Migrating legacy desktop layout to top-right\n");
        }
    }
    kfree(text);
}

static void save_layout(void) {
    if (!g_mutable || !g_desktop_dir[0]) return;
    char* text = (char*)kmalloc(DESKTOP_LAYOUT_MAX);
    if (!text) return;
    memset(text, 0, DESKTOP_LAYOUT_MAX);
    size_t used = 0;
    used += (size_t)snprintf(text + used, DESKTOP_LAYOUT_MAX - used,
                             DESKTOP_LAYOUT_HEADER "\r\n");
    for (int index = 0; index < g_item_count && used < DESKTOP_LAYOUT_MAX;
         index++) {
        int written;
        if (g_items[index].anchored_right) {
            written = snprintf(text + used, DESKTOP_LAYOUT_MAX - used,
                               "r,%d,%d,%s\r\n",
                               g_items[index].right_offset,
                               g_items[index].y, g_items[index].name);
        } else {
            written = snprintf(text + used, DESKTOP_LAYOUT_MAX - used,
                               "a,%d,%d,%s\r\n", g_items[index].x,
                               g_items[index].y, g_items[index].name);
        }
        if (written <= 0) break;
        used += (size_t)written;
    }
    (void)vfs_write_file(g_layout_path, (const uint8_t*)text,
                         (unsigned int)used);
    kfree(text);
}

static void sort_items(void) {
    for (int left = 0; left < g_item_count; left++) {
        for (int right = left + 1; right < g_item_count; right++) {
            if (strcasecmp(g_items[left].name, g_items[right].name) > 0) {
                desktop_item_t temporary = g_items[left];
                g_items[left] = g_items[right];
                g_items[right] = temporary;
            }
        }
    }
}

typedef struct desktop_scan {
    char names[DESKTOP_MAX_ITEMS][128];
    bool directories[DESKTOP_MAX_ITEMS];
    int count;
} desktop_scan_t;

static desktop_scan_t* g_pending_scan;

static void collect_entry(const vfs_dirent_t* entry) {
    if (!entry || !entry->name[0] ||
        text_equal_ci(entry->name, DESKTOP_LAYOUT_NAME) ||
        text_equal_ci(entry->name, ".") ||
        text_equal_ci(entry->name, "..")) {
        return;
    }
    desktop_scan_t* scan = (desktop_scan_t*)g_pending_scan;
    if (!scan || scan->count >= DESKTOP_MAX_ITEMS) return;
    copy_text(scan->names[scan->count], sizeof(scan->names[0]),
              entry->name);
    scan->directories[scan->count] = entry->is_dir != 0;
    scan->count++;
}

void desktop_refresh(void) {
    if (!g_initialized || !g_desktop_path[0]) return;
    desktop_scan_t* scan = (desktop_scan_t*)kmalloc(sizeof(*scan));
    if (!scan) return;
    memset(scan, 0, sizeof(*scan));
    g_pending_scan = scan;
    int listing = vfs_list_dir(g_desktop_dir, collect_entry);
    g_pending_scan = NULL;
    if (listing != VFS_OK) {
        kfree(scan);
        return;
    }

    desktop_item_t* next = (desktop_item_t*)kmalloc(sizeof(g_items));
    if (!next) {
        kfree(scan);
        return;
    }
    memset(next, 0, sizeof(g_items));
    int count = 0;
    bool layout_changed = false;
    for (int index = 0; index < scan->count; index++) {
        desktop_item_t* item = &next[count];
        copy_text(item->name, sizeof(item->name), scan->names[index]);
        join_path(item->path, sizeof(item->path), g_desktop_dir,
                  item->name);
        display_name_from_file(item->name, item->display_name,
                               sizeof(item->display_name));
        if (scan->directories[index]) {
            item->kind = DESKTOP_ITEM_FOLDER;
            item->target_kind = DESKTOP_TARGET_FOLDER;
            copy_text(item->target, sizeof(item->target), item->path);
        } else if (ends_with_ci(item->name, ".pnk")) {
            item->kind = DESKTOP_ITEM_SHORTCUT;
            item->target_kind = DESKTOP_TARGET_APP;
            parse_shortcut(item);
            if (item->target[0] && item->target[1] != ':' &&
                g_desktop_path[0]) {
                char resolved[256];
                resolved[0] = g_desktop_path[0];
                resolved[1] = ':';
                copy_text(resolved + 2, sizeof(resolved) - 2,
                          item->target);
                copy_text(item->target, sizeof(item->target), resolved);
            }
        } else {
            item->kind = DESKTOP_ITEM_FILE;
            item->target_kind = DESKTOP_TARGET_FILE;
            copy_text(item->target, sizeof(item->target), item->path);
        }
        if (is_locked_shortcut(item->name)) item->locked = true;
        if (settings_is_language_zh()) {
            if (text_equal_ci(item->display_name, "File Management")) copy_text(item->display_name, sizeof(item->display_name), "文件管理");
            else if (text_equal_ci(item->display_name, "About This Machine")) copy_text(item->display_name, sizeof(item->display_name), "关于本机");
            else if (text_equal_ci(item->display_name, "Clock")) copy_text(item->display_name, sizeof(item->display_name), "时钟");
            else if (text_equal_ci(item->display_name, "Calculator")) copy_text(item->display_name, sizeof(item->display_name), "计算器");
            else if (text_equal_ci(item->display_name, "Control Panel")) copy_text(item->display_name, sizeof(item->display_name), "控制面板");
            else if (text_equal_ci(item->display_name, "System Information")) copy_text(item->display_name, sizeof(item->display_name), "系统信息");
            else if (text_equal_ci(item->display_name, "Pixel CPU Detect")) copy_text(item->display_name, sizeof(item->display_name), "CPU 检测");
            else if (text_equal_ci(item->display_name, "Notepad")) copy_text(item->display_name, sizeof(item->display_name), "记事本");
            else if (text_equal_ci(item->display_name, "Media Player")) copy_text(item->display_name, sizeof(item->display_name), "媒体播放器");
            else if (text_equal_ci(item->display_name, "Image Viewer")) copy_text(item->display_name, sizeof(item->display_name), "图片查看器");
            else if (text_equal_ci(item->display_name, "GAME")) copy_text(item->display_name, sizeof(item->display_name), "游戏");
        }
        desktop_item_t* previous = old_item(item->name);
        if (previous && previous->kind == item->kind &&
            previous->icon_state == 1 &&
            text_equal_ci(previous->target, item->target)) {
            item->icon_state = previous->icon_state;
            memcpy(item->icon, previous->icon, sizeof(item->icon));
        }
        desktop_layout_entry_t* saved = layout_entry(item->name);
        if (saved) {
            item->anchored_right = saved->anchored_right;
            item->right_offset = saved->anchored_right ?
                DESKTOP_RIGHT_MARGIN : 0;
            item->x = saved->anchored_right ?
                screen_w - DESKTOP_RIGHT_MARGIN : saved->x;
            item->y = saved->y;
        } else if (previous) {
            item->anchored_right = previous->anchored_right;
            item->right_offset = previous->anchored_right ?
                DESKTOP_RIGHT_MARGIN : 0;
            item->x = previous->x;
            item->y = previous->y;
        } else {
            item->anchored_right = true;
            item->right_offset = DESKTOP_RIGHT_MARGIN;
            default_position(count, &item->x, &item->y);
            layout_changed = true;
        }
        int clamped_x = item->x;
        int clamped_y = item->y;
        clamp_position(&clamped_x, &clamped_y);
        if (clamped_x != item->x || clamped_y != item->y) {
            item->x = clamped_x;
            item->y = clamped_y;
            layout_changed = true;
        }
        count++;
    }
    memcpy(g_items, next, sizeof(g_items));
    g_item_count = count;
    kfree(next);
    kfree(scan);
    sort_items();
    for (int index = 0; index < g_item_count; index++) {
        if (!g_items[index].anchored_right) continue;
        int old_x = g_items[index].x;
        int old_y = g_items[index].y;
        apply_anchored_position(&g_items[index], index);
        if (old_x != g_items[index].x || old_y != g_items[index].y) {
            layout_changed = true;
        }
    }
    if (layout_changed && g_mutable) save_layout();
}

bool desktop_poll(void) {
    if (!g_initialized || !g_refresh_requested) return false;
    g_refresh_requested = false;
    desktop_refresh();
    return true;
}

void desktop_on_resolution_change(int width, int height) {
    (void)width;
    (void)height;
    bool changed = false;
    for (int index = 0; index < g_item_count; index++) {
        desktop_item_t* item = &g_items[index];
        int old_x = item->x;
        int old_y = item->y;
        if (item->anchored_right) {
            apply_anchored_position(item, index);
        } else {
            clamp_position(&item->x, &item->y);
        }
        if (old_x != item->x || old_y != item->y) changed = true;
    }
    if (changed) save_layout();
}

void desktop_init(void) {
    memset(g_items, 0, sizeof(g_items));
    memset(g_layout, 0, sizeof(g_layout));
    g_item_count = 0;
    g_layout_count = 0;
    g_initialized = false;
    g_mutable = false;
    g_refresh_requested = false;
    g_desktop_path[0] = '\0';
    g_desktop_dir[0] = '\0';
    g_game_dir[0] = '\0';
    g_layout_path[0] = '\0';

    settings_boot_mode_t mode = settings_get_boot_mode();
    if (mode == SETTINGS_BOOT_INSTALLER) return;
    g_mutable = mode == SETTINGS_BOOT_LOCAL;
    char drive = g_mutable ? find_local_drive() : find_boot_media_drive();
    if (!drive) {
        log_printf("[DESK] No boot media available for Desktop\n");
        return;
    }

    make_drive_path(g_desktop_dir, sizeof(g_desktop_dir), drive,
                    "\\Desktop");
    make_drive_path(g_game_dir, sizeof(g_game_dir), drive, "\\GAME");
    make_drive_path(g_layout_path, sizeof(g_layout_path), drive,
                    "\\Desktop\\" DESKTOP_LAYOUT_NAME);
    g_desktop_path[0] = drive;
    g_desktop_path[1] = ':';
    g_desktop_path[2] = '\0';

    bool desktop_existed = path_exists_assume(g_desktop_dir);
    if (g_mutable) {
        (void)ensure_directory(g_game_dir);
        (void)ensure_directory(g_desktop_dir);
        seed_defaults(drive, !desktop_existed);
        (void)install_missing_locked_defaults(drive);
    }
    g_initialized = true;
    load_layout();
    desktop_refresh();
    if (g_mutable) save_layout();
    log_printf("[DESK] %s mode root=%s items=%d layout=%s\n",
               g_mutable ? "local" : "live/read-only", g_desktop_dir,
               g_item_count, g_layout_path);
}

int desktop_count(void) {
    return g_item_count;
}

desktop_item_t* desktop_get(int index) {
    return index >= 0 && index < g_item_count ? &g_items[index] : NULL;
}

bool desktop_load_icon(int index, uint8_t out_icon[16][16]) {
    desktop_item_t* item = desktop_get(index);
    if (!item || !out_icon) return false;
    if (item->icon_state == 0) {
        if (item->kind == DESKTOP_ITEM_SHORTCUT &&
            pxe_load_icon16_path(item->target, item->icon)) {
            item->icon_state = 1;
        } else {
            item->icon_state = 2;
        }
    }
    if (item->icon_state != 1) return false;
    memcpy(out_icon, item->icon, sizeof(item->icon));
    return true;
}

bool desktop_is_mutable(void) {
    return g_mutable;
}

static bool apps_path_filename(const char* path, char* filename,
                               size_t capacity) {
    if (!path || !filename || !capacity) return false;
    const char* apps = NULL;
    for (const char* cursor = path; *cursor; cursor++) {
        if ((*cursor == '\\' || *cursor == '/') &&
            (text_starts_with_ci(cursor + 1, "APPS\\") ||
             text_starts_with_ci(cursor + 1, "APPS/"))) {
            apps = cursor + 6;
        }
    }
    if (!apps || !*apps) return false;
    copy_text(filename, capacity, apps);
    return true;
}

static bool launch_system_app_path(const char* path) {
    char filename[64];
    if (apps_path_filename(path, filename, sizeof(filename))) {
        if (text_equal_ci(filename, "install.pxe") &&
            settings_get_boot_mode() != SETTINGS_BOOT_INSTALLER) {
            wm_notify_banner("INSTALL is disabled in Local/Live mode");
            return false;
        }
        bool bypass = text_equal_ci(filename, "sysinfo.pxe") ||
                      text_equal_ci(filename, "cpuz.pxe") ||
                      text_equal_ci(filename, "taskmgr.pxe") ||
                      text_equal_ci(filename, "install.pxe");
        log_printf("[DESK] Launch system app %s from %s\n", filename, path);
        return pxe_launch_builtin_app(filename, NULL, bypass) >= 0;
    }
    return pxe_ui_launch_from_path(path, NULL) >= 0;
}

static bool launch_files_for(const char* path) {
    return pxe_launch_builtin_app("files.pxe", path, false) >= 0;
}

void desktop_open_item(int index) {
    desktop_item_t* item = desktop_get(index);
    if (!item) return;
    if (item->kind == DESKTOP_ITEM_FOLDER) {
        (void)launch_files_for(item->path);
        return;
    }
    if (item->kind == DESKTOP_ITEM_SHORTCUT) {
        if (item->target_kind == DESKTOP_TARGET_FOLDER) {
            (void)launch_files_for(item->target);
        } else if (ends_with_ci(item->target, ".pxe")) {
            (void)launch_system_app_path(item->target);
        } else {
            (void)launch_files_for(item->target);
        }
        return;
    }
    if (ends_with_ci(item->path, ".pxe")) {
        (void)launch_system_app_path(item->path);
    } else {
        (void)launch_files_for(item->path);
    }
}

void desktop_rearrange(void) {
    if (!desktop_is_mutable()) return;
    for (int index = 0; index < g_item_count; index++) {
        g_items[index].anchored_right = true;
        g_items[index].right_offset = DESKTOP_RIGHT_MARGIN;
        apply_anchored_position(&g_items[index], index);
    }
    save_layout();
}

bool desktop_set_position(int index, int x, int y) {
    desktop_item_t* item = desktop_get(index);
    if (!item || !g_mutable) return false;
    if (x < 4) x = 4;
    if (x + 36 > screen_w) x = screen_w - 36;
    if (y < 24) y = 24;
    if (y + 70 > screen_h) y = screen_h - 70;
    item->anchored_right = false;
    item->right_offset = 0;
    item->x = x;
    item->y = y;
    save_layout();
    return true;
}

int desktop_delete_item(int index, char* error, size_t error_capacity) {
    desktop_item_t* item = desktop_get(index);
    if (!item) return VFS_ERR_NOT_FOUND;
    if (!g_mutable) {
        if (error && error_capacity) {
            snprintf(error, error_capacity,
                     "Live Desktop is read-only.");
        }
        return VFS_ERR_READONLY;
    }
    if (item->locked || vfs_path_is_protected(item->path)) {
        if (error && error_capacity) {
            snprintf(error, error_capacity,
                     "This required shortcut is protected.");
        }
        return VFS_ERR_PROTECTED;
    }
    int result = vfs_unlink(item->path);
    if (result != VFS_OK) {
        if (error && error_capacity) {
            snprintf(error, error_capacity,
                     result == VFS_ERR_IO ?
                         "The folder is not empty or the disk reported an error." :
                         "Unable to delete this desktop item.");
        }
        return result;
    }
    desktop_refresh();
    save_layout();
    return VFS_OK;
}

int desktop_create_folder(char* error, size_t error_capacity) {
    if (!g_mutable) {
        if (error && error_capacity) {
            snprintf(error, error_capacity,
                     "Live Desktop is read-only.");
        }
        return VFS_ERR_READONLY;
    }
    char path[128];
    char name[64];
    int created = -1;
    for (int suffix = 0; suffix < 100; suffix++) {
        if (suffix == 0) {
            copy_text(name, sizeof(name), "New Folder");
        } else {
            snprintf(name, sizeof(name), "New Folder %d", suffix + 1);
        }
        join_path(path, sizeof(path), g_desktop_dir, name);
        if (path_exists_assume(path)) continue;
        created = vfs_mkdir(path);
        break;
    }
    if (created != VFS_OK) {
        if (error && error_capacity) {
            snprintf(error, error_capacity, "Could not create a folder.");
        }
        return created;
    }
    desktop_refresh();
    save_layout();
    return VFS_OK;
}
