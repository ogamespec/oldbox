#!/usr/bin/env python3
"""Create a clean 500 MiB FAT16 target disk for the Windows 95 setup.

The reference 500 MiB hard-disk image already contains a primary FAT16
partition and the Megafortress installation used by the game boot option.
Installing Windows 95 into that image would destroy the game, so this tool
copies the image and empties the partition (all files and directories in the
root are deleted and their clusters are returned to the FAT).  The MBR,
partition table, boot sector and volume label are left untouched.

Only the Python standard library is used.

Usage:
    python3 tools/make_win95_hdd.py
    python3 tools/make_win95_hdd.py \
        --source assets/hdd/oldbox-hdd-500mb.img \
        --out    assets/hdd/win95-hdd.img
"""
from __future__ import annotations

import argparse
import shutil
import struct
from pathlib import Path

SECTOR = 512
ATTR_VOLUME_ID = 0x08
ATTR_DIRECTORY = 0x10
ATTR_LONG_NAME = 0x0F


def u16(buf: bytes | bytearray, off: int) -> int:
    return struct.unpack_from("<H", buf, off)[0]


def u32(buf: bytes | bytearray, off: int) -> int:
    return struct.unpack_from("<I", buf, off)[0]


def put16(buf: bytearray, off: int, value: int) -> None:
    struct.pack_into("<H", buf, off, value)


def fat16_get(fat: bytearray, cluster: int) -> int:
    return u16(fat, cluster * 2)


def fat16_set(fat: bytearray, cluster: int, value: int) -> None:
    put16(fat, cluster * 2, value)


class Fat16Volume:
    def __init__(self, image: bytearray, base: int) -> None:
        self.image = image
        self.base = base
        bs = image[base:base + SECTOR]
        if bs[510:512] != b"\x55\xaa":
            raise SystemExit("partition boot sector has no 0x55AA signature")
        self.bps = u16(bs, 11)
        self.spc = bs[13]
        self.reserved = u16(bs, 14)
        self.fats = bs[16]
        self.root_entries = u16(bs, 17)
        self.total = u16(bs, 19) or u32(bs, 32)
        self.fat_size = u16(bs, 22) or u32(bs, 36)
        if self.bps != SECTOR or self.fats not in (1, 2):
            raise SystemExit("unsupported FAT16 BPB")
        self.root_start = base + (self.reserved + self.fats * self.fat_size) * self.bps
        self.data_start = self.root_start + self.root_entries * 32
        self.cluster_bytes = self.bps * self.spc
        self.max_cluster = (self.total * self.bps - (self.data_start - base)) // self.cluster_bytes + 1
        self.fat = bytearray(image[base + self.reserved * self.bps:
                                   base + (self.reserved + self.fat_size) * self.bps])

    def cluster_offset(self, cluster: int) -> int:
        return self.data_start + (cluster - 2) * self.cluster_bytes

    def free_chain(self, first: int) -> None:
        cluster = first
        seen = 0
        while 2 <= cluster < self.max_cluster:
            nxt = fat16_get(self.fat, cluster)
            fat16_set(self.fat, cluster, 0)
            cluster = nxt
            seen += 1
            if seen > self.max_cluster:
                raise SystemExit("FAT loop while freeing a cluster chain")

    def clear_directory(self, first: int, size: int) -> None:
        entries = size // 32 if size else 0x10000
        cluster = first
        entries_left = entries
        while 2 <= cluster < self.max_cluster and entries_left:
            off = self.cluster_offset(cluster)
            count = min(entries_left, self.cluster_bytes // 32)
            for i in range(count):
                entry = off + i * 32
                name = self.image[entry:entry + 11]
                attr = self.image[entry + 11]
                if name[0] == 0:
                    return
                if name[0] == 0xE5 or attr == ATTR_LONG_NAME or attr & ATTR_VOLUME_ID:
                    continue
                if name in (b".          ", b"..         "):
                    continue
                child = u16(self.image, entry + 26)
                child_size = u32(self.image, entry + 28)
                if attr & ATTR_DIRECTORY:
                    self.clear_directory(child, child_size)
                self.free_chain(child)
                self.image[entry] = 0xE5
            entries_left -= count
            cluster = fat16_get(self.fat, cluster)

    def clear_root(self) -> None:
        for i in range(self.root_entries):
            entry = self.root_start + i * 32
            name = self.image[entry:entry + 11]
            attr = self.image[entry + 11]
            if name[0] == 0:
                break
            if name[0] == 0xE5 or attr == ATTR_LONG_NAME or attr & ATTR_VOLUME_ID:
                continue
            first = u16(self.image, entry + 26)
            size = u32(self.image, entry + 28)
            if attr & ATTR_DIRECTORY:
                self.clear_directory(first, size)
            self.free_chain(first)
            self.image[entry] = 0xE5

    def flush_fat(self) -> None:
        for i in range(self.fats):
            off = self.base + (self.reserved + i * self.fat_size) * self.bps
            self.image[off:off + len(self.fat)] = self.fat


def find_partition(mbr: bytes) -> tuple[int, int]:
    for i in range(4):
        entry = 446 + i * 16
        status = mbr[entry]
        ptype = mbr[entry + 4]
        lba = u32(mbr, entry + 8)
        sectors = u32(mbr, entry + 12)
        if status in (0x00, 0x80) and ptype in (0x04, 0x06, 0x0E) and lba and sectors:
            return lba * SECTOR, sectors * SECTOR
    raise SystemExit("no FAT16 primary partition found in the source image")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--source", type=Path,
                    default=Path("assets/hdd/oldbox-hdd-500mb.img"))
    ap.add_argument("--out", type=Path,
                    default=Path("assets/hdd/win95-hdd.img"))
    args = ap.parse_args()

    if not args.source.exists():
        raise SystemExit(f"{args.source} not found; build it with tools/make_hdd.py")
    if args.source.resolve() == args.out.resolve():
        raise SystemExit("--source and --out must be different files")
    if args.out.exists():
        args.out.unlink()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.source, args.out)

    image = bytearray(args.out.read_bytes())
    base, size = find_partition(image[:SECTOR])
    volume = Fat16Volume(image, base)
    volume.clear_root()
    volume.flush_fat()
    args.out.write_bytes(image)

    print(f"wrote {args.out} ({len(image)} bytes)")
    print(f"  FAT16 partition at LBA {base // SECTOR}, {size // (1024 * 1024)} MiB, root emptied")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
