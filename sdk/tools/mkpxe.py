#!/usr/bin/env python3
"""
mkpxe.py - Packager for Pixel Neko OS Native Executables (*.pxe).

Combines a declarative .pxi application manifest and an ELF64 x86_64 PIE
executable payload into a single binary *.pxe image with an embedded 16x16 icon
and IEEE 802.3 CRC32 integrity checksum.
"""

import argparse
import struct
import sys
import zlib

PXE_MAGIC = b"PXE\x01"
PXE_ABI_VERSION_1_0 = 0x0100
PXE_ARCH_X86_64 = 0x8664

PXE_HEADER_SIZE = 128
PXE_RSRC_OFFSET = 128
PXE_RSRC_SIZE = 384
PXE_ELF_OFFSET = 4096
PXE_MAX_FILE_BYTES = 8 * 1024 * 1024
PXE_R0_SLOT_STRIDE = 16 * 1024 * 1024

SUBSYSTEM_MAP = {
    "GUI": 1,
    "BACKGROUND": 2,
}

FLAG_MAP = {
    "NONE": 0,
    "SINGLE_INST": 1 << 0,
    "RESIZABLE": 1 << 1,
    "NO_CLOSE": 1 << 2,
    "SYS_SETTINGS": 1 << 3,
}

SUPPORTED_RELA_TYPES = {
    0,  # R_X86_64_NONE
    1,  # R_X86_64_64
    6,  # R_X86_64_GLOB_DAT
    7,  # R_X86_64_JUMP_SLOT
    8,  # R_X86_64_RELATIVE
}

DEFAULT_ICON_R3 = [
    "................",
    ".00000000000000.",
    ".01111111111110.",
    ".011ffffffff110.",
    ".01111111111110.",
    ".0ffffffffffff0.",
    ".0f7777777777f0.",
    ".0f7f77999977f0.",
    ".0f7f779bb977f0.",
    ".0f7f779bb977f0.",
    ".0f7f77999977f0.",
    ".0f7777777777f0.",
    ".0f7777777777f0.",
    ".00000000000000.",
    "................",
    "................",
]

DEFAULT_ICON_R0 = [
    "................",
    "..000000000000..",
    ".04444444444440.",
    ".04cc444444cc40.",
    ".04c4eeeeeeee40.",
    ".044eee00eee440.",
    ".044ee0ee0ee440.",
    ".044ee0ee0ee440.",
    ".044eee00eee440.",
    ".044eeeeeeee440.",
    "..044eeeeee440..",
    "...044eeee440...",
    "....044ee440....",
    ".....004400.....",
    ".......00.......",
    "................",
]


def parse_size(val: str) -> int:
    s = val.strip().upper()
    mult = 1
    if s.endswith("KB"):
        mult = 1024
        s = s[:-2].strip()
    elif s.endswith("K"):
        mult = 1024
        s = s[:-1].strip()
    elif s.endswith("MB"):
        mult = 1024 * 1024
        s = s[:-2].strip()
    elif s.endswith("M"):
        mult = 1024 * 1024
        s = s[:-1].strip()
    return int(s, 0) * mult


def parse_flags(val: str) -> int:
    mask = 0
    for token in val.replace("|", ",").replace("+", ",").split(","):
        t = token.strip().upper()
        if not t:
            continue
        if t not in FLAG_MAP:
            raise ValueError(f"Unknown [app] flag '{t}'")
        mask |= FLAG_MAP[t]
    return mask


def encode_icon_rows(rows) -> bytes:
    out = bytearray(256)
    for y in range(16):
        row_str = rows[y].replace(" ", "").strip()
        if len(row_str) != 16:
            raise ValueError(f"[icon] row{y:02d} must have 16 characters, got {len(row_str)}")
        for x, ch in enumerate(row_str):
            if ch in (".", "_", "-"):
                out[y * 16 + x] = 255
            else:
                try:
                    c = int(ch, 16)
                except ValueError as exc:
                    raise ValueError(f"[icon] row{y:02d} invalid pixel char '{ch}'") from exc
                out[y * 16 + x] = c
    return bytes(out)


def parse_pxi(path: str):
    app = {
        "name": "",
        "title": "",
        "author": "Pixel Neko OS",
        "version": "1.0",
        "ring": 3,
        "subsystem": "GUI",
        "flags": 0,
        "stack_size": 65536,
        "heap_init_size": 131072,
        "def_w": 320,
        "def_h": 220,
        "min_w": 200,
        "min_h": 140,
        "ext_assoc": "",
    }
    icon_rows = {}
    section = None

    with open(path, "r", encoding="utf-8") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith("#") or line.startswith(";"):
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1].strip().lower()
                continue
            if "=" not in line:
                raise ValueError(f"{path}:{lineno}: Invalid line '{line}'")
            key, val = line.split("=", 1)
            key = key.strip().lower()
            val = val.strip()
            # Strip inline comments outside quotes
            if len(val) >= 2 and val[0] == '"':
                end_q = val.find('"', 1)
                if end_q != -1:
                    val = val[1:end_q]
            else:
                for cmt in ("#", ";"):
                    if cmt in val:
                        val = val.split(cmt, 1)[0].strip()

            if section == "app":
                if key == "name":
                    app["name"] = val
                elif key == "title":
                    app["title"] = val
                elif key == "author":
                    app["author"] = val
                elif key == "version":
                    app["version"] = val
                elif key == "ring":
                    ring_val = int(val, 0)
                    if ring_val not in (0, 3):
                        raise ValueError(f"{path}:{lineno}: ring must be 0 or 3, got {ring_val}")
                    app["ring"] = ring_val
                elif key == "subsystem":
                    sub = val.upper()
                    if sub not in SUBSYSTEM_MAP:
                        raise ValueError(f"{path}:{lineno}: unknown subsystem '{val}'")
                    app["subsystem"] = sub
                elif key == "flags":
                    app["flags"] = parse_flags(val)
                elif key == "stack_size":
                    app["stack_size"] = parse_size(val)
                elif key == "heap_init_size":
                    app["heap_init_size"] = parse_size(val)
                elif key == "def_w":
                    app["def_w"] = int(val, 0)
                elif key == "def_h":
                    app["def_h"] = int(val, 0)
                elif key == "min_w":
                    app["min_w"] = int(val, 0)
                elif key == "min_h":
                    app["min_h"] = int(val, 0)
                elif key == "ext_assoc":
                    app["ext_assoc"] = val
                else:
                    raise ValueError(f"{path}:{lineno}: Unknown [app] key '{key}'")
            elif section == "icon":
                if key.startswith("row") and len(key) == 5 and key[3:].isdigit():
                    ridx = int(key[3:], 10)
                    if 0 <= ridx <= 15:
                        icon_rows[ridx] = val
                        continue
                raise ValueError(f"{path}:{lineno}: Unknown [icon] key '{key}'")
            else:
                raise ValueError(f"{path}:{lineno}: Key '{key}' outside [app]/[icon]")

    if not app["name"]:
        raise ValueError(f"{path}: Missing required [app] name")
    if not app["title"]:
        app["title"] = app["name"]

    if len(icon_rows) == 16:
        icon_bytes = encode_icon_rows([icon_rows[i] for i in range(16)])
    elif len(icon_rows) == 0:
        default_rows = DEFAULT_ICON_R0 if app["ring"] == 0 else DEFAULT_ICON_R3
        icon_bytes = encode_icon_rows(default_rows)
    else:
        raise ValueError(f"{path}: [icon] must define all row00..row15 entries (got {len(icon_rows)})")

    return app, icon_bytes


def validate_elf64_pxe(elf_bytes: bytes, elf_path: str, ring: int):
    if len(elf_bytes) < 64:
        raise ValueError(f"{elf_path}: File too small to be ELF64")
    if elf_bytes[:4] != b"\x7fELF":
        raise ValueError(f"{elf_path}: Invalid ELF magic")
    if elf_bytes[4] != 2 or elf_bytes[5] != 1:
        raise ValueError(f"{elf_path}: Expected 64-bit little-endian ELF64")

    (
        e_type,
        e_machine,
        _e_version,
        e_entry,
        e_phoff,
        e_shoff,
        _e_flags,
        _e_ehsize,
        e_phentsize,
        e_phnum,
        e_shentsize,
        e_shnum,
        _e_shstrndx,
    ) = struct.unpack_from("<HHIQQQIHHHHHH", elf_bytes, 16)

    if e_machine != 62:
        raise ValueError(f"{elf_path}: Expected EM_X86_64 (62), got {e_machine}")
    if ring == 0:
        if e_type != 3:
            raise ValueError(f"{elf_path}: Ring 0 PXE must be ELF64 PIE (ET_DYN=3), got {e_type}")
    else:
        if e_type not in (2, 3):
            raise ValueError(f"{elf_path}: Expected ET_DYN (3) or ET_EXEC (2), got {e_type}")

    if e_entry == 0:
        raise ValueError(f"{elf_path}: ELF e_entry cannot be 0")
    if e_phoff == 0 or e_phnum == 0 or e_phentsize < 56:
        raise ValueError(f"{elf_path}: Missing ELF64 program headers")
    if e_phoff + e_phnum * e_phentsize > len(elf_bytes):
        raise ValueError(f"{elf_path}: Truncated ELF64 program headers")

    pt_loads = []
    entry_mapped = False
    for idx in range(e_phnum):
        off = e_phoff + idx * e_phentsize
        p_type, p_flags, p_offset, p_vaddr, _p_paddr, p_filesz, p_memsz, _p_align = (
            struct.unpack_from("<IIQQQQQQ", elf_bytes, off)
        )
        if p_type != 1 or p_memsz == 0:
            continue

        if p_filesz > p_memsz or p_offset + p_filesz > len(elf_bytes):
            raise ValueError(f"{elf_path}: PT_LOAD[{idx}] file bounds exceed ELF size")
        if (p_flags & 3) == 3:
            raise ValueError(f"{elf_path}: PT_LOAD[{idx}] violates W^X (PF_W | PF_X)")
        if ring == 0 and (p_vaddr + p_memsz > PXE_R0_SLOT_STRIDE):
            raise ValueError(
                f"{elf_path}: Ring 0 PT_LOAD[{idx}] exceeds 16 MB slot stride "
                f"(end=0x{p_vaddr + p_memsz:x})"
            )

        page_start = p_vaddr & ~0xFFF
        page_end = (p_vaddr + p_memsz + 0xFFF) & ~0xFFF
        for prev_start, prev_end in pt_loads:
            if not (page_end <= prev_start or page_start >= prev_end):
                raise ValueError(
                    f"{elf_path}: PT_LOAD[{idx}] [0x{page_start:x}, 0x{page_end:x}) overlaps "
                    f"[0x{prev_start:x}, 0x{prev_end:x})"
                )
        pt_loads.append((page_start, page_end))

        if p_vaddr <= e_entry < p_vaddr + p_memsz:
            entry_mapped = True

    if not (1 <= len(pt_loads) <= 4):
        raise ValueError(f"{elf_path}: Expected 1..4 PT_LOAD segments, got {len(pt_loads)}")
    if not entry_mapped:
        raise ValueError(f"{elf_path}: Entry point 0x{e_entry:x} is outside mapped PT_LOADs")

    if e_shoff != 0 and e_shnum > 0 and e_shentsize >= 64:
        if e_shoff + e_shnum * e_shentsize > len(elf_bytes):
            raise ValueError(f"{elf_path}: Truncated ELF64 section headers")
        sections = []
        for idx in range(e_shnum):
            off = e_shoff + idx * e_shentsize
            sections.append(struct.unpack_from("<IIQQQQIIQQ", elf_bytes, off))

        for idx, sh in enumerate(sections):
            sh_type, _sh_flags, _sh_addr, sh_offset, sh_size, sh_link, _sh_info, _sh_align, sh_entsize = (
                sh[1], sh[2], sh[3], sh[4], sh[5], sh[6], sh[7], sh[8], sh[9]
            )
            if sh_type == 19:  # SHT_RELR
                raise ValueError(f"{elf_path}: Unsupported SHT_RELR compressed relocations")
            if sh_type == 4 and sh_size > 0:  # SHT_RELA
                entsize = sh_entsize if sh_entsize >= 24 else 24
                if sh_offset + sh_size > len(elf_bytes):
                    raise ValueError(f"{elf_path}: Truncated SHT_RELA section {idx}")
                for ridx in range(sh_size // entsize):
                    _r_off, r_info, _r_add = struct.unpack_from(
                        "<QQq", elf_bytes, sh_offset + ridx * entsize
                    )
                    rtype = r_info & 0xFFFFFFFF
                    if rtype not in SUPPORTED_RELA_TYPES:
                        raise ValueError(
                            f"{elf_path}: Unsupported x86_64 relocation type {rtype} in section {idx}"
                        )
            elif sh_type in (2, 11) and sh_size > 0:  # SHT_SYMTAB or SHT_DYNSYM
                if sh_link >= len(sections):
                    raise ValueError(f"{elf_path}: Invalid symbol table sh_link in section {idx}")
                str_sh = sections[sh_link]
                strtab = elf_bytes[str_sh[4] : str_sh[4] + str_sh[5]]
                entsize = sh_entsize if sh_entsize >= 24 else 24
                for sidx in range(sh_size // entsize):
                    st_name, st_info, _st_other, st_shndx, _st_val, _st_sz = struct.unpack_from(
                        "<IBBHQQ", elf_bytes, sh_offset + sidx * entsize
                    )
                    st_bind = st_info >> 4
                    end = strtab.find(b"\x00", st_name)
                    sym_name = (
                        strtab[st_name:end].decode("utf-8", errors="replace")
                        if end != -1
                        else ""
                    )
                    if sidx > 0 and st_shndx == 0 and sym_name and st_bind in (1, 2):
                        raise ValueError(
                            f"{elf_path}: Unresolved external symbol '{sym_name}' "
                            f"(use libpxe_r{ring}.a / <pxe/*.h> instead)"
                        )

    return len(pt_loads), e_entry


def pack_fixed_str(text: str, length: int) -> bytes:
    raw = text.encode("utf-8")[: length - 1]
    return raw + b"\x00" * (length - len(raw))


def main():
    parser = argparse.ArgumentParser(description="Build Pixel Neko OS *.pxe application image")
    parser.add_argument("--manifest", required=True, help="Input .pxi manifest file")
    parser.add_argument("--elf", required=True, help="Input linked ELF64 PIE executable")
    parser.add_argument("--output", required=True, help="Output .pxe binary file")
    args = parser.parse_args()

    app, icon_bytes = parse_pxi(args.manifest)
    with open(args.elf, "rb") as f:
        elf_bytes = f.read()

    seg_count, entry_va = validate_elf64_pxe(elf_bytes, args.elf, app["ring"])

    rsrc_blob = struct.pack(
        "<256s48s32s32s16s",
        icon_bytes,
        pack_fixed_str(app["title"], 48),
        pack_fixed_str(app["author"], 32),
        pack_fixed_str(app["ext_assoc"], 32),
        b"\x00" * 16,
    )
    assert len(rsrc_blob) == PXE_RSRC_SIZE

    pad_len = PXE_ELF_OFFSET - (PXE_HEADER_SIZE + PXE_RSRC_SIZE)
    assert pad_len >= 0

    header_zero_crc = struct.pack(
        "<4sHHBBHIIIIIIIHHHHI32s16s28s",
        PXE_MAGIC,
        PXE_ABI_VERSION_1_0,
        PXE_ARCH_X86_64,
        app["ring"],
        SUBSYSTEM_MAP[app["subsystem"]],
        app["flags"],
        PXE_HEADER_SIZE,
        PXE_RSRC_OFFSET,
        PXE_RSRC_SIZE,
        PXE_ELF_OFFSET,
        len(elf_bytes),
        app["stack_size"],
        app["heap_init_size"],
        app["def_w"],
        app["def_h"],
        app["min_w"],
        app["min_h"],
        0,  # checksum_crc32 placeholder at offset 0x30..0x33
        pack_fixed_str(app["name"], 32),
        pack_fixed_str(app["version"], 16),
        b"\x00" * 28,
    )
    assert len(header_zero_crc) == PXE_HEADER_SIZE

    image_zero_crc = header_zero_crc + rsrc_blob + (b"\x00" * pad_len) + elf_bytes
    if len(image_zero_crc) > PXE_MAX_FILE_BYTES:
        raise ValueError(
            f"Final PXE size ({len(image_zero_crc)} bytes) exceeds {PXE_MAX_FILE_BYTES}B limit"
        )

    crc32_val = zlib.crc32(image_zero_crc) & 0xFFFFFFFF
    final_header = (
        header_zero_crc[:0x30]
        + struct.pack("<I", crc32_val)
        + header_zero_crc[0x34:]
    )
    final_image = final_header + rsrc_blob + (b"\x00" * pad_len) + elf_bytes

    with open(args.output, "wb") as f:
        f.write(final_image)

    print(
        f"[mkpxe] {args.output}: name='{app['name']}' ring={app['ring']} "
        f"flags=0x{app['flags']:x} segs={seg_count} entry=0x{entry_va:x} "
        f"elf={len(elf_bytes)}B total={len(final_image)}B crc32=0x{crc32_val:08x}"
    )


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"[mkpxe] ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
