# Pixel Neko OS (64-Bit)

[![Build and Release OS ISO](https://github.com/harry33333333/pixel-neko-os/actions/workflows/build.yml/badge.svg)](https://github.com/harry33333333/pixel-neko-os/actions/workflows/build.yml)

> **Pixel Neko OS (Version Darwin 26A350d - Codename Snow Neko)**
> 现代化 64 位模块化宏内核与复古像素桌面操作系统（完整支持简体中文 / English 双语与拼音输入法）

---

## 🌟 核心理念与技术纲领

Pixel Neko OS 是一款兼具**现代 64 位内核架构（AMD64 / x86-64 长模式）**与**经典复古桌面交互（Classic Mac OS 9 / Win 3.1 / Windows 风格）**的独立操作系统。

### 1. 引导与内核体系结构
- **Limine v8.x 双模引导**：原生支持现代 UEFI 固件与传统 Legacy BIOS (El Torito Hybrid MBR/GPT ISO)，提供 Live Desktop、Graphical Installer、Safe Mode（`nopdv=1` 跳过外部 `.pdv` 并回退安全显示模式）与 Safe Graphics 引导入口。
- **SMP 多核对称多处理（高达 128 CPU）**：支持多核 AP 启动与负载均衡、1000 Hz 每核 LAPIC 定时器、XSAVE/AVX2 扩展上下文保存、内核互斥锁（`kmutex_t`）与自旋锁，以及合成器 CPU0 隔离调度。
- **64 位长模式与特权级隔离**：建立规范的 Ring 0 内核态与 Ring 3 用户态隔离，支持 4 级分页虚拟内存（PML4）与安全 Checked HHDM 映射，并提供 Ring 3 多线程与 Futex 同步原语（`<pxe/thread.h>`）。
- **严密系统调用安全边界**：
  - 在 `SYSRET` 前严格校验用户 RIP 与用户 RSP 是否为规范地址（Canonical）且位于合法用户低半区空间（`< 0x0000800000000000`）。
  - 强制校验 CS/SS 选择子 RPL 为 3，严格限制 RFLAGS 掩码（IOPL 强制为 0、IF 强制置位、拦截 NT 任务嵌套与 VM 虚拟8086模式）。
  - 违规调用统一触发 `.bad_return` 安全路径，终止非法进程并由调度器通过 `IRETQ` 硬件安全返回。详情参阅 [`docs/SECURITY.md`](docs/SECURITY.md)。
- **Double Fault 独立 IST 虚拟栈与 NMI 防护**：为 CPU 异常提供独立虚拟栈与 Guard Page 防御，杜绝 Triple Fault 硬件重启；采用 System V ABI 高速系统调用流水线与三窗口 Paranoid NMI 安全判定。
- **双特权环原生应用体系（`*.pxe`）**：提供统一 `sdk/` 工具链（Ring 3 W^X 隔离与 System Bomb 崩溃隔离窗；Ring 0 高半区 PIE 特权运行环境），系统工具打包于 `APPS\*.pxe`。详情参阅 [`docs/executable_develop.md`](docs/executable_develop.md)。
- **自包含可加载驱动体系（`*.pdv`）**：采用 DDK 隔离的独立可重定位 ELF64 驱动模块部署于 `PDEVICE\`，按硬件 PCI/类别按需加载与去重。详情参阅 [`docs/driver_develop.md`](docs/driver_develop.md)。
- **现代中断与电源控制**：支持 LAPIC、IOAPIC、MADT ISO 中断重定向、ACPI `_PIC`/`_PRT` INTx→GSI、PCI shared IRQ 路由与 ACPI S5 软关机（参阅 [`docs/ACPI_S5.md`](docs/ACPI_S5.md)）。

### 2. 简体中文国际化、字体与拼音输入法
- **完整双语与热切换**：系统默认以简体中文启动，支持在控制面板中一键切换 **简体中文 / English** 并持久化保存至 `.pse`，开机加载画面、桌面、顶部状态栏、全部内置应用与游戏均可实时同步切换。
- **UTF-8 内核与点阵字库引擎**：内置内存常驻的 `system/FONT_CJK.FNT` 字库，支持 `8×16` ASCII + `12×12` 中日韩点阵字模混排（`5×7` 小字体行自动基线居中对齐），提供按像素宽度度量与完整 UTF-8 字符边界截断工具。
- **内置拼音输入法（IME）**：
  - 词库 `system/IME.PNY` 基于 Rime 朙月拼音与八股文模型预先编译并常驻物理内存，支持全拼、简拼、词组与整句联想、候选翻页及中文标点输入。
  - 支持与 Windows 7 一致的 **`Ctrl+Shift`** 快捷键切换中/英文输入状态，顶部状态栏实时显示 `[中]` / `[EN]` 指示器并支持点击切换。

### 3. 图形显示、开机画面与桌面环境
- **开机画面与双语加载**：纯黑背景 Windows XP 风格四色徽标与三段式微光流动进度条，随后进入支持简体中文/英文的内核与扩展初始化加载界面。
- **EDID 解析与动态分辨率/色深切换**：
  - 启动时优先解析 UEFI GOP / Legacy VBE 获取的 EDID 1.3/1.4 时序（含 Established/Standard/DTD 与常见 CEA-861 模式），EDID 缺失时回退固件模式表。
  - 由已加载显卡 `.pdv` 动态枚举分辨率与 `8/16/24/32-bit` 色深，结合快速 Write-Combining SSE2/AVX2 合成器（`fb_blit_wc`）与三缓冲 `SHM` 共享窗口画布槽实现无撕裂流畅刷新：
    - **NVIDIA**：支持 NV43 (GeForce 6600 LE `10DE:0142`)、G72/G73 与 GeForce GTX 1060 3GB (`10DE:1C02`, GP106 Pascal)，支持运行时两级 VPLL/PCRTC/RAMDAC 无重启模式切换、同步极性配置与 EDID 刷新率范围校验（防止超出显示器支持范围）。
    - **ATI Mach64 / 3D Rage II+DVD**：支持内部 PLL、CRTC、`32-qword` Display FIFO 水位线校准与晶振自动标定，按 2MB/4MB 真实显存过滤并运行时切换模式。
    - **VMware SVGA II / SVGA3D**：支持完整虚拟模式列表、2D 加速与 Screen Target (`STDU`)。
    - 通用 LFB / 回退路径提供安全兼容模式，需重启切换时先弹出确认框。
- **持久化 Windows 风格桌面（`Desktop\`）**：
  - 桌面直接映射系统盘 `Desktop\` 目录中的 `*.pnk` 快捷方式（自动隐藏后缀并绘制右下角快捷箭头），图标坐标由 `.desktop-layout` 持久化保存，支持自动对齐布局与自由拖拽；`文件管理` 与 `关于本机` 等系统关键快捷方式及 `APPS\`、`PDEVICE\`、`boot\`、`system\` 目录受保护禁止删除或重命名。Live 模式使用只读桌面，不读写本地系统盘。
  - 壁纸支持平铺、拉伸与置中（1:1 原始像素）模式，并支持一键恢复默认壁纸。
  - 顶部状态栏提供日期/秒钟独立开关、输入法状态指示，以及 Windows XP 风格音量弹出面板（支持滑块拖拽、独立静音按钮与滚轮 5% 步进调节）。
  - 全局支持 **`Ctrl+Shift+Esc`** 快捷键呼出或聚焦任务管理器，支持 **`Alt+Tab` / `Alt+Shift+Tab`** 窗口切换弹出面板；鼠标滚轮全面统一为 Windows 方向（`PXE_EV_MOUSE_WHEEL`）。

### 4. 内置应用程序与游戏生态
- **系统应用（`APPS\*.pxe`）**：
  - `files`（文件管理器）：支持多盘符浏览、长文件名、文件夹递归复制、复制/剪切/粘贴/重命名/新建文件夹/删除/属性查看、右键一键打开 CMD 或发送到桌面、磁盘弹出，以及基于 `system\assoc.ini` 与 `pxe_resource_t.ext_assoc` 的动态文件关联和右键“打开方式...”自定义默认程序；与桌面双向共享剪贴板及实时同步，支持独立桌面壳生命周期。
  - `cmd`（命令提示符）：Ring 3 终端与批处理解释器，支持可调窗口尺寸与 500 行回滚缓冲、CJK 等宽双字宽显示、ANSI 16 色、管道 `|` / 重定向 `>` / `>>` / 环境变量、`*.pat` 批处理脚本（`.pat` 关联至 `cmd.pxe`）以及核内只读 `\PROC\` 虚拟文件系统查询。
  - `notepad`（记事本）：完整接入 UTF-8 与拼音输入法（`PXE_KEY_UTF8`），支持中文编辑与滚轮浏览。
  - `cpanel`（控制面板）：配置屏幕分辨率/色深、桌面壁纸模式、顶栏时钟选项、系统音量及简体中文/英文界面语言。
  - `player`（媒体播放器）：支持 WAV、FLAC 与 MP3 解码播放与播放列表控制。
  - `vplayer`（视频播放器，Ring 3 SMP）：集成 OpenH264、FAAD2 (AAC-LC) 与 minimp4，支持 1080p60 MP4/H.264 多线程视频解码与低延迟音画同步播放。
  - `viewer`（图片查看器）、`calc`（计算器）、`clock`（数字时钟）、`about`（关于本机）。
  - `cpuz`（Pixel CPU Detect，Ring 0）：基于裁剪版 libcpuid 0.8.2 的处理器、缓存、PCI 拓扑、内存、显卡检测工具，支持将完整诊断报告导出为 `*.TXT`。
  - `sysinfo`（系统信息，Ring 0）：查看系统资源、PCI/USB 设备及已加载 `.pdv` 驱动，支持手动加载与重扫 `PDEVICE\`。
  - `taskmgr`（任务管理器，Ring 0）：实时监控并管理运行进程、内存及多核 CPU 性能曲线（Performance 标签页），支持终止系统核心进程的风险警告确认（终止 `idle` 将触发蓝屏停机）。
  - `install`（图形化离线安装器，Ring 0）：仅在安装引导模式下运行，支持全新安装或保留文件系统的升级安装。
- **内置游戏（`GAME\*.pxe`）**：
  - 全部 7 款游戏均支持简体中文/英文实时切换：`puzzle`（数字推盘）、`solitaire`（纸牌接龙）、`gomoku`（五子棋）、`aerochess`（飞行棋）、`2048`（2048 数字方块）、`minesweeper`（扫雷，含难度与音效设置）、`othello`（黑白棋）。

### 5. 全栈 USB 架构与外设输入
- **全速域控制器支持**：覆盖 USB 1.1 (UHCI/OHCI)、USB 2.0 (EHCI)、USB 3.0 (xHCI)。
- **现代芯片组与实体机适配**：针对 Intel 600 系列（如 H610）等现代主板实施根集线器端口强制复位、xHCI ERST Change 位正确维护、USB Hub 复位与 `HUB_PORT_ENABLE` 验证及 USB BOT 短传输/残留字节容错；同时保留 PS/2 键鼠（含 IntelliMouse 滚轮）硬件兜底。详情参阅 [`docs/USB.md`](docs/USB.md)。

### 6. 统一 64 位音频引擎与抗卡顿流调度
- **多硬件驱动支持（`PDV_CLASS_AUDIO`）**：原生 `.pdv` 驱动支持 Intel High Definition Audio (HDAudio Azalia)、AC'97 2.3、Ensoniq AudioPCI / Creative Sound Blaster PCI (ES1371 / `ct4810`，支持 PCI IRQ 9 中断续传双半区 DMA)、C-Media CMI8738 及 PC Speaker；音频随播放进程退出或终止自动释放停止。
- **多格式解码与低延迟缓冲**：支持 WAV (PCM 8/16/24-bit 智能降采样)、FLAC 无损及 MP3 (minimp3 16 字节栈/内存对齐)；采用 256 KiB PCM 环形缓冲队列、预填充双 DMA 半缓冲、低延迟音频流（`audio_stream_open`）与微秒级主时钟音画同步机制。详情参阅 [`docs/AUDIO.md`](docs/AUDIO.md)。

### 7. 存储堆栈与用户数据安全防线
- **未初始化磁盘保护机制**：面对全新或未分区的硬盘，内核严禁静默自动分区与格式化，通过窗口管理器交互式弹窗向用户请求显式授权，防止误抹除数据。
- **批量 I/O、预读与多核并发安全**：支持批量存储 I/O、VFS 256–512 KiB 顺序预读加速及 SMP 存储/VFS 并发锁保护。
- **多文件系统与软盘驱动**：全面支持 MBR/GPT、FAT12/16/32（UTF-8 VFAT 长文件名）、exFAT（UTF-16LE 长文件名）、ISO9660/Joliet（512/2048 字节扇区转换）与 8237 DMA 软盘驱动器（A:/B: 换盘与弹出检测）。详情参阅 [`docs/STORAGE.md`](docs/STORAGE.md)。

---

## 🛠️ 构建与测试

### 本地编译（Linux / Docker / WSL2）
```bash
# 依赖工具：gcc, nasm, xorriso, mtools, python3
make iso
```

### QEMU 启动测试
- **UEFI 模式**:
```bash
qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -cdrom pixel-neko-os_26A350d.iso -m 512M -serial stdio
```
- **Legacy BIOS 模式**:
```bash
qemu-system-x86_64 -cdrom pixel-neko-os_26A350d.iso -m 512M -serial stdio -boot d
```

### 自动化冒烟测试
`scripts/qemu_smoke_test.sh` 执行桌面引导、多控制器热插拔与安装器全流程测试：

```bash
bash scripts/qemu_smoke_test.sh pixel-neko-os_26A350d.iso
```

图形化安装器、离线载荷与独立启动流程详见 [`docs/P6_INSTALLER.md`](docs/P6_INSTALLER.md)。

---

## 📚 详细文档索引
- [PDV 显卡与声卡硬件驱动开发指南 (`*.pdv`)](docs/driver_develop.md)
- [PXE 原生可执行程序开发指南 (`*.pxe`)](docs/executable_develop.md)
- [USB 子系统规范与芯片组兼容矩阵](docs/USB.md)
- [统一音频引擎与解码流设计](docs/AUDIO.md)
- [系统调用安全边界与特权隔离](docs/SECURITY.md)
- [存储驱动模型与未初始化磁盘策略](docs/STORAGE.md)
- [P6 离线安装器与独立引导](docs/P6_INSTALLER.md)
- [持久化桌面、快捷方式与 Live 只读规则](docs/DESKTOP.md)
- [ACPI S5 软关机说明](docs/ACPI_S5.md)

---

## 📜 许可证

- **内核与用户态核心**：[MIT License](LICENSE)
- **第三方开源组件、词库与驱动清单**：详情参阅 [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)
