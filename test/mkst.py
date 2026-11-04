"""Write Atari ST test pictures (Degas, NEOchrome, Spectrum, Photochrome,
QuantumPaint) from a photo, plus random-content Spectrum files whose
layout can be cross-checked against an independent decoder."""
import struct, random, sys
from PIL import Image
out = sys.argv[2]
src = Image.open(sys.argv[1]).convert('RGB')
random.seed(7)

def stword(r, g, b):          # 8-bit -> ST 0x0RGB (3 bits per channel)
    return ((r * 7 + 127) // 255) << 8 | ((g * 7 + 127) // 255) << 4 | ((b * 7 + 127) // 255)

def quant(im, n):
    q = im.quantize(n, dither=Image.Dither.FLOYDSTEINBERG)
    pal = q.getpalette()[:n * 3]
    pal += [0] * (n * 3 - len(pal))
    words = [stword(*pal[i * 3:i * 3 + 3]) for i in range(n)]
    return list(q.getdata()), words

def interleave(pix, w, h, planes):
    out = bytearray()
    for y in range(h):
        for wx in range(w // 16):
            ws = [0] * planes
            for i in range(16):
                v = pix[y * w + wx * 16 + i]
                for p in range(planes):
                    if v >> p & 1: ws[p] |= 0x8000 >> i
            for p in range(planes): out += struct.pack('>H', ws[p])
    return bytes(out)

def packbits(data):
    o = bytearray(); i = 0
    while i < len(data):
        j = i
        while j < len(data) and data[j] == data[i] and j - i < 128: j += 1
        if j - i >= 3:
            o += bytes([257 - (j - i), data[i]]); i = j
        else:
            k = i
            while k < len(data) and k - i < 128 and not (k + 2 < len(data) and data[k] == data[k+1] == data[k+2]): k += 1
            if k == i: k = i + 1
            o += bytes([k - i - 1]) + data[i:k]; i = k
    return bytes(o)

def plane_lines(screen, w, h, planes):   # interleaved -> per line, per plane bytes
    bpl = w // 8 * planes
    out = []
    for y in range(h):
        line = screen[y * bpl:(y + 1) * bpl]
        for p in range(planes):
            pb = bytearray()
            for wx in range(w // 16):
                pb += line[wx * planes * 2 + p * 2: wx * planes * 2 + p * 2 + 2]
            out.append(bytes(pb))
    return out

modes = {0: (320, 200, 4), 1: (640, 200, 2), 2: (640, 400, 1)}
for mode, (w, h, planes) in modes.items():
    im = src.resize((w, h))
    if planes == 1:
        pix = [0 if v > 127 else 1 for v in im.convert('L').convert('1', dither=Image.Dither.FLOYDSTEINBERG).convert('L').getdata()]
        pix = [0 if v else 1 for v in im.convert('1').getdata()]
        words = [0x777, 0] + [0] * 14
    else:
        pix, words = quant(im, 1 << planes)
        words += [0] * (16 - len(words))
    scr = interleave(pix, w, h, planes)
    pal = b''.join(struct.pack('>H', x) for x in words[:16])
    open(f'{out}/test.pi{mode+1}', 'wb').write(struct.pack('>H', mode) + pal + scr)
    comp = b''.join(packbits(l) for l in plane_lines(scr, w, h, planes))
    open(f'{out}/test.pc{mode+1}', 'wb').write(struct.pack('>H', 0x8000 | mode) + pal + comp + bytes(32))
    if mode == 0:
        hdr = struct.pack('>HH', 0, 0) + pal + bytes(128 - 36)
        open(f'{out}/test.neo', 'wb').write(hdr + scr)
        # NEOchrome Master rasters: change colours 1..15 on lines 50, 100, 150
        rst = bytearray()
        for ly in (50, 100, 150):
            alt = [words[0]] + [((x * 3 + ly) & 0x777) for x in words[1:16]]
            rst += struct.pack('>H', ly) + b''.join(struct.pack('>H', x) for x in alt)
        open(f'{out}/rast.neo', 'wb').write(hdr + scr)
        open(f'{out}/rast.rst', 'wb').write(bytes(rst) + bytes(6800 - len(rst)))
        # NEOchrome Master 640x400 big picture
        big = src.resize((640, 400))
        bp, bw = quant(big, 16)
        bhdr = b'\xba\xbe' + struct.pack('>H', 0) + b''.join(struct.pack('>H', x) for x in bw) + bytes(128 - 36)
        open(f'{out}/big.neo', 'wb').write(bhdr + interleave(bp, 640, 400, 4))

# ---- Spectrum 512: random content, so layout errors show
spec_scr = bytearray(160) + bytes(random.getrandbits(8) for _ in range(32000 - 160))
spec_pal = [random.getrandbits(12) & 0x777 for _ in range(199 * 48)]
pal_bytes = b''.join(struct.pack('>H', x) for x in spec_pal)
open(f'{out}/test.spu', 'wb').write(bytes(spec_scr) + pal_bytes)

def spc_rle(data):            # SPC: literal n+1, repeat 258-b
    o = bytearray(); i = 0
    while i < len(data):
        j = i
        while j < len(data) and data[j] == data[i] and j - i < 129: j += 1
        if j - i >= 3:
            o += bytes([258 - (j - i), data[i]]); i = j
        else:
            k = min(len(data), i + 128); o += bytes([k - i - 1]) + data[i:k]; i = k
    return bytes(o)

# make the random bitmap a bit compressible: runs of equal bytes
for y in range(1, 199, 3):
    for x in range(0, 160, 16):
        spec_scr[y * 160 + x:y * 160 + x + 8] = bytes([0x55]) * 8
order = bytearray()
for plane in range(0, 8, 2):
    for off in range(160 + plane, 32000, 8):
        order += spec_scr[off:off + 2]
comp = spc_rle(bytes(order))
palc = bytearray()
for p in range(597):
    cols = spec_pal[p * 16:(p + 1) * 16]
    mask = 0
    for i in range(16):
        if i not in (0, 15) and cols[i]: mask |= 1 << i
    palc += struct.pack('>H', mask)
    for i in range(16):
        if mask >> i & 1: palc += struct.pack('>H', cols[i])
open(f'{out}/test.spc', 'wb').write(b'SP\0\0' + struct.pack('>II', len(comp), len(palc)) + comp + bytes(palc))
# matching SPU for comparison (colours 0 and 15 are not stored in SPC)
p2 = list(spec_pal)
for p in range(597): p2[p * 16] = 0; p2[p * 16 + 15] = 0
open(f'{out}/spcref.spu', 'wb').write(bytes(spec_scr) + b''.join(struct.pack('>H', x) for x in p2))

# SPS (column order bitmap, literal/repeat swapped, bit-packed palettes)
def sps_rle(data):            # repeat: n+3 times (n<128); literals: b-127
    o = bytearray(); i = 0
    while i < len(data):
        j = i
        while j < len(data) and data[j] == data[i] and j - i < 130: j += 1
        if j - i >= 3:
            o += bytes([j - i - 3, data[i]]); i = j
        else:
            k = min(len(data), i + 128); o += bytes([127 + k - i]) + data[i:k]; i = k
    return bytes(o)
cols_order = bytearray()
for plane in range(0, 8, 2):
    for x in range(40):
        for off in range(160 + ((x & ~1) << 2) + plane + (x & 1), 32000, 160):
            cols_order.append(spec_scr[off])
compS = sps_rle(bytes(cols_order))
bits = []
def put(v, n):
    for i in range(n - 1, -1, -1): bits.append(v >> i & 1)
for p in range(597):
    cols = p2[p * 16:(p + 1) * 16]
    mask = 0
    for i in range(1, 15):
        if cols[i]: mask |= 1 << i
    put(mask >> 1, 14)
    for i in range(15, -1, -1):
        if mask >> i & 1:
            c = cols[i]
            put(((c >> 8) & 7) << 6 | ((c >> 4) & 7) << 3 | (c & 7), 9)
while len(bits) % 8: bits.append(0)
palS = bytes(int(''.join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))
open(f'{out}/test.sps', 'wb').write(b'SP\0\0' + struct.pack('>II', len(compS), len(palS)) + compS + palS + b'\0')

# ---- Photochrome PCS: a real picture with one palette on every line
def pcs_rle_block(values, word):   # literal blocks (b >= 128: 256-b) and repeats (b=2..127)
    cmds = bytearray(); n = 0; i = 0
    def val(v): return struct.pack('>H', v) if word else bytes([v])
    while i < len(values):
        j = i
        while j < len(values) and values[j] == values[i] and j - i < 127: j += 1
        if j - i >= 3:
            cmds += bytes([j - i]) + val(values[i]); i = j
        else:
            k = min(len(values), i + 128)
            cmds += bytes([256 - (k - i)]) + b''.join(val(v) for v in values[i:k]); i = k
        n += 1
    return struct.pack('>H', n) + bytes(cmds)
im = src.resize((320, 200))
pix, words = quant(im, 16)
sep = bytearray(32000)
for y in range(200):
    for x in range(320):
        v = pix[y * 320 + x]
        for p in range(4):
            if v >> p & 1: sep[p * 8000 + y * 40 + (x >> 3)] |= 0x80 >> (x & 7)
palw = (words * 3) * (199 * 3 + 1 // 1)
palw = palw[:(199 * 3 + 1) * 16]
pcs = struct.pack('>HHBB', 320, 200, 0, 0) + pcs_rle_block(list(sep), False) + pcs_rle_block(palw, True)
open(f'{out}/test.pcs', 'wb').write(pcs)

# ---- QuantumPaint PBX mode 0 (16 colours, palette at line 0), packed and plain
def pbx0(packed):
    hdr = bytearray(128); hdr[3] = 0
    if packed: hdr[4] = 0x80; hdr[5] = 1
    pals = bytearray(384)
    pals[0:32] = b''.join(struct.pack('>H', x) for x in words)
    pals[33] = 0
    # a second palette from line 100 (colours rotated)
    w2 = words[8:] + words[:8]
    pals[48:80] = b''.join(struct.pack('>H', x) for x in w2); pals[48 + 33] = 100; pals[48 + 35] = 1
    scr = interleave(pix, 320, 200, 4)
    if not packed:
        return bytes(hdr) + bytes(pals) + scr
    body = bytearray(pals)
    for plane in range(0, 8, 2):
        for x in range(plane, 160, 8):
            for y in range(200):
                body += scr[y * 160 + x:y * 160 + x + 2]
    return bytes(hdr) + packbits(bytes(body))
open(f'{out}/plain.pbx', 'wb').write(pbx0(False))
open(f'{out}/packed.pbx', 'wb').write(pbx0(True))
print('written')
