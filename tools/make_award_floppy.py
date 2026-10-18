#!/usr/bin/env python3
"""Create a bootable FreeDOS 1.44M floppy carrying AWDFLASH and an Award BIOS.

Only the FreeDOS boot sector, KERNEL.SYS and COMMAND.COM are taken from a
FreeDOS floppy image; the destination filesystem is rebuilt from scratch so a
256 KiB BIOS image fits on the disk.
"""
from __future__ import annotations
import argparse, struct
from pathlib import Path


def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def p16(b, o, v): struct.pack_into('<H', b, o, v)
def p32(b, o, v): struct.pack_into('<I', b, o, v)


def fat12_get(fat, n):
    o = n + n // 2
    v = fat[o] | (fat[o + 1] << 8)
    return (v >> 4) & 0xFFF if n & 1 else v & 0xFFF


def fat12_set(fat, n, value):
    o = n + n // 2
    v = fat[o] | (fat[o + 1] << 8)
    v = (v & 0x000F) | ((value & 0xFFF) << 4) if n & 1 else (v & 0xF000) | (value & 0xFFF)
    fat[o] = v & 0xFF
    fat[o + 1] = (v >> 8) & 0xFF


def fat_read_file(img, fat, root_start, root_entries, data_start, cluster_bytes, name):
    for i in range(root_entries):
        o = root_start + i * 32
        if img[o] == 0:
            break
        if bytes(img[o:o + 11]) != name:
            continue
        first = u16(img, o + 26)
        size = u32(img, o + 28)
        out = bytearray()
        c = first
        while 2 <= c < 0xFF8 and len(out) < size:
            off = data_start + (c - 2) * cluster_bytes
            out += img[off:off + cluster_bytes]
            c = fat12_get(fat, c)
        return bytes(out[:size])
    raise SystemExit(f'base image does not contain {name!r}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', required=True, type=Path)
    ap.add_argument('--awardflash', required=True, type=Path)
    ap.add_argument('--bios', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--bios-name', default='BXC00920.BIN')
    ap.add_argument('--autoexec', default=None, help='override AUTOEXEC.BAT command line')
    a = ap.parse_args()

    base = bytearray(a.base.read_bytes())
    if len(base) != 1474560 or base[510:512] != b'\x55\xaa':
        raise SystemExit('base must be a bootable 1.44M FreeDOS image')
    bps, spc, reserved, fats, root_entries, total, fat_size = (
        u16(base, 11), base[13], u16(base, 14), base[16], u16(base, 17),
        u16(base, 19), u16(base, 22))
    if (bps, spc, fats) != (512, 1, 2):
        raise SystemExit('unsupported BPB')
    root_start = (reserved + fats * fat_size) * bps
    data_start = root_start + root_entries * 32
    cluster_bytes = bps * spc
    max_cluster = (total * bps - data_start) // cluster_bytes + 1
    base_fat_off = reserved * bps
    base_fat = base[base_fat_off:base_fat_off + fat_size * bps]
    kernel = fat_read_file(base, base_fat, root_start, root_entries, data_start, cluster_bytes, b'KERNEL  SYS')
    command = fat_read_file(base, base_fat, root_start, root_entries, data_start, cluster_bytes, b'COMMAND COM')

    img = bytearray(1474560)
    img[:512] = base[:512]
    fat = bytearray(fat_size * bps)
    fat[0], fat[1], fat[2] = 0xF0, 0xFF, 0xFF

    def alloc(data):
        need = (len(data) + cluster_bytes - 1) // cluster_bytes
        free = [c for c in range(2, max_cluster + 1) if fat12_get(fat, c) == 0 and c < 0xFF8]
        if len(free) < need:
            raise SystemExit(f'not enough space: need {need} clusters, have {len(free)}')
        chain = free[:need]
        for i, c in enumerate(chain):
            fat12_set(fat, c, chain[i + 1] if i + 1 < len(chain) else 0xFFF)
            off = data_start + (c - 2) * cluster_bytes
            chunk = data[i * cluster_bytes:(i + 1) * cluster_bytes]
            img[off:off + len(chunk)] = chunk
        return chain[0]

    files = [
        ('KERNEL  SYS', kernel),
        ('COMMAND COM', command),
        ('AWDFLASHEXE', a.awardflash.read_bytes()),
        (a.bios_name.upper().replace('.', '').ljust(11)[:11], a.bios.read_bytes()),
        ('AUTOEXECBAT', b'@ECHO OFF\r\n' + (a.autoexec.encode() if a.autoexec else
         ('A:\\AWDFLASH.EXE ' + a.bios_name.upper() + ' OLD.BIN /PY /SY /E').encode()) + b'\r\n'),
    ]
    for i, (name, data) in enumerate(files):
        assert len(name) == 11, name
        first = alloc(data)
        o = root_start + i * 32
        img[o:o + 11] = name.encode('ascii')
        img[o + 11] = 0x20
        p16(img, o + 22, 0)
        p16(img, o + 24, 0x21)
        p16(img, o + 26, first)
        p32(img, o + 28, len(data))

    img[base_fat_off:base_fat_off + len(fat)] = fat
    img[base_fat_off + len(fat):base_fat_off + 2 * len(fat)] = fat
    a.out.write_bytes(img)
    print(f'wrote {a.out} ({len(img)} bytes), BIOS={len(a.bios.read_bytes())} bytes')


if __name__ == '__main__':
    main()
