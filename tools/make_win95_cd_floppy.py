#!/usr/bin/env python3
"""Build the FreeDOS A: disk used by run.bat option [10].

The disk boots FreeDOS 1.2, loads HIMEMX, the QCDROM ATAPI driver and
Microsoft MSCDEX, so the Windows 95 OSR2 ISO attached as secondary master
appears as drive D:.

QCDROM is taken from ``DOS Drivers.zip`` because the OAKCDROM.SYS from the
same archive does not see oldbox's ATAPI CD-ROM (it reports "No drives
found"), while QCDROM + MSCDEX reads the ISO correctly.

Usage:
    python3 tools/make_win95_cd_floppy.py
    python3 tools/make_win95_cd_floppy.py --zip "DOS Drivers.zip" \
        --out assets/floppy/freedos-win95.img
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path, PurePosixPath

from make_freedos_floppy import read_file, u16

FDCONFIG = (
    b"DEVICE=HIMEMX.EXE\r\n"
    b"DOS=HIGH\r\n"
    b"FILES=40\r\n"
    b"BUFFERS=20\r\n"
    b"DEVICE=QCDROM.SYS /D:OLDBOX /UX\r\n"
    b"LASTDRIVE=Z\r\n"
)

AUTOEXEC = (
    b"@ECHO OFF\r\n"
    b"PROMPT $P$G\r\n"
    b"MSCDEX.EXE /D:OLDBOX /L:D\r\n"
    b"ECHO.\r\n"
    b"ECHO Windows 95 OSR2 CD-ROM is ready as drive D:.\r\n"
    b"ECHO Type  D:  and then  SETUP  to start the installation.\r\n"
    b"ECHO.\r\n"
)


def zip_member(archive: zipfile.ZipFile, name: str) -> bytes:
    for member in archive.namelist():
        if PurePosixPath(member).name.lower() == name.lower():
            return archive.read(member)
    raise SystemExit(f"{name} not found in {archive.filename}")


def extract_fat12(img: bytearray, name: bytes, out: Path) -> None:
    bps, spc, reserved, fats, root_entries, total, fat_size = (
        u16(img, 11), img[13], u16(img, 14), img[16], u16(img, 17),
        u16(img, 19), u16(img, 22))
    root_start = (reserved + fats * fat_size) * bps
    data_start = root_start + root_entries * 32
    cluster_bytes = bps * spc
    fat = img[reserved * bps:reserved * bps + fat_size * bps]
    data = read_file(img, fat, root_start, root_entries, data_start,
                     cluster_bytes, name)
    out.write_bytes(data)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--zip", type=Path, default=Path("DOS Drivers.zip"),
                    help="archive with qcdrom.sys and mscdex.exe")
    ap.add_argument("--base", type=Path,
                    default=Path("assets/floppy/awardflash1440.img"),
                    help="bootable FreeDOS 1.44M image used for KERNEL.SYS/COMMAND.COM")
    ap.add_argument("--games", type=Path,
                    default=Path("assets/floppy/freedos-games.img"),
                    help="image that supplies HIMEMX.EXE")
    ap.add_argument("--out", type=Path,
                    default=Path("assets/floppy/freedos-win95.img"))
    args = ap.parse_args()

    if not args.zip.exists():
        raise SystemExit(f"{args.zip} not found")
    if not args.base.exists():
        raise SystemExit(f"{args.base} not found; build it with tools/prepare_assets.py")
    if not args.games.exists():
        raise SystemExit(f"{args.games} not found; it supplies HIMEMX.EXE")

    with tempfile.TemporaryDirectory(prefix="win95-floppy-") as tmp:
        work = Path(tmp)
        with zipfile.ZipFile(args.zip) as archive:
            (work / "QCDROM.SYS").write_bytes(zip_member(archive, "qcdrom.sys"))
            (work / "MSCDEX.EXE").write_bytes(zip_member(archive, "mscdex.exe"))

        games = bytearray(args.games.read_bytes())
        extract_fat12(games, b"HIMEMX  EXE", work / "HIMEMX.EXE")
        (work / "FDCONFIG.SYS").write_bytes(FDCONFIG)
        (work / "AUTOEXEC.BAT").write_bytes(AUTOEXEC)

        cmd = [
            sys.executable, str(Path(__file__).with_name("make_freedos_floppy.py")),
            "--base", str(args.base),
            "--out", str(args.out),
            "--autoexec", str(work / "AUTOEXEC.BAT"),
            "--file", f"HIMEMX={work / 'HIMEMX.EXE'}",
            "--file", f"FDCONFIG={work / 'FDCONFIG.SYS'}",
            "--file", f"QCDROM={work / 'QCDROM.SYS'}",
            "--file", f"MSCDEX={work / 'MSCDEX.EXE'}",
        ]
        subprocess.run(cmd, check=True)

    print(f"  boot: A: FreeDOS -> HIMEMX -> QCDROM /D:OLDBOX -> MSCDEX /L:D")
    print(f"  CD:   D: (Windows 95 OSR2 ISO)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
