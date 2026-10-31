"""Convert the raw RGB files tools\\menu_render.bat writes into PNGs, with the
stdlib only (zlib), so no imaging package is needed to look at the layout."""
import glob, os, struct, sys, zlib

def png(path, w, h, rgb):
    raw = b''.join(b'\x00' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    body = (b'\x89PNG\r\n\x1a\n'
            + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw, 6))
            + chunk(b'IEND', b''))
    open(path, 'wb').write(body)

os.chdir(sys.argv[1] if len(sys.argv) > 1 else '.')
for f in sorted(glob.glob('menu_android_*.raw')):
    data = open(f, 'rb').read()
    w, h = 2400, 1080
    if len(data) != w * h * 3:
        side = (len(data) // 3) ** 0.5
        print('%s: unexpected size %d bytes' % (f, len(data)))
        continue
    png(f[:-4] + '.png', w, h, data)
    print(f[:-4] + '.png')
