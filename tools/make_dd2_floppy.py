#!/usr/bin/env python3
"""Build a bootable Dangerous Dave 2 floppy (FreeDOS + the game on A:).

The image contains the FreeDOS kernel and shell, HIMEMX (DOS=HIGH) so the
game gets nearly all of the 640K conventional memory, the original game
files from C:\\Games\\DD2 and an AUTOEXEC.BAT that starts DAVE2.EXE.
Booting it in the emulator goes straight into the game.

Usage:
    python3 tools/make_dd2_floppy.py
    python3 tools/make_dd2_floppy.py --dir /mnt/c/Games/DD2
"""
from __future__ import annotations

import argparse
import struct
import subprocess
import tempfile
from pathlib import Path

from make_freedos_floppy import fat12_get, read_file, u16

AUTOEXEC = b"@ECHO OFF\r\nPROMPT $P$G\r\nDAVE2.EXE\r\n"
FDCONFIG = b"DEVICE=HIMEMX.EXE\r\nDOS=HIGH\r\n"


def extract_himemx(games_img: Path, out: Path) -> None:
    """Copy HIMEMX.EXE out of the existing FreeDOS games image."""
    img = bytearray(games_img.read_bytes())
    bps = u16(img, 11)
    spc = img[13]
    reserved = u16(img, 14)
    fats = img[16]
    root_entries = u16(img, 17)
    fat_size = u16(img, 22)
    root_start = (reserved + fats * fat_size) * bps
    data_start = root_start + root_entries * 32
    cluster_bytes = bps * spc
    fat_off = reserved * bps
    fat = img[fat_off:fat_off + fat_size * bps]
    data = read_file(img, fat, root_start, root_entries, data_start, cluster_bytes, b"HIMEMX  EXE")
    if not data:
        raise SystemExit(f"{games_img} has no HIMEMX.EXE")
    out.write_bytes(data)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", type=Path, default=Path("/mnt/c/Games/DD2"),
                    help="directory with the game files (default C:\\Games\\DD2)")
    ap.add_argument("--out", type=Path, default=Path("assets/floppy/dd2.img"))
    args = ap.parse_args()

    if not args.dir.is_dir():
        raise SystemExit(f"{args.dir} is not a directory")

    with tempfile.TemporaryDirectory(prefix="dd2-floppy-") as td:
        tmp = Path(td)
        autoexec = tmp / "AUTOEXEC.BAT"
        autoexec.write_bytes(AUTOEXEC)
        fdconfig = tmp / "FDCONFIG.SYS"
        fdconfig.write_bytes(FDCONFIG)
        himemx = tmp / "HIMEMX.EXE"
        extract_himemx(Path("assets/floppy/freedos-games.img"), himemx)
        cmd = [
            "python3", "tools/make_freedos_floppy.py",
            "--base", "assets/floppy/awardflash1440.img",
            "--out", str(args.out),
            "--autoexec", str(autoexec),
            "--dir", str(args.dir),
            "--file", f"HIMEMX={himemx}",
            "--file", f"FDCONFIG={fdconfig}",
        ]
        subprocess.run(cmd, check=True)


if __name__ == "__main__":
    main()
