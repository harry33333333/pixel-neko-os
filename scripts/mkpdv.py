#!/usr/bin/env python3
"""
mkpdv.py - Packager for Pixel Neko OS native self-contained drivers (*.pdv).

Combines a declarative .pdi manifest and a freestanding x86_64 ELF64 ET_REL (.o)
payload into a single binary *.pdv driver image with IEEE 802.3 CRC32 integrity.
"""

import argparse
import struct
import sys
import zlib

PDV_MAGIC = b"PDV1"
PDV_FORMAT_VERSION = 0x0100
PDV_HEADER_SIZE = 128
PDV_RULE_SIZE = 16
PDV_PAGE_ALIGN = 4096

CLASS_MAP = {
    "UNKNOWN": 0,
    "DISPLAY": 1,
    "AUDIO": 2,
    "INPUT": 3,
    "STORAGE": 4,
    "NETWORK": 5,
    "SYSTEM": 6,
}

START_MAP = {
    "BOOT": 1 << 0,
    "DEMAND": 1 << 1,
}

MATCH_ANY = 0
MATCH_PCI_ID = 1
MATCH_PCI_SUBSYS = 2
MATCH_PCI_CLASS = 3
MATCH_PCI_VEN_CLASS = 4
MATCH_ISA_PNP = 5

ALLOWED_UNDEF_SYMBOLS = {
    "",
    "memcpy",
    "memset",
    "memmove",
    "memcmp",
    "strlen",
    "strcmp",
    "strncmp",
}


def parse_int_or_wildcard(token: str, bits: int) -> int:
    t = token.strip()
    if t == "*":
        return (1 << bits) - 1
    val = int(t, 0)
    if val < 0 or val >= (1 << bits):
        raise ValueError(f"Value '{token}' out of {bits}-bit unsigned range")
    return val


def parse_kv_tokens(rhs: str) -> dict:
    result = {}
    for part in rhs.replace(",", " ").split():
        if "=" not in part:
            raise ValueError(f"Expected key=value in match rule, got '{part}'")
        k, v = part.split("=", 1)
        result[k.strip().lower()] = v.strip()
    return result


def parse_pci_rule(rhs: str) -> bytes:
    kv = parse_kv_tokens(rhs)
    ven = parse_int_or_wildcard(kv.get("ven", "*"), 16)
    dev = parse_int_or_wildcard(kv.get("dev", "*"), 16)
    subven = parse_int_or_wildcard(kv.get("subven", "*"), 16)
    subdev = parse_int_or_wildcard(kv.get("subdev", "*"), 16)
    cls = parse_int_or_wildcard(kv.get("class", "*"), 8)
    subcls = parse_int_or_wildcard(kv.get("subclass", "*"), 8)
    progif = parse_int_or_wildcard(kv.get("progif", "*"), 8)

    has_ven = ven != 0xFFFF
    has_dev = dev != 0xFFFF
    has_subsys = (subven != 0xFFFF) or (subdev != 0xFFFF)
    has_cls = (cls != 0xFF) or (subcls != 0xFF)

    if has_ven and has_dev and has_subsys:
        match_type = MATCH_PCI_SUBSYS
    elif has_ven and has_dev:
        match_type = MATCH_PCI_ID
    elif has_ven and has_cls:
        match_type = MATCH_PCI_VEN_CLASS
    elif has_cls:
        match_type = MATCH_PCI_CLASS
    else:
        raise ValueError(f"Ambiguous or empty PCI match rule: '{rhs}'")

    return struct.pack(
        "<BBBBHHHHHBB",
        match_type,
        cls,
        subcls,
        progif,
        ven,
        dev,
        subven,
        subdev,
        0xFFFF,
        0xFF,
        0xFF,
    )


def parse_isa_rule(rhs: str) -> bytes:
    kv = parse_kv_tokens(rhs)
    port = parse_int_or_wildcard(kv.get("port", "*"), 16)
    irq = parse_int_or_wildcard(kv.get("irq", "*"), 8)
    dma = parse_int_or_wildcard(kv.get("dma", "*"), 8)
    return struct.pack(
        "<BBBBHHHHHBB",
        MATCH_ISA_PNP,
        0xFF,
        0xFF,
        0xFF,
        0xFFFF,
        0xFFFF,
        0xFFFF,
        0xFFFF,
        port,
        irq,
        dma,
    )


def parse_any_rule() -> bytes:
    return struct.pack(
        "<BBBBHHHHHBB",
        MATCH_ANY,
        0xFF,
        0xFF,
        0xFF,
        0xFFFF,
        0xFFFF,
        0xFFFF,
        0xFFFF,
        0xFFFF,
        0xFF,
        0xFF,
    )


def parse_pdi(path: str):
    driver_info = {
        "name": "",
        "provider": "Pixel Neko OS",
        "class": "UNKNOWN",
        "version": "1.0.0",
        "ddk_version": 0x0100,
        "start": "BOOT",
        "safe_mode": False,
    }
    rules = []
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
            if len(val) >= 2 and val[0] == '"' and val[-1] == '"':
                val = val[1:-1]

            if section == "driver":
                if key == "name":
                    driver_info["name"] = val
                elif key == "provider":
                    driver_info["provider"] = val
                elif key == "class":
                    driver_info["class"] = val.upper()
                elif key == "version":
                    driver_info["version"] = val
                elif key == "ddk_version":
                    driver_info["ddk_version"] = int(val, 0)
                elif key == "start":
                    driver_info["start"] = val.upper()
                elif key == "safe_mode":
                    driver_info["safe_mode"] = val.lower() in ("1", "true", "yes")
                else:
                    raise ValueError(f"{path}:{lineno}: Unknown [driver] key '{key}'")
            elif section == "match":
                if key == "pci":
                    rules.append(parse_pci_rule(val))
                elif key == "isa":
                    rules.append(parse_isa_rule(val))
                elif key == "any":
                    rules.append(parse_any_rule())
                else:
                    raise ValueError(f"{path}:{lineno}: Unknown [match] key '{key}'")
            else:
                raise ValueError(f"{path}:{lineno}: Key '{key}' outside [driver]/[match]")

    if not driver_info["name"]:
        raise ValueError(f"{path}: Missing required [driver] name")
    if driver_info["class"] not in CLASS_MAP:
        raise ValueError(f"{path}: Unknown driver class '{driver_info['class']}'")
    if driver_info["start"] not in START_MAP:
        raise ValueError(f"{path}: Unknown start mode '{driver_info['start']}'")
    if not rules:
        raise ValueError(f"{path}: At least one [match] rule is required")

    return driver_info, rules


def validate_elf64_rel(elf_bytes: bytes, obj_path: str):
    if len(elf_bytes) < 64:
        raise ValueError(f"{obj_path}: File too small to be ELF64")
    if elf_bytes[:4] != b"\x7fELF":
        raise ValueError(f"{obj_path}: Invalid ELF magic")
    ei_class, ei_data = elf_bytes[4], elf_bytes[5]
    if ei_class != 2 or ei_data != 1:
        raise ValueError(f"{obj_path}: Expected 64-bit little-endian ELF")

    (
        e_type,
        e_machine,
        _e_version,
        _e_entry,
        _e_phoff,
        e_shoff,
        _e_flags,
        _e_ehsize,
        e_phentsize,
        _e_phnum,
        e_shentsize,
        e_shnum,
        e_shstrndx,
    ) = struct.unpack_from("<HHIQQQIHHHHHH", elf_bytes, 16)

    if e_type != 1:
        raise ValueError(f"{obj_path}: Expected ET_REL (1), got e_type={e_type}")
    if e_machine != 62:
        raise ValueError(f"{obj_path}: Expected EM_X86_64 (62), got e_machine={e_machine}")
    if e_shoff == 0 or e_shnum == 0 or e_shentsize < 64:
        raise ValueError(f"{obj_path}: Missing ELF64 section headers")
    if e_shoff + e_shnum * e_shentsize > len(elf_bytes):
        raise ValueError(f"{obj_path}: Truncated ELF64 section headers")

    sections = []
    for idx in range(e_shnum):
        off = e_shoff + idx * e_shentsize
        sh = struct.unpack_from("<IIQQQQIIQQ", elf_bytes, off)
        sections.append(sh)
        sh_flags = sh[2]
        # SHF_WRITE (0x1) | SHF_EXECINSTR (0x4) must never coexist (W^X)
        if (sh_flags & 0x5) == 0x5:
            raise ValueError(f"{obj_path}: Section {idx} violates W^X (WRITE|EXEC)")

    found_entry = False
    for idx, sh in enumerate(sections):
        sh_type, _sh_flags, _sh_addr, sh_offset, sh_size, sh_link, _sh_info, _sh_align, sh_entsize = (
            sh[1], sh[2], sh[3], sh[4], sh[5], sh[6], sh[7], sh[8], sh[9]
        )
        if sh_type != 2:  # SHT_SYMTAB
            continue
        if sh_link >= len(sections):
            raise ValueError(f"{obj_path}: Invalid SHT_SYMTAB sh_link")
        str_sh = sections[sh_link]
        str_off, str_size = str_sh[4], str_sh[5]
        strtab = elf_bytes[str_off : str_off + str_size]
        entsize = sh_entsize if sh_entsize >= 24 else 24
        count = sh_size // entsize
        for sidx in range(count):
            soff = sh_offset + sidx * entsize
            st_name, st_info, _st_other, st_shndx, _st_value, _st_size = struct.unpack_from(
                "<IBBHQQ", elf_bytes, soff
            )
            end = strtab.find(b"\x00", st_name)
            sym_name = strtab[st_name:end].decode("utf-8", errors="replace") if end != -1 else ""
            st_bind = st_info >> 4
            if sym_name == "pdv_driver_entry" and st_shndx != 0 and st_bind in (1, 2):
                found_entry = True
            if sidx > 0 and st_shndx == 0 and sym_name not in ALLOWED_UNDEF_SYMBOLS:
                raise ValueError(
                    f"{obj_path}: Forbidden external symbol '{sym_name}' "
                    f"(use kapi->... instead of direct kernel globals)"
                )

    if not found_entry:
        raise ValueError(f"{obj_path}: Missing global 'pdv_driver_entry' symbol")


def pack_fixed_str(text: str, length: int) -> bytes:
    raw = text.encode("utf-8")[: length - 1]
    return raw + b"\x00" * (length - len(raw))


def main():
    parser = argparse.ArgumentParser(description="Build Pixel Neko OS *.pdv driver image")
    parser.add_argument("--manifest", required=True, help="Input .pdi manifest file")
    parser.add_argument("--object", required=True, help="Input ELF64 ET_REL (.o) file")
    parser.add_argument("--output", required=True, help="Output .pdv binary file")
    args = parser.parse_args()

    driver_info, rules = parse_pdi(args.manifest)
    with open(args.object, "rb") as f:
        elf_bytes = f.read()

    validate_elf64_rel(elf_bytes, args.object)

    match_blob = b"".join(rules)
    after_rules = PDV_HEADER_SIZE + len(match_blob)
    elf_off = (after_rules + PDV_PAGE_ALIGN - 1) & ~(PDV_PAGE_ALIGN - 1)
    pad_len = elf_off - after_rules

    flags = START_MAP[driver_info["start"]]
    if driver_info["safe_mode"]:
        flags |= 1 << 2

    header_zero_crc = struct.pack(
        "<4sHHHHIIIII32s32s16s16s",
        PDV_MAGIC,
        PDV_FORMAT_VERSION,
        driver_info["ddk_version"],
        CLASS_MAP[driver_info["class"]],
        flags,
        len(rules),
        PDV_HEADER_SIZE,
        elf_off,
        len(elf_bytes),
        0,  # file_crc32 placeholder
        pack_fixed_str(driver_info["name"], 32),
        pack_fixed_str(driver_info["provider"], 32),
        pack_fixed_str(driver_info["version"], 16),
        b"\x00" * 16,
    )
    assert len(header_zero_crc) == PDV_HEADER_SIZE

    image_zero_crc = header_zero_crc + match_blob + (b"\x00" * pad_len) + elf_bytes
    crc32_val = zlib.crc32(image_zero_crc) & 0xFFFFFFFF

    final_header = (
        header_zero_crc[:28]
        + struct.pack("<I", crc32_val)
        + header_zero_crc[32:]
    )
    final_image = final_header + match_blob + (b"\x00" * pad_len) + elf_bytes

    with open(args.output, "wb") as f:
        f.write(final_image)

    print(
        f"[mkpdv] {args.output}: name='{driver_info['name']}' "
        f"class={driver_info['class']} rules={len(rules)} "
        f"elf={len(elf_bytes)}B total={len(final_image)}B crc32=0x{crc32_val:08x}"
    )


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"[mkpdv] ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
