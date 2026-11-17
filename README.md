# Pixel Neko OS (64-Bit)

[![Build and Release OS ISO](https://github.com/harry33333333/pixel-neko-os/actions/workflows/build.yml/badge.svg)](https://github.com/harry33333333/pixel-neko-os/actions/workflows/build.yml)

> **Pixel Neko OS (Version Darwin 26A230a - Codename Snow Neko)**
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
- **现代中断体系**：支持 LAPIC、IOAPIC、MADT ISO 中断重定向、PCI shared IRQ 路由与 MSI/MSI-X 消息中断。

### 2. 视觉盛宴：开机动画、动态显示能力与复古像素 UI
- **Windows XP 风格开机界面与滚动进度条**：纯黑背景开机画卷，居中呈现四色复古 Pixel Neko 徽标，搭配三段式微光高亮蓝色流动滑块进度条（Marquee Progress Bar）。
- **精简 Limine 引导菜单**：提供 Live Desktop、Graphical Installer 以及 Safe Mode（`nopdv=1` 跳过外部 `.pdv` 并安全回退至 `1024×768×16b`）与 Safe Graphics 恢复入口。
- **动态分辨率、VRAM 精确探测与显存 WC 加速**：
  - 由已加载显卡 `.pdv`（如 `vmwgfx`、`mach64`、`nvg7x`）动态枚举分辨率与 `8/16/24/32-bit` 色深能力，彻底移除硬编码上限；VMware 模式下兼容 SVGA II 2D（放宽 `BAR1` 探测）与 SVGA3D Screen Target（`STDU`）。
  - 结合显存 PAT Write-Combining 纯写脏矩形双缓冲与 64 MiB `SHM` 共享窗口画布槽，支持控制面板可滚动高分辨率选择（含滑块滚动条）且无撕裂卡顿。
- **经典内置应用程序（`APPS\*.pxe`）**：`files`（文件管理器）、`notepad`（文本编辑器）、`calc`（简易计算器）、`cpanel`（控制面板）、`player`（多媒体音乐播放器）、`viewer`（图片查看器）、`clock`（数字时钟）、`about`（关于）、`sysinfo`（系统与驱动诊断）、`taskmgr`（Ring 0 任务管理器）、`install`（图形化离线安装器）。Puzzle、Solitaire、Gomoku 与 Aero Chess 统一位于根目录 `GAME\`。
- **持久化 Windows 风格桌面（`Desktop\`）**：桌面文件直接存放在系统盘 `Desktop\` 中，应用默认以 `*.pnk` 快捷方式出现，桌面隐藏后缀并绘制右下角快捷箭头；`.desktop-layout` 保存全部图标位置。文件管理与关于本机快捷方式锁定，`APPS\`、`PDEVICE\`、`boot\`、`system\` 等系统文件禁止删除或重命名。Live 模式使用只读桌面，不会读取本地系统盘的桌面。

### 26A230a 存储兼容修复
- ASUS P7H55-M LE / Intel H55：枚举全部 AHCI 控制器，并在 AHCI 已注册设备但无法取得可读卷时回退到 ATA PIO；不再以“块设备数量非零”误判存储成功。
- USB 启动：支持 USB 混合 ISO 的原生 ISO9660 挂载与 512/2048 字节扇区转换；EHCI/UHCI/OHCI 枚举失败可重试，USB BOT 正确处理短传输、CSW residue 与设备就绪延迟。

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
qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -cdrom pixel-neko-os_26A230a.iso -m 512M -serial stdio
```
- **Legacy BIOS 模式**:
```bash
qemu-system-x86_64 -cdrom pixel-neko-os_26A230a.iso -m 512M -serial stdio -boot d
```

### P6 自动化测试套件
`scripts/qemu_smoke_test.sh` 执行完整的桌面引导、多控制器热插拔与安装器全流程测试：

```bash
bash scripts/qemu_smoke_test.sh pixel-neko-os_26A230a.iso
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
- [26A230a 持久化桌面、快捷方式与 Live 只读规则](docs/DESKTOP.md)

---

## 📜 许可证

- **内核与用户态核心**：[MIT License](LICENSE)
- **第三方开源组件与驱动清单**：详情参阅 [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)
