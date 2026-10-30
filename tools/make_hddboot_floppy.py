#!/usr/bin/env python3
"""Build a 1.44M floppy that chainloads the MBR of the primary master HDD.

The Award BIOS on this board currently reports "DISK BOOT FAILURE" when
booting the IDE disk directly, but its INT 13h read path works (MS-DOS reads
and writes the same disk).  This tiny boot sector reads LBA 0 of drive 80h
into 0000:7C00 and jumps to it, so Windows 95 can perform its first HDD boot
without relying on the BIOS boot-device selection.

Usage:
    python3 tools/make_hddboot_floppy.py
"""
from pathlib import Path
import struct

SECTOR = 512
TOTAL = 1440 * 1024


def main() -> int:
    template = Path("assets/floppy/msdos7-win95.img")
    out = Path("assets/floppy/hddboot.img")
    if len(template.read_bytes()) < SECTOR:
        raise SystemExit(f"{template} not found or too small")
    floppy = bytearray(template.read_bytes()[:SECTOR])

    # Real-mode boot code, placed after a valid FAT12 BPB so the BIOS accepts
    # the floppy as bootable.  Reads one sector (MBR) to 0000:7C00 and jumps.
    code = bytes.fromhex(
        "FA"            # cli
        "31C0"          # xor ax, ax
        "8ED0"          # mov ss, ax
        "BC0070"        # mov sp, 7000h
        "8EC0"          # mov es, ax
        "B402"          # mov ah, 2
        "B001"          # mov al, 1
        "B500"          # mov ch, 0
        "B101"          # mov cl, 1
        "B600"          # mov dh, 0
        "BA8000"        # mov dx, 80h
        "BB007C"        # mov bx, 7C00h
        "CD13"          # int 13h
        "7205"          # jc fail
        "EA007C0000"    # jmp 0000:7C00
        "BE707C"        # fail: mov si, 7C70h
        "AC"            # lodsb
        "84C0"          # test al, al
        "7407"          # jz halt
        "B40E"          # mov ah, 0Eh
        "CD10"          # int 10h
        "EBF5"          # jmp lodsb
        "F4"            # halt
    )
    image = bytearray(TOTAL)
    boot = bytearray(floppy)
    boot[0:3] = b"\xeb\x3c\x90"
    boot[0x3E:0x3E + len(code)] = code
    boot[0x70:0x70 + 13] = b"HDD READ ERR\0"
    boot[510:512] = b"\x55\xaa"
    image[:SECTOR] = boot
    out.write_bytes(image)
    print(f"wrote {out} ({len(image)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
