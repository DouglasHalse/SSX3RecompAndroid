"""Read the game's own code: XBE section map, then the list class's methods.

    python tools/xbe_code.py <vtable-va> [count]

Any object the port hooks has a vtable; the methods behind it are the only
reliable statement of where its fields live and how its arrays are walked. This
maps virtual addresses to file offsets through the XBE section table and
disassembles each method, so a field like "+0x96 items" can be traced to the
store and load that matter.
"""
import struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

XBE = r'C:\Users\dougl\GitHub\SSX3-recomp\game\default.xbe'
img = open(XBE, 'rb').read()
base = struct.unpack_from('<I', img, 0x104)[0]
nsect = struct.unpack_from('<I', img, 0x11C)[0]
# 0x120 holds the *address* of the section table; the headers sit in the image's
# first pages, so the file offset is that address minus the base.
sect_tbl = struct.unpack_from('<I', img, 0x120)[0] - base
sects = []
for i in range(nsect):
    off = sect_tbl + i * 0x38
    flags, va, vsize, faddr, fsize = struct.unpack_from('<5I', img, off)
    sects.append((va, vsize, faddr, fsize))
print('base %08X, %d sections' % (base, nsect))
for va, vsize, faddr, fsize in sects:
    print('  section %08X-%08X file %06X (%d bytes)' % (va, va + vsize, faddr, fsize))


def va2off(va):
    for sva, vsize, faddr, fsize in sects:
        if sva <= va < sva + vsize:
            return faddr + (va - sva)
    return None


def code(va, n):
    off = va2off(va)
    if off is None:
        return b''
    return img[off:off + n]


if len(sys.argv) > 1:
    arg = sys.argv[1]
    if arg.startswith('fn:'):
        fn = int(arg[3:], 16)
        n = int(sys.argv[2]) if len(sys.argv) > 2 else 200
        md = Cs(CS_ARCH_X86, CS_MODE_32)
        print('\nfunction %08X' % fn)
        for ins in md.disasm(code(fn, n * 8), fn):
            print('  %08X  %-8s %s' % (ins.address, ins.mnemonic, ins.op_str))
            if ins.mnemonic == 'ret':
                n -= 1
                if n <= 0:
                    break
        raise SystemExit(0)

    vt = int(arg, 16)
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 6
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    off = va2off(vt)
    print('\nvtable %08X' % vt)
    for i in range(count):
        fn = struct.unpack_from('<I', img, off + i * 4)[0]
        if not (base <= fn < 0x00400000):
            print('  [%d] %08X (not code)' % (i, fn))
            continue
        print('  [%d] %08X' % (i, fn))
        for ins in md.disasm(code(fn, 400), fn):
            print('      %08X  %-8s %s' % (ins.address, ins.mnemonic, ins.op_str))
            if ins.mnemonic in ('ret', 'jmp') and ins.op_str.startswith('dword'):
                break
