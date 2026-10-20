#!/usr/bin/env python3
"""Rebuild the FreeDOS games floppy (assets/floppy/freedos-games.img).

The image boots FreeDOS 1.2 with HIMEMX (DOS=HIGH) for conventional-memory
hungry DOS games and loads the oldbox PS/2 mouse driver (MOUSE.COM) from
AUTOEXEC.BAT, so Megafortress finds a resident INT 33h mouse driver.
MOUSE.INI selects MouseType=PS2 for Microsoft Mouse Driver 8.20.

KERNEL.SYS, COMMAND.COM, HIMEMX.EXE and FDCONFIG.SYS are taken from the
existing games image, so the script can be re-run to upgrade the image
in place; only the driver and the AUTOEXEC.BAT are replaced.

With --cd the same disk also loads QCDROM.SYS + MSCDEX.EXE (taken from
DOS Drivers.zip), so the ATAPI CD-ROM appears as drive D:.  This variant is
built as assets/floppy/freedos-games-cd.img and used by run-world9.bat.

The PS/2 mouse driver is assembled from tools/mouse_drv.S when binutils
(as/ld) are available; otherwise the prebuilt assets/floppy/MOUSE.COM is
used unchanged.

Usage:
    python3 tools/make_games_floppy.py
    python3 tools/make_games_floppy.py --cd \
        --out assets/floppy/freedos-games-cd.img
"""
from __future__ import annotations

import argparse
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path, PurePosixPath

from make_freedos_floppy import fat12_get, read_file, u16

AUTOEXEC = (
    b"@ECHO OFF\r\n"
    b"PROMPT $P$G\r\n"
    b"MOUSE.COM\r\n"
)

FDCONFIG_CD = (
    b"DEVICE=HIMEMX.EXE\r\n"
    b"DOS=HIGH\r\n"
    b"FILES=40\r\n"
    b"BUFFERS=20\r\n"
    b"DEVICE=QCDROM.SYS /D:OLDBOX /UX\r\n"
    b"LASTDRIVE=Z\r\n"
)

AUTOEXEC_CD = (
    b"@ECHO OFF\r\n"
    b"PROMPT $P$G\r\n"
    b"MOUSE.COM\r\n"
    b"MSCDEX.EXE /D:OLDBOX /L:D\r\n"
    b"ECHO.\r\n"
    b"ECHO CD-ROM is ready as drive D:.\r\n"
    b"ECHO.\r\n"
)

# Microsoft Mouse Driver 8.20 needs the PS/2 selection in MOUSE.INI; without
# it the driver loads but the 8042 mouse packets are not translated to INT 33h.
MOUSE_INI = b"[mouse]\r\nMouseType=PS2\r\n"


def zip_member(archive: zipfile.ZipFile, name: str) -> bytes:
    for member in archive.namelist():
        if PurePosixPath(member).name.lower() == name.lower():
            return archive.read(member)
    raise SystemExit(f"{name} not found in {archive.filename}")


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
    ap.add_argument("--out", type=Path, default=None,
                    help="image to write (default: freedos-games-cd.img with --cd, "
                         "else freedos-games.img)")
    ap.add_argument("--source", type=Path, default=None,
                    help="image that supplies KERNEL.SYS/COMMAND.COM/HIMEMX.EXE "
                         "(default: --out if it exists, else the stock games image)")
    ap.add_argument("--cd", action="store_true",
                    help="also add QCDROM.SYS + MSCDEX.EXE, so the ATAPI CD-ROM "
                         "appears as drive D:")
    ap.add_argument("--zip", type=Path, default=Path("DOS Drivers.zip"),
                    help="archive with qcdrom.sys and mscdex.exe (used by --cd)")
    ap.add_argument("--driver", type=Path, default=None,
                    help="INT 33h mouse driver .COM to install (default: MMOUSE.COM in "
                         "the repository root, else the prebuilt assets/floppy/MOUSE.COM, "
                         "else assemble tools/mouse_drv.S)")
    args = ap.parse_args()
    if args.out is None:
        args.out = Path("assets/floppy/freedos-games-cd.img" if args.cd
                        else "assets/floppy/freedos-games.img")

    source = args.source
    if source is None:
        source = args.out if args.out.exists() else Path("assets/floppy/freedos-games.img")
    if not source.exists():
        raise SystemExit(f"{source} does not exist; build the base games floppy first")
    if args.cd and not args.zip.exists():
        raise SystemExit(f"{args.zip} not found; it supplies qcdrom.sys and mscdex.exe")

    img = bytearray(source.read_bytes())
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
    if args.cd:
        fdconfig.write_bytes(FDCONFIG_CD)
    else:
        extract_file(img, fat, root_start, root_entries, data_start, cluster_bytes,
                     b"FDCONFIGSYS", fdconfig)
    autoexec.write_bytes(AUTOEXEC_CD if args.cd else AUTOEXEC)

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

    extra = [f"HIMEMX={himem}", f"FDCONFIG={fdconfig}", f"MOUSE={mouse}"]
    ini_dir = work / "ini"
    ini_dir.mkdir()
    (ini_dir / "MOUSE.INI").write_bytes(MOUSE_INI)
    print("using MOUSE.INI (MouseType=PS2)")
    if args.cd:
        with zipfile.ZipFile(args.zip) as archive:
            qcdrom = work / "QCDROM.SYS"
            mscdex = work / "MSCDEX.EXE"
            qcdrom.write_bytes(zip_member(archive, "qcdrom.sys"))
            mscdex.write_bytes(zip_member(archive, "mscdex.exe"))
            print(f"using {args.zip} (qcdrom.sys + mscdex.exe)")
        extra += [f"QCDROM={qcdrom}", f"MSCDEX={mscdex}"]

    cmd = [
        sys.executable, "tools/make_freedos_floppy.py",
        "--base", "assets/floppy/awardflash1440.img",
        "--out", str(args.out),
        "--autoexec", str(autoexec),
    ]
    for spec in extra:
        cmd += ["--file", spec]
    cmd += ["--dir", str(ini_dir)]
    subprocess.run(cmd, check=True)

    if args.cd:
        print("  boot: A: FreeDOS games + HIMEMX + MOUSE + QCDROM + MSCDEX")
        print("  CD:   D: (any ATAPI image attached with --cdrom)")


if __name__ == "__main__":
    main()
