"""Parse game/data/fonts/fefont.ffn: the front end's font.

    python tools/fe_font.py

FNTF container. Header words at 0x00.. : magic, glyph count, ..., glyph records
of 12 bytes from 0x80, atlas from 0x870. This prints the header, the first
records as raw fields, and writes the atlas to shots/fe_font_atlas.png so the
bit depth and channel order can be read off it.
"""
import struct, sys, os
from PIL import Image

P = r'C:\Users\dougl\GitHub\SSX3-recomp\game\data\fonts\fefont.ffn'
d = open(P, 'rb').read()
print('size', len(d), 'magic', d[:4])
words = struct.unpack_from('<8I', d, 4)
print('header words:', ' '.join('%08X' % w for w in words), '(as ints:', words, ')')

count = words[0] if 0 < words[0] < 400 else None
print('glyph count guess:', count)

base = 0x80
for i in range(6):
    off = base + i * 12
    f = struct.unpack_from('<6H', d, off)
    print('  glyph %d @%04X:' % (i, off), ' '.join('%5d' % v for v in f), '| hex', ' '.join('%04X' % v for v in f))

atlas = 0x870
w, h = 512, 512
raw = d[atlas:atlas + w * h]
if len(raw) == w * h:
    img = Image.frombytes('L', (w, h), raw)
    img.save(r'C:\Users\dougl\GitHub\SSX3-recomp\shots\fe_font_atlas.png')
    print('wrote atlas %dx%d from %d bytes' % (w, h, len(raw)))
else:
    print('atlas short:', len(raw))
