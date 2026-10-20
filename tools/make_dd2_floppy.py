#!/usr/bin/env python3
"""Build a bootable Dangerous Dave 2 floppy (FreeDOS + the game on A:).

The image contains the FreeDOS kernel and shell, HIMEMX (DOS=HIGH) so the
game gets nearly all of the 640K conventional memory, the original game
files and an AUTOEXEC.BAT that starts DAVE2.EXE.  Booting it in the emulator
goes straight into the game.

The game executable must be the **correctly unpacked** LZEXE 0.91 image.
The old ``C:\\Games\\DD2\\DAVE2.EXE`` is a holey unpack (42 % zero bytes, no
``55 8B EC`` prologues, broken MZ header); starting it makes the guest run
data, which FreeDOS reports as "Invalid Opcode at ...".  The reference
unpacked image lives in ``C:\\Work\\dave2\\re\\ref\\orig`` as
``DAVE2_UNPACKED.EXE``.  The game data next to it is the original 1991
release; the copies in ``C:\\Games\\DD2`` carry extra trailing bytes and a
different TITLE2.DD2 and are not suitable for this executable.

Usage:
    python3 tools/make_dd2_floppy.py
    python3 tools/make_dd2_floppy.py --dir C:\\Work\\dave2\\re\\ref\\orig
    python3 tools/make_dd2_floppy.py --dir C:\\Games\\DD2 \\
        --exe C:\\Work\\dave2\\re\\ref\\orig\\DAVE2_UNPACKED.EXE
"""
from __future__ import annotations

import argparse
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from make_freedos_floppy import fat12_get, read_file, u16

AUTOEXEC = b"@ECHO OFF\r\nPROMPT $P$G\r\nDAVE2.EXE\r\n"
FDCONFIG = b"DEVICE=HIMEMX.EXE\r\nDOS=HIGH\r\n"


def default_game_dir() -> Path:
    """Prefer the reference unpacked game tree, then the user's game copy."""
    candidates = [
        Path(r"C:\Work\dave2\re\ref\orig"),
        Path("/mnt/c/Work/dave2/re/ref/orig"),
        Path(r"C:\Games\DD2"),
        Path("/mnt/c/Games/DD2"),
    ]
    for candidate in candidates:
        if (candidate / "LEVEL01.DD2").is_file() or (candidate / "LEVEL01.dd2").is_file():
            return candidate
    return candidates[0]


def check_exe(path: Path) -> None:
    """Reject the holey LZEXE unpack and packed originals with a clear error."""
    data = path.read_bytes()
    if len(data) < 0x1C or data[:2] != b"MZ":
        raise SystemExit(f"{path}: not an MZ executable")
    e_cparhdr = u16(data, 0x08)
    e_ip = u16(data, 0x14)
    e_cs = u16(data, 0x16)
    # A correct LZEXE 0.91 unpack of DAVE2.EXE has a 0x20-paragraph header,
    # CS:IP = 0000:0000 and the usual Borland frame prologues.  The bad unpack
    # kept a zero-sized header and entry 0000:0000 but lost most back-refs,
    # the packed original has CS:IP = 0000:0080 and a zero header size.
    if e_cparhdr < 0x20 or e_cs != 0 or e_ip != 0 or b"\x55\x8b\xec" not in data:
        raise SystemExit(
            f"{path}: this is not the correctly unpacked DAVE2.EXE\n"
            "  (the holey LZEXE unpack is 211639 bytes with ~42% zero bytes;\n"
            "   the packed original has CS:IP = 0000:0080 and e_cparhdr = 0).\n"
            "  Use the reference image instead:\n"
            r"    --exe C:\Work\dave2\re\ref\orig\DAVE2_UNPACKED.EXE"
        )


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
    ap.add_argument("--dir", type=Path, default=default_game_dir(),
                    help="directory with the original *.DD2 files "
                         "(default: the reference tree if present, else C:\\Games\\DD2)")
    ap.add_argument("--exe", type=Path, default=None,
                    help="correctly unpacked DAVE2.EXE "
                         "(default: <dir>/DAVE2_UNPACKED.EXE, else <dir>/DAVE2.EXE)")
    ap.add_argument("--out", type=Path, default=Path("assets/floppy/dd2.img"))
    args = ap.parse_args()

    if not args.dir.is_dir():
        raise SystemExit(f"{args.dir} is not a directory")
    dd2_files = sorted(p for p in args.dir.iterdir()
                       if p.is_file() and p.suffix.upper() == ".DD2")
    if not dd2_files:
        raise SystemExit(f"{args.dir} contains no *.DD2 game files")

    exe = args.exe
    if exe is None:
        for name in ("DAVE2_UNPACKED.EXE", "DAVE2.EXE"):
            candidate = args.dir / name
            if candidate.is_file():
                exe = candidate
                break
        else:
            raise SystemExit(f"{args.dir} has no DAVE2_UNPACKED.EXE or DAVE2.EXE")
    check_exe(exe)

    with tempfile.TemporaryDirectory(prefix="dd2-floppy-") as td:
        tmp = Path(td)
        stage = tmp / "game"
        stage.mkdir()
        for path in dd2_files:
            shutil.copy2(path, stage / path.name)
        shutil.copy2(exe, stage / "DAVE2.EXE")

        autoexec = tmp / "AUTOEXEC.BAT"
        autoexec.write_bytes(AUTOEXEC)
        fdconfig = tmp / "FDCONFIG.SYS"
        fdconfig.write_bytes(FDCONFIG)
        himemx = tmp / "HIMEMX.EXE"
        extract_himemx(Path("assets/floppy/freedos-games.img"), himemx)
        cmd = [
            sys.executable, "tools/make_freedos_floppy.py",
            "--base", "assets/floppy/awardflash1440.img",
            "--out", str(args.out),
            "--autoexec", str(autoexec),
            "--dir", str(stage),
            "--file", f"HIMEMX={himemx}",
            "--file", f"FDCONFIG={fdconfig}",
        ]
        subprocess.run(cmd, check=True)
    print(f"DAVE2.EXE: {exe}")
    print(f"game data: {args.dir}")


if __name__ == "__main__":
    main()
