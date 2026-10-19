#!/usr/bin/env python3
"""Build a plain bootable FreeDOS 1.44M floppy image for the integration test.

The boot sector, KERNEL.SYS and COMMAND.COM are taken from an existing
FreeDOS-formatted image (by default assets/floppy/awardflash1440.img, which is
also produced from the FreeDOS 1.2 boot floppy).  The FAT12 filesystem is
rebuilt from scratch with just the kernel, the shell and a minimal
AUTOEXEC.BAT.  No AWDFLASH payload is included, so the machine ends at the
A:\\> command prompt.

Usage:
    python3 tools/make_freedos_floppy.py \
        --base assets/floppy/awardflash1440.img \
        --out  assets/floppy/freedos1440.img
"""
from __future__ import annotations

import argparse
import struct
from pathlib import Path

AUTOEXEC = b"@ECHO OFF\r\nPROMPT $P$G\r\n"


def u16(buf: bytearray, off: int) -> int:
    return struct.unpack_from("<H", buf, off)[0]


def u32(buf: bytearray, off: int) -> int:
    return struct.unpack_from("<I", buf, off)[0]


def put16(buf: bytearray, off: int, value: int) -> None:
    struct.pack_into("<H", buf, off, value)


def put32(buf: bytearray, off: int, value: int) -> None:
    struct.pack_into("<I", buf, off, value)


def fat12_get(fat: bytearray, n: int) -> int:
    off = n + n // 2
    value = fat[off] | (fat[off + 1] << 8)
    return (value >> 4) & 0xFFF if n & 1 else value & 0xFFF


def fat12_set(fat: bytearray, n: int, value: int) -> None:
    off = n + n // 2
    old = fat[off] | (fat[off + 1] << 8)
    new = (old & 0x000F) | ((value & 0xFFF) << 4) if n & 1 else (old & 0xF000) | (value & 0xFFF)
    fat[off] = new & 0xFF
    fat[off + 1] = (new >> 8) & 0xFF


def read_file(img: bytearray, fat: bytearray, root_start: int, root_entries: int,
              data_start: int, cluster_bytes: int, name: bytes) -> bytes:
    for i in range(root_entries):
        entry = root_start + i * 32
        if img[entry] == 0:
            break
        if bytes(img[entry:entry + 11]) != name:
            continue
        first = u16(img, entry + 26)
        size = u32(img, entry + 28)
        out = bytearray()
        cluster = first
        while 2 <= cluster < 0xFF8 and len(out) < size:
            off = data_start + (cluster - 2) * cluster_bytes
            out += img[off:off + cluster_bytes]
            cluster = fat12_get(fat, cluster)
        return bytes(out[:size])
    raise SystemExit(f"base image does not contain {name!r}")


def dos83_name(name: str, path: Path) -> bytes:
    """Validate a DOS 8.3 name and return its 11-byte padded form."""
    if "." in name:
        base, ext = name.rsplit(".", 1)
    else:
        base, ext = name, ""
    base = base.upper()
    ext = ext.upper()
    if not base or len(base) > 8 or len(ext) > 3:
        raise SystemExit(f"{path}: {name!r} does not fit DOS 8.3")
    for part in (base, ext):
        if any(not (c.isalnum() or c in "_-$~!#%&'()@^`{}") for c in part):
            raise SystemExit(f"{path}: {name!r} is not a valid DOS 8.3 name")
    return base.encode().ljust(8) + ext.encode().ljust(3)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", type=Path, default=Path("assets/floppy/awardflash1440.img"))
    ap.add_argument("--out", type=Path, default=Path("assets/floppy/freedos1440.img"))
    ap.add_argument("--autoexec", type=Path, default=None,
                    help="optional AUTOEXEC.BAT to install (default: @ECHO OFF + PROMPT)")
    ap.add_argument("--no-autoexec", action="store_true")
    ap.add_argument("--dir", action="append", default=[], metavar="PATH",
                    help="copy every file from this directory into the image "
                         "root, keeping its 8.3 name (repeatable)")
    ap.add_argument("--file", action="append", default=[], metavar="NAME=PATH",
                    help="copy an additional file into the image root (repeatable); "
                         "NAME is the 8.3 name without a dot, e.g. MOUSE=COM")
    args = ap.parse_args()

    base = bytearray(args.base.read_bytes())
    if len(base) != 1474560 or base[510:512] != b"\x55\xaa":
        raise SystemExit("base must be a bootable 1.44M FreeDOS image")

    bps, spc, reserved, fats, root_entries, total, fat_size = (
        u16(base, 11), base[13], u16(base, 14), base[16], u16(base, 17),
        u16(base, 19), u16(base, 22))
    if (bps, spc, fats) != (512, 1, 2):
        raise SystemExit("unsupported BPB")

    root_start = (reserved + fats * fat_size) * bps
    data_start = root_start + root_entries * 32
    cluster_bytes = bps * spc
    max_cluster = (total * bps - data_start) // cluster_bytes + 1
    base_fat_off = reserved * bps
    base_fat = base[base_fat_off:base_fat_off + fat_size * bps]
    kernel = read_file(base, base_fat, root_start, root_entries, data_start, cluster_bytes, b"KERNEL  SYS")
    command = read_file(base, base_fat, root_start, root_entries, data_start, cluster_bytes, b"COMMAND COM")

    img = bytearray(total * bps)
    img[:512] = base[:512]  # boot sector and BPB
    fat = bytearray(fat_size * bps)
    fat[0], fat[1], fat[2] = 0xF0, 0xFF, 0xFF

    def alloc(data: bytes) -> int:
        need = (len(data) + cluster_bytes - 1) // cluster_bytes
        free = [c for c in range(2, max_cluster + 1) if fat12_get(fat, c) == 0 and c < 0xFF8]
        if len(free) < need:
            raise SystemExit(f"not enough space: need {need} clusters, have {len(free)}")
        chain = free[:need]
        for i, cluster in enumerate(chain):
            fat12_set(fat, cluster, chain[i + 1] if i + 1 < len(chain) else 0xFFF)
            off = data_start + (cluster - 2) * cluster_bytes
            img[off:off + len(data[i * cluster_bytes:(i + 1) * cluster_bytes])] = data[i * cluster_bytes:(i + 1) * cluster_bytes]
        return chain[0]

    files = [(b"KERNEL  SYS", kernel), (b"COMMAND COM", command)]
    for spec in args.file:
        if "=" not in spec:
            raise SystemExit("--file expects NAME=PATH")
        name, path = spec.split("=", 1)
        if len(name) > 8 or not name or any(not (c.isalnum() or c in "_-") for c in name):
            raise SystemExit(f"bad 8.3 name {name!r}")
        name_b = name.encode().upper().ljust(8)
        ext = Path(path).suffix.lstrip(".").upper().encode()
        if len(ext) > 3:
            raise SystemExit(f"bad extension for {path}")
        files.append((name_b + ext.ljust(3), Path(path).read_bytes()))
    for directory in args.dir:
        for path in sorted(p for p in Path(directory).iterdir() if p.is_file()):
            if path.name.upper() in ("KERNEL.SYS", "COMMAND.COM", "AUTOEXEC.BAT"):
                continue
            files.append((dos83_name(path.name, path), path.read_bytes()))
    if not args.no_autoexec:
        autoexec = args.autoexec.read_bytes() if args.autoexec else AUTOEXEC
        files.append((b"AUTOEXECBAT", autoexec))

    for i, (name, data) in enumerate(files):
        first = alloc(data)
        entry = root_start + i * 32
        img[entry:entry + 11] = name
        img[entry + 11] = 0x20
        put16(img, entry + 22, 0)
        put16(img, entry + 24, 0x21)
        put16(img, entry + 26, first)
        put32(img, entry + 28, len(data))

    img[base_fat_off:base_fat_off + len(fat)] = fat
    img[base_fat_off + len(fat):base_fat_off + 2 * len(fat)] = fat
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(img)
    print(f"wrote {args.out} ({len(img)} bytes), kernel={len(kernel)}, command={len(command)}")


if __name__ == "__main__":
    main()
