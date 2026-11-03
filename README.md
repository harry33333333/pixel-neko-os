# Pixel Neko OS (64-Bit)

[![Build and Release OS ISO](https://github.com/harry33333333/pixel-neko-os/actions/workflows/build.yml/badge.svg)](https://github.com/harry33333333/pixel-neko-os/actions/workflows/build.yml)

> **Pixel Neko OS (Version Darwin 26A131a - Codename Snow Neko)**
> 现代化 64 位模块化宏内核与复古像素桌面操作系统

---

## 🌟 核心理念与技术纲领

Pixel Neko OS 是一款兼具**现代 64 位内核架构（AMD64 / x86-64 长模式）**与**经典复古桌面交互（Classic Mac OS 9 / Win 3.1 像素艺术风格）**的独立操作系统。

### 1. 引导与体系结构革新
- **Limine v8.x 双模引导**：原生支持现代 UEFI 固件与传统 Legacy BIOS (El Torito Hybrid MBR/GPT ISO)，完全脱离 GPL 限制。
- **64 位长模式与特权级隔离**：建立规范的 Ring 0 内核态与 Ring 3 用户态隔离，支持 4 级分页虚拟内存（PML4）与安全 Checked HHDM 映射。
- **Double Fault 独立 IST 虚拟栈**：为 CPU 异常提供独立虚拟栈与 Guard Page 防御，杜绝 Triple Fault 硬件重启。
- **NMI 安全的 SYSCALL / SYSRET**：采用 System V ABI 高速系统调用流水线与三窗口 Paranoid NMI 安全判定。
- **现代中断体系**：支持 LAPIC, IOAPIC, MADT ISO 中断重定向与 PCI MSI/MSI-X 消息中断。

### 2. 经典像素级 UI 保持
- **Happy Neko 开机动画**：50% 棋盘底纹、一体机线条、PC 扬声器 4 音符升调开机和弦（C5 -> E5 -> G5 -> C6）。
- **Win 3.1 风格 3D 浮雕视窗**：45° 动态斜向条纹理发店进度条（Barber-Pole）与 Extension Parade 硬件扩展图标游行。
- **显存 PAT (Page Attribute Table) WC 加速**：Write-Combining 显存纯写脏矩形双缓冲，杜绝显存读瓶颈与画面撕裂。
- **9 大经典内置应用程序**：文件管理器、文本编辑器、简易计算器、控制面板、多媒体音乐播放器、图片查看器、系统信息查看器、15 数字拼图、系统安装器。

### 3. 全栈 USB 架构
- 覆盖 USB 1.1 (UHCI/OHCI)、USB 2.0 (EHCI)、USB 3.0 (xHCI)。
- 实体机动态拓扑探测与伴随路由裁决，彻底解决 Intel 5 系列芯片组（如 ASUS P7H55-M LE）USB 掉电失灵难题。
- 保留最稳定可靠的 PS/2 键鼠硬件兜底。

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
qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -cdrom pixel-neko-os_26A131a.iso -m 512M -serial stdio
```
- **Legacy BIOS 模式**:
```bash
qemu-system-x86_64 -cdrom pixel-neko-os_26A131a.iso -m 512M -serial stdio -boot d
```

### P4b automated smoke test

`scripts/qemu_smoke_test.sh` boots the ISO through Q35/OVMF and Legacy BIOS,
tests UHCI, OHCI, EHCI and xHCI controllers, generic Report Protocol HID,
USB-storage boot, a USB hub and hotplug. It requires real USB HID input plus
P3/P4a regressions and validates the rendered welcome window from a framebuffer
screenshot:

```bash
bash scripts/qemu_smoke_test.sh pixel-neko-os_26A131a.iso
```

Controller and transfer architecture details are documented in
[`docs/USB.md`](docs/USB.md).

---

## 📜 许可证

- **内核与用户态核心**：[MIT License](LICENSE)
- **第三方开源组件与驱动清单**：详情参阅 [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)
