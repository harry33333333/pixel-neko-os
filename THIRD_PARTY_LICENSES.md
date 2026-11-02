# Pixel Neko OS 第三方开源许可证与版权记录

本项目严格区分内核本体代码、移植外部驱动源码、引导加载程序以及宿主构建工具。

## 1. 移植驱动与组件逐文件版权清单
| 源文件路径 (本项目) | 上游项目 | 上游分支 / Commit | 原始文件路径 | 原始版权声明 | 许可证类型 | 修改概要 |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `drivers/usb/core/usb_core.c` | FreeBSD | `releng/13.2-RELEASE` | `sys/dev/usb/usb_core.c` | (c) 2008 Hans Petter Selasky | BSD-2-Clause | 桥接至 Pixel Neko 轻量兼容层 |
| `drivers/usb/core/usb_hub.c` | FreeBSD | `releng/13.2-RELEASE` | `sys/dev/usb/usb_hub.c` | (c) 2008 Hans Petter Selasky | BSD-2-Clause | 状态机时序调整与动态拓扑适配 |
| `drivers/usb/core/usb_hid.c` | FreeBSD | `releng/13.2-RELEASE` | `sys/dev/usb/usb_hid.c` | (c) 2008 Hans Petter Selasky | BSD-2-Clause | 增加 Boot 3字节与 Report 滚轮分流 |
| `drivers/usb/hcd/usb_ehci.c` | FreeBSD | `releng/13.2-RELEASE` | `sys/dev/usb/controller/ehci.c`| (c) 2001-2008 The FreeBSD Project | BSD-2-Clause | 适配 IAAD 门铃与三步拓扑判断 |
| `drivers/usb/hcd/usb_xhci.c` | FreeBSD | `releng/13.2-RELEASE` | `sys/dev/usb/controller/xhci.c`| (c) 2010-2016 The FreeBSD Project | BSD-2-Clause | 增加 Stop Endpoint 取消同步 |
| `drivers/acpi/uacpi/*` | uACPI | `master (2024+)` | `source/*` | (c) 2023-2024 uACPI Authors | MIT | 轻量 ACPI 命名空间遍历与 _S5 提取 |

## 2. 引导与公共库组件
- **Limine Bootloader v8.x**：BSD-2-Clause。二进制打包进 Hybrid ISO 的 EFI/BOOT 与 boot/limine 目录，保留 LICENSE 于光盘文档中。
- **minimp3**：CC0 / MIT 许可证。保留于 `drivers/audio/minimp3.h`。

## 3. 宿主编译与镜像生成工具（不打入目标内核映像）
- **GCC / Clang**：GPLv3 / Apache 2.0 with LLVM Exception。仅作为交叉构建工具，生成的目标代码不具传染性。
- **NASM**：BSD-2-Clause。
- **xorriso**：GPLv3。仅用于宿主制作 ISO 镜像，工具源码不进入系统运行时。
