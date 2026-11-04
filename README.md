# Pixel Neko OS (64-Bit)

[![Build and Release OS ISO](https://github.com/harry33333333/pixel-neko-os/actions/workflows/build.yml/badge.svg)](https://github.com/harry33333333/pixel-neko-os/actions/workflows/build.yml)

> **Pixel Neko OS (Version Darwin 26A174a - Codename Snow Neko)**
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
- **现代中断体系**：支持 LAPIC, IOAPIC, MADT ISO 中断重定向与 PCI MSI/MSI-X 消息中断。

### 2. 经典像素级 UI 保持
- **Happy Neko 开机动画**：50% 棋盘底纹、一体机线条、PC 扬声器 4 音符升调开机和弦（C5 -> E5 -> G5 -> C6）。
- **Win 3.1 风格 3D 浮雕视窗**：45° 动态斜向条纹理发店进度条（Barber-Pole）与 Extension Parade 硬件扩展图标游行。
- **显存 PAT (Page Attribute Table) WC 加速**：Write-Combining 显存纯写脏矩形双缓冲，杜绝显存读瓶颈与画面撕裂。
- **9 大经典内置应用程序**：文件管理器、文本编辑器、简易计算器、控制面板、多媒体音乐播放器、图片查看器、系统信息查看器、15 数字拼图、系统安装器。

### 3. 全栈 USB 架构与现代芯片组兼容
- **全速域硬件支持**：覆盖 USB 1.1 (UHCI/OHCI)、USB 2.0 (EHCI)、USB 3.0 (xHCI)。
- **现代芯片组与实体机适配**：针对 Intel 600 系列（如铭瑄 H610M-D）等现代主板实施根集线器端口强制复位（PR），将 BIOS 已使能端口强拉回默认地址 0 态，彻底解决进系统后 USB 鼠标无法移动的难题；屏蔽旧版芯片组路由寄存器误写，并引入端口退避重试。
- **USB 集线器（Hub）规范化使能**：严格复位完成检测，显式验证 `HUB_PORT_ENABLE`（位 1）状态，杜绝复位失败场景下的假使能与总线阻塞。详情参阅 [`docs/USB.md`](docs/USB.md)。
- **容灾机制**：保留最稳定可靠的 PS/2 键鼠硬件兜底。

### 4. 统一 64 位音频引擎与抗卡顿流调度
- **多硬件驱动支持**：原生驱动 Intel High Definition Audio (HDAudio Azalia)、AC'97 2.3、Creative Sound Blaster PCI (CT4810/ES1371)、C-Media CMI8738 及 PC Speaker。
- **多格式解码**：支持 WAV (PCM 8/16/24-bit 智能降采样)、FLAC 无损及 MP3 (基于 minimp3 引擎，强化 16 字节栈与内存对齐，彻底消除硬件 SSE `#GP` 异常)。
- **抗卡顿与低延迟缓冲架构**：采用 256 KiB 大容量 PCM 环形缓冲队列、2 的幂次快速位掩码寻址、向量化批量 `memcpy` 内存传输、播放前预填充 2 个 DMA 半缓冲，以及在调度器空闲线程（Idle Loop）与屏幕刷新切片多点轮询，确保重度 UI 交互下音频平滑无卡顿。详情参阅 [`docs/AUDIO.md`](docs/AUDIO.md)。

### 5. 存储堆栈与用户数据安全防线
- **未初始化磁盘保护机制**：面对全新或未分区的硬盘，内核严禁静默自动分区与格式化，通过窗口管理器交互式弹窗向用户请求显式授权，防止误抹除数据。
- **多文件系统与路径导航**：全面支持 MBR/GPT、FAT12/16/32（长文件名）、exFAT、ISO9660/Joliet，修复文件管理器深层目录双击进出与路径解析。详情参阅 [`docs/STORAGE.md`](docs/STORAGE.md)。

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
qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -cdrom pixel-neko-os_26A174a.iso -m 512M -serial stdio
```
- **Legacy BIOS 模式**:
```bash
qemu-system-x86_64 -cdrom pixel-neko-os_26A174a.iso -m 512M -serial stdio -boot d
```

### P6 自动化测试套件
`scripts/qemu_smoke_test.sh` 执行完整的桌面引导、多控制器热插拔与安装器全流程测试：

```bash
bash scripts/qemu_smoke_test.sh pixel-neko-os_26A174a.iso
```

图形化安装器、离线载荷与独立启动流程详见 [`docs/P6_INSTALLER.md`](docs/P6_INSTALLER.md)。

---

## 📚 详细文档索引
- [USB 子系统规范与芯片组兼容矩阵](docs/USB.md)
- [统一音频引擎与解码流设计](docs/AUDIO.md)
- [系统调用安全边界与特权隔离](docs/SECURITY.md)
- [存储驱动模型与未初始化磁盘策略](docs/STORAGE.md)
- [P6 离线安装器与独立引导](docs/P6_INSTALLER.md)

---

## 📜 许可证

- **内核与用户态核心**：[MIT License](LICENSE)
- **第三方开源组件与驱动清单**：详情参阅 [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)
