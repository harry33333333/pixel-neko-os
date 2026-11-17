# Pixel Neko OS 显卡与声卡 `*.pdv` 硬件驱动开发指南（PDV v1.0 / `v26A232a`）

在当前 `main` 主干（基准版本 **`v26A232a`**）中，Pixel Neko OS 采用**原生自包含可加载驱动体系（`*.pdv`, Pixel Driver v1.0）**。所有可插拔硬件驱动（显卡、音频、磁盘控制器等）均以独立模块封装，并在内核启动阶段或运行时按硬件匹配表动态重定位加载。

本文档重点说明 Pixel Neko OS 的 PDV 驱动架构，包括：
1. **显卡 `*.pdv` 规范**：动态分辨率、色深（`8/16/24/32-bit`）、真实显存（VRAM）精确探测（含 `16–32 MB` 显存支持 `1920x1080@32-bit` 的单缓冲计算说明、VMware SVGA II `SVGA_REG_MEMORY_SIZE` / SVGA3D Screen Target `STDU` 与 2D `BAR1` 阈值修正）与 `1024x768@16b` 无有效驱动回退规范；
2. **声卡 `*.pdv` 规范**：`PDV_CLASS_AUDIO` / `pdv_audio_driver_t` 接口契约、`64 KB` 双半区 DMA 环形缓冲、PCI IRQ 中断续传（`pci_register_irq` + `audio_handle_dma_half_irq`）、暂停/恢复时序同步与无卡顿轮询约束。

---

## 一、 核心架构与规范（`v26A232a`）

### 1.1 `*.pdv` 打包与按需自动加载（`PDEVICE\`）

- **二进制封装（`PDV1`）**：由 [scripts/mkpdv.py](../scripts/mkpdv.py) 根据声明式清单（`*.pdi`）与可重定位 ELF64 目标文件（`ET_REL`）封装生成，格式定义见 [sdk/ddk/include/pdv/format.h](../sdk/ddk/include/pdv/format.h)：
  - 128 字节 `pdv_header_t`（魔数 `"PDV1"`、`format_version = 0x0100U`、`min_ddk_version = 0x0100U`、`driver_class`、`flags`、IEEE 802.3 CRC32 校验）。
  - 若干条 16 字节 `pdv_match_rule_t` 匹配规则（支持 `PDV_MATCH_PCI_ID`、`PDV_MATCH_PCI_SUBSYS`、`PDV_MATCH_PCI_CLASS`、`PDV_MATCH_PCI_VEN_CLASS`、`PDV_MATCH_ISA_PNP`、`PDV_MATCH_ANY`）。
  - 按 4096 字节页对齐嵌入的 `ELF64 ET_REL` 载荷。
- **部署路径**：
  - Live CD：`<CD>:\PDEVICE\*.pdv`
  - 已安装系统：`<SYS>:\PDEVICE\*.pdv`
- **加载流程与幂等去重**：内核在 [kernel/pdv.c](../kernel/pdv.c) 中扫描 PCI 设备并匹配 `PDEVICE\` 下的 `.pdv` 文件，校验 CRC32 与 `ddk_version` 后将其动态重定位映射到高半区内核地址空间执行 `pdv_driver_entry()`；进入桌面后亦可通过 **系统信息（`sysinfo.pxe`）** 手动加载 `.pdv` 或重新扫描 `PDEVICE\` 目录。自 `v26A222a` 起，内核加载器按模块名与基文件名自动跳过已激活模块，显示/音频与通用驱动注册表亦按驱动名去重，多次执行 **Rescan `PDEVICE\`** 不会产生重复条目。

### 1.2 DDK 隔离原则（`pdv_kernel_api_t`）

- `.pdv` 模块编译为 freestanding ELF64 可重定位对象（`-mcmodel=kernel -mno-red-zone -mgeneral-regs-only -fno-pie -fno-pic`），**严禁直接引用或链接内核私有全局变量与未导出符号**。
- 与内核的全部交互必须通过入口函数收到的 `const pdv_kernel_api_t* kapi`（定义于 [sdk/ddk/include/pdv/ddk.h](../sdk/ddk/include/pdv/ddk.h)，内核装配表位于 [kernel/pdv.c](../kernel/pdv.c)）完成：
  - **日志与内存**：`log_printf`、`kzalloc`、`kfree`、`dma_alloc`、`dma_free`、`dma_sync_for_cpu`、`dma_sync_for_device`、`isadma_*`
  - **MMIO 与端口 I/O**：`mmio_map_uc`、`mmio_map_wc`、`inb`/`outb`、`inw`/`outw`、`inl`/`outl`（以及 DDK 内联函数 `pdv_mmio_read8/16/32`、`pdv_mmio_write8/16/32`、`pdv_outb/inb/outw/inw/outl/inl`）
  - **PCI 总线与中断**：`pci_get_device_count`、`pci_get_device`、`pci_find_by_id`、`pci_find_class`、`pci_config_read`、`pci_config_write`、`pci_enable_io`、`pci_enable_memory`、`pci_enable_bus_master`、`pci_get_bar_size`、`pci_get_bar_address`、`pci_find_capability`、`pci_register_irq`
  - **时序与定时器**：`sleep_ms`、`delay_ms`、`timer_get_ticks`、`boot_screen_tick`
  - **帧缓冲与子系统注册**：`fb_get_surface`、`fb_set_surface`、`fb_get_desktop_mode`、`fb_get_lfb`、`fb_flush`、`register_display_driver`、`register_audio_driver`、`audio_set_probe_status`、`audio_handle_dma_half_irq`、`block_register`、`block_unregister`、`block_set_capacity`、`register_driver_descriptor`
  - 注：`fb_apply_ati_cap` 仅作历史 ABI 兼容保留（内核现为空实现），显卡显存容量与分辨率裁决已完全交由显卡驱动本身负责。

### 1.3 显示模式、色深、显存与回退强制规范

自 **`v26A220a`** 起，**分辨率列表与色深支持不再硬编码于内核（`gfx_accel.c`）或应用（`cpanel.pxe`）中，而由激活的显卡驱动 `pdv_display_driver_t` 动态提供**。所有显卡驱动与图形消费者均须遵守以下约束：

| 规则维度 | 规范要求 |
| :--- | :--- |
| **最低分辨率** | 严格从 **`640x480`** 起步（`PDV_DISPLAY_MIN_WIDTH = 640`, `PDV_DISPLAY_MIN_HEIGHT = 480`）。`320x200` 及所有低于 `640x480` 的模式已彻底废弃。 |
| **最高分辨率** | **不设人工分辨率上限**。只要显卡驱动激活且硬件上限、已探测显存（VRAM）及内核 PMM 后备缓冲内存均满足需求，即可支持 `1280x1024`、`1600x1200`、`1920x1080` 及更高分辨率。 |
| **允许色深** | **仅限 `8`、`16`、`24`、`32` 位色**（对应掩码 `PDV_BPP_MASK_8`、`PDV_BPP_MASK_16`、`PDV_BPP_MASK_24`、`PDV_BPP_MASK_32`，标准组合掩码 `PDV_BPP_MASK_ALL_STD`）。**不得**暴露或接受 `15`-bit 模式。 |
| **无有效驱动回退** | 若无任何 `.pdv` 显卡驱动激活（`gfx_accel_has_valid_driver() == 0`，如处于 `nopdv=1` 安全模式或未知显卡回退 Limine LFB），内核强制限制最高模式为 **`1024x768 @ 16-bit`**（`PDV_FALLBACK_MAX_WIDTH = 1024`, `PDV_FALLBACK_MAX_HEIGHT = 768`, `PDV_FALLBACK_MAX_BPP = 16`），色深仅允许 **`8` 位与 `16` 位**（**不支持 `24`/`32` 位**）。 |
| **显存容量校验** | 显卡驱动必须真实、准确探测物理/虚拟 VRAM 与 PCI Aperture 尺寸，`can_set_mode()` / `supports_bpp()` / `get_mode()` 必须对超出可用显存的模式返回拒绝，严禁让超出显存的分辨率/色深组合通过探测。 |

---

## 二、 显示驱动核心接口（`pdv_display_driver_t`）

定义位于 [sdk/ddk/include/pdv/ddk.h](../sdk/ddk/include/pdv/ddk.h)：

### 2.1 模式描述与辅助定义

```c
#define PDV_DISPLAY_MIN_WIDTH   640
#define PDV_DISPLAY_MIN_HEIGHT  480
#define PDV_FALLBACK_MAX_WIDTH  1024
#define PDV_FALLBACK_MAX_HEIGHT 768
#define PDV_FALLBACK_MAX_BPP    16

#define PDV_BPP_MASK_8          (1u << 0)
#define PDV_BPP_MASK_16         (1u << 1)
#define PDV_BPP_MASK_24         (1u << 2)
#define PDV_BPP_MASK_32         (1u << 3)
#define PDV_BPP_MASK_ALL_STD    (PDV_BPP_MASK_8 | PDV_BPP_MASK_16 | PDV_BPP_MASK_24 | PDV_BPP_MASK_32)

static inline uint8_t pdv_bpp_to_mask(int bpp);

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  bpp_mask;   /* PDV_BPP_MASK_* 的按位或，限定该分辨率允许的色深 */
    uint8_t  preferred;  /* 1 表示当前硬件/引导原生或首选模式 */
    uint16_t reserved;
} pdv_display_mode_t;
```

### 2.2 `pdv_display_driver_t` 回调表

```c
typedef struct {
    const char*    name;
    int            priority;             /* 优先级（如 90~100；多驱动匹配时取高者） */
    int            requires_hw_mode_set; /* 1=驱动可编程切换硬件时序; 0=仅接管引导帧缓冲 LFB */
    int            (*probe)(void);
    int            (*start)(void);
    void           (*stop)(void);
    const char*    (*get_name)(void);
    const char*    (*get_status)(void);
    int            (*is_active)(void);
    int            (*is_2d_active)(void);
    unsigned int   (*get_vram_size)(void);
    int            (*can_set_mode)(int w, int h, int bpp);
    int            (*supports_bpp)(int w, int h, int bpp);
    int            (*set_mode)(int w, int h, int bpp);
    void           (*fill_rect)(int x, int y, int w, int h, unsigned int color);
    void           (*screen_blt)(int sx, int sy, int dx, int dy, int w, int h);
    void           (*present_rect)(int x, int y, int w, int h);
    void           (*sync)(void);
    int            (*has_hw_cursor)(void);
    int            (*set_cursor)(int x, int y, int visible);

    /* v26A220a 新增：动态显示能力枚举（不实现时由内核按 fallback/合成逻辑兜底，但强烈建议实现） */
    int            (*get_mode_count)(void);
    int            (*get_mode)(int index, pdv_display_mode_t* out_mode); /* 成功返回 1，越界返回 0 */
    int            (*get_max_width)(void);
    int            (*get_max_height)(void);
} pdv_display_driver_t;
```

### 2.3 内核侧协调流程（`gfx_accel.c`）

1. `gfx_accel_init()` 按 `priority` 从高到低尝试已注册驱动的 `probe()`。若成功，设该驱动为当前有效显卡驱动；若失败或未注册，则停留在回退模式（`gfx_accel_has_valid_driver() == 0`）。
2. 当驱动提供 `get_mode_count()` 与 `get_mode()` 时，内核直接采集驱动上报的模式表，过滤掉 `< 640x480` 或无合法 `8/16/24/32` 掩码的项，同时叠加系统物理内存预算检查（`ui_can_allocate_resolution(w, h, bpp)`）。
3. 应用层（如控制面板 `cpanel.pxe`、系统信息 `sysinfo.pxe`）通过系统调用 `NEKO_SYSCALL_GET_DISP_CAPS` / `pxe_display_get_caps()` 获取**与当前驱动实时一致**的分辨率列表、每模式允许的 `bpp_mask`、VRAM 容量、最大分辨率上限与驱动激活状态。
4. 在 `8-bit`（256 色）模式下，内核已统一 `6x6x6 + 16` 色系统调色板映射；若显卡具备硬件 CLUT/DAC（如 VMware `SVGA_PALETTE_BASE`），驱动需在 `set_mode(..., 8)` 时写入调色板；若仅工作在引导帧缓冲上（如 `boot_framebuffer` 软件转译路径），内核负责将系统 8-bit 索引缓冲精确 blit 至目标像素格式。

---

## 三、 从零编写一个新的显卡驱动

以开发 `drivers/pdv/mygpu/` 为例，只需准备清单文件 `mygpu.pdi` 与源码 `mygpu.c`（以及可选的独立 `Makefile`）。

### 步骤 1：编写硬件匹配清单（`mygpu.pdi`）

```ini
[driver]
name        = mygpu
provider    = Your Name / Vendor
class       = DISPLAY
version     = 1.0.0
ddk_version = 0x0100
start       = BOOT
safe_mode   = false

[match]
# 方式 A：精确匹配 PCI Vendor ID 与 Device ID
pci = ven=0x8086, dev=0x0166

# 方式 B：支持同系列多个设备 ID
pci = ven=0x8086, dev=0x0152

# 方式 C：按厂商 + PCI 显示类通配 (0x03 为 Display Controller)
# pci = ven=0x8086, class=0x03, subclass=*
```

> 安全提示：保持 `safe_mode = false`，这样当新驱动调试异常时，用户可在 Limine 启动菜单选择 `Safe Mode (Skip PDEVICE Drivers)`（传递 `nopdv=1`）跳过所有 `.pdv` 加载并进入 `1024x768@16b` 回退桌面。

### 步骤 2：编写驱动源码（`mygpu.c`）

```c
#include <pdv/ddk.h>

static const pdv_kernel_api_t* g_kapi = NULL;
static const pdv_pci_device_t* g_pci_dev = NULL;
static int g_active = 0;

static unsigned int g_vram_bytes = 0;
static volatile unsigned char* g_mmio_base = NULL;
static volatile unsigned char* g_fb_virt = NULL;
static uint64_t g_fb_phys = 0;

static int g_max_w = 1920, g_max_h = 1200;
static int g_cur_w = 1024, g_cur_h = 768, g_cur_bpp = 32;

#define MAX_MYGPU_MODES 24
static pdv_display_mode_t g_modes[MAX_MYGPU_MODES];
static int g_mode_count = 0;

#define klog(fmt, ...) g_kapi->log_printf("[MYGPU] " fmt "\n", ##__VA_ARGS__)

static int mygpu_supports_bpp(int w, int h, int bpp) {
    if (!g_active && g_vram_bytes == 0) return 0;
    if (w < PDV_DISPLAY_MIN_WIDTH || h < PDV_DISPLAY_MIN_HEIGHT) return 0;
    if (w > g_max_w || h > g_max_h) return 0;
    if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return 0;
    uint64_t need = (uint64_t)w * (uint64_t)h * (uint64_t)(bpp / 8);
    return (need <= (uint64_t)g_vram_bytes) ? 1 : 0;
}

static uint8_t mygpu_calc_bpp_mask(int w, int h) {
    uint8_t mask = 0;
    const int bpps[4] = { 8, 16, 24, 32 };
    for (int i = 0; i < 4; i++) {
        if (mygpu_supports_bpp(w, h, bpps[i])) {
            mask |= pdv_bpp_to_mask(bpps[i]);
        }
    }
    return mask;
}

static void mygpu_rebuild_modes(void) {
    static const uint16_t k_cand_w[] = {
        640, 800, 1024, 1152, 1280, 1280, 1280, 1280,
        1366, 1400, 1440, 1600, 1600, 1680, 1920, 1920, 2560
    };
    static const uint16_t k_cand_h[] = {
        480, 600, 768, 864, 720, 800, 960, 1024,
        768, 1050, 900, 900, 1200, 1050, 1080, 1200, 1440
    };
    int n = (int)(sizeof(k_cand_w) / sizeof(k_cand_w[0]));
    g_mode_count = 0;
    for (int i = 0; i < n && g_mode_count < MAX_MYGPU_MODES; i++) {
        int w = k_cand_w[i];
        int h = k_cand_h[i];
        if (w < PDV_DISPLAY_MIN_WIDTH || h < PDV_DISPLAY_MIN_HEIGHT) continue;
        if (w > g_max_w || h > g_max_h) continue;
        uint8_t mask = mygpu_calc_bpp_mask(w, h);
        if (mask == 0) continue;
        g_modes[g_mode_count].width     = (uint16_t)w;
        g_modes[g_mode_count].height    = (uint16_t)h;
        g_modes[g_mode_count].bpp_mask  = mask;
        g_modes[g_mode_count].preferred = (w == g_cur_w && h == g_cur_h) ? 1 : 0;
        g_modes[g_mode_count].reserved  = 0;
        g_mode_count++;
    }
}

static int mygpu_probe(void) {
    g_active = 0;
    for (int i = 0; i < g_kapi->pci_get_device_count(); i++) {
        const pdv_pci_device_t* dev = g_kapi->pci_get_device(i);
        if (!dev || dev->vendor != 0x8086 || dev->class_code != 0x03) continue;

        g_kapi->pci_enable_memory(dev);
        g_kapi->pci_enable_bus_master(dev);
        g_pci_dev = dev;

        if (dev->bar[0].valid && !dev->bar[0].is_io) {
            g_mmio_base = (volatile unsigned char*)g_kapi->mmio_map_uc(
                dev->bar[0].address, dev->bar[0].size);
        }
        if (dev->bar[1].valid && !dev->bar[1].is_io) {
            g_fb_phys = dev->bar[1].address;
            g_vram_bytes = (unsigned int)dev->bar[1].size;
            g_fb_virt = (volatile unsigned char*)g_kapi->mmio_map_wc(
                g_fb_phys, g_vram_bytes);
        }
        if (!g_fb_virt || g_vram_bytes < 640U * 480U) continue;

        /* TODO: 从硬件寄存器读取实际 VRAM、MAX_WIDTH、MAX_HEIGHT 并设置 g_max_w/g_max_h */
        pdv_fb_surface_t surf;
        g_kapi->fb_get_surface(&surf);
        g_cur_w   = (surf.width  >= PDV_DISPLAY_MIN_WIDTH)  ? (int)surf.width  : 1024;
        g_cur_h   = (surf.height >= PDV_DISPLAY_MIN_HEIGHT) ? (int)surf.height : 768;
        g_cur_bpp = (surf.bpp == 8 || surf.bpp == 16 || surf.bpp == 24 || surf.bpp == 32) ? (int)surf.bpp : 32;

        mygpu_rebuild_modes();
        if (g_mode_count == 0) continue;

        g_active = 1;
        klog("Ready: %04x:%04x VRAM=%u KB Modes=%d Max=%dx%d",
             dev->vendor, dev->device, g_vram_bytes / 1024U, g_mode_count, g_max_w, g_max_h);
        return 1;
    }
    return 0;
}

static int mygpu_can_set_mode(int w, int h, int bpp) {
    if (!g_active) return 0;
    return mygpu_supports_bpp(w, h, bpp);
}

static int mygpu_set_mode(int w, int h, int bpp) {
    if (!mygpu_can_set_mode(w, h, bpp)) return 0;

    /* 1. 下发硬件寄存器/FIFO 设置宽、高、色深、Pitch 与 8-bit 调色板 */
    g_cur_w = w;
    g_cur_h = h;
    g_cur_bpp = bpp;

    /* 2. 更新内核全局帧缓冲描述 */
    pdv_fb_surface_t surf;
    g_kapi->fb_get_surface(&surf);
    surf.framebuffer_phys = g_fb_phys;
    surf.framebuffer_virt = (void*)g_fb_virt;
    surf.width  = (uint32_t)w;
    surf.height = (uint32_t)h;
    surf.bpp    = (uint32_t)bpp;
    surf.pitch  = (uint32_t)(w * (bpp / 8));
    surf.format = (bpp == 8)  ? PDV_PIXEL_FORMAT_PAL8
                : (bpp == 16) ? PDV_PIXEL_FORMAT_RGB565
                              : PDV_PIXEL_FORMAT_RGB888;
    g_kapi->fb_set_surface(&surf, (unsigned char*)g_fb_virt);
    return 1;
}

static int mygpu_get_mode_count(void) { return g_active ? g_mode_count : 0; }
static int mygpu_get_mode(int index, pdv_display_mode_t* out) {
    if (!g_active || !out || index < 0 || index >= g_mode_count) return 0;
    *out = g_modes[index];
    return 1;
}
static int mygpu_get_max_width(void)  { return g_active ? g_max_w : 0; }
static int mygpu_get_max_height(void) { return g_active ? g_max_h : 0; }

static void mygpu_sync(void) { g_kapi->fb_flush(); }

static const pdv_display_driver_t g_mygpu_drv = {
    .name                 = "My Custom GPU Driver",
    .priority             = 90,
    .requires_hw_mode_set = 1,
    .probe                = mygpu_probe,
    .start                = 0,
    .stop                 = 0,
    .get_name             = 0,
    .get_status           = 0,
    .is_active            = 0,
    .is_2d_active         = 0,
    .get_vram_size        = 0,
    .can_set_mode         = mygpu_can_set_mode,
    .supports_bpp         = mygpu_supports_bpp,
    .set_mode             = mygpu_set_mode,
    .fill_rect            = 0,
    .screen_blt           = 0,
    .present_rect         = 0,
    .sync                 = mygpu_sync,
    .has_hw_cursor        = 0,
    .set_cursor           = 0,
    .get_mode_count       = mygpu_get_mode_count,
    .get_mode             = mygpu_get_mode,
    .get_max_width        = mygpu_get_max_width,
    .get_max_height       = mygpu_get_max_height,
};

int pdv_driver_entry(const pdv_kernel_api_t* kapi) {
    if (!kapi || kapi->ddk_version < PDV_DDK_VERSION_1_0) return -1;
    g_kapi = kapi;
    return kapi->register_display_driver(&g_mygpu_drv);
}
```

---

## 四、 显存探测与主流显卡实现参考

### 4.1 真实显存探测的关键陷阱（以 ATI Mach64 / 3D Rage II+DVD 为例）

老式 PCI 显卡（如 ATI Mach64 / 3D Rage II / II+DVD，`2MB` / `4MB` 等）在 PCI BAR 上往往固定声明一个 `8MB` 或 `16MB` 的线性帧缓冲窗口（Aperture），不能直接拿 `bar[0].size` 当作实际物理显存！参考 [drivers/pdv/mach64/mach64.c](../drivers/pdv/mach64/mach64.c)：

1. **优先读取配置寄存器**：同时通过 BAR2（MMIO `+0x000`/`+0x400`）或 PCI I/O BAR 读取 `MEM_CNTL (0x00B0)` 低 3 位硬件内存表（`512KB / 1MB / 2MB / 4MB / 6MB / 8MB / 12MB / 16MB`）以及 `CNFG_CNTL (0x00DC)` 的 Aperture 大小（`4MB` / `8MB`）。
2. **禁止使用 WC 或单一整兆偏移回环探测**：
   - 在 4MB 小端+大端双窗口卡上，`bar0 + 0x400000 (4MB)` 可能命中小端帧缓冲的大端镜像（Big-Endian Mirror Alias），若在此处盲写/回读，2MB 卡容易误判为 4MB 或 8MB！
   - 正确做法：使用 **Uncached（`kapi->mmio_map_uc`）** 仅映射检测所需的单个 4KB 页，在 `0x081000`（512K）、`0x101000`（1M）、`0x201000`（2M）、`0x301000`（3M）、`0x401000`（4M）、`0x601000`（6M）等带页内偏移且绝不跨越 `CNFG_CNTL` Aperture 上限的地址写入双魔数与字节序敏感校验字并 `volatile` 读回、立即恢复原值，从而精确区分 `2MB` 与 `4MB` 等配置。
3. **基于有效显存生成动态模式表**：将算出的真实 VRAM 与引导表面尺寸共同作为约束，只在驱动内部暴露真正放得下的分辨率 + 色深组合（例如 2MB 卡上仅允许能完整容纳单屏 LFB 的组合，杜绝花屏/越界写）。

### 4.2 全功能虚拟显卡（参考 [drivers/pdv/vmwgfx/vmwgfx.c](../drivers/pdv/vmwgfx/vmwgfx.c)）

- **避免 `SVGA_REG_FB_SIZE` 误锁显存上限（`v26A222a` 修复）**：`SVGA_REG_FB_SIZE (16)` 返回的是**当前激活扫描输出缓冲**字节数（例如 `800x600@16bpp` 时仅为约 `940 KB`），并非显卡总可用显存或孔径上限！**切勿**将 `SVGA_REG_FB_SIZE` 纳入探测期的 `svga_effective_fb_limit()`（`min(eff_vram, bar1)` 即可）。
- **精准识别 VMware SVGA II / SVGA3D 显存与能力上限**：
  1. 先向 `SVGA_REG_GUEST_ID (23)` 写入 `GUEST_OS_OTHER (0x5010U)`，再读取 `SVGA_REG_CAPABILITIES (17)`。
  2. 读取 `SVGA_REG_VRAM_SIZE (15)`（`raw_vram`）与 `BAR1.size`；若能力位包含 `SVGA_CAP_GMR (0x00100000U)` / `SVGA_CAP_GMR2 (0x00400000U)` / `SVGA_CAP_GBOBJECTS (0x08000000U)`，继续读取 `SVGA_REG_MEMORY_SIZE (47)`。在 2D 模式下若 `BAR1` 拥有充足孔径（如 `128 MiB`）而 `raw_vram` 被报告为 `4096 KB`，则以 `min(mem_sz, BAR1)` 或实际 `BAR1` 可用范围放宽 2D 模式容量判断，不再被 `raw_vram = 4096 KB` 限制死在 `1152x864`。
  3. 当有效显存分别达到 `4 MiB` / `16 MiB` / `32 MiB` 且硬件报告的 `MAX_WIDTH x MAX_HEIGHT` 仍停留在低值（如 `1176x885`）时，允许探测上限按显存梯度解锁至 `1920x1200` / `2560x1600` / `3840x2160`，同时在 `set_mode()` 写寄存器后严密回读校验 `SVGA_REG_ENABLE != 0`、`SVGA_REG_WIDTH == width`、`SVGA_REG_HEIGHT == height`、`SVGA_REG_FB_SIZE >= cand_pitch * height` 及 `cand_off + fb_need <= BAR1`，若宿主机拒绝或截断则安全回滚。
  4. **SVGA3D Screen Target（`STDU`）与 Guest-Backed Objects（`v26A225e`）**：在启用 3D Screen Target（`SVGA_CAP_GBOBJECTS` + `SVGA_CAP_DX` 等）的高分辨率模式时，使用扁平连续 DMA 内存搭配 `SVGA3dCmdDefineGBMob`（`ptDepth = 0`）、`SVGA3dCmdSetOTableBase`、`SVGA3dCmdDefineGBSurface`、`SVGA3dCmdDefineGBScreenTarget`、`SVGA3dCmdBindGBScreenTarget` 与 `SVGA3dCmdUpdateGBImage` 序列完成像素提交，确保超过 `1152x864` 切换到 `1280x720@32` 等高分辨率时正常亮屏输出。
- **切勿硬编码上限拦截启动分辨率**：Limine 菜单或保存配置可能以高于 `1024` 的宽度引导。`probe()` 与 `set_mode()` 中不得人为截断宽度，须下发真实 SVGA 寄存器切换（写 `ENABLE=0` -> `WIDTH/HEIGHT/BITS_PER_PIXEL/DEPTH` -> `ENABLE=1` -> 重读 `BYTES_PER_LINE/FB_START/FB_OFFSET/FB_SIZE` 与更新 `fb_set_surface`）。
- **8-bit 支持**：检查 `SVGA_CAP_8BIT_EMULATION` 能力位，写 `SVGA_PALETTE_BASE (1024)` 起 256 项颜色查找表（`16` 色标准 VGA + `6x6x6` 颜色立方）。

### 4.3 仅优化帧缓冲类显卡（参考 [drivers/pdv/nvg7x/nvg7x.c](../drivers/pdv/nvg7x/nvg7x.c)）

- 设置 `.requires_hw_mode_set = 0`。
- 通过显存 BAR / 设备 ID 表确定精确显存（如 `128MB` / `256MB`），使用 `kapi->mmio_map_wc()` 将显存重映射为 Write-Combining 显著加速写入。
- 在 `get_mode()`、`can_set_mode()`、`get_max_width()`、`get_max_height()` 中声明基于引导分辨率上限与 VRAM 的动态支持矩阵，`supports_bpp()` 允许 `8/16/24/32`。

### 4.4 显存容量需求与 `16–32 MB` 支持 `1080p @ 32-bit` 说明

Pixel Neko OS 的合成器后备缓冲、窗口离屏表面与工作台图层均分配在**系统主存（PMM）**，显卡显存（LFB / Aperture）仅需容纳**单帧扫描输出画面**（`need = width * height * (bpp / 8)`，见 [drivers/video/gfx_accel.c](../drivers/video/gfx_accel.c)）：

| 分辨率 | 8-bit (`1 B/px`) | 16-bit (`2 B/px`) | 24-bit (`3 B/px`) | 32-bit (`4 B/px`) | 最低显存建议 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `640x480` | `300 KiB` | `600 KiB` | `900 KiB` | `1.17 MiB` | `2 MiB` |
| `800x600` | `469 KiB` | `938 KiB` | `1.37 MiB` | `1.83 MiB` | `2 MiB` |
| `1024x768` | `768 KiB` | `1.50 MiB` | `2.25 MiB` | `3.00 MiB` | `2–4 MiB` |
| `1280x1024` | `1.25 MiB` | `2.50 MiB` | `3.75 MiB` | `5.00 MiB` | `8 MiB`（`16b` 仅需 `4 MiB`） |
| `1920x1080` (`1080p`) | `1.98 MiB` | `3.96 MiB` | `5.93 MiB` | **`7.91 MiB`** (`8,294,400 B`) | **`16–32 MiB`** 充裕支持 |

因此，**`16–32 MB` 显存（即便预留双缓冲/页对齐余量）在理论上与驱动实现上均可轻松支持 `1920x1080 @ 32-bit`**；仅 `4 MB`（`4,194,304 B`）显存上限下只能支持到 `1920x1080 @ 16-bit`（`4,147,200 B`）或 `1024x768 @ 32-bit`（`3,145,728 B`）。

---

## 五、 声卡（`Audio`）`*.pdv` 驱动开发规范（`PDV_CLASS_AUDIO`）

除显示与存储控制器外，声卡驱动同样以标准 **`*.pdv`** 模块形式构建并在 `PDEVICE\` 下按需加载（当前内置驱动包括 `hdaudio`、`ac97`、`ct4810`、`cmi8738`）。

### 5.1 `.pdi` 清单规范与类别声明

声卡驱动清单中必须将 `class` 设为 `AUDIO`（对应 [sdk/ddk/include/pdv/format.h](../sdk/ddk/include/pdv/format.h) 中的 `PDV_CLASS_AUDIO = 2U`），并按 PCI 类别或 Vendor/Device ID 编写匹配规则。参考 [drivers/pdv/hdaudio/hdaudio.pdi](../drivers/pdv/hdaudio/hdaudio.pdi)：

```ini
[driver]
name        = hdaudio
provider    = Pixel Neko OS
class       = AUDIO
version     = 1.0.0
ddk_version = 0x0100
start       = BOOT
safe_mode   = false

[match]
pci = class=0x04, subclass=0x03
pci = ven=0x15ad, dev=0x1977
```

- **常用 PCI 音频类编码**：`class=0x04, subclass=0x03`（High Definition Audio）、`class=0x04, subclass=0x01`（Multimedia Audio，如 AC'97 / Ensoniq / C-Media）。
- **`safe_mode = false`**：加入 `nopdv=1` 时内核不会加载声卡 `.pdv`。

### 5.2 核心接口：`pdv_audio_driver_t`（[sdk/ddk/include/pdv/ddk.h](../sdk/ddk/include/pdv/ddk.h)）

```c
typedef struct {
    const char*  name;
    int          priority;
    int          (*probe)(void);
    const char*  (*get_name)(void);
    int          (*is_present)(void);
    void         (*set_format)(int rate, int bits, int stereo);
    void         (*start_dma)(void);
    void         (*stop_dma)(void);
    void         (*pause_dma)(int pause);
    void         (*set_volume)(int master, int pcm);
    int          (*poll_half_needed)(void);
    void         (*write_half)(int half, const void* data, unsigned int bytes);
    void         (*silence_half)(int half);
    unsigned int (*get_dma_frame_pos)(void);
    const void*  (*get_dma_buffer)(void);
} pdv_audio_driver_t;
```

驱动入口 `pdv_driver_entry(const pdv_kernel_api_t* kapi)` 检查 `kapi->ddk_version >= PDV_DDK_VERSION_1_0` 后调用 `kapi->register_audio_driver(&g_audio_drv)`（内核按 `drv->name` 自动去重）。可选地通过 `kapi->audio_set_probe_status(status)` 上报探测状态字符串（用于桌面诊断面板）。

### 5.3 双半区 DMA 环形缓冲与成员语义

Kernel 音频子系统（[drivers/audio/audio.c](../drivers/audio/audio.c)）采用固定大小的双半区乒乓 DMA 环：

- **缓冲规格**：`AUDIO_DMA_BUFFER_BYTES = 65536`（`64 KiB`），拆分为 **`half = 0`** 与 **`half = 1`** 各 `AUDIO_DMA_HALF_BYTES = 32768`（`32 KiB`）。在 `48 kHz / 16-bit / 立体声`（`4 B/frame`）下，每半区为 `8192` 帧，约 **`170.67 ms`** 音频窗口。
- **DMA 内存分配与缓存同步**：
  - 使用 `pdv_dma_alloc_aligned(kapi, size, alignment)`（底层调用 `kapi->dma_alloc(..., PDV_DMA_BIT_MASK(32), ...)`）申请 BDL / CORB / RIRB 与 PCM 物理连续 DMA 缓冲，并用 `pdv_dma_is_valid(&buf)` 校验。
  - 更新 DMA 描述符、命令环或写入 PCM 后必须调用 `pdv_dma_flush(kapi, &buf)`（即 `kapi->dma_sync_for_device`）；读取硬件状态环前调用 `kapi->dma_sync_for_cpu`。

| 回调 | 规范要求与实现要点 |
| :--- | :--- |
| `probe()` | 探测 PCI 设备并启用 Bus Master/MMIO/IO（`pci_enable_bus_master` 等），复位控制器、验证编解码器/DAC 存在后返回 `1`（失败返回 `0`），并停止多余 DMA 引擎。 |
| `get_name()` / `is_present()` | 返回稳定可读设备描述串（供播放器 `DEV:` 状态栏显示）及硬件就绪标志（`1` / `0`）。 |
| `set_format(rate, bits, stereo)` | 配置 DAC/流格式（主流 `44100/48000 Hz`, `16-bit`, `stereo=1/2`）并重置/清零环指针。 |
| `start_dma()` / `stop_dma()` | 清零半区状态跟踪，启动/干净停止硬件流 DMA，不得遗留阻塞等待或死锁。 |
| `pause_dma(pause)` | **`pause=1`** 时暂停流引擎并保留/静音输出；**`pause=0`** 恢复前**必须重同步半区跟踪与超时计时器**（重新根据当前 DMA 位置计算 `last_half`，清除滞留中断状态位，必要时刷新 `timer_get_ticks()` 基准），严禁恢复瞬间重复播完旧半区导致声音倒退回弹或永久停滞。 |
| `set_volume(master, pcm)` | 支持 `0..100` 百分比音量与 `0` 静音映射；如未变化应幂等跳过慢速编解码器写命令以降低总线延迟。 |
| `poll_half_needed()` | 检测硬件位置跨区（如 LPIB/PICB/CIV 或定时器节拍兜底）：若刚完成第 `0` 或第 `1` 半区播放，则返回待填充半区编号（`0` 或 `1`），否则立即返回 `-1`。**单次调用绝不可长时间自旋**。 |
| `write_half(half, data, bytes)` | 将最多 `32768` 字节 PCM 数据复制到对应半区（不足部分补零），并执行 `pdv_dma_flush()`。 |
| `silence_half(half)` | 将对应半区缓冲清零并 flush，用于暂停、Seek 与文件末尾排空（drain），消除尾音循环杂音。 |
| `get_dma_frame_pos()` / `get_dma_buffer()` | 返回当前硬件播放帧偏移与只读 DMA 缓冲虚拟基址（`65536` 字节视图，供频谱 VU 实时采样读取）。 |

### 5.4 实时性红线（防爆音、防回退、防卡死）

1. **有界短轮询**：HDA CORB/直接命令、流复位（`CRST`/`RUN`）等硬件握手里，等待循环**严禁执行数十万次慢速 `outb(0x80)` 端口延迟**，应优先使用短 CPU `pause` 自旋搭配少量有界超时重试（单次传输命令通常在微秒内完成，全路径总耗时远小于 `170 ms` 半区窗口）。
2. **8-bit 状态寄存器写保护**：修改带 W1C 状态位的混合寄存器（例如 HDA `SD_CTL` 与紧邻 `SD_STS`）时，使用精确字节访问避免误清正在触发的缓冲完成状态标志。
3. **IRQ 中断续传与启动状态机对齐（`v26A225f` ES1371/CT4810 实践）**：对于通过 PCI IRQ 触发半区中断的声卡（如 Ensoniq ES1371/CT4810，`probe()` 通过 `kapi->pci_register_irq()` 挂载 ISR）：
   - `start_dma()` 在写入 DMA 基址、缓冲区大小与半区采样计数后，必须**显式清零暂停位（如 `ES_P2_PAUSE = 0`）、置位中断使能与启动位**，确保首次启动即可直接拉起硬件 DMA 传输。
   - ISR 检测到 DAC 中断状态后，须执行**“先清零中断使能确认并消抖 -> 立即调用 `kapi->audio_handle_dma_half_irq(half_idx)` 填充待播放半区（或置位挂起标志）-> 再重新使能中断位”**的标准流程，防止播完首个半区后中断挂死。
4. **无独立硬件中断时的轮询协同**：内核在主循环及合成器节拍中多次调用 `audio_poll()`，驱动的 `poll_half_needed()` 必须非阻塞、快速返回，确保在 `170 ms` 窗口内及时交替喂入第 `0`/`1` 半区。

---

## 六、 构建配置与验证流程

### 6.1 方式 1：使用 DDK 独立构建（[sdk/ddk/pdv_driver.mk](../sdk/ddk/pdv_driver.mk)）

```makefile
DRIVER_NAME := mygpu
DRIVER_SRCS := mygpu.c
DRIVER_PDI  := mygpu.pdi
PDV_DDK_DIR := ../../../sdk/ddk
include $(PDV_DDK_DIR)/pdv_driver.mk
```

执行 `make` 后会生成 `build/mygpu.pdv`。

### 6.2 方式 2：集成进内核主构建系统

在 [Makefile](../Makefile) 的 `PDV_DRIVERS` 中追加驱动目录名（无论是显示类还是音频类 `.pdv`）：

```makefile
PDV_DRIVERS = vmwgfx mach64 nvg7x fdc ac97 ct4810 cmi8738 hdaudio mygpu
```

执行 `make all` 与 `make iso` 时会自动编译为 `build/pdevice/mygpu.pdv`，并由 [scripts/make_iso.sh](../scripts/make_iso.sh) 打包至光盘 `PDEVICE\` 以及离线安装器 `payload/PDEVICE\` 目录中。

### 6.3 桌面实测验证

1. 正常启动（不启用 `nopdv=1`），打开 **控制面板（`cpanel.pxe`）**：查看 **Active Driver**、**VRAM**、**Limits** 以及分辨率下拉列表与 `8 / 16 / 24 / 32-bit` 色深按钮启用状态是否随当前显卡实时变化（`16–32 MB` 显存下可选择 `1920x1080 @ 32-bit`）。
2. 切换不同分辨率（如 VMware 下从 `800x600` 切到 `1024x768` 或 `1920x1080`），确认宿主机/物理屏幕发生真实分辨率切换而非软件拉伸。
3. 打开 **系统信息（`sysinfo.pxe`）**，确认 `VRAM` 容量、`Max WxH`、`Drv: Active/Fallback` 与 **Drivers (`.pdv`)** 列表正确，且反复点击 **Rescan `PDEVICE\`** 不会产生重复驱动条目。
4. 打开 **音乐播放器（`player.pxe`）**：验证 `.flac`/`.mp3` 连续播放时窗口无闪烁、音频无倒退回弹、执行 **Pause -> Play** 立即恢复不死机。
5. 使用 Limine 菜单的 `Safe Mode (Skip PDEVICE Drivers)`（`nopdv=1`）启动，验证在无有效显卡驱动时最高仅允许 **`1024x768`**、色深仅可选 **`8-bit` / `16-bit`**（`24`/`32` 置灰禁用）。

