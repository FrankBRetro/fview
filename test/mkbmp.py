import struct, sys, random
from PIL import Image
out = sys.argv[1]
src = Image.open(sys.argv[2]).convert('RGB').resize((101, 67))
W, H = src.size
random.seed(1)

def fileh(dib_and_pal, pixels):
    off = 14 + len(dib_and_pal)
    return b'BM' + struct.pack('<IHHI', off + len(pixels), 0, 0, off) + dib_and_pal + pixels

def v3(bpp, comp=0, ncol=0, h=None, extra=b'', hsize=40, masks=None):
    h = H if h is None else h
    d = struct.pack('<IiiHHIIiiII', hsize, W, h, 1, bpp, comp, 0, 2835, 2835, ncol, 0)
    if hsize > 40:
        m = masks or (0, 0, 0, 0)
        d += struct.pack('<IIII', *m) + b'\0' * (hsize - 56)
    return d + extra

def rows(fn, bpp, topdown=False):
    rb = ((W * bpp + 31) // 32) * 4
    data = b''
    order = range(H) if topdown else range(H - 1, -1, -1)
    for y in order:
        r = fn(y)
        data += r + b'\0' * (rb - len(r))
    return data

def ref(name, img):
    img.save(f'{out}/{name}.ref.png')

# palette pictures
for bpp in (1, 4, 8):
    q = src.quantize(colors=1 << bpp)
    pal = q.getpalette()[:3 * (1 << bpp)]
    pal += [0] * (3 * (1 << bpp) - len(pal))
    pb = b''.join(bytes([pal[i*3+2], pal[i*3+1], pal[i*3], 0]) for i in range(1 << bpp))
    px = q.load()
    def row(y, bpp=bpp):
        vals = [px[x, y] for x in range(W)]
        if bpp == 8: return bytes(vals)
        if bpp == 4:
            vals += [0]
            return bytes((vals[i] << 4) | vals[i+1] for i in range(0, W, 2))
        b = bytearray((W + 7) // 8)
        for x, v in enumerate(vals):
            if v & 1: b[x >> 3] |= 0x80 >> (x & 7)
        return bytes(b)
    open(f'{out}/p{bpp}.bmp', 'wb').write(fileh(v3(bpp) + pb, rows(row, bpp)))
    ref(f'p{bpp}', q.convert('RGB'))
    if bpp == 8:
        open(f'{out}/p8top.bmp', 'wb').write(fileh(v3(8, h=-H) + pb, rows(row, 8, True)))
        ref('p8top', q.convert('RGB'))
        # OS/2 1.x core header, 3 byte palette
        core = struct.pack('<IHHHH', 12, W, H, 1, 8)
        pb3 = b''.join(bytes([pal[i*3+2], pal[i*3+1], pal[i*3]]) for i in range(256))
        open(f'{out}/os2.bmp', 'wb').write(fileh(core + pb3, rows(row, 8)))
        ref('os2', q.convert('RGB'))
        # RLE8: runs where equal, literals otherwise, an EOL per row
        data = b''
        for y in range(H - 1, -1, -1):
            v = [px[x, y] for x in range(W)]
            x = 0
            while x < W:
                n = 1
                while x + n < W and v[x + n] == v[x] and n < 255: n += 1
                if n >= 3 or W - x < 3:
                    data += bytes([n, v[x]]); x += n
                else:
                    k = 3
                    while x + k < W and k < 255 and not (x + k + 2 < W and v[x+k] == v[x+k+1] == v[x+k+2]): k += 1
                    data += bytes([0, k]) + bytes(v[x:x+k]) + (b'\0' if k & 1 else b''); x += k
            data += b'\0\0'
        data += b'\0\1'
        open(f'{out}/rle8.bmp', 'wb').write(fileh(v3(8, 1) + pb, data))
        ref('rle8', q.convert('RGB'))
    if bpp == 4:
        data = b''
        for y in range(H - 1, -1, -1):
            v = [px[x, y] for x in range(W)]
            x = 0
            while x < W:
                n = 1
                while x + n < W and n < 255 and v[x + n] == v[x]: n += 1
                if n >= 2 or W - x < 3:
                    data += bytes([n, (v[x] << 4) | v[x]]); x += n
                else:
                    k = min(W - x, 7)
                    lit = v[x:x+k] + [0]
                    by = bytes((lit[i] << 4) | lit[i+1] for i in range(0, k, 2))
                    data += bytes([0, k]) + by + (b'\0' if len(by) & 1 else b''); x += k
            data += b'\0\0'
        data += b'\0\1'
        open(f'{out}/rle4.bmp', 'wb').write(fileh(v3(4, 2) + pb, data))
        ref('rle4', q.convert('RGB'))

px = src.load()
def scale(v, bits): return v * 255 // ((1 << bits) - 1)
# 24 bit
open(f'{out}/rgb24.bmp', 'wb').write(fileh(v3(24), rows(lambda y: b''.join(bytes([px[x,y][2], px[x,y][1], px[x,y][0]]) for x in range(W)), 24)))
ref('rgb24', src)
# 16 bit 555 default
r555 = Image.new('RGB', (W, H)); rp = r555.load()
def row555(y):
    b = b''
    for x in range(W):
        r, g, bb = (c >> 3 for c in px[x, y]); rp[x, y] = (scale(r,5), scale(g,5), scale(bb,5))
        b += struct.pack('<H', (r << 10) | (g << 5) | bb)
    return b
open(f'{out}/rgb555.bmp', 'wb').write(fileh(v3(16), rows(row555, 16))); ref('rgb555', r555)
# 16 bit 565 bit fields after a 40 byte header
r565 = Image.new('RGB', (W, H)); rq = r565.load()
def row565(y):
    b = b''
    for x in range(W):
        r, g, bb = px[x, y]; r >>= 3; g >>= 2; bb >>= 3; rq[x, y] = (scale(r,5), scale(g,6), scale(bb,5))
        b += struct.pack('<H', (r << 11) | (g << 5) | bb)
    return b
open(f'{out}/rgb565.bmp', 'wb').write(fileh(v3(16, 3, extra=struct.pack('<III', 0xF800, 0x07E0, 0x1F)), rows(row565, 16))); ref('rgb565', r565)
# 32 bit BGRX
open(f'{out}/rgb32.bmp', 'wb').write(fileh(v3(32), rows(lambda y: b''.join(bytes([px[x,y][2], px[x,y][1], px[x,y][0], 0]) for x in range(W)), 32))); ref('rgb32', src)
# 32 bit V5 with real alpha: left half opaque, right half 50%, composited on white
ra = Image.new('RGB', (W, H)); rap = ra.load()
def rowa(y):
    b = b''
    for x in range(W):
        r, g, bb = px[x, y]; a = 255 if x < W // 2 else 128
        rap[x, y] = tuple((c * a + 255 * (255 - a)) // 255 for c in (r, g, bb))
        b += bytes([bb, g, r, a])
    return b
open(f'{out}/argb32.bmp', 'wb').write(fileh(v3(32, 3, hsize=124, masks=(0xFF0000, 0xFF00, 0xFF, 0xFF000000)), rows(rowa, 32))); ref('argb32', ra)
# 32 bit V4 with an alpha mask but every alpha byte zero: shown opaque
open(f'{out}/zalpha32.bmp', 'wb').write(fileh(v3(32, 3, hsize=108, masks=(0xFF0000, 0xFF00, 0xFF, 0xFF000000)), rows(lambda y: b''.join(bytes([px[x,y][2], px[x,y][1], px[x,y][0], 0]) for x in range(W)), 32))); ref('zalpha32', src)
