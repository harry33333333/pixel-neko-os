# Pixel Neko OS 第三方开源许可证与版权记录

本项目严格区分内核本体代码、外部第三方源码、仅作设计与规范参考的实现、
引导加载程序以及宿主构建工具。

## 1. 第三方源码与参考实现

### 1.1 随项目分发的第三方源码
| 源文件路径 (本项目) | 上游项目 | 上游分支 / Commit | 原始文件路径 | 原始版权声明 | 许可证类型 | 修改概要 |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `drivers/acpi/uacpi/*` | uACPI | `master (2024+)` | `source/*` | (c) 2023-2024 uACPI Authors | MIT | 轻量 ACPI 命名空间遍历与 _S5 提取 |

### 1.2 FreeBSD 设计与规范参考（非第三方源码）

本项目的 `usb_core.c`、`usb_hub.c`、`usb_hid.c`、`usb_ehci.c` 和
`usb_xhci.c` 在接口与时序设计上参考了 FreeBSD 13.2、USB 规范及
xHCI/EHCI 规范。实现为本项目面向 freestanding 内核独立编写，无上游
FreeBSD 源代码衍生；这些文件不应作为 FreeBSD 移植代码或 BSD-2-Clause
衍生组件分发。

## 2. 引导与公共库组件
- **Limine Bootloader v8.x**：BSD-2-Clause。二进制打包进 Hybrid ISO 的 EFI/BOOT 与 boot/limine 目录，保留 LICENSE 于光盘文档中。
- **minimp3**：CC0 / MIT 许可证。保留于 `drivers/audio/minimp3.h`。

## 3. 宿主编译与镜像生成工具（不打入目标内核映像）
- **GCC / Clang**：GPLv3 / Apache 2.0 with LLVM Exception。仅作为交叉构建工具，生成的目标代码不具传染性。
- **NASM**：BSD-2-Clause。
- **xorriso**：GPLv3。仅用于宿主制作 ISO 镜像，工具源码不进入系统运行时。
