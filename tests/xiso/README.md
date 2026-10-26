# XDVDFS extractor tests

`src/android/xiso.c` reads a player's own disc image on the device: it finds the
Xbox filesystem, fingerprints `default.xbe` against the PAL disc this port was
recompiled from, and extracts the tree to app storage. It has no Android
dependency, so it is tested here against synthetic images instead of a disc.

    tests/xiso/run.sh

What it checks, with nothing but a C compiler, `python3` and `diff`:

- A trimmed `.xiso` (volume descriptor at 0x10000) and a redump-style image
  (partition at 0x18300000, behind the 405 MiB video partition) are both found.
- The XBE fingerprint matches the image this port was built for; a different
  region's disc is refused before anything is extracted.
- Extraction is byte-identical to the source tree, including a name with a
  space and a nested directory.
- A dirent that names `../evil.txt` cannot escape the destination.

The images are built by `gen_xiso.py`: one sector of volume descriptor in
sector 32, directory tables chained through the B-tree `right` link, and a
3,428,352-byte XBE header with the PAL geometry. Total size is about 3.5 MiB,
plus a sparse 405 MiB prefix for the redump case.
