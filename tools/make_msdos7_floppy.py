#!/usr/bin/env python3
"""Build the MS-DOS 7 boot floppy used by run.bat option [11].

Windows 95 Setup's standard-mode DOS extender (DOSX) is happiest when it runs
under the DOS it was built for.  This tool builds a 1.44M floppy with
Microsoft's IO.SYS / MSDOS.SYS / COMMAND.COM from the OSR2 ISO, HIMEM.SYS,
QCDROM and MSCDEX.  It boots to an A:\\> prompt with the CD as D: and starts
the graphical setup.

The MSWIN4.1 boot sector comes from FORMAT.COM on the OSR2 CD (FORMAT.COM
carries the floppy/hard-disk boot-sector templates used by ``FORMAT /S``).
The template is patched to the 1.44M BPB and the FAT12 filesystem is rebuilt
from scratch.

Usage:
    python3 tools/make_msdos7_floppy.py
    python3 tools/make_msdos7_floppy.py --iso /mnt/c/Isos/Windows95OSR2.iso \
        --out assets/floppy/msdos7-win95.img
"""
from __future__ import annotations

import argparse
import struct
import zipfile
from pathlib import Path, PurePosixPath

SECTOR = 2048
FLOPPY_SIZE = 1474560
BPS, SPC, RESERVED, FATS, ROOT_ENTRIES = 512, 1, 1, 2, 224
TOTAL_SECTORS = 2880
MEDIA = 0xF0
FAT_SIZE = 9

MSDOS_SYS = (
    b"[Paths]\r\n"
    b"WinDir=C:\\WINDOWS\r\n"
    b"WinBootDir=C:\\WINDOWS\r\n"
    b"HostWinBootDrv=C\r\n"
    b"[Options]\r\n"
    b"BootGUI=0\r\n"
    b"BootMulti=0\r\n"
    b"BootMenu=0\r\n"
)

CONFIG_SYS = (
    b"DEVICE=HIMEM.SYS\r\n"
    b"DOS=HIGH\r\n"
    b"FILES=40\r\n"
    b"BUFFERS=20\r\n"
    b"DEVICE=QCDROM.SYS /D:OLDBOX /UX\r\n"
    b"LASTDRIVE=Z\r\n"
)


AUTOEXEC_BAT = (
    b"@ECHO OFF\r\n"
    b"PROMPT $P$G\r\n"
    b"MSCDEX.EXE /D:OLDBOX /L:D\r\n"
    b"ECHO.\r\n"
    b"ECHO MS-DOS 7 + QCDROM + MSCDEX ready; CD is D:.\r\n"
    b"ECHO Type  D:  and then  SETUP  to install Windows 95.\r\n"
    b"ECHO.\r\n"
)


def u16(buf: bytes | bytearray, off: int) -> int:
    return struct.unpack_from("<H", buf, off)[0]


def u32(buf: bytes | bytearray, off: int) -> int:
    return struct.unpack_from("<I", buf, off)[0]


def fat12_get(fat: bytearray, n: int) -> int:
    off = n + n // 2
    v = fat[off] | (fat[off + 1] << 8)
    return (v >> 4) & 0xFFF if n & 1 else v & 0xFFF


def fat12_set(fat: bytearray, n: int, value: int) -> None:
    off = n + n // 2
    old = fat[off] | (fat[off + 1] << 8)
    new = (old & 0x000F) | ((value & 0xFFF) << 4) if n & 1 else (old & 0xF000) | (value & 0xFFF)
    fat[off] = new & 0xFF
    fat[off + 1] = (new >> 8) & 0xFF


def iso_file(iso: Path, name: str) -> bytes:
    with iso.open("rb") as f:
        f.seek(16 * SECTOR)
        pvd = f.read(SECTOR)
        if pvd[1:6] != b"CD001":
            raise SystemExit(f"{iso} has no ISO9660 primary volume descriptor")
        root = pvd[156:190]
        extent, size = u32(root, 2), u32(root, 10)
        f.seek(extent * SECTOR)
        directory = f.read(size)
        i = 0
        while i < len(directory):
            length = directory[i]
            if length == 0:
                i = (i // SECTOR + 1) * SECTOR
                continue
            rec = directory[i:i + length]
            entry = rec[33:33 + rec[32]].decode("latin1").split(";")[0]
            if entry.upper() == name.upper():
                f.seek(u32(rec, 2) * SECTOR)
                return f.read(u32(rec, 10))
            i += length
    raise SystemExit(f"{name} not found in {iso}")


def zip_member(archive: zipfile.ZipFile, name: str) -> bytes:
    for member in archive.namelist():
        if PurePosixPath(member).name.lower() == name.lower():
            return archive.read(member)
    raise SystemExit(f"{name} not found in {archive.filename}")


def find_boot_template(format_com: bytes) -> bytearray:
    """Return the MSWIN4.x floppy-capable boot sector stored in FORMAT.COM."""
    for i in range(len(format_com) - 512):
        b = format_com[i:i + 512]
        if b[0] == 0xEB and b[2] == 0x90 and b[3:11].startswith(b"MSWIN4") and b[510:512] == b"\x55\xaa":
            bps = u16(b, 11)
            total = u16(b, 19)
            if bps == 512 and total:
                return bytearray(b)
    raise SystemExit("no MSWIN4.x boot-sector template found in FORMAT.COM")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", type=Path, default=Path("/mnt/c/Isos/Windows95OSR2.iso"))
    ap.add_argument("--zip", type=Path, default=Path("DOS Drivers.zip"))
    ap.add_argument("--out", type=Path, default=Path("assets/floppy/msdos7-win95.img"))
    args = ap.parse_args()

    if not args.iso.exists():
        raise SystemExit(f"{args.iso} not found")
    if not args.zip.exists():
        raise SystemExit(f"{args.zip} not found")

    boot = find_boot_template(iso_file(args.iso, "FORMAT.COM"))
    # Rewrite the BPB for a 1.44M floppy; the boot code itself is geometry
    # agnostic and reads everything from these fields.
    struct.pack_into("<H", boot, 11, BPS)
    boot[13] = SPC
    struct.pack_into("<H", boot, 14, RESERVED)
    boot[16] = FATS
    struct.pack_into("<H", boot, 17, ROOT_ENTRIES)
    struct.pack_into("<H", boot, 19, TOTAL_SECTORS)
    boot[21] = MEDIA
    struct.pack_into("<H", boot, 22, FAT_SIZE)
    struct.pack_into("<H", boot, 24, 18)   # sectors/track
    struct.pack_into("<H", boot, 26, 2)    # heads
    struct.pack_into("<I", boot, 28, 0)    # hidden sectors
    struct.pack_into("<I", boot, 32, 0)    # 32-bit total
    boot[36] = 0x00                        # BIOS drive number: floppy
    boot[38] = 0x29                        # extended boot signature

    with zipfile.ZipFile(args.zip) as archive:
        qcdrom = zip_member(archive, "qcdrom.sys")
        mscdex = zip_member(archive, "mscdex.exe")

    files = [
        (b"IO      SYS", iso_file(args.iso, "IO.SYS")),
        (b"MSDOS   SYS", MSDOS_SYS),
        (b"COMMAND COM", iso_file(args.iso, "COMMAND.COM")),
        (b"HIMEM   SYS", iso_file(args.iso, "HIMEM.SYS")),
        (b"QCDROM  SYS", qcdrom),
        (b"MSCDEX  EXE", mscdex),
        (b"CONFIG  SYS", CONFIG_SYS),
        (b"AUTOEXECBAT", AUTOEXEC_BAT),
    ]

    img = bytearray(TOTAL_SECTORS * BPS)
    img[:512] = boot
    fat = bytearray(FAT_SIZE * BPS)
    fat[0], fat[1], fat[2] = 0xF0, 0xFF, 0xFF
    root_start = (RESERVED + FATS * FAT_SIZE) * BPS
    data_start = root_start + ROOT_ENTRIES * 32
    cluster_bytes = BPS * SPC
    max_cluster = (TOTAL_SECTORS * BPS - data_start) // cluster_bytes + 1

    def alloc(data: bytes) -> int:
        need = (len(data) + cluster_bytes - 1) // cluster_bytes
        free = [c for c in range(3, max_cluster) if fat12_get(fat, c) == 0 and c < 0xFF8]
        if len(free) < need:
            raise SystemExit(f"not enough space: need {need} clusters")
        chain = free[:need]
        for i, cluster in enumerate(chain):
            fat12_set(fat, cluster, chain[i + 1] if i + 1 < len(chain) else 0xFFF)
            off = data_start + (cluster - 2) * cluster_bytes
            img[off:off + len(data[i * cluster_bytes:(i + 1) * cluster_bytes])] = \
                data[i * cluster_bytes:(i + 1) * cluster_bytes]
        return chain[0]

    for i, (name, data) in enumerate(files):
        first = alloc(data)
        e = root_start + i * 32
        img[e:e + 11] = name
        img[e + 11] = 0x20
        struct.pack_into("<H", img, e + 26, first)
        struct.pack_into("<I", img, e + 28, len(data))

    fat_off = RESERVED * BPS
    img[fat_off:fat_off + len(fat)] = fat
    img[fat_off + len(fat):fat_off + 2 * len(fat)] = fat
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(img)
    print(f"wrote {args.out} ({len(img)} bytes)")
    print(f"  boot sector: {bytes(boot[3:11]).decode('latin1')}, "
          f"IO.SYS {len(files[0][1])}, COMMAND.COM {len(files[2][1])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
