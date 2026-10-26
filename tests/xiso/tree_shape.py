"""Report the shape of every directory table in an XDVDFS image.

The extractor walks each table's binary-search tree recursively, so what
matters for it is the tree's *depth*, not its size: a table built as a chain
(left/right pointing along a line) recurses once per entry, and Android threads
come with a 1 MB stack.

    python tree_shape.py "/path/game.iso"
"""
import struct
import sys

SECTOR = 2048


def main(path):
    f = open(path, "rb")
    f.seek(0x10000)
    vd = f.read(36)
    if vd[:20] != b"MICROSOFT*XBOX*MEDIA":
        print("no descriptor at 0x10000")
        return 1
    root_sector, root_size = struct.unpack("<II", vd[20:28])
    print("root: sector %d, %d bytes" % (root_sector, root_size))

    worst = []
    work = [("", root_sector, root_size, 0)]
    seen_dirs = set()
    while work:
        prefix, sector, size, depth = work.pop()
        if (sector, size) in seen_dirs or size == 0:
            continue
        seen_dirs.add((sector, size))
        f.seek(sector * SECTOR)
        table = f.read(size)
        if len(table) != size:
            print("  %-30s truncated" % (prefix or "/"))
            continue

        entries = []      # (off, left, right, name, is_dir, start, size)
        stack = [(0, 0)]
        visited = set()
        max_depth = 0
        while stack:
            off, d = stack.pop()
            if off + 14 > size or off in visited:
                continue
            visited.add(off)
            max_depth = max(max_depth, d)
            left, right, start, dsize = struct.unpack("<HHII", table[off:off + 12])
            attrs, namelen = table[off + 12], table[off + 13]
            name = table[off + 14:off + 14 + namelen].decode("latin-1")
            entries.append((name, attrs, start, dsize))
            if left:
                stack.append((left * 4, d + 1))
            if right:
                stack.append((right * 4, d + 1))

        print("  %-30s %4d entries, tree depth %d" % (prefix or "/", len(entries), max_depth))
        worst.append((max_depth, len(entries), prefix or "/"))
        for name, attrs, start, dsize in entries:
            if attrs & 0x10:
                work.append((prefix + "/" + name if prefix else name, start, dsize, depth + 1))

    worst.sort(reverse=True)
    print("\ndeepest tables:")
    for d, n, p in worst[:5]:
        print("  depth %d, %d entries: %s" % (d, n, p))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
