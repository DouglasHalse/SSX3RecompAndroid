#!/usr/bin/env python3
"""Build a synthetic XDVDFS image plus the tree it should extract to.

Sector numbering is relative to the partition origin, which a trimmed image
shares with the file: the volume descriptor lives in sector 32 (offset 0x10000)
and file/directory sectors are plain `sector * 2048` file offsets.
"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
VD_SECTOR = 32          # the volume descriptor sits at 0x10000


def dirent(name, start, size, attrs, right=0):
    nb = name.encode("latin-1")
    rec = struct.pack("<HHIIBB", 0, right, start, size, attrs, len(nb)) + nb
    while len(rec) % 4:
        rec += b"\0"
    return rec


def table(entries):
    """entries: list of (name, start, size, attrs). Chained through `right`."""
    recs = [dirent(n, s, sz, a) for (n, s, sz, a) in entries]
    offs = []
    o = 0
    for r in recs:
        offs.append(o)
        o += len(r)
    out = b""
    for i, (n, s, sz, a) in enumerate(entries):
        right = (offs[i + 1] // 4) if i + 1 < len(entries) else 0
        out += dirent(n, s, sz, a, right)
    return out


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "."
    iso_path = os.path.join(out_dir, "test.iso")
    exp_dir = os.path.join(out_dir, "expected")

    files = {
        "data/one.bin": bytes(range(256)) * 300,      # 76800 bytes
        "data/deeper/two.dat": b"A" * 5000,
        "data/with space.bin": b"spaces are legal" * 10,
    }
    xbe = bytearray(3428352)
    xbe[0:4] = b"XBEH"
    struct.pack_into("<I", xbe, 0x104, 0x00010000)
    struct.pack_into("<I", xbe, 0x108, 0x1000)
    struct.pack_into("<I", xbe, 0x10C, 0x003AF780)
    struct.pack_into("<I", xbe, 0x110, 0x184)
    struct.pack_into("<I", xbe, 0x118, 0x10184)
    struct.pack_into("<I", xbe, 0x184 + 8, 0x4541003C)   # certificate title id
    struct.pack_into("<I", xbe, 0x10000, 0x8910408B)     # entry code dword
    files["default.xbe"] = bytes(xbe)

    def sectors(n):
        return max(1, (n + SECTOR - 1) // SECTOR)

    alloc = {}
    sector = VD_SECTOR + 1
    for rel in sorted(files):
        alloc[rel] = (sector, len(files[rel]))
        sector += sectors(len(files[rel]))

    deep_entries = [(os.path.basename(rel), s, sz, 0x20)
                    for rel, (s, sz) in sorted(alloc.items())
                    if rel.startswith("data/deeper/")]
    deep_table = table(deep_entries)
    deep_sector = sector
    sector += sectors(len(deep_table))

    data_entries = [(os.path.basename(rel), s, sz, 0x20)
                    for rel, (s, sz) in sorted(alloc.items())
                    if rel.startswith("data/") and rel.count("/") == 1]
    data_entries.append(("deeper", deep_sector, len(deep_table), 0x10))
    data_table = table(data_entries)
    data_sector = sector
    sector += sectors(len(data_table))

    root_entries = [("default.xbe", alloc["default.xbe"][0], alloc["default.xbe"][1], 0x20),
                    ("data", data_sector, len(data_table), 0x10),
                    ("../evil.txt", 0, 0, 0x20)]      # must be refused by the extractor
    root_table = table(root_entries)
    root_sector = sector
    sector += sectors(len(root_table))

    img = bytearray(sector * SECTOR)
    vd = bytearray(SECTOR)
    vd[0:20] = MAGIC
    struct.pack_into("<II", vd, 20, root_sector, len(root_table))
    img[VD_SECTOR * SECTOR:(VD_SECTOR + 1) * SECTOR] = vd
    for rel, (s, sz) in alloc.items():
        img[s * SECTOR:s * SECTOR + sz] = files[rel]
    img[deep_sector * SECTOR:deep_sector * SECTOR + len(deep_table)] = deep_table
    img[data_sector * SECTOR:data_sector * SECTOR + len(data_table)] = data_table
    img[root_sector * SECTOR:root_sector * SECTOR + len(root_table)] = root_table

    with open(iso_path, "wb") as f:
        f.write(img)
    for rel, data in files.items():
        p = os.path.join(exp_dir, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)

    print("wrote %s (%d bytes), root table at sector %d, %s" %
          (iso_path, len(img), root_sector, exp_dir))


if __name__ == "__main__":
    main()
