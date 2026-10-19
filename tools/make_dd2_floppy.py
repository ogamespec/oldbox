#!/usr/bin/env python3
"""Build a bootable Dangerous Dave 2 floppy (FreeDOS + the game on A:).

The image contains the FreeDOS kernel and shell, the original game files
from C:\\Games\\DD2 (kept as a directory next to the repository or passed
with --dir) and an AUTOEXEC.BAT that starts DAVE2.EXE.  Booting it in the
emulator goes straight into the game.

Usage:
    python3 tools/make_dd2_floppy.py
    python3 tools/make_dd2_floppy.py --dir /mnt/c/Games/DD2
"""
from __future__ import annotations

import argparse
import subprocess
import tempfile
from pathlib import Path

AUTOEXEC = b"@ECHO OFF\r\nPROMPT $P$G\r\nDAVE2.EXE\r\n"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", type=Path,
                    default=Path("/mnt/c/Games/DD2"),
                    help="directory with the game files (default C:\\Games\\DD2)")
    ap.add_argument("--out", type=Path, default=Path("assets/floppy/dd2.img"))
    args = ap.parse_args()

    if not args.dir.is_dir():
        raise SystemExit(f"{args.dir} is not a directory")

    with tempfile.TemporaryDirectory(prefix="dd2-floppy-") as td:
        autoexec = Path(td) / "AUTOEXEC.BAT"
        autoexec.write_bytes(AUTOEXEC)
        cmd = [
            "python3", "tools/make_freedos_floppy.py",
            "--base", "assets/floppy/awardflash1440.img",
            "--out", str(args.out),
            "--autoexec", str(autoexec),
            "--dir", str(args.dir),
        ]
        subprocess.run(cmd, check=True)


if __name__ == "__main__":
    main()
