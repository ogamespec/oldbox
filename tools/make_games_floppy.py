#!/usr/bin/env python3
"""Rebuild the FreeDOS games floppy (assets/floppy/freedos-games.img).

The image boots FreeDOS 1.2 with HIMEMX (DOS=HIGH) for conventional-memory
hungry DOS games and loads the oldbox PS/2 mouse driver (MOUSE.COM) from
AUTOEXEC.BAT, so Megafortress finds a resident INT 33h mouse driver.

KERNEL.SYS, COMMAND.COM, HIMEMX.EXE and FDCONFIG.SYS are taken from the
existing games image, so the script can be re-run to upgrade the image
in place; only the driver and the AUTOEXEC.BAT are replaced.

The PS/2 mouse driver is assembled from tools/mouse_drv.S when binutils
(as/ld) are available; otherwise the prebuilt assets/floppy/MOUSE.COM is
used unchanged.

Usage:
    python3 tools/make_games_floppy.py
"""
from __future__ import annotations

import argparse
import struct
import subprocess
import tempfile
from pathlib import Path

from make_freedos_floppy import fat12_get, read_file, u16

AUTOEXEC = (
    b"@ECHO OFF\r\n"
    b"PROMPT $P$G\r\n"
    b"MOUSE.COM\r\n"
)


def extract_file(img: bytearray, fat: bytearray, root_start: int, root_entries: int,
                 data_start: int, cluster_bytes: int, name: bytes, out: Path) -> None:
    for i in range(root_entries):
        entry = root_start + i * 32
        if img[entry] == 0:
            break
        if bytes(img[entry:entry + 11]) != name:
            continue
        first = u16(img, entry + 26)
        size = struct.unpack("<I", img[entry + 28:entry + 32])[0]
        data = bytearray()
        cluster = first
        while 2 <= cluster < 0xFF8 and len(data) < size:
            off = data_start + (cluster - 2) * cluster_bytes
            data += img[off:off + cluster_bytes]
            cluster = fat12_get(fat, cluster)
        out.write_bytes(data[:size])
        print(f"extracted {out.name} ({len(data[:size])} bytes)")
        return
    raise SystemExit(f"{name!r} not found in the games image")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=Path("assets/floppy/freedos-games.img"))
    ap.add_argument("--driver", type=Path, default=None,
                    help="INT 33h mouse driver .COM to install (default: MMOUSE.COM in "
                         "the repository root, else the prebuilt assets/floppy/MOUSE.COM, "
                         "else assemble tools/mouse_drv.S)")
    args = ap.parse_args()

    games = args.out
    if not games.exists():
        raise SystemExit(f"{games} does not exist; build the base games floppy first")

    img = bytearray(games.read_bytes())
    bps, spc, reserved, fats, root_entries, total, fat_size = (
        u16(img, 11), img[13], u16(img, 14), img[16], u16(img, 17),
        u16(img, 19), u16(img, 22))
    root_start = (reserved + fats * fat_size) * bps
    data_start = root_start + root_entries * 32
    cluster_bytes = bps * spc
    fat = img[reserved * bps:reserved * bps + fat_size * bps]

    work = Path(tempfile.mkdtemp(prefix="games-floppy-"))
    kernel = work / "KERNEL.SYS"
    command = work / "COMMAND.COM"
    himem = work / "HIMEMX.EXE"
    fdconfig = work / "FDCONFIG.SYS"
    autoexec = work / "AUTOEXEC.BAT"
    extract_file(img, fat, root_start, root_entries, data_start, cluster_bytes,
                 b"KERNEL  SYS", kernel)
    extract_file(img, fat, root_start, root_entries, data_start, cluster_bytes,
                 b"COMMAND COM", command)
    extract_file(img, fat, root_start, root_entries, data_start, cluster_bytes,
                 b"HIMEMX  EXE", himem)
    extract_file(img, fat, root_start, root_entries, data_start, cluster_bytes,
                 b"FDCONFIGSYS", fdconfig)
    autoexec.write_bytes(AUTOEXEC)

    # The Microsoft-compatible INT 33h driver (the Microsoft mouse driver
    # MMOUSE.COM by default; any other .COM can be given with --driver).
    mouse = Path("assets/floppy/MOUSE.COM")
    mouse.parent.mkdir(parents=True, exist_ok=True)
    driver = args.driver if args.driver else Path("MMOUSE.COM")
    if driver.exists():
        mouse.write_bytes(driver.read_bytes())
        print(f"using {driver} ({mouse.stat().st_size} bytes)")
    elif mouse.exists():
        print(f"using prebuilt {mouse} ({mouse.stat().st_size} bytes)")
    else:
        raise SystemExit("no mouse driver found: put MMOUSE.COM in the repository "
                         "root or pass --driver PATH")

    cmd = [
        "python3", "tools/make_freedos_floppy.py",
        "--base", "assets/floppy/awardflash1440.img",
        "--out", str(games),
        "--autoexec", str(autoexec),
        "--file", f"HIMEMX={himem}",
        "--file", f"FDCONFIG={fdconfig}",
        "--file", f"MOUSE={mouse}",
    ]
    subprocess.run(cmd, check=True)


if __name__ == "__main__":
    main()
