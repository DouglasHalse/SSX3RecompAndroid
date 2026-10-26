#!/usr/bin/env python3
"""Extract an original Xbox (XDVDFS) ISO to a folder.

XDVDFS dirent (little endian):
    u16 left, u16 right, u32 start_sector, u32 size, u8 attributes, u8 namelen, name
`left`/`right` are B-tree child offsets in 4-BYTE UNITS from the start of the
same directory table. `start_sector` is relative to the partition origin.
Handles trimmed .xiso images (partition at 0) and full redump-style images
(partition at 0x18300000).

    python xiso_extract.py "game/SSX 3 (USA).iso" --out game --only default.xbe
    python xiso_extract.py "game/SSX 3 (USA).iso" --out game
"""
import argparse
import os
import struct
import sys

MAGIC = b"MICROSOFT*XBOX*MEDIA"
SECTOR = 2048


def find_vd(f, size):
    if size > 0x10000:
        f.seek(0x10000)
        if f.read(20) == MAGIC:
            return 0x10000
    cand = 0x18300000 + 0x10000
    if cand + 20 <= size:
        f.seek(cand)
        if f.read(20) == MAGIC:
            return cand
    for off in range(0x10000, min(size, 0x40000000), SECTOR):
        f.seek(off)
        if f.read(20) == MAGIC:
            return off
    raise SystemExit("no XDVDFS volume descriptor found (not an Xbox ISO?)")


def tree(table, off, seen, out):
    """Walk one directory table's B-tree from byte offset `off`."""
    if off in seen or off + 14 > len(table):
        return
    seen.add(off)
    left, right, start, size, attrs, namelen = struct.unpack_from("<HHIIBB", table, off)
    if namelen == 0 or off + 14 + namelen > len(table):
        return
    if left:
        tree(table, left * 4, seen, out)
    if right:
        tree(table, right * 4, seen, out)
    out.append((table[off + 14:off + 14 + namelen].decode("latin-1"), start, size, attrs))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("iso")
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", nargs="*", default=None,
                    help="only files with these base names (case-insensitive)")
    a = ap.parse_args()
    only = {n.lower() for n in a.only} if a.only else None

    size = os.path.getsize(a.iso)
    nfiles = nbytes = 0
    with open(a.iso, "rb") as f:
        vd = find_vd(f, size)
        base = vd - 0x10000
        f.seek(vd)
        hdr = f.read(36)
        root_sector, root_size = struct.unpack_from("<II", hdr, 20)
        print("partition origin 0x%X, root table sector %d (%d bytes)" % (base, root_sector, root_size))

        dirs = [("", root_sector, root_size)]
        seen_dirs = set()
        while dirs:
            dpath, sec, dsize = dirs.pop()
            if (sec, dsize) in seen_dirs or dsize == 0:
                continue
            seen_dirs.add((sec, dsize))
            f.seek(base + sec * SECTOR)
            table = f.read(dsize)
            ents = []
            tree(table, 0, set(), ents)
            for name, start, fsize, attrs in ents:
                this = os.path.join(dpath, name) if dpath else name
                if attrs & 0x10:
                    dirs.append((this, start, fsize))
                    continue
                if only and os.path.basename(name).lower() not in only:
                    continue
                dest = os.path.join(a.out, this)
                os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
                f.seek(base + start * SECTOR)
                rem = fsize
                with open(dest, "wb") as o:
                    while rem:
                        chunk = f.read(min(1 << 22, rem))
                        if not chunk:
                            raise SystemExit("truncated image at %s" % this)
                        o.write(chunk)
                        rem -= len(chunk)
                nfiles += 1
                nbytes += fsize
                print("%10d  %s" % (fsize, this))
                sys.stdout.flush()
    print("extracted %d files, %.1f MiB" % (nfiles, nbytes / 2**20))


if __name__ == "__main__":
    main()
