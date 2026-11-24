# Pixel Neko OS 原生可执行程序开发指南（PXE v1.0 / `v26A300c`）

在当前 **`v26A300c`** 中，Pixel Neko OS 的应用程序与系统工具统一采用**双特权环原生可执行文件体系（`*.pxe`, Pixel Executable v1.0）**，通过统一的 `sdk/` 工具链与头文件构建，全面支持 UTF-8 简体中文显示、Rime 拼音输入法（`PXE_KEY_UTF8`）、动态界面语言切换、图形窗口合成（Compositor）、VFS 文件系统、音频引擎、动态显示能力查询及特权系统管理。

---

## 一、 架构概述与双环模型

### 1.1 文件格式与存储布局

- **`*.pxe` 容器（`PXE\x01`）**：由 [sdk/tools/mkpxe.py](../sdk/tools/mkpxe.py) 将声明式清单（`*.pxi`）与位置无关 ELF64 可执行体（`PIE`）打包而成，结构定义见 [sdk/include/pxe/format.h](../sdk/include/pxe/format.h)：
  - **128 字节 `pxe_header_t`**：魔数 `0x01455850u`（`"PXE\x01"`）、`abi_version = 0x0100u`、`arch = 0x8664u`（x86_64）、`exec_ring`（`3` 或 `0`）、`subsystem`、`flags`、`stack_size`、`heap_init_size`、`def_win_w/h`、`min_win_w/h`、全文件 IEEE 802.3 CRC32 校验、`app_name` 与 `app_version`。
  - **384 字节 `pxe_resource_t`**：内嵌 `16x16` 8-bit 索引色图标（`255` 为透明）、窗口标题 `title`、作者 `author`、文件扩展名关联 `ext_assoc`（如 `"txt,log,md"`）。
  - **4096 字节对齐的 ELF64 PIE 载荷**：单文件大小上限 `8 MB`（`PXE_MAX_FILE_BYTES`）。
- **磁盘部署路径**：
  - 内置系统应用：`\APPS\*.pxe`（包含 `about`、`clock`、`calc`、`notepad`、`cpanel`、`player`、`viewer`、`files`、`sysinfo`、`cpuz`、`taskmgr`、`install`）
  - 默认游戏：`\GAME\*.pxe`（`puzzle`、`solitaire`、`gomoku`、`aerochess`、`2048`、`minesweeper`、`othello`）
  - 桌面快捷方式：`\Desktop\*.pnk`，图标位置保存在 `\Desktop\.desktop-layout`
  - 用户双击文件管理（`files.pxe`）中任意 `.pxe`（或关联扩展名的文件）即可由内核动态加载运行。

### 1.2 Ring 3 与 Ring 0 执行对比

| 特性 | Ring 3 用户态（`PXE_RING = 3`，默认） | Ring 0 特权态（`PXE_RING = 0`） |
| :--- | :--- | :--- |
| **适用场景** | 普通 GUI 应用、工具、媒体播放器、设置客户端 | 系统信息、CPU 检测、磁盘分区/安装器、任务管理器（如 `sysinfo`、`cpuz`、`taskmgr`、`install`、`mini_taskmon`） |
| **编译参数** | `-fPIE -msse2` | `-fPIE -mgeneral-regs-only`（禁用 SSE/FPU） |
| **入口与运行库** | `crt0_r3.asm` (`_start`) + `runtime_r3.c` | `crt0_r0.asm` (`_start_r0`) + `runtime_r0.c` |
| **地址空间** | 独立进程页表（W^X 隔离）、`SYSCALL`/`SYSRET` 系统调用 | 高半区动态 PIE 槽位（`0xFFFFFFFFC0000000`~`0xFFFFFFFFEFFFFFFF`，`16 MB` 槽宽），通过 `g_pxe_ring0_api`（`PXE_R0_API_VERSION = 0x0101u`）直接调用内核函数 |
| **安全与故障隔离** | 发生 `#PF`/`#GP` 异常仅终止当前应用并弹出 System Bomb 隔离窗，不影响系统稳定性 | 启动非内置路径需经桌面特权确认框；拥有硬件/进程底层能力 |

> 注意：`SHM` 共享窗口画布槽步长在 `v26A220a` 中已扩展为 **`NEKO_SHM_STRIDE = 0x04000000ULL`（64 MB）**，足以支持 `4K+` 超高分辨率真彩色可调大小窗口表面映射。

> Ring 0 移植示例：[`apps/cpuz`](../apps/cpuz) 将 libcpuid 的 x86 识别内核裁剪后静态链接进 `*.pxe`。该端口只保留 Intel/AMD/Centaur 数据库与 CPUID 逻辑，通过 `compat/` 映射 PXE 的字符串和堆接口；不加载 MSR 驱动、不读取本地 CPUID 转储文件，并在没有线程亲和 ABI 时按当前执行核心识别缓存与拓扑。libcpuid 的 BSD-2-Clause 原文保存在 [`apps/cpuz/libcpuid/COPYING`](../apps/cpuz/libcpuid/COPYING)。诊断报告通过 `Save Report As...` 对话框选择完整目标路径（必须是带盘符的绝对 `*.TXT` 路径），并支持键盘输入和 `Ctrl+V` 粘贴。

---

## 二、 清单文件规范（`*.pxi`）

`.pxi` 采用 INI 语法，由 [sdk/tools/mkpxe.py](../sdk/tools/mkpxe.py) 解析：

```ini
[app]
name           = myapp
title          = "My PXE App"
author         = "Pixel Neko Developer"
version        = "1.0.0"
ring           = 3              ; 3 = Ring 3 用户态, 0 = Ring 0 特权态
subsystem      = GUI            ; GUI 或 BACKGROUND
flags          = SINGLE_INST | RESIZABLE
stack_size     = 64K            ; 支持 K / KB / M / MB 后缀
heap_init_size = 128K
def_w          = 360
def_h          = 240
min_w          = 260
min_h          = 180
; ext_assoc    = "txt,md"       ; 可选：关联双击打开的文件扩展名（嵌入 pxe_resource_t.ext_assoc，供 files.pxe 与 system\assoc.ini 协同解析）

[icon]
row00 = ................
row01 = ..000000000000..
row02 = .09999999999990.
...
row15 = ................
```

### 可选 `[app]` 标志（`flags`，支持 `|`、`,`、`+` 组合）

- `NONE`：无特殊标志
- `SINGLE_INST`（`PXE_FLAG_SINGLE_INST`）：单实例模式，重复启动时聚焦已运行窗口而非新开进程
- `RESIZABLE`（`PXE_FLAG_RESIZABLE`）：允许窗口右下角拖拽调整大小，内核会自动分配充足的窗口画布并发送 `PXE_EV_RESIZE` 事件
- `NO_CLOSE`（`PXE_FLAG_NO_CLOSE`）：隐藏标题栏关闭/折叠按钮
- `SYS_SETTINGS`（`PXE_FLAG_SYS_SETTINGS`）：授予 Ring 3 程序写系统显示/桌面配置的权限（如 `pxe_display_apply_mode()`、`pxe_display_set_mode()`；只读查询 `pxe_display_get_caps()` 不受此标志限制）

### `[icon]` 字符调色板

若省略 `[icon]`，打包器会自动填充默认 R3/R0 图标；若提供则必须完整编写 `row00` 到 `row15`（每行 16 个字符）：
- `.`、`_`、`-`：透明像素（索引 `255`）
- `0`~`9`、`a`~`f`（十六进制）：标准 16 色调色板索引（例如 `0`=黑，`7`=浅灰，`8`=深灰，`9`=蓝，`c`=红，`e`=黄，`f`=白）

---

## 三、 PXE SDK 核心头文件与 API 参考

头文件统一位于 [sdk/include/pxe/](../sdk/include/pxe/)，应用代码只需 `#include <pxe/sys.h>` 等对应头。

### 3.1 系统与动态显示能力（`<pxe/sys.h>`, `<pxe/abi.h>`）

- **进程、时间与启动参数**：
  - 入口函数支持标准 `int main(int argc, char** argv)`（`crt0_r3.asm` / `crt0_r0.asm` 传入 `argc`/`argv`，双击关联文档启动时 `argv[1]` 为目标路径；`SINGLE_INST` 重复唤醒通过 `PXE_EV_ACTIVATE` 事件的 `ev.text` 传递）
  - `void pxe_exit(int status)`、`int pxe_getpid(void)`、`void pxe_yield(void)`、`void pxe_sleep_ms(uint32_t ms)`、`uint64_t pxe_get_ticks(void)`、`void pxe_debug_log(const char* msg)`
  - `int pxe_spawn(const char* path, int argc, const char* const* argv)`、`int pxe_waitpid(int pid, int* out_status)`
  - `int pxe_get_rtc_time(pxe_rtc_time_t* out)`、`int pxe_sys_get_basic_info(pxe_basic_sysinfo_t* out)`（含 `ui_lang` 字段，取值为 `PXE_LANG_ZH` = `1` 或 `PXE_LANG_EN` = `0`）
  - **多语言控制（`v26A280a` 新增）**：`int pxe_is_zh(void)`（判断当前系统是否处于简体中文模式）、`int pxe_set_language(int lang)`（需 `SYS_SETTINGS` 或 Ring 0，写入 `SYSTEM.PSE` 并广播全桌面窗口重绘）
- **显示能力探测与分辨率/色深控制（`v26A220a` 新增）**：
  - **`int pxe_display_get_caps(pxe_display_caps_t* out)`**：从当前激活的显卡驱动动态读取显示能力，核心字段如下：
    - `flags`：`PXE_DISP_FLAG_DRIVER_ACTIVE`（存在有效显卡驱动）、`PXE_DISP_FLAG_HW_2D`（硬件 2D 加速激活）、`PXE_DISP_FLAG_UNLIMITED_RES`（驱动已开启无人工上限模式）
    - `min_w` / `min_h`：固定从 `640x480` 起步（彻底移除 `320x200`）
    - `max_w` / `max_h`：当前显卡及系统内存允许的最大分辨率（回退无驱动模式时为 `1024x768`）
    - `current_w` / `current_h` / `current_bpp`：当前屏幕实际分辨率与色深
    - `vram_bytes`：显卡驱动精确探测到的显存字节数（如 ATI 2MB/4MB 或 VMware 实际分配显存）
    - `bpp_mask`：全局支持的色深掩码（`PXE_DISP_BPP_8` | `16` | `24` | `32`；无有效驱动回退时仅有 `8 | 16`）
    - `mode_count` 与 `modes[0..PXE_DISP_MAX_MODES-1]`：每个条目 `pxe_display_mode_t` 包含 `{ width, height, bpp_mask, preferred }`
    - `driver_name[48]`、`driver_status[48]`、`fallback_reason[64]`
  - `int pxe_display_set_mode(int width, int height, int bpp)` / `int pxe_display_apply_mode(int width, int height, int bpp)`：动态设置（并可选持久化保存）分辨率与色深（需要 `.pxi` 携带 `SYS_SETTINGS` 标志或以 Ring 0 运行）。固定 LFB 模式仅登记待切换目标，应用应先请求用户确认，再调用 `pxe_display_set_mode()` 与 `pxe_system_reboot()`。
  - `int pxe_system_reboot(void)`：在用户明确确认后重启系统（需要 `SYS_SETTINGS` 权限）。

### 3.2 窗口合成与图形界面（`<pxe/gui.h>`, `<pxe/gfx.h>`）

- **窗口生命周期**：
  - `int pxe_win_create(const pxe_win_desc_t* desc, pxe_win_surface_t* out_surface)`
  - `int pxe_win_destroy(int win_id)`
  - `int pxe_win_set_title(int win_id, const char* title)`
  - `int pxe_win_set_menu(int win_id, const pxe_menu_bar_t* menu_bar)`
  - `int pxe_win_get_surface(int win_id, pxe_win_surface_t* out_surface)`（收到 `PXE_EV_RESIZE` 后**务必调用**以刷新 `width`/`height`/`stride`）
  - `int pxe_win_present(int win_id)` / `int pxe_win_present_rect(int win_id, int x, int y, int w, int h)`
  - `int pxe_win_poll_event(int win_id, pxe_event_t* out_ev)` / `int pxe_win_wait_event(int win_id, pxe_event_t* out_ev)`
  - **键盘与中文输入法事件**：普通按键返回 `PXE_EV_KEY_DOWN` 并在 `ev.key` 中携带 ASCII 或 `PXE_KEY_*` 控制键；当内置拼音输入法上屏中文/标点时，窗口收到 `ev.type == PXE_EV_KEY_DOWN`、`ev.key == PXE_KEY_UTF8`（`0x1020`），上屏的 UTF-8 多字节文本位于 `ev.text[32]`。
  - **鼠标滚轮事件**：`PXE_EV_MOUSE_WHEEL`（`10`）仅在鼠标位于窗口客户区时发送；`ev.x`/`ev.y` 为客户区坐标，`ev.arg0` 为有符号滚轮增量，正值向上、负值向下。复用既有字段以保持 `pxe_event_t` ABI 大小不变。
  - `int pxe_clipboard_get(char* buf, size_t max_len)` / `int pxe_clipboard_set(const char* text)`、`void pxe_notify_banner(const char* message)`、`void pxe_beep(void)`
- **基础与控件绘图（同时兼容 `PXE_CANVAS_PAL8` 与 `PXE_CANVAS_RGB32`）**：
  - 像素与几何：`pxe_palette_to_rgb32`、`pxe_put_pixel`、`pxe_put_rgb32`、`pxe_fill_rect`、`pxe_draw_rect`、`pxe_draw_hline`、`pxe_draw_vline`
  - 字体与经典 UI 控件：`pxe_draw_char`、`pxe_draw_text`、`pxe_draw_text_small`、`pxe_text_width`、`pxe_text_width_small`、`pxe_draw_bevel_box`、`pxe_draw_button`、`pxe_draw_scrollbar`、`pxe_point_in_rect`

### 3.3 内存、字符串、文件系统与音频

- **动态内存与字符串（`<pxe/mem.h>`, `<pxe/str.h>`）**：`pxe_sbrk`、`malloc`、`calloc`、`realloc`、`free`、`memcpy`、`memmove`、`memset`、`memcmp`、`strlen`/`strcpy`/`strncpy`/`strcat`/`strcmp`/`strncmp`/`strcasecmp`/`strchr`/`strrchr`/`snprintf`/`vsnprintf`。
- **UTF-8 / UTF-16 文本处理（`<pxe/str.h>`，`v26A280a` 新增）**：
  - `utf8_decode_next`、`utf8_encode_one`、`utf8_prev_char_bytes`、`utf8_next_char_bytes`、`utf8_clip_by_pixels`、`utf8_validate`
  - `utf16le_to_utf8`、`utf8_to_utf16le`、`ucs2be_to_utf8`（供 VFS 长文件名与外部文本转换使用）
- **虚拟文件系统（`<pxe/fs.h>`）**：使用 Windows 风格盘符路径（如 `"C:\\TEST.TXT"`）：
  - `pxe_open`、`pxe_read`、`pxe_seek`、`pxe_close`、`pxe_stat`、`pxe_read_file`、`pxe_write_file`、`pxe_copy_file`、`pxe_listdir`、`pxe_mkdir`、`pxe_unlink`、`pxe_rename`、`pxe_get_drives`、`pxe_eject_drive`。
- **音频控制与流式输入（`<pxe/audio.h>`）**：
  - `pxe_audio_play_file`、`pxe_audio_pause`、`pxe_audio_resume`、`pxe_audio_stop`、`pxe_audio_seek`、`pxe_audio_set_volume`、`pxe_audio_toggle_mute`、`pxe_audio_get_status`、`pxe_audio_feed_pcm`。

### 3.4 Ring 0 专属硬件与系统接口（`<pxe/ring0.h>`，仅 `PXE_RING = 0`）

- 进程管理：`pxe_r0_enum_processes`、`pxe_r0_kill_process`、`pxe_r0_focus_process_window`、`pxe_r0_get_memory_summary`
- 硬件与驱动管理：`pxe_r0_enum_pci_devices`、`pxe_r0_enum_usb_devices`、`pxe_r0_enum_pdv_drivers`、`pxe_r0_load_pdv_file`、`pxe_r0_rescan_pdevice_dir`
- 存储与电源控制：`pxe_r0_enum_disks_and_parts`、`pxe_r0_format_partition`、`pxe_r0_raw_disk_rw`、`pxe_r0_vfs_remount_all`、`pxe_r0_sync_filesystems`、`pxe_r0_init_uninit_disk`、`pxe_r0_power_control`

---

## 四、 开发示例：创建并集成一个 GUI 应用

### 4.1 最小 GUI 应用（带动态显示信息与调整大小支持）

```c
#include <pxe/sys.h>
#include <pxe/gui.h>
#include <pxe/gfx.h>
#include <pxe/str.h>

static void render(const pxe_win_surface_t* surf, const pxe_display_caps_t* caps) {
    char line[96];
    pxe_fill_rect(surf, 0, 0, surf->width, surf->height, PXE_COL_LIGHT_GRAY);
    pxe_draw_bevel_box(surf, 8, 8, surf->width - 16, surf->height - 16, 1, PXE_COL_WHITE);

    snprintf(line, sizeof(line), "Driver: %s", caps->driver_name[0] ? caps->driver_name : "Fallback");
    pxe_draw_text(surf, 16, 18, line, PXE_COL_BLACK);

    snprintf(line, sizeof(line), "Cur: %ux%ux%u  Max: %ux%u  Modes: %u",
             caps->current_w, caps->current_h, caps->current_bpp,
             caps->max_w, caps->max_h, caps->mode_count);
    pxe_draw_text(surf, 16, 38, line, PXE_COL_BLUE);

    snprintf(line, sizeof(line), "VRAM: %u KB  Status: %s",
             (unsigned int)(caps->vram_bytes / 1024U),
             (caps->flags & PXE_DISP_FLAG_DRIVER_ACTIVE) ? "Active Driver" : "Fallback (8/16b <=1024x768)");
    pxe_draw_text(surf, 16, 58, line, PXE_COL_BLACK);
}

int main(void) {
    pxe_win_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.title = "Display Caps Demo";
    desc.w = 380;
    desc.h = 160;
    desc.min_w = 280;
    desc.min_h = 120;
    desc.flags = PXE_WIN_RESIZABLE;
    desc.format = PXE_CANVAS_PAL8; /* 可选 PXE_CANVAS_PAL8 或 PXE_CANVAS_RGB32 */

    pxe_win_surface_t surf;
    int win = pxe_win_create(&desc, &surf);
    if (win < 0) return 1;

    pxe_display_caps_t caps;
    pxe_display_get_caps(&caps);

    render(&surf, &caps);
    pxe_win_present(win);

    pxe_event_t ev;
    while (pxe_win_wait_event(win, &ev) >= 0) {
        if (ev.type == PXE_EV_CLOSE) break;
        if (ev.type == PXE_EV_RESIZE) {
            pxe_win_get_surface(win, &surf);
            pxe_display_get_caps(&caps);
            render(&surf, &caps);
            pxe_win_present(win);
        } else if (ev.type == PXE_EV_REDRAW) {
            render(&surf, &caps);
            pxe_win_present(win);
        }
    }
    pxe_win_destroy(win);
    return 0;
}
```

### 4.2 构建 `Makefile`

```makefile
APP_NAME := myapp
APP_SRCS := myapp.c
APP_PXI  := myapp.pxi
PXE_RING := 3
SDK_DIR  := ../../sdk
include $(SDK_DIR)/pxe_app.mk
```

- 独立开发目录运行 `make`（首次构建可先确保 `sdk_libs` 已编译）即生成 `build/myapp.pxe`。
- 如需编译为系统内置应用并进入 ISO 镜像的 `APPS\` 目录：
  1. 将源码与 `Makefile`、`.pxi` 放在 `pixel-neko-os/apps/myapp/`；
  2. 在 [Makefile](../Makefile) 的 `APPS_PXE_NAMES` 中追加 `myapp`；
  3. 在 [scripts/make_iso.sh](../scripts/make_iso.sh) 的 `REQUIRED_APPS` 中追加 `myapp`；
  4. 参考完整内置示例：[apps/cpanel/cpanel.c](../apps/cpanel/cpanel.c)（Ring 3 + `SYS_SETTINGS` 动态分辨率与语言切换）、[apps/notepad/notepad.c](../apps/notepad/notepad.c)（UTF-8 中文编辑与 `PXE_KEY_UTF8` 接入）、[apps/sysinfo/sysinfo.c](../apps/sysinfo/sysinfo.c)（Ring 0 系统诊断）、[apps/taskmgr/taskmgr.c](../apps/taskmgr/taskmgr.c)（Ring 0 任务管理器）、[sdk/examples/hello_gui/hello.c](../sdk/examples/hello_gui/hello.c)、[sdk/examples/mini_taskmon/mini_taskmon.c](../sdk/examples/mini_taskmon/mini_taskmon.c)。

---

## 五、 简体中文、UTF-8 与拼音输入法开发指南（`v26A280a`）

### 5.1 字体渲染与像素宽度度量

- **内置字体与字库**：系统启动时加载 `system/FONT_CJK.FNT`，`pxe_draw_text()` 使用 **8×16 ASCII + 12×12 中日韩点阵字模**（中文单字宽 `12px`，英文半角宽 `8px`，行高建议 `16px`~`18px`）；`pxe_draw_text_small()` 在纯 ASCII 行使用 **5×7** 字模（宽 `6px`），若同一行混排中文等非 ASCII 字符，SDK 与内核字体引擎会自动对 ASCII 字符做 `+3px` 基线居中对齐，中文仍占 `12px` 宽。
- **严禁用 `strlen(s) * 8` 估算中文宽度**：由于 1 个 UTF-8 汉字占 3 字节但仅占 `12px`（而非 `24px`），必须改用像素度量与按码点截断工具：
  - `int pxe_text_width(const char* text)` / `int pxe_text_width_small(const char* text)`
  - `void utf8_clip_by_pixels(const char* src, int max_pixels, int is_small_font, char* dst, size_t dst_cap)`（保证在完整 UTF-8 字符边界处截断，绝不切碎多字节序列导致乱码）。

### 5.2 接入拼音输入法（`PXE_KEY_UTF8`）与光标编辑

系统窗口管理器（`wm/ime.c`）内置与 Windows 7 一致的 **`Ctrl + Shift`** 快捷键以及顶部状态栏 `[中]`/`[EN]` 切换按钮，词库 `system/IME.PNY` 由 [scripts/gen_ime_dict.py](../scripts/gen_ime_dict.py) 基于 Rime 朙月拼音/八股文词库预先构建，支持全拼、简拼、词组与整句联想。应用无需手动实现输入法，只需正确处理 `PXE_KEY_UTF8` 事件与 UTF-8 光标移动：

```c
if (ev.type == PXE_EV_KEY_DOWN) {
    if (ev.key == PXE_KEY_BACKSPACE && len > 0) {
        /* 关键：按完整 UTF-8 字符退格，不能 len-- */
        int step = utf8_prev_char_bytes(buf, len);
        len -= step;
        buf[len] = '\0';
    } else if (ev.key == PXE_KEY_UTF8 && ev.text[0]) {
        /* 输入法上屏的中文或中文标点（UTF-8） */
        int n = (int)strlen(ev.text);
        if (len + n < (int)sizeof(buf)) {
            memcpy(buf + len, ev.text, (size_t)n);
            len += n;
            buf[len] = '\0';
        }
    } else if (ev.key >= 32 && ev.key <= 126) {
        if (len + 1 < (int)sizeof(buf)) {
            buf[len++] = (char)ev.key;
            buf[len] = '\0';
        }
    }
}
```

### 5.3 双语界面适配（简体中文 / English）与 ABI 缓冲区限制

1. **启动与运行时同步**：在 `main()` 初始化时调用 `int zh = pxe_is_zh();`（系统默认即为简体中文 `1`）。用户在控制面板切换语言后，内核会向所有窗口投递 `PXE_EV_REDRAW`；在事件循环收到 `PXE_EV_REDRAW` 时重新比对 `pxe_is_zh()`，若发生变化则调用 `pxe_win_set_title()` 和 `pxe_win_set_menu()` 更新标题栏与菜单栏，并重绘界面。
2. **注意 ABI 结构体字节长度上限**（UTF-8 中 1 个汉字占 **3 字节**，编译与赋值前请留足 `\0` 结尾）：
   - `pxe_win_desc_t.title[48]`（约 15 个汉字以内）
   - `pxe_menu_t.title[16]`（顶部菜单名，最多 **5 个汉字**）
   - `pxe_menu_item_t.label[24]`（菜单项标签，最多 **7 个汉字**）
   - `*.pxi` 清单中的静态窗口标题同样受编译期打包检查约束，建议在程序 `main()` 内根据 `pxe_is_zh()` 动态设置窗口标题以支持双语热切换。
3. **第三方词库声明**：拼音词库转换与引擎构建规范见 [THIRD_PARTY_LICENSES.md](../THIRD_PARTY_LICENSES.md)。
