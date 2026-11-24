#ifndef _IME_H
#define _IME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IME_PAGE_SIZE 7

void ime_init(void);
bool ime_is_enabled(void);
bool ime_has_full_lexicon(void);
uint32_t ime_get_phrase_count(void);
uint32_t ime_get_single_count(void);
void ime_set_enabled(bool enabled);
void ime_toggle(void);

bool ime_has_composition(void);
void ime_clear_composition(void);

/*
 * Process a key event in the active window context.
 * Returns 1 if consumed by IME (with optional UTF-8 text in out_commit),
 * or 0 if WM should forward the raw key to the active application.
 */
int ime_process_key(int key, int modifiers, char* out_commit, size_t out_cap);

/*
 * Handle mouse clicks when the IME candidate popup is visible.
 * Returns 1 if the click hit the IME popup (out_commit receives chosen UTF-8 text if any).
 */
int ime_handle_mouse(int mx, int my, int mb_press, char* out_commit, size_t out_cap);

/* Draw IME composition / candidate bar near active window or cursor area. */
void ime_draw_popup(unsigned char* v, int anchor_x, int anchor_y);

#endif /* _IME_H */
