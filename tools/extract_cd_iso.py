#!/usr/bin/env python3
"""Extract the data track of a BIN/CUE image into a 2048-byte ISO.

oldbox's ATAPI CD model exposes 2048-byte user-data sectors, while many
CD-ROM dumps are stored as MODE1/2352 BIN/CUE images with 2352-byte raw
sectors (often followed by audio tracks).  This tool copies the first data
track's 2048-byte payload into a plain ISO9660 image that can be passed to
``--cdrom``.  Audio tracks are not included.

The CUE's FILE entry is resolved relative to the CUE itself.  If a BIN is
given without a CUE, a same-named CUE is used when present; otherwise the
BIN is assumed to contain one MODE1/2352 data track.

Usage:
    python3 tools/extract_cd_iso.py \
        "C:\\Isos\\Heroes of Might and Magic 2.cue" \
        --out assets/cdrom/heroes2.iso
"""
from __future__ import annotations

import argparse
import re
import struct
from pathlib import Path

SECTOR_2352 = 2352
SECTOR_2048 = 2048
SYNC = b"\x00" + b"\xff" * 10 + b"\x00"


def msf_to_lba(text: str) -> int:
    minutes, seconds, frames = (int(part) for part in text.split(":"))
    return (minutes * 60 + seconds) * 75 + frames


def parse_cue(cue: Path) -> tuple[Path, list[dict[str, object]]]:
    file_name: str | None = None
    file_type = "BINARY"
    tracks: list[dict[str, object]] = []
    current: dict[str, object] | None = None

    for line in cue.read_text(encoding="latin1", errors="replace").splitlines():
        match = re.match(r'\s*FILE\s+"([^"]+)"\s+(\S+)', line, re.IGNORECASE)
        if not match:
            match = re.match(r"\s*FILE\s+(\S+)\s+(\S+)", line, re.IGNORECASE)
        if match:
            file_name, file_type = match.group(1), match.group(2).upper()
            continue

        match = re.match(r"\s*TRACK\s+(\d+)\s+(\S+)", line, re.IGNORECASE)
        if match:
            current = {"number": int(match.group(1)), "mode": match.group(2).upper(),
                       "lba": None}
            tracks.append(current)
            continue

        match = re.match(r"\s*INDEX\s+0*1\s+(\d+:\d+:\d+)", line, re.IGNORECASE)
        if match and current is not None:
            current["lba"] = msf_to_lba(match.group(1))

    if file_name is None:
        raise SystemExit(f"{cue}: no FILE entry")
    if file_type != "BINARY":
        raise SystemExit(f"{cue}: only BINARY tracks are supported (got {file_type})")

    image = Path(file_name)
    if not image.is_absolute():
        image = cue.parent / image
    if not image.exists():
        raise SystemExit(f"{cue}: file not found: {image}")
    if not tracks:
        raise SystemExit(f"{cue}: no TRACK entries")
    return image, tracks


def infer_bin_track(image: Path) -> tuple[Path, list[dict[str, object]]]:
    """Assume a single MODE1/2352 (or already cooked 2048) data track."""
    size = image.stat().st_size
    if size % SECTOR_2352 == 0:
        return image, [{"number": 1, "mode": "MODE1/2352", "lba": 0}]
    if size % SECTOR_2048 == 0:
        return image, [{"number": 1, "mode": "MODE1/2048", "lba": 0}]
    raise SystemExit(f"{image}: size is neither a 2352- nor a 2048-byte sector multiple")


def data_track(tracks: list[dict[str, object]]) -> tuple[int, str, int, int]:
    supported = {"MODE1/2352", "MODE2/2352", "MODE1/2048"}
    for index, track in enumerate(tracks):
        if track["mode"] not in supported or track["lba"] is None:
            continue
        start = int(track["lba"])
        if index + 1 < len(tracks) and tracks[index + 1]["lba"] is not None:
            end = int(tracks[index + 1]["lba"])
        else:
            end = -1
        return start, str(track["mode"]), index, end
    raise SystemExit("no data track (MODE1/2352, MODE2/2352 or MODE1/2048) found")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path, help=".cue or .bin image")
    ap.add_argument("--out", type=Path, default=None,
                    help="output ISO (default: assets/cdrom/<input stem>.iso)")
    args = ap.parse_args()

    image = args.input
    if image.suffix.lower() == ".cue":
        image, tracks = parse_cue(image)
    else:
        cue = image.with_suffix(".cue")
        if cue.exists():
            print(f"using cue sheet: {cue}")
            image, tracks = parse_cue(cue)
        else:
            image, tracks = infer_bin_track(image)

    start, mode, track_index, end = data_track(tracks)
    sector_size = SECTOR_2048 if mode == "MODE1/2048" else SECTOR_2352
    total_sectors = image.stat().st_size // sector_size
    if end < 0:
        end = total_sectors
    if end <= start:
        raise SystemExit(f"empty data track: LBA {start}..{end - 1}")
    if end > total_sectors:
        end = total_sectors

    out = args.out
    if out is None:
        out = Path("assets/cdrom") / (args.input.stem + ".iso")
    out.parent.mkdir(parents=True, exist_ok=True)

    data_offset = 24 if mode == "MODE2/2352" else 16
    with image.open("rb") as src, out.open("wb") as dst:
        src.seek(start * sector_size)
        if sector_size == SECTOR_2352:
            first = src.read(SECTOR_2352)
            if len(first) < SECTOR_2352 or first[:12] != SYNC:
                out.unlink(missing_ok=True)
                raise SystemExit(f"{image}: sector at LBA {start} has no Mode 1/2 sync header")
            dst.write(first[data_offset:data_offset + SECTOR_2048])
            for _ in range(start + 1, end):
                raw = src.read(SECTOR_2352)
                if len(raw) < SECTOR_2352:
                    break
                dst.write(raw[data_offset:data_offset + SECTOR_2048])
        else:
            src.seek(start * SECTOR_2048)
            for _ in range(start, end):
                raw = src.read(SECTOR_2048)
                if len(raw) < SECTOR_2048:
                    break
                dst.write(raw)

    with out.open("rb") as f:
        f.seek(16 * SECTOR_2048)
        pvd = f.read(SECTOR_2048)
    if len(pvd) < SECTOR_2048 or pvd[1:6] != b"CD001":
        out.unlink(missing_ok=True)
        raise SystemExit(f"{image}: extracted track has no ISO9660 primary volume descriptor")
    volume_sectors = struct.unpack_from("<I", pvd, 80)[0]
    if volume_sectors == 0:
        out.unlink(missing_ok=True)
        raise SystemExit(f"{image}: ISO9660 volume size is zero")
    with out.open("r+b") as f:
        f.truncate(volume_sectors * SECTOR_2048)

    print(f"data track: {mode}, LBA {start}..{end - 1} ({end - start} sectors)")
    print(f"wrote {out} ({out.stat().st_size} bytes, {volume_sectors} sectors)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
