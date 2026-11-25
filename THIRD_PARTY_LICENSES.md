# Pixel Neko OS 第三方开源许可证与版权记录

本项目严格区分内核本体代码、外部第三方源码、仅作设计与规范参考的实现、
引导加载程序以及宿主构建工具。

## 1. 第三方源码与参考实现

### 1.1 随项目分发的第三方源码
| 源文件路径 (本项目) | 上游项目 | 上游分支 / Commit | 原始文件路径 | 原始版权声明 | 许可证类型 | 修改概要 |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `drivers/acpi/uacpi/*` | uACPI | `master (2024+)` | `source/*` | (c) 2023-2024 uACPI Authors | MIT | 轻量 ACPI 命名空间遍历与 _S5 提取 |
| `apps/cpuz/libcpuid/*` | libcpuid | `master (0.8.2 snapshot, 2026-11-17)` | `libcpuid/*` | Copyright 2008 Veselin Georgiev and contributors | BSD-2-Clause | 裁剪 Windows/Linux 驱动、MSR 与 ARM 后端；增加 freestanding PXE libc shim、静态线程错误变量和无亲和性 Ring 0 构建 |
| `apps/vplayer/third_party/minimp4/*` | minimp4 | `master` | `minimp4.h` | Public Domain / CC0 1.0 | CC0-1.0 | 用于 Ring 3 视频播放器 MP4/MOV 解封装与 64 位偏移索引解析 |
| `apps/vplayer/third_party/openh264/*` | Cisco OpenH264 | `master` | `codec/{api,common,decoder}/*` | Copyright (c) 2013, Cisco Systems | BSD-2-Clause | 裁剪仅保留 H.264 解码器与 x86-64 NASM SIMD 汇编，通过 freestanding PXE libc/pthread/C++ shim 运行于 Ring 3 |
| `apps/vplayer/third_party/faad2/*` | FAAD2 | `master` | `include/neaacdec.h`, `libfaad/*` | Copyright (c) Nero AG | GPL-2.0 | 以定点 AAC-LC 模式 (`FIXED_POINT` + `LC_ONLY_DECODER`) 编译为 Ring 3 视频播放器音频解码模块 |

### 1.2 FreeBSD 设计与规范参考（非第三方源码）

本项目的 `usb_core.c`、`usb_hub.c`、`usb_hid.c`、`usb_ehci.c` 和
`usb_xhci.c` 在接口与时序设计上参考了 FreeBSD 13.2、USB 规范及
xHCI/EHCI 规范。实现为本项目面向 freestanding 内核独立编写，无上游
FreeBSD 源代码衍生；这些文件不应作为 FreeBSD 移植代码或 BSD-2-Clause
衍生组件分发。

## 2. 引导与公共库组件
- **Limine Bootloader v8.x**：BSD-2-Clause。二进制打包进 Hybrid ISO 的 EFI/BOOT 与 boot/limine 目录，保留 LICENSE 于光盘文档中。
- **minimp3**：CC0 / MIT 许可证。保留于 `drivers/audio/minimp3.h`。
- **GNU Unifont (v16.0.02)**：SIL Open Font License 1.1 / GPLv2+ with Font Exception 双重许可。本项目仅依据 **SIL OFL 1.1** 使用其 16×16 点阵字形生成 `system/FONT_CJK.FNT` 与 `include/cjk_font_core.h`。
- **Zpix (最像素, v3.3.0)**：SIL Open Font License 1.1 (`https://github.com/SolidZORO/zpix-pixel-font`)。用于生成 12×12 小号中文点阵字形至 `system/FONT_CJK.FNT` 与 `include/cjk_font_core.h`。
- **Rime 核心引擎设计参考 (librime)**：BSD-3-Clause 许可（Copyright (c) 2014, RIME Developers，`https://github.com/rime/librime`）。`wm/ime.c` 与 `scripts/gen_ime_dict.py` 的拼音音节分词图（DAG）与维特比整句组词模型参考自 librime 设计（独立 C11 freestanding 实现，未直接编译链接上游 C++ 源码）。
- **Rime 朙月拼音基础词库与八股文频率模型 (`rime-luna-pinyin`, `rime-essay`)**：LGPL-3.0 许可（RIME Developers，`https://github.com/rime/rime-luna-pinyin`、`https://github.com/rime/rime-essay`）。由 `scripts/gen_ime_dict.py` 预编译为系统外部数据词库文件 `system/IME.PNY` 及 `include/ime_core.h`。
- **OpenCC 繁简转换映射表 (`TSCharacters.txt`, `TSPhrases.txt`)**：Apache-2.0 许可（BYVoid/OpenCC，`https://github.com/BYVoid/OpenCC`）。构建期用于将朙月拼音与八股文数据统一转换并筛选为匹配 `FONT_CJK.FNT` 的简体中文词条。

## 3. 宿主编译与镜像生成工具（不打入目标内核映像）
- **GCC / Clang**：GPLv3 / Apache 2.0 with LLVM Exception。仅作为交叉构建工具，生成的目标代码不具传染性。
- **NASM**：BSD-2-Clause。
- **xorriso**：GPLv3。仅用于宿主制作 ISO 镜像，工具源码不进入系统运行时。

## 4. Display timing reference implementations

The ATI Mach64 CRTC/PLL and NVIDIA G7x PCRTC/VPLL register sequences were
implemented against the X.Org `xf86-video-mach64` and `xf86-video-nv` reference
drivers. The Pixel Neko implementations are reduced freestanding code and do
not link either X.Org driver.

- `xf86-video-mach64`: Copyright 1997-2004 Marc Aurele La France; Copyright
  1999-2000 ATI Technologies Inc., Precision Insight, Inc. and VA Linux
  Systems Inc.

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files, to deal in the
  Software without restriction, including without limitation the rights to
  use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
  IN THE SOFTWARE.

- `xf86-video-nv`: Copyright 1993-2003, 2007-2010 NVIDIA Corporation and
  contributors.

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files, to deal in the
  Software without restriction, including without limitation the rights to
  use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
  IN THE SOFTWARE.

Reference sources:

- `https://github.com/X11Libre/xf86-video-mach64`
- `https://github.com/X11Libre/xf86-video-nv`
