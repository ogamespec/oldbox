#!/usr/bin/env python3
"""Create the OldBox hard-disk image: unpartitioned 500 MiB raw file.

The reference board attaches the image as Primary Master.  The ATA model
advertises 16 heads / 63 sectors of 512 bytes, so a 500 MiB image (1 024 000
sectors) is detected by the Award autodetect as ``LBA, Mode 4, 524MB``.

The image is deliberately unpartitioned and zero-filled: no MBR, no volume.
Partitioning and formatting is left to the guest (FreeDOS ``FDISK`` /
``FORMAT C:`` inside the emulator), which is how the reference machine was
set up in 1998.  The file is created sparse where the file system supports it.

Only the Python standard library is used.

Usage:
    python3 tools/make_hdd.py
    python3 tools/make_hdd.py --out assets/hdd/oldbox-hdd-500mb.img --size-mb 500
"""
from __future__ import annotations

import argparse
from pathlib import Path

SECTOR = 512


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=Path("assets/hdd/oldbox-hdd-500mb.img"))
    ap.add_argument("--size-mb", type=int, default=500, help="image size in MiB (default 500)")
    args = ap.parse_args()

    if args.size_mb < 1:
        raise SystemExit("image size must be positive")
    size = args.size_mb * 1024 * 1024
    sectors = size // SECTOR

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("wb") as f:
        f.truncate(size)          # zero-filled, sparse on NTFS/ext4

    print(f"wrote {args.out} ({size} bytes, {sectors} sectors)")
    print(f"  unpartitioned raw image; ATA geometry: {sectors // (16 * 63) + 1} "
          f"cylinders x 16 heads x 63 sectors")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
