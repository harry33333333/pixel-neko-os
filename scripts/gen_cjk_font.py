#!/usr/bin/env python3
"""
Generate Pixel Neko OS CJK bitmap font assets:
  1. system/FONT_CJK.FNT        (Full GB2312 + CJK punctuation, 16x16 & 12x12)
  2. include/cjk_font_core.h    (Built-in ~500-glyph core fallback blob)

Font sources (both SIL OFL 1.1):
  - 16x16: GNU Unifont 16.0.02 (dual-licensed SIL OFL 1.1 / GPLv2+FE)
  - 12x12: Zpix v3.3.0 (SIL OFL 1.1), fallback-scaled from Unifont for rare symbols
"""

import argparse
import gzip
import os
import struct
import sys

NKFT_MAGIC = 0x54464B4E  # "NKFT"
NKFT_VERSION = 1

CORE_TEXT = (
    # Punctuation & symbols
    "　、。·ˉˇ¨〃々—～‖…‘’“”〔〕〈〉《》「」『』【】±×÷∶∧∨∑∏∪∩∈∷√⊥∥∠⌒⊙∫∮≡≌≈∽∝≠≮≯≤≥∞∵∴"
    "♂♀°′″℃＄¤￠£‰§№☆★○●◎◇◆□■△▲※→←↑↓＝！"
    "＂＃￥％＆＇（）＊＋，－．／：；＜＝＞？＠［＼］＾＿｀｛｜｝～"
    # UI, Desktop, File Manager, Notepad, Control Panel, Installer, System & Common CJK
    "中文测试新建文件夹系统控制面板记事本计算器时钟管理图片媒体播放器信息关于任务安装重启关机"
    "确定取消打开保存删除重命名复制粘贴剪切撤销全选清除刷新属性帮助设置桌面壁纸屏幕分辨率色深"
    "音量静音模式亮度语言简体英文目录路径磁盘分区格式化挂载弹出驱动硬件内存处理器显卡声卡网络"
    "用户权限进程运行错误警告提示成功失败正在加载完成返回上步下跳过退出创建修改查看编辑视图"
    "大小名称类型时间日期秒状态版本版权许可猫咪像素启动引导光盘镜像本地实时当前默认确认是否"
    "的一是在不了有和人这中大为上个国我以要他时来用们生到作地于出就分对成会可主发年动同工也能"
    "下过子说产种面而方后多定行学法所民得经十三之进着等部度家电力里如水化高自二理起小物现实加"
    "量都两体制机当使点从业本去把性好应开它合还因由其些然前外天政四日那社义事平形相全表间样与"
    "关各重新线内数正心反你明看原又么利比或但质气第向道命此变条只没结解问意建月公无系军很情者"
    "最立代想已通并提直题党程展五果料象员革位入常文总次品式活设及管特件长求老头基资边流路级少"
    "图山统接知较将组见计别她手角期根论运农指几九区强放决西被干做必战先回则任取据处队南给色光"
    "门即保治北造百规热领七海口东导器压志世金增争济阶油思术极交受联什认六共权收证改清己美再采"
    "转更单风切打白教速花带安场身车例真务具万每目至达走积示议声报斗完类八离华名确才科张信马节"
    "话米整空元况今集温传土许步群广石记需段研界拉林律叫且究观越织装影算低持音众书布复容儿须际"
    "商非验连断深难近矿千周委素技备半办青省列习响约支般史感劳便团往酸历市克何除消构府称太准精"
    "值号率族维划选标写存候毛亲快效斯院查江型眼王按格养易置派层片始却专状育厂京识适属圆包火住"
    "调满县局照参红细引听该铁价严龙飞"
)


def parse_unifont_hex(path):
    glyphs = {}
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rt", encoding="ascii", errors="ignore") as f:
        for line in f:
            line = line.strip()
            if ":" not in line:
                continue
            hx_cp, hx_data = line.split(":", 1)
            cp = int(hx_cp, 16)
            if cp > 0xFFFF:
                continue
            raw = bytes.fromhex(hx_data)
            if len(raw) == 32:
                glyphs[cp] = raw
            elif len(raw) == 16:
                # Center 8x16 glyph inside 16x16 cell
                out = bytearray(32)
                for r in range(16):
                    val = raw[r] << 4
                    out[r * 2] = (val >> 8) & 0xFF
                    out[r * 2 + 1] = val & 0xFF
                glyphs[cp] = bytes(out)
    return glyphs


def parse_bdf_12x12(path):
    glyphs = {}
    font_ascent = 10
    cur = None
    in_bmp = False
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        for raw_line in f:
            line = raw_line.strip()
            if line.startswith("FONT_ASCENT "):
                font_ascent = int(line.split()[1])
            elif line.startswith("STARTCHAR"):
                cur = {"bmp": [], "bbx": [0, 0, 0, 0]}
                in_bmp = False
            elif cur is not None:
                if line.startswith("ENCODING "):
                    cur["enc"] = int(line.split()[1])
                elif line.startswith("BBX "):
                    cur["bbx"] = [int(x) for x in line.split()[1:5]]
                elif line == "BITMAP":
                    in_bmp = True
                elif line == "ENDCHAR":
                    cp = cur.get("enc", -1)
                    if 0 <= cp <= 0xFFFF:
                        bw, bh, xoff, yoff = cur["bbx"]
                        rows12 = [0] * 12
                        start_row = font_ascent - yoff - bh
                        shift_x = xoff
                        if shift_x < 0:
                            shift_x = 0
                        for r_idx, hx in enumerate(cur["bmp"]):
                            if r_idx >= bh:
                                break
                            dy = start_row + r_idx
                            if 0 <= dy < 12:
                                val = int(hx, 16)
                                row_bytes = (len(hx) + 1) // 2
                                # Normalize MSB to bit 15
                                bits16 = (val << (16 - row_bytes * 8)) & 0xFFFF
                                bits16 = (bits16 >> shift_x) & 0xFFF0
                                rows12[dy] |= bits16
                        out = bytearray(24)
                        for r in range(12):
                            out[r * 2] = (rows12[r] >> 8) & 0xFF
                            out[r * 2 + 1] = rows12[r] & 0xFF
                        glyphs[cp] = bytes(out)
                    cur = None
                    in_bmp = False
                elif in_bmp:
                    cur["bmp"].append(line)
    return glyphs


def downsample_16_to_12(g16):
    out = bytearray(24)
    for dy in range(12):
        sy0 = (dy * 16) // 12
        sy1 = ((dy + 1) * 16 + 11) // 12
        if sy1 <= sy0:
            sy1 = sy0 + 1
        row_bits = 0
        for dx in range(12):
            sx0 = (dx * 16) // 12
            sx1 = ((dx + 1) * 16 + 11) // 12
            if sx1 <= sx0:
                sx1 = sx0 + 1
            on = False
            for sy in range(sy0, min(sy1, 16)):
                r16 = (g16[sy * 2] << 8) | g16[sy * 2 + 1]
                for sx in range(sx0, min(sx1, 16)):
                    if r16 & (0x8000 >> sx):
                        on = True
                        break
                if on:
                    break
            if on:
                row_bits |= (0x8000 >> dx)
        out[dy * 2] = (row_bits >> 8) & 0xFF
        out[dy * 2 + 1] = row_bits & 0xFF
    return bytes(out)


def collect_gb2312_codepoints():
    cps = set()
    for b1 in range(0xA1, 0xF8):
        for b2 in range(0xA1, 0xFF):
            try:
                ch = bytes([b1, b2]).decode("gb2312")
                if len(ch) == 1 and ord(ch) >= 0x80:
                    cps.add(ord(ch))
            except UnicodeDecodeError:
                pass
    for ch in CORE_TEXT:
        if ord(ch) >= 0x80:
            cps.add(ord(ch))
    # Common CJK punctuation & Hiragana/Katakana extras
    for cp in range(0x3000, 0x3018):
        cps.add(cp)
    for cp in range(0xFF01, 0xFF5F):
        cps.add(cp)
    cps.add(0xFFE5)  # ￥
    cps.add(0xFFFD)  # replacement character
    return sorted(cps)


def build_nkft_blob(codepoints, g16_map, g12_map):
    cps = sorted(cp for cp in codepoints if 0x80 <= cp <= 0xFFFF and cp in g16_map)
    n = len(cps)
    cp_offset = 32
    cp_bytes_len = n * 2
    # Align g16_offset to 4 bytes
    g16_offset = (cp_offset + cp_bytes_len + 3) & ~3
    g16_bytes_len = n * 32
    g12_offset = g16_offset + g16_bytes_len
    g12_bytes_len = n * 24
    total_size = g12_offset + g12_bytes_len

    buf = bytearray(total_size)
    struct.pack_into(
        "<IHHIIIIII",
        buf,
        0,
        NKFT_MAGIC,
        NKFT_VERSION,
        0,
        n,
        cp_offset,
        g16_offset,
        g12_offset,
        total_size,
        0,
    )

    for i, cp in enumerate(cps):
        struct.pack_into("<H", buf, cp_offset + i * 2, cp)
        g16 = g16_map[cp]
        buf[g16_offset + i * 32 : g16_offset + (i + 1) * 32] = g16
        g12 = g12_map.get(cp)
        if g12 is None:
            g12 = downsample_16_to_12(g16)
        buf[g12_offset + i * 24 : g12_offset + (i + 1) * 24] = g12
    return bytes(buf), n


def write_core_header(out_path, blob, glyph_count):
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("#ifndef _CJK_FONT_CORE_H\n")
        f.write("#define _CJK_FONT_CORE_H\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(
            f"/* Built-in NKFT core fallback font ({glyph_count} glyphs, "
            f"{len(blob)} bytes, SIL OFL 1.1) */\n"
        )
        f.write(f"static const uint8_t g_cjk_font_core_blob[{len(blob)}] = {{\n")
        for off in range(0, len(blob), 16):
            chunk = blob[off : off + 16]
            line = ", ".join(f"0x{b:02X}" for b in chunk)
            if off + 16 < len(blob):
                f.write(f"    {line},\n")
            else:
                f.write(f"    {line}\n")
        f.write("};\n\n")
        f.write("#endif /* _CJK_FONT_CORE_H */\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--unifont", required=True, help="Path to unifont .hex or .hex.gz")
    parser.add_argument("--zpix", required=True, help="Path to zpix.bdf")
    parser.add_argument("--out-fnt", required=True, help="Output FONT_CJK.FNT path")
    parser.add_argument("--out-core-h", required=True, help="Output cjk_font_core.h path")
    args = parser.parse_args()

    print("Loading Unifont 16x16...")
    g16_map = parse_unifont_hex(args.unifont)
    print(f"  Loaded {len(g16_map)} 16x16 glyphs.")

    print("Loading Zpix 12x12...")
    g12_map = parse_bdf_12x12(args.zpix)
    print(f"  Loaded {len(g12_map)} 12x12 glyphs.")

    full_cps = collect_gb2312_codepoints()
    full_blob, full_count = build_nkft_blob(full_cps, g16_map, g12_map)
    os.makedirs(os.path.dirname(args.out_fnt), exist_ok=True)
    with open(args.out_fnt, "wb") as f:
        f.write(full_blob)
    print(f"Wrote {args.out_fnt}: {full_count} glyphs, {len(full_blob)} bytes")

    core_cps = sorted({ord(ch) for ch in CORE_TEXT if ord(ch) >= 0x80} | {0xFFFD})
    core_blob, core_count = build_nkft_blob(core_cps, g16_map, g12_map)
    write_core_header(args.out_core_h, core_blob, core_count)
    print(f"Wrote {args.out_core_h}: {core_count} glyphs, {len(core_blob)} bytes")


if __name__ == "__main__":
    main()

