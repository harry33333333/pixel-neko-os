# Pixel Neko OS (64-Bit)

[![Build and Release OS ISO](https://github.com/harry33333333/pixel-neko-os/actions/workflows/build.yml/badge.svg)](https://github.com/harry33333333/pixel-neko-os/actions/workflows/build.yml)

> **Pixel Neko OS (Version Darwin 26A262d - Codename Snow Neko)**
> 现代化 64 位模块化宏内核与复古像素桌面操作系统

---

## 🌟 核心理念与技术纲领

Pixel Neko OS 是一款兼具**现代 64 位内核架构（AMD64 / x86-64 长模式）**与**经典复古桌面交互（Classic Mac OS 9 / Win 3.1 像素艺术风格）**的独立操作系统。

### 1. 引导与体系结构革新
- **Limine v8.x 双模引导**：原生支持现代 UEFI 固件与传统 Legacy BIOS (El Torito Hybrid MBR/GPT ISO)，完全脱离 GPL 限制。
- **64 位长模式与特权级隔离**：建立规范的 Ring 0 内核态与 Ring 3 用户态隔离，支持 4 级分页虚拟内存（PML4）与安全 Checked HHDM 映射。
- **严密系统调用安全边界**：
  - 在 `SYSRET` 前严格校验用户 RIP 与用户 RSP 是否为规范地址（Canonical）且位于合法用户低半区空间（`< 0x0000800000000000`）。
  - 强制校验 CS/SS 选择子 RPL 为 3，严格限制 RFLAGS 掩码（IOPL 强制为 0、IF 强制置位、拦截 NT 任务嵌套与 VM 虚拟8086模式）。
  - 违规调用统一触发 `.bad_return` 安全路径，终止非法进程并由调度器通过 `IRETQ` 硬件安全返回。详情参阅 [`docs/SECURITY.md`](docs/SECURITY.md)。
- **Double Fault 独立 IST 虚拟栈**：为 CPU 异常提供独立虚拟栈与 Guard Page 防御，杜绝 Triple Fault 硬件重启。
- **NMI 安全机制**：采用 System V ABI 高速系统调用流水线与三窗口 Paranoid NMI 安全判定。
- **双特权环原生应用体系（`*.pxe`）**：提供统一 `sdk/` 工具链（Ring 3 W^X 隔离与 System Bomb 崩溃隔离窗；Ring 0 高半区 PIE 特权运行环境），全部系统工具打包于 `APPS\*.pxe`（含 `taskmgr.pxe` 任务管理器）。详情参阅 [`docs/executable_develop.md`](docs/executable_develop.md)。
- **自包含可加载驱动体系（`*.pdv`）**：采用 DDK 隔离的独立可重定位 ELF64 驱动模块部署于 `PDEVICE\`，按硬件 PCI/类别按需加载与去重。详情参阅 [`docs/driver_develop.md`](docs/driver_develop.md)。
- **现代中断体系**：支持 LAPIC、IOAPIC、MADT ISO 中断重定向、ACPI `_PIC`/`_PRT` INTx→GSI 与 PCI shared IRQ 路由（MSI/MSI-X 消息中断规划中）。

### 2. 视觉盛宴：开机动画、动态显示能力与复古像素 UI
- **Windows XP 风格开机界面与滚动进度条**：纯黑背景开机画卷，居中呈现四色复古 Pixel Neko 徽标，搭配三段式微光高亮蓝色流动滑块进度条（Marquee Progress Bar）。
- **精简 Limine 引导菜单**：提供 Live Desktop、Graphical Installer 以及 Safe Mode（`nopdv=1` 跳过外部 `.pdv` 并安全回退至 `1024×768×16b`）与 Safe Graphics 恢复入口。
- **动态分辨率、VRAM 精确探测与显存 WC 加速**：
  - 由已加载显卡 `.pdv`（如 `vmwgfx`、`mach64`、`nvg7x`）动态枚举分辨率与 `8/16/24/32-bit` 色深能力，彻底移除硬编码上限；VMware 模式下兼容 SVGA II 2D（放宽 `BAR1` 探测）与 SVGA3D Screen Target（`STDU`）。
  - 结合显存 PAT Write-Combining 纯写脏矩形双缓冲与 64 MiB `SHM` 共享窗口画布槽，支持控制面板可滚动高分辨率选择（含滑块滚动条）且无撕裂卡顿。
- **经典内置应用程序（`APPS\*.pxe`）**：`files`（文件管理器）、`notepad`（文本编辑器）、`calc`（简易计算器）、`cpanel`（控制面板）、`player`（多媒体音乐播放器）、`viewer`（图片查看器）、`clock`（数字时钟）、`about`（关于）、`sysinfo`（系统与驱动诊断）、`cpuz`（Pixel CPU Detect，Ring 0 处理器检测器）、`taskmgr`（Ring 0 任务管理器）、`install`（图形化离线安装器）。Puzzle、Solitaire、Gomoku、Aero Chess、2048、Minesweeper 与 Reversi 统一位于根目录 `GAME\`。
- **持久化 Windows 风格桌面（`Desktop\`）**：桌面文件直接存放在系统盘 `Desktop\` 中，应用默认以 `*.pnk` 快捷方式出现，桌面隐藏后缀并绘制右下角快捷箭头；`.desktop-layout` 保存全部图标位置。文件管理与关于本机快捷方式锁定，`APPS\`、`PDEVICE\`、`boot\`、`system\` 等系统文件禁止删除或重命名。Live 模式使用只读桌面，不会读取本地系统盘的桌面。

### 26A232c 桌面与硬件兼容修复
- 自动图标右边距由 104px 收紧到 72px，多列间距由 184px 收紧到 96px，贴近 Windows 桌面参考布局。
- 已有 `PNK_LAYOUT2` 自动图标会在启动时自动迁移到新间距。
- xHCI ERST 段表设置 Change 位，修复 Intel H610 上事件环首次回卷后 HID 中断事件不再被回收、鼠标约 15–30 秒后停止的问题。
- 桌面图标缓存跨目录刷新保留；图标加载失败时按应用使用不同 fallback，不再全部显示为 Control Panel。
- 桌面名称每行最多 7 个英文字符，超过两行时第二行以 `...` 省略。
- `GAME\` 新增 `2048.pxe`、`minesweeper.pxe` 与 `othello.pxe`（Reversi）；Live ISO、安装负载和现有系统升级均会自动包含。
- Live 快捷方式统一经系统应用解析器启动，Ring 0 系统应用使用显式受信任路径，修复 Live 下点击快捷方式直接崩溃的问题。
- 桌面长名称按 Windows 风格显示为两行，超过第二行容量后使用 `...` 省略。
- 控制面板新增恢复默认壁纸、日期显示开关和秒钟显示开关；默认同时显示日期与秒钟。
- 文件管理器新增 Cut 剪切/移动，支持工具栏、右键菜单和 `Ctrl+X`。
- ASUS P7H55-M LE / Intel H55：枚举全部 AHCI 控制器，并在 AHCI 已注册设备但无法取得可读卷时回退到 ATA PIO；不再以“块设备数量非零”误判存储成功。
- USB 启动：支持 USB 混合 ISO 的原生 ISO9660 挂载与 512/2048 字节扇区转换；EHCI/UHCI/OHCI 枚举失败可重试，USB BOT 正确处理短传输、CSW residue 与设备就绪延迟。

### 26A235a Pixel CPU Detect
- 新增 Ring 0 系统应用 `APPS\cpuz.pxe`，窗口标题为 **Pixel CPU Detect**，提供 CPU、缓存、主板 PCI 拓扑、内存、显卡与关于六个页面。
- CPU 型号、代号、Family/Model/Stepping、指令集、核心/线程与缓存拓扑来自裁剪后的 **libcpuid 0.8.2**；保留 Intel、AMD、Centaur 的 x86 识别数据库，不携带 Windows/Linux MSR 驱动、文件序列化器或 ARM 后端。
- 新增 freestanding PXE 兼容层，将内存与字符串操作接到 `pxe/mem.h` / `pxe/str.h`；线程错误状态改为静态变量，CPU 亲和性接口在 Ring 0 构建中安全禁用。
- `About` 页面新增 **Save Report As...** 对话框，用户可输入或粘贴任意合法绝对路径，将完整解码信息、PCI/PDV/内存状态及原始 CPUID 叶数据保存为 `*.TXT`，便于真机排障。
- 桌面默认新增 `Pixel CPU Detect.pnk`，文件管理器管理入口与关于本机入口继续锁定；现有系统升级后也会自动补齐该快捷方式。
- libcpuid 的 BSD-2-Clause 版权与许可证记录已加入 `THIRD_PARTY_LICENSES.md`。

### 26A250a 真机输入/音频、桌面命中与显示模式修复
- 修复 ASUS P7H55-M LE 等真实 ATA/ATAPI 平台上的周期性卡顿：桌面不再每秒遍历 FAT 并重读全部快捷方式，VFS 也不再每秒对光驱发送阻塞式 `TEST UNIT READY`；Desktop 文件操作通过变更通知触发一次同步刷新，介质改为用户刷新时检查。
- 修复桌面图标点击范围硬编码为 10 项的问题。CPU 检测器、Sysinfo 以及之后新增的所有桌面项目现在都会使用实时项目数量进行命中测试。
- 新增共享显示模式目录，优先读取 Limine 从 **UEFI GOP / Legacy VBE+EDID** 获取的 EDID 1.3/1.4 时序，并解析 Established Timings、Standard Timings、DTD 与常见 CEA-861 HDMI 模式；EDID 缺失时回退到固件模式表。
- Live、安装器、本机启动以及所有安全模式统一使用 `800x600x16` 作为 Limine 初始扫描输出，确保开机进度条和所有真机使用相同的兼容模式；桌面内仍可根据 EDID/固件模式目录切换更高逻辑分辨率。
- NVIDIA G7x 与 ATI Mach64 不再把启动分辨率当作最大分辨率，而是按 EDID/固件模式列表过滤。ATI 2MB 可显示 `1024x768x16`，4MB 可继续使用其显存允许的更高模式；VMware SVGA 保持原有完整虚拟模式列表且不依赖 EDID。

### 26A261a 真实分辨率、通知区音量与鼠标滚轮
- 物理 NVIDIA/ATI 驱动不再把大画布缩放到旧扫描输出。控制面板选择模式后，`Keep` 会保存真实 Limine 启动分辨率并重启，由 GOP/VBE 重新点亮目标模式；VMware 等支持运行时硬件 modeset 的驱动仍然即时切换。
- 通知区在时钟左侧新增音量状态，显示 `VOL nn%` 或 `MUTE`；点击可切换静音，鼠标位于音量区域时滚轮可按 5% 调节。
- 输入系统新增 `PXE_EV_MOUSE_WHEEL` 事件，使用 `ev.arg0` 传递滚轮增量，不改变既有 `pxe_event_t` 布局。
- USB HID 与 IntelliMouse 兼容 PS/2 鼠标均支持滚轮；文件管理器、Notepad、Sysinfo 和 Control Panel 的分辨率列表已接入上下滚动。

### 26A262a Mach64/G7x 无重启模式切换与 XP 音量面板
- ATI Mach64/Rage II 驱动现在直接编程内部 PLL、CRTC、pitch 与像素格式；2MB 与 4MB 显存分别按真实容量过滤可用模式。
- NVIDIA G72/G73 驱动现在使用 two-stage VPLL、PCRTC 与 RAMDAC 完成运行时分辨率/色深切换，并保留失败回滚。
- EDID Detailed Timing Descriptor 的 pixel clock、blanking、sync 起止位置与极性已完整传递到 PDV；经典 800x600、1024x768 等模式使用内置 VESA/DMT 时序。
- 通用 LFB 与 Intel 回退路径仅提供 `800x600x16`、`1024x768x16`。必须重启时先显示确认框，拒绝后保持当前分辨率与色深且不写入启动配置。
- 通知区喇叭图标改为 Windows XP 风格弹出面板，包含可拖拽音量滑杆和独立 Mute 按钮；图标区域滚轮继续支持快速调节。

### 26A262b 实体机 NVIDIA 模式切换修复
- 修复 NVIDIA G7x 的 VGA CRTC 写入路径：改用 BAR0 `0x601000` MMIO 镜像，头 1 使用 `0x2000` 偏移，确保 CRTC 时序与 PLL 同步更新，不再出现只改像素时钟导致的“超出显示屏能力上限”。
- 修复运行时帧缓冲 WC 窗口只按启动 `800x600x16` 大小映射的问题。`fb_set_surface()` 现在会在新 pitch/height 下重新映射完整专用 PAT WC 窗口，避免切换后访问 `0xffff9000...` 越界并触发 `#PF` 与自动重启。
- 模式切换前增加 VBlank 等待，并在写入后校验水平总长、扩展水平高位、pitch 与像素模式；G7x 模式日志增加 pixel clock 和完整同步时序，便于后续实体机诊断。

### 26A262c 实体机 NVIDIA 同步极性修复
- 修复 G7x 只更新 CRTC 和 PLL、却没有更新 VGA PVIO Misc Output 同步极性的问题。`1024x768@60` 需要负 H/V 同步，而启动的 `800x600@60` 为正 H/V；极性未切换会让部分显示器直接提示“超出最高显示上限”。
- 运行时模式切换现在同步写入 VGA Misc Output、Sequencer 和 Graphics 寄存器，并在日志中记录 `VGA misc output old -> new`、H/V 极性以及 CRTC 是否处于 DFP/TV 从模式。
- 继续保留 BAR0 MMIO CRTC、VPLL、动态 WC 窗口和 VBlank/回读保护。

### 26A262d 实体机 NVIDIA 模式切换提交修复
- 修复 Sequencer Reset 寄存器被错误写成 `0` 导致同步复位未释放的问题；现在与 X.Org `vgaHW` 一致，不再写 Sequencer 0，只更新 Sequencer 1–4。
- 修复 G7x 在部分实体板上把 CRTC pitch/pixel 只读镜像值不一致误判为切换失败的问题。水平总长与扩展高位仍作为回滚依据，pitch/pixel 改为诊断日志，不再阻止有效的 CRTC/PLL 切换。
- 新增完整 CRTC 回读日志，包含实际值、期望值和 advisory mismatch 提示。

### 3. 全栈 USB 架构与现代芯片组兼容
- **全速域硬件支持**：覆盖 USB 1.1 (UHCI/OHCI)、USB 2.0 (EHCI)、USB 3.0 (xHCI)。
- **现代芯片组与实体机适配**：针对 Intel 600 系列（如铭瑄 H610M-D）等现代主板实施根集线器端口强制复位（PR），将 BIOS 已使能端口强拉回默认地址 0 态，彻底解决进系统后 USB 鼠标无法移动的难题；屏蔽旧版芯片组路由寄存器误写，并引入端口退避重试。
- **USB 集线器（Hub）规范化使能**：严格复位完成检测，显式验证 `HUB_PORT_ENABLE`（位 1）状态，杜绝复位失败场景下的假使能与总线阻塞。详情参阅 [`docs/USB.md`](docs/USB.md)。
- **容灾机制**：保留最稳定可靠的 PS/2 键鼠硬件兜底。

### 4. 统一 64 位音频引擎与抗卡顿流调度
- **多硬件驱动支持（`PDV_CLASS_AUDIO`）**：原生 `.pdv` 驱动支持 Intel High Definition Audio (HDAudio Azalia)、AC'97 2.3、Ensoniq AudioPCI / Creative Sound Blaster PCI (ES1371 / `ct4810`，支持 PCI IRQ 9 中断续传双半区 DMA)、C-Media CMI8738 及 PC Speaker；音频随播放进程退出或终止自动释放停止。
- **多格式解码**：支持 WAV (PCM 8/16/24-bit 智能降采样)、FLAC 无损及 MP3 (基于 minimp3 引擎，强化 16 字节栈与内存对齐，彻底消除硬件 SSE `#GP` 异常)。
- **抗卡顿与低延迟缓冲架构**：采用 256 KiB 大容量 PCM 环形缓冲队列、2 的幂次快速位掩码寻址、向量化批量 `memcpy` 内存传输、播放前预填充 2 个 DMA 半缓冲，以及在调度器空闲线程（Idle Loop）与屏幕刷新切片多点轮询，确保重度 UI 交互下音频平滑无卡顿。详情参阅 [`docs/AUDIO.md`](docs/AUDIO.md)。

### 5. 存储堆栈与用户数据安全防线
- **未初始化磁盘保护机制**：面对全新或未分区的硬盘，内核严禁静默自动分区与格式化，通过窗口管理器交互式弹窗向用户请求显式授权，防止误抹除数据。
- **多文件系统与路径导航**：全面支持 MBR/GPT、FAT12/16/32（长文件名）、exFAT、ISO9660/Joliet，修复文件管理器深层目录双击进出与路径解析。详情参阅 [`docs/STORAGE.md`](docs/STORAGE.md)。
- **软盘与 ISA DMA**：内置 8237 通道 2 和 FDC 驱动，A:/B: 按物理软驱 0/1 映射，按 FAT12 BPB 识别实际容量；空驱不显示盘符，支持换盘、弹出和重新插入。

---

## 🛠️ 构建与测试

### 本地编译（Linux / Docker / WSL2）
```bash
# 依赖工具：gcc, nasm, xorriso, mtools
make iso
```

### QEMU 启动测试
- **UEFI 模式**:
```bash
qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -cdrom pixel-neko-os_26A262d.iso -m 512M -serial stdio
```
- **Legacy BIOS 模式**:
```bash
qemu-system-x86_64 -cdrom pixel-neko-os_26A262d.iso -m 512M -serial stdio -boot d
```

### P6 自动化测试套件
`scripts/qemu_smoke_test.sh` 执行完整的桌面引导、多控制器热插拔与安装器全流程测试：

```bash
bash scripts/qemu_smoke_test.sh pixel-neko-os_26A262d.iso
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
- [26A262d 持久化桌面、快捷方式与 Live 只读规则](docs/DESKTOP.md)

---

## 📜 许可证

- **内核与用户态核心**：[MIT License](LICENSE)
- **第三方开源组件与驱动清单**：详情参阅 [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)
