#ifndef _CMD_H
#define _CMD_H

#include <pxe/abi.h>
#include <pxe/sys.h>
#include <pxe/gui.h>
#include <pxe/gfx.h>
#include <pxe/fs.h>
#include <pxe/mem.h>
#include <pxe/str.h>
#include <pxe/format.h>

#define TERM_DEFAULT_COLS  80
#define TERM_DEFAULT_ROWS  25
#define TERM_MAX_COLS      160
#define TERM_MAX_ROWS      60
#define TERM_SCROLLBACK    500
#define TERM_CELL_W        8
#define TERM_CELL_H        16
#define TERM_PAD_X         4
#define TERM_PAD_Y         4
#define TERM_SB_W          14

#define CELL_FLAG_NORMAL     0x00u
#define CELL_FLAG_WIDE_LEAD  0x01u
#define CELL_FLAG_WIDE_TRAIL 0x02u
#define CELL_FLAG_BOLD       0x04u

#define CMD_MAX_LINE       512
#define CMD_HISTORY_MAX    32
#define CMD_MAX_ENV        48
#define CMD_MAX_PAT_DEPTH  8
#define CMD_MAX_STDIN_BUF  65536
#define CMD_CAP_INIT       8192
#define CMD_CAP_MAX        131072

typedef struct {
    uint32_t codepoint;
    uint8_t  fg;
    uint8_t  bg;
    uint8_t  flags;
    uint8_t  pad;
} term_cell_t;

typedef struct {
    char name[48];
    char val[256];
} cmd_env_t;

typedef struct {
    int win_id;
    int running;
    int zh;
    int dirty;

    /* Terminal grid (ring/flat 500 lines x 160 cols) */
    term_cell_t* cells;
    int buf_cols;
    int buf_rows;
    int vis_cols;
    int vis_rows;
    int top_row;      /* Oldest scrolled-off logical line index */
    int view_row;     /* Viewport first visible line in [0..cursor_row] */
    int cursor_row;   /* Logical cursor row in [0..buf_rows-1] */
    int cursor_col;   /* Logical cursor col in [0..vis_cols-1] */
    uint8_t cur_fg;
    uint8_t cur_bg;
    uint8_t def_fg;
    uint8_t def_bg;
    int bold;

    /* Prompt / line editor */
    char cwd[256];
    char line[CMD_MAX_LINE];
    int  line_len;
    int  line_cursor;
    int  prompt_start_row;
    int  prompt_start_col;
    int  waiting_input; /* Non-zero when editing command prompt */

    /* History */
    char history[CMD_HISTORY_MAX][CMD_MAX_LINE];
    int  hist_count;
    int  hist_nav;      /* -1 when not browsing history */
    char hist_saved[CMD_MAX_LINE];

    /* Tab completion */
    char tab_prefix[CMD_MAX_LINE];
    char tab_matches[64][128];
    int  tab_match_count;
    int  tab_match_idx;
    int  tab_active;
    int  tab_token_start;

    /* Mouse selection & context menu */
    int  sel_active;
    int  sel_dragging;
    int  sel_r0, sel_c0;
    int  sel_r1, sel_c1;
    int  sb_dragging;
    int  sb_drag_y;
    int  sb_drag_view;
    int  ctx_open;
    int  ctx_x, ctx_y;
    int  blink_phase;

    /* Output capture (for | and > / >>) */
    char* cap_buf;
    size_t cap_len;
    size_t cap_cap;
    int   capturing;

    /* Pipe input stream passed to current command */
    const char* stdin_data;
    size_t      stdin_len;

    /* Interrupt & execution state */
    volatile int interrupted;
    int last_errorlevel;
    int echo_on;

    /* Environment variables */
    cmd_env_t envs[CMD_MAX_ENV];
    int env_count;

    /* Batch call parameters (%0..%9) */
    char pat_args[10][128];
    int  pat_arg_count;
    int  pat_depth;

    /* Offscreen surface */
    uint8_t* back_pixels;
    size_t   back_cap;
} cmd_state_t;

#define CMD_TR(st, z, e) ((st)->zh ? (z) : (e))

/* Terminal output & interactive helpers (cmd.c) */
void term_clear(cmd_state_t* st);
void term_putc_cp(cmd_state_t* st, uint32_t cp);
void term_puts(cmd_state_t* st, const char* utf8);
void term_printf(cmd_state_t* st, const char* fmt, ...);
void term_set_attr(cmd_state_t* st, uint8_t bg, uint8_t fg);
void term_render_now(cmd_state_t* st);
int  term_wait_key_blocking(cmd_state_t* st, pxe_event_t* out_ev);
int  term_poll_interrupt(cmd_state_t* st);
int  term_more_prompt(cmd_state_t* st);

/* Environment & path helpers (cmd_shell.c) */
void shell_init_env(cmd_state_t* st);
const char* shell_get_env(cmd_state_t* st, const char* name);
void shell_set_env(cmd_state_t* st, const char* name, const char* val);
void shell_expand_vars(cmd_state_t* st, const char* in, char* out, size_t cap);
int  shell_resolve_path(cmd_state_t* st, const char* spec, char* out, size_t cap);
void shell_sync_cwd_env(cmd_state_t* st);
int  shell_exec_line(cmd_state_t* st, const char* raw_line);
int  shell_run_pat_file(cmd_state_t* st, const char* path, int argc, const char** argv);
void shell_tab_complete(cmd_state_t* st);
void shell_tab_reset(cmd_state_t* st);

/* Built-in commands (cmd_cmds.c) */
int  cmd_dispatch_builtin(cmd_state_t* st, int argc, char** argv, const char* raw_tail);
int  cmd_wildcard_match(const char* pat, const char* str);
int  cmd_resolve_app_path(cmd_state_t* st, const char* app_name, char* out, size_t cap);
int  cmd_open_with_assoc(cmd_state_t* st, const char* target_path);

#endif

