#!/usr/bin/env python3
"""Build a BIOS-bootable wrapper for MEMTEST.IMG.

The stock MEMTEST.IMG boot sector has no DOS BPB and relies on a loader that
leaves DX=0x3ff4.  Award's boot block therefore does not chainload it.

This tool runs MEMTEST.IMG in the built-in loader just far enough for Setup.S
to be entered, takes a snapshot of 0x10000..0x9ffff, and builds a normal
1.44 MiB FAT12 floppy containing:

  * the FreeDOS 1.2 boot sector, which the Award boot block accepts;
  * KERNEL.SYS, a one-sector loader copied to 0x0060:0000 by that boot sector;
  * SNAP.BIN with the snapshot, loaded by the loader to 0x10000..0x9ffff;
  * a jump to 0x9020:0000, the entry point used by MEMTEST's own Setup.S.

All GDT data needed by Setup.S is already inside the snapshot.
"""
from __future__ import annotations

import argparse
import subprocess
import tempfile
from pathlib import Path

SECTOR = 512
TOTAL_SECTORS = 2880
SNAPSHOT_BASE = 0x10000
SNAPSHOT_SIZE = 0x90000
SNAPSHOT_SECTORS = SNAPSHOT_SIZE // SECTOR  # 1152
DATA_LBA = 36
START_CLUSTER = 5  # FAT data area starts at LBA 33, one sector per cluster


def run_snapshot(emulator: Path, source: Path) -> bytes:
    with tempfile.TemporaryDirectory() as td:
        dump = Path(td) / "ram.bin"
        cmd = [
            str(emulator),
            "--firmware", "builtin",
            "--floppy", str(source),
            "--cpu", "333",
            "--memory", "64",
            "--headless",
            "--max-instructions", "8100",
            "--dump-ram", str(dump),
        ]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
        ram = dump.read_bytes()
    if len(ram) < SNAPSHOT_BASE + SNAPSHOT_SIZE:
        raise SystemExit("short RAM dump")
    snap = ram[SNAPSHOT_BASE:SNAPSHOT_BASE + SNAPSHOT_SIZE]
    if snap[0x80200:0x80205] != b"\xfa\xb0\x80\xe6\x70":
        raise SystemExit("snapshot does not contain MEMTEST Setup.S at 0x90200")
    return snap


def build_boot_code() -> bytes:
    # Copied to 0x0060:0000 by the FreeDOS boot sector.  It loads 64 tracks
    # (18 sectors each) to 0x10000..0x9ffff and jumps to MEMTEST Setup.S.
    return bytes.fromhex(
        "fbb800108ec031c08ed8c70600054000b501b600b101b200b8120231dbcd13722a8cc00540028ec0fec680fe027204b600fec5832e00050175de31c08ed8c606f83f12baf43fea00002090b8460ecd10faf4"
    )


def build_bpb(freedos: Path) -> bytearray:
    # Use the exact FreeDOS 1.2 boot sector: Award's boot block already accepts it.
    boot = bytearray(freedos.read_bytes()[:SECTOR])
    if boot[510:512] != b"\x55\xaa":
        raise SystemExit(f"{freedos}: not a boot sector")
    return boot


def fat_set(fat: bytearray, index: int, value: int) -> None:
    off = index + index // 2
    if index & 1:
        fat[off] = (fat[off] & 0x0F) | ((value << 4) & 0xF0)
        fat[off + 1] = (value >> 4) & 0xFF
    else:
        fat[off] = value & 0xFF
        fat[off + 1] = (fat[off + 1] & 0xF0) | ((value >> 8) & 0x0F)


def build_fat() -> bytes:
    fat = bytearray(9 * SECTOR)
    fat_set(fat, 0, 0xFF0)
    fat_set(fat, 1, 0xFFF)
    fat_set(fat, 2, 0xFFF)  # KERNEL.SYS is one sector
    last = START_CLUSTER + SNAPSHOT_SECTORS - 1
    for cl in range(START_CLUSTER, last + 1):
        fat_set(fat, cl, 0xFFF if cl == last else cl + 1)
    return bytes(fat)


def build_root(loader_size: int) -> bytes:
    root = bytearray(14 * SECTOR)

    def entry(name: bytes, cluster: int, size: int, attr: int = 0x20) -> bytes:
        e = bytearray(32)
        e[0:11] = name
        e[11] = attr
        e[26:28] = cluster.to_bytes(2, "little")
        e[28:32] = size.to_bytes(4, "little")
        return bytes(e)

    root[0:32] = entry(b"KERNEL  SYS", 2, loader_size)
    root[32:64] = entry(b"SNAP    BIN", START_CLUSTER, SNAPSHOT_SIZE)
    return bytes(root)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("source", type=Path, nargs="?", default=Path("assets/floppy/MEMTEST.IMG"))
    ap.add_argument("output", type=Path, nargs="?", default=Path("assets/floppy/MEMTEST_WRAPPER.IMG"))
    ap.add_argument("--emulator", type=Path, default=Path("build/oldbox"))
    ap.add_argument("--freedos", type=Path, default=Path("assets/floppy/freedos1440.img"))
    args = ap.parse_args()

    snap = run_snapshot(args.emulator, args.source)
    boot = build_bpb(args.freedos)
    code = build_boot_code()
    # KERNEL.SYS is a one-sector raw loader at cluster 2.
    image = bytearray(b"\x00" * (TOTAL_SECTORS * SECTOR))
    image[0:SECTOR] = boot
    fat = build_fat()
    image[1 * SECTOR:10 * SECTOR] = fat
    image[10 * SECTOR:19 * SECTOR] = fat
    image[19 * SECTOR:33 * SECTOR] = build_root(len(code))
    image[33 * SECTOR:33 * SECTOR + len(code)] = code
    image[DATA_LBA * SECTOR:DATA_LBA * SECTOR + len(snap)] = snap
    args.output.write_bytes(image)
    print(f"wrote {args.output} ({len(image)} bytes)")


if __name__ == "__main__":
    main()
