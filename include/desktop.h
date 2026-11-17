#ifndef DESKTOP_H
#define DESKTOP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DESKTOP_MAX_ITEMS 64

typedef enum {
    DESKTOP_ITEM_FILE = 0,
    DESKTOP_ITEM_FOLDER,
    DESKTOP_ITEM_SHORTCUT
} desktop_item_kind_t;

typedef enum {
    DESKTOP_TARGET_APP = 0,
    DESKTOP_TARGET_FOLDER,
    DESKTOP_TARGET_FILE
} desktop_target_kind_t;

typedef struct {
    char name[128];
    char display_name[96];
    char path[256];
    char target[256];
    desktop_item_kind_t kind;
    desktop_target_kind_t target_kind;
    bool locked;
    int x;
    int y;
    int icon_state;
    uint8_t icon[16][16];
} desktop_item_t;

void desktop_init(void);
void desktop_refresh(void);
bool desktop_poll(void);
void desktop_on_resolution_change(int width, int height);
int desktop_count(void);
desktop_item_t* desktop_get(int index);
bool desktop_load_icon(int index, uint8_t out_icon[16][16]);
bool desktop_is_mutable(void);
void desktop_open_item(int index);
void desktop_rearrange(void);
bool desktop_set_position(int index, int x, int y);
int desktop_delete_item(int index, char* error, size_t error_capacity);
int desktop_create_folder(char* error, size_t error_capacity);

#endif
