#!/usr/bin/env python3
"""
make_floppy.py - Create a 1.44 MB FAT12 floppy image from a directory,
using only the Python standard library.

Supports subdirectories (recursively). Filenames are converted to FAT 8.3.
"""

import os
import sys
import struct

# --- Geometry constants for a standard 1.44 MB floppy ---
SECTOR_SIZE       = 512
TOTAL_SECTORS     = 2880
RESERVED_SECTORS  = 1
NUM_FATS          = 2
ROOT_ENTRIES      = 224
SECTORS_PER_FAT   = 9
ROOT_DIR_SECTORS  = 14
DATA_START_SECTOR = RESERVED_SECTORS + NUM_FATS * SECTORS_PER_FAT + ROOT_DIR_SECTORS  # 33
CLUSTER_SIZE      = SECTOR_SIZE  # 1 sector per cluster
FIRST_DATA_CLUSTER = 2
MAX_CLUSTERS      = 2847          # (2880 - 33) data sectors

ENTRIES_PER_CLUSTER = CLUSTER_SIZE // 32  # 16

ATTR_DIRECTORY = 0x10
ATTR_ARCHIVE   = 0x20


# --------------------------------------------------------------------------
# Name conversion
# --------------------------------------------------------------------------
def to_fat83(name):
    """Convert a single path component to (name8, ext3) bytes, uppercase."""
    up = name.upper()
    if '.' in up:
        base, ext = up.rsplit('.', 1)
    else:
        base, ext = up, ''
    allowed = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")
    base = ''.join(c for c in base if c in allowed)[:8]
    ext  = ''.join(c for c in ext  if c in allowed)[:3]
    return base.ljust(8).encode('ascii'), ext.ljust(3).encode('ascii')


# --------------------------------------------------------------------------
# Directory entry helpers
# --------------------------------------------------------------------------
def make_dirent(name8, ext3, attr, cluster, size):
    """Build a 32-byte FAT directory entry."""
    e = bytearray(32)
    e[0:8]   = name8
    e[8:11]  = ext3
    e[11]    = attr
    # Timestamps left as zero.
    struct.pack_into('<H', e, 26, cluster & 0xFFFF)
    struct.pack_into('<I', e, 28, size & 0xFFFFFFFF)
    return bytes(e)


def make_dot_entry(name8, ext3, cluster):
    """Entry for '.' or '..' - starts with the name, has ATTR_DIRECTORY."""
    return make_dirent(name8, ext3, ATTR_DIRECTORY, cluster, 0)


# --------------------------------------------------------------------------
# FAT12 packing
# --------------------------------------------------------------------------
def pack_fat12(entries):
    """
    Pack a list of 12-bit FAT entries into bytes (9 sectors = 4608 bytes).
    `entries` is a list of ints (length up to 4085).
    """
    fat = list(entries)
    # Pad to even length so pairing works.
    if len(fat) % 2:
        fat.append(0)
    raw = bytearray()
    for i in range(0, len(fat), 2):
        e0 = fat[i]   & 0xFFF
        e1 = fat[i+1] & 0xFFF
        raw.append(e0 & 0xFF)
        raw.append(((e0 >> 8) & 0x0F) | ((e1 & 0x0F) << 4))
        raw.append((e1 >> 4) & 0xFF)
    target = SECTORS_PER_FAT * SECTOR_SIZE
    if len(raw) < target:
        raw.extend(b'\x00' * (target - len(raw)))
    return bytes(raw[:target])


# --------------------------------------------------------------------------
# Boot sector
# --------------------------------------------------------------------------
def make_boot_sector(volume_label=b'NO NAME    '):
    bs = bytearray(SECTOR_SIZE)
    bs[0:3]   = b'\xEB\x3C\x90'
    bs[3:11]  = b'MSDOS5.0'
    struct.pack_into('<H', bs, 11, SECTOR_SIZE)
    bs[13] = 1
    struct.pack_into('<H', bs, 14, RESERVED_SECTORS)
    bs[16] = NUM_FATS
    struct.pack_into('<H', bs, 17, ROOT_ENTRIES)
    struct.pack_into('<H', bs, 19, TOTAL_SECTORS)
    bs[21] = 0xF0
    struct.pack_into('<H', bs, 22, SECTORS_PER_FAT)
    struct.pack_into('<H', bs, 24, 18)
    struct.pack_into('<H', bs, 26, 2)
    struct.pack_into('<I', bs, 28, 0)
    struct.pack_into('<I', bs, 32, 0)
    bs[36] = 0x00
    bs[37] = 0x00
    bs[38] = 0x29
    struct.pack_into('<I', bs, 39, 0x12345678)
    bs[43:54] = volume_label
    bs[54:62] = b'FAT12   '
    bs[510] = 0x55
    bs[511] = 0xAA
    return bytes(bs)


# --------------------------------------------------------------------------
# Floppy image builder
# --------------------------------------------------------------------------
class FloppyBuilder:
    def __init__(self):
        self.image = bytearray(TOTAL_SECTORS * SECTOR_SIZE)
        self.fat = [0] * 4085
        self.fat[0] = 0xFF8   # media descriptor
        self.fat[1] = 0xFFF   # reserved
        self.next_free_cluster = FIRST_DATA_CLUSTER
        # Pre-allocate space tracking (in clusters)
        self.max_clusters = MAX_CLUSTERS

    # ---- cluster helpers -------------------------------------------------
    def alloc_clusters(self, count):
        """Reserve `count` consecutive clusters and return their numbers."""
        if self.next_free_cluster + count - 1 > self.max_clusters + 1:
            raise RuntimeError("Floppy image is full (out of clusters)")
        start = self.next_free_cluster
        clusters = list(range(start, start + count))
        self.next_free_cluster += count
        return clusters

    def link_chain(self, clusters):
        """Mark a chain of clusters in the FAT; last one becomes EOF."""
        for i, cl in enumerate(clusters):
            if i == len(clusters) - 1:
                self.fat[cl] = 0xFFF
            else:
                self.fat[cl] = clusters[i + 1]

    def write_cluster_data(self, clusters, data):
        """Copy `data` into the given (contiguous) clusters."""
        for i, cl in enumerate(clusters):
            offset = (DATA_START_SECTOR * SECTOR_SIZE) + (cl - FIRST_DATA_CLUSTER) * CLUSTER_SIZE
            chunk = data[i * CLUSTER_SIZE:(i + 1) * CLUSTER_SIZE]
            self.image[offset:offset + len(chunk)] = chunk

    # ---- directory serialization ----------------------------------------
    def serialize_dir(self, entries):
        """
        Given a list of 32-byte directory entries, allocate enough clusters,
        write the entries, link the chain, and return the first cluster
        (or 0 if the directory is empty - it never is, thanks to '.' and '..').
        """
        if not entries:
            return 0
        num_clusters = (len(entries) + ENTRIES_PER_CLUSTER - 1) // ENTRIES_PER_CLUSTER
        clusters = self.alloc_clusters(num_clusters)
        self.link_chain(clusters)
        blob = b''.join(entries)
        self.write_cluster_data(clusters, blob)
        return clusters[0]

    # ---- recursive directory building -----------------------------------
    def build_directory(self, src_path, self_cluster, parent_cluster):
        """
        Recursively populate a directory inside the image.

        src_path       : local directory to read from
        self_cluster   : cluster number assigned to this directory
        parent_cluster : cluster of parent directory (0 for root)

        Returns (list_of_32byte_entries, first_cluster_used_by_this_dir).
        The caller writes entries for each child, but here we build the
        full entry list including '.' and '..'.
        """
        entries = []
        # '.' and '..'
        entries.append(make_dot_entry(b'.       ', b'   ', self_cluster))
        entries.append(make_dot_entry(b'..      ', b'   ', parent_cluster if parent_cluster != 0 else 0))

        # Files first (keeps directory tidy; order is not mandated by FAT)
        subdirs = []
        for item in sorted(os.listdir(src_path)):
            full = os.path.join(src_path, item)
            if os.path.isdir(full):
                subdirs.append((item, full))
                continue

            name8, ext3 = to_fat83(item)
            with open(full, 'rb') as f:
                content = f.read()
            size = len(content)

            if size == 0:
                entries.append(make_dirent(name8, ext3, ATTR_ARCHIVE, 0, 0))
                continue

            n_clusters = (size + CLUSTER_SIZE - 1) // CLUSTER_SIZE
            clusters = self.alloc_clusters(n_clusters)
            self.link_chain(clusters)
            self.write_cluster_data(clusters, content)
            entries.append(make_dirent(name8, ext3, ATTR_ARCHIVE, clusters[0], size))

        # Then subdirectories: first reserve a cluster for each one so we can
        # write its '.' entry with the correct self-cluster.
        for item, full in subdirs:
            name8, ext3 = to_fat83(item)
            # Reserve first cluster for the subdirectory's own listing
            first = self.alloc_clusters(1)[0]
            entries.append(make_dirent(name8, ext3, ATTR_DIRECTORY, first, 0))
            # Recursively build children; we need to append subsequent clusters
            # to the same chain if the directory listing exceeds one cluster.
            child_entries, extra_clusters = self.build_directory_contents(
                full, first, self_cluster
            )
            # child_entries includes '.' and '..'; append extra clusters to chain
            self.link_chain([first] + extra_clusters)
            # Write entries across [first] + extra_clusters
            blob = b''.join(child_entries)
            self.write_cluster_data([first] + extra_clusters, blob)

        return entries

    def build_directory_contents(self, src_path, first_cluster, parent_cluster):
        """
        Build the entry list for a subdirectory whose first cluster is
        already reserved. Allocate additional clusters if needed.

        Returns (entries, extra_clusters) where extra_clusters is the list
        of clusters *after* the first one. The caller links the chain.
        """
        entries = []
        entries.append(make_dot_entry(b'.       ', b'   ', first_cluster))
        entries.append(make_dot_entry(b'..      ', b'   ', parent_cluster))

        subdirs = []
        for item in sorted(os.listdir(src_path)):
            full = os.path.join(src_path, item)
            if os.path.isdir(full):
                subdirs.append((item, full))
                continue

            name8, ext3 = to_fat83(item)
            with open(full, 'rb') as f:
                content = f.read()
            size = len(content)

            if size == 0:
                entries.append(make_dirent(name8, ext3, ATTR_ARCHIVE, 0, 0))
                continue

            n_clusters = (size + CLUSTER_SIZE - 1) // CLUSTER_SIZE
            clusters = self.alloc_clusters(n_clusters)
            self.link_chain(clusters)
            self.write_cluster_data(clusters, content)
            entries.append(make_dirent(name8, ext3, ATTR_ARCHIVE, clusters[0], size))

        for item, full in subdirs:
            name8, ext3 = to_fat83(item)
            child_first = self.alloc_clusters(1)[0]
            entries.append(make_dirent(name8, ext3, ATTR_DIRECTORY, child_first, 0))
            child_entries, extra = self.build_directory_contents(
                full, child_first, first_cluster
            )
            self.link_chain([child_first] + extra)
            blob = b''.join(child_entries)
            self.write_cluster_data([child_first] + extra, blob)

        # How many clusters do we need for the entries we built?
        needed = (len(entries) + ENTRIES_PER_CLUSTER - 1) // ENTRIES_PER_CLUSTER
        if needed <= 1:
            extra_clusters = []
        else:
            extra_clusters = self.alloc_clusters(needed - 1)
        return entries, extra_clusters

    # ---- root directory (special: not in data area, fixed location) -----
    def build_root(self, src_path):
        """Build root directory entries and write them into the reserved area."""
        entries = []
        subdirs = []
        for item in sorted(os.listdir(src_path)):
            full = os.path.join(src_path, item)
            if os.path.isdir(full):
                subdirs.append((item, full))
                continue
            name8, ext3 = to_fat83(item)
            with open(full, 'rb') as f:
                content = f.read()
            size = len(content)
            if size == 0:
                entries.append(make_dirent(name8, ext3, ATTR_ARCHIVE, 0, 0))
                continue
            n_clusters = (size + CLUSTER_SIZE - 1) // CLUSTER_SIZE
            clusters = self.alloc_clusters(n_clusters)
            self.link_chain(clusters)
            self.write_cluster_data(clusters, content)
            entries.append(make_dirent(name8, ext3, ATTR_ARCHIVE, clusters[0], size))

        for item, full in subdirs:
            name8, ext3 = to_fat83(item)
            child_first = self.alloc_clusters(1)[0]
            entries.append(make_dirent(name8, ext3, ATTR_DIRECTORY, child_first, 0))
            # Parent of a top-level subdir is the root, whose first cluster is 0.
            child_entries, extra = self.build_directory_contents(full, child_first, 0)
            self.link_chain([child_first] + extra)
            blob = b''.join(child_entries)
            self.write_cluster_data([child_first] + extra, blob)

        if len(entries) > ROOT_ENTRIES:
            raise RuntimeError(
                f"Root directory has {len(entries)} entries; max is {ROOT_ENTRIES}"
            )

        root_offset = (RESERVED_SECTORS + NUM_FATS * SECTORS_PER_FAT) * SECTOR_SIZE
        for idx, e in enumerate(entries):
            off = root_offset + idx * 32
            self.image[off:off + 32] = e

    # ---- finalize --------------------------------------------------------
    def finalize(self, out_path):
        # Boot sector
        self.image[0:SECTOR_SIZE] = make_boot_sector()
        # FAT (both copies)
        fat_bytes = pack_fat12(self.fat)
        fat_off = RESERVED_SECTORS * SECTOR_SIZE
        for i in range(NUM_FATS):
            off = fat_off + i * SECTORS_PER_FAT * SECTOR_SIZE
            self.image[off:off + len(fat_bytes)] = fat_bytes
        with open(out_path, 'wb') as f:
            f.write(self.image)


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------
def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <source_directory> <output_image>")
        sys.exit(1)
    src, out = sys.argv[1], sys.argv[2]
    if not os.path.isdir(src):
        print(f"Error: '{src}' is not a directory.")
        sys.exit(1)

    builder = FloppyBuilder()
    builder.build_root(src)
    builder.finalize(out)
    print(f"Floppy image created: {out}")


if __name__ == '__main__':
    main()