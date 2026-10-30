#!/usr/bin/env python3
"""Build a FreeDOS A: disk for experiments (run.bat no longer uses it).

This was the image for the removed run.bat option [10].  The FreeDOS variant
is not suitable for installing Windows 95: its text-mode Setup writes
zero-length MINI.CAB/EXTRACT.EXE and the GUI then reports a CAB read error.
Use make_msdos7_floppy.py and run.bat option [11] instead.


The disk boots FreeDOS 1.2, loads Microsoft HIMEM.SYS (taken from the
Windows 95 OSR2 ISO), the QCDROM ATAPI driver and Microsoft MSCDEX, so the
ISO attached as secondary master appears as drive D:.

HIMEM.SYS is required by Windows 95 Setup's "standard mode" phase.  FreeDOS
HIMEMX provides XMS, but Windows 95 Setup stops with "Cannot start Windows in
standard mode. Make sure you have installed HIMEM.SYS..." when it is used.
The Microsoft driver works with FreeDOS as long as DOS=HIGH is not requested
(that combination hangs FreeCom).

QCDROM is taken from ``DOS Drivers.zip`` because the OAKCDROM.SYS from the
same archive does not see oldbox's ATAPI CD-ROM (it reports "No drives
found"), while QCDROM + MSCDEX reads the ISO correctly.

Usage:
    python3 tools/make_win95_cd_floppy.py
    python3 tools/make_win95_cd_floppy.py --iso "C:\\Isos\\Windows95OSR2.iso" \
        --out assets/floppy/freedos-win95.img
"""
from __future__ import annotations

import argparse
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path, PurePosixPath

SECTOR = 2048

FDCONFIG = (
    b"DEVICE=HIMEM.SYS\r\n"
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


def le32(buf: bytes, off: int) -> int:
    return struct.unpack_from("<I", buf, off)[0]


def default_iso() -> Path:
    """Return the usual Windows 95 image path on Windows or under WSL."""
    for candidate in (Path(r"C:\Isos\Windows95OSR2.iso"),
                      Path("/mnt/c/Isos/Windows95OSR2.iso")):
        if candidate.exists():
            return candidate
    return Path(r"C:\Isos\Windows95OSR2.iso")


def iso_file(path: Path, name: str) -> bytes:
    """Read one file from the root directory of an ISO9660 image."""
    with path.open("rb") as f:
        f.seek(16 * SECTOR)
        pvd = f.read(SECTOR)
        if pvd[1:6] != b"CD001":
            raise SystemExit(f"{path} has no ISO9660 primary volume descriptor")
        root = pvd[156:190]
        extent = le32(root, 2)
        size = le32(root, 10)
        f.seek(extent * SECTOR)
        directory = f.read(size)

        i = 0
        while i < len(directory):
            length = directory[i]
            if length == 0:
                i = (i // SECTOR + 1) * SECTOR
                continue
            record = directory[i:i + length]
            name_len = record[32]
            entry = record[33:33 + name_len].decode("latin1").split(";")[0]
            if entry.upper() == name.upper():
                file_extent = le32(record, 2)
                file_size = le32(record, 10)
                f.seek(file_extent * SECTOR)
                return f.read(file_size)
            i += length
    raise SystemExit(f"{name} not found in {path}")


def zip_member(archive: zipfile.ZipFile, name: str) -> bytes:
    for member in archive.namelist():
        if PurePosixPath(member).name.lower() == name.lower():
            return archive.read(member)
    raise SystemExit(f"{name} not found in {archive.filename}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--zip", type=Path, default=Path("DOS Drivers.zip"),
                    help="archive with qcdrom.sys and mscdex.exe")
    ap.add_argument("--iso", type=Path, default=None,
                    help="Windows 95 OSR2 ISO that supplies HIMEM.SYS "
                         "(default: C:\\Isos\\Windows95OSR2.iso)")
    ap.add_argument("--base", type=Path,
                    default=Path("assets/floppy/awardflash1440.img"),
                    help="bootable FreeDOS 1.44M image used for KERNEL.SYS/COMMAND.COM")
    ap.add_argument("--out", type=Path,
                    default=Path("assets/floppy/freedos-win95.img"))
    args = ap.parse_args()
    iso = args.iso if args.iso is not None else default_iso()

    if not args.zip.exists():
        raise SystemExit(f"{args.zip} not found")
    if not args.base.exists():
        raise SystemExit(f"{args.base} not found; build it with tools/prepare_assets.py")
    if not iso.exists():
        raise SystemExit(f"{iso} not found; pass --iso PATH")

    with tempfile.TemporaryDirectory(prefix="win95-floppy-") as tmp:
        work = Path(tmp)
        with zipfile.ZipFile(args.zip) as archive:
            (work / "QCDROM.SYS").write_bytes(zip_member(archive, "qcdrom.sys"))
            (work / "MSCDEX.EXE").write_bytes(zip_member(archive, "mscdex.exe"))

        (work / "HIMEM.SYS").write_bytes(iso_file(iso, "HIMEM.SYS"))
        (work / "FDCONFIG.SYS").write_bytes(FDCONFIG)
        (work / "AUTOEXEC.BAT").write_bytes(AUTOEXEC)

        cmd = [
            sys.executable, str(Path(__file__).with_name("make_freedos_floppy.py")),
            "--base", str(args.base),
            "--out", str(args.out),
            "--autoexec", str(work / "AUTOEXEC.BAT"),
            "--file", f"HIMEM={work / 'HIMEM.SYS'}",
            "--file", f"FDCONFIG={work / 'FDCONFIG.SYS'}",
            "--file", f"QCDROM={work / 'QCDROM.SYS'}",
            "--file", f"MSCDEX={work / 'MSCDEX.EXE'}",
        ]
        subprocess.run(cmd, check=True)

    print("  boot: A: FreeDOS -> HIMEM.SYS -> QCDROM /D:OLDBOX -> MSCDEX /L:D")
    print("  CD:   D: (Windows 95 OSR2 ISO)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
