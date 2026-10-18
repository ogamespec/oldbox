#!/usr/bin/env python3
"""Prepare oldbox firmware and media assets from the supplied specs/ tree.

The repository intentionally does not commit vendor firmware.  This script
extracts the files that are already present in specs/ and places them in the
asset directories understood by the emulator:

  assets/bios/EP-ZXA.bin        reference-board update image (bxc09406.zip)
  assets/bios/BXC09716.BIN      alternate full 256 KiB Award image (optional)
  assets/vga/S3Trio64_1_14-04.bin
  assets/floppy/blank1440.img   blank 1.44 MiB floppy for POST tests

It uses only the Python standard library.  The extracted files remain subject
to their original vendor licences; they are not part of oldbox itself.
"""
from __future__ import annotations

import argparse
import io
import pathlib
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def extract_zip(path: pathlib.Path, out_dir: pathlib.Path, wanted: set[str]) -> list[pathlib.Path]:
    if not path.exists():
        print(f"skip: {path} not present")
        return []
    out_dir.mkdir(parents=True, exist_ok=True)
    written = []
    with zipfile.ZipFile(path) as z:
        for info in z.infolist():
            name = pathlib.Path(info.filename).name
            if name.upper() in wanted or not wanted:
                data = z.read(info)
                dest = out_dir / name
                dest.write_bytes(data)
                written.append(dest)
                print(f"wrote {dest.relative_to(ROOT)} ({len(data)} bytes)")
    return written


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--floppy", action="store_true", help="also create a blank 1.44 MiB floppy")
    args = ap.parse_args()

    bios = ROOT / "assets/bios"
    vga = ROOT / "assets/vga"
    floppy = ROOT / "assets/floppy"

    # Real 256 KiB Award BIOS image for the EP-61BXA-M reference board.
    # It is supplied inside the vendor zip as a full 2 Mbit flash image.
    extract_zip(ROOT / "specs/mb/bxc09406.zip", bios, {"EP-ZXA.BIN"})
    # Optional second real 256 KiB BIOS image from the vendor self-extractor.
    extract_zip(ROOT / "specs/mb/bxc09716.exe", bios, {"BXC09716.BIN"})

    s3 = ROOT / "specs/S3Trio64_1_14-04.bin"
    if s3.exists():
        vga.mkdir(parents=True, exist_ok=True)
        dest = vga / s3.name
        dest.unlink(missing_ok=True)
        dest.write_bytes(s3.read_bytes())
        print(f"wrote {(vga / s3.name).relative_to(ROOT)} ({s3.stat().st_size} bytes)")

    if args.floppy:
        floppy.mkdir(parents=True, exist_ok=True)
        img = floppy / "blank1440.img"
        with img.open("wb") as f:
            f.truncate(80 * 2 * 18 * 512)
        print(f"wrote {img.relative_to(ROOT)} (1440 KiB)")

    print("\nAsset preparation complete.  Example:")
    print("  ./build/oldbox --bios assets/bios/EP-ZXA.bin --floppy assets/floppy/blank1440.img")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
