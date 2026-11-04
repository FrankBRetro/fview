"""Write a small GEM metafile (Intel word order) using common VDI calls."""
import struct, sys
recs = []
def rec(op, pts=(), ints=(), sub=0):
    w = [op, len(pts) // 2, len(ints), sub] + list(pts) + list(ints)
    recs.append(b''.join(struct.pack('<h', v) for v in w))
def text(x, y, s): rec(8, (x, y), [ord(c) for c in s])
# header: id, length 24, version 101, NDC flag 0, extents, page 2100x2970 (A4 in 0.1mm), window
hdr = [-1, 24, 101, 0, 0, 0, 0, 0, 2100, 2970, 0, 0, 0, 0, 0] + [0] * 9
rec(5, (), [1, 0, 0, 2100, 2970], 99)          # vm_coords: 0.1 mm units, y up
rec(17, (), [1]); rec(16, (3, 0))                # black, 3 wide
rec(6, (100, 2800, 2000, 2800))                  # top rule
rec(12, (0, 60)); rec(22, (), [4])               # text height, blue
text(150, 2850, 'FView GEM metafile test')
rec(23, (), [1]); rec(25, (), [2])               # solid red fill
rec(11, (200, 2000, 900, 2500), (), 1)           # v_bar
rec(23, (), [2]); rec(24, (), [4]); rec(25, (), [3])   # green 50% pattern
rec(11, (1450, 2250, 0, 0, 350, 0), (), 4)       # v_circle radius 350
rec(17, (), [4]); rec(16, (1, 0))
rec(11, (1050, 1300, 600, 300), (), 5)           # v_ellipse (filled, green pattern)
rec(23, (), [1]); rec(25, (), [6])               # yellow
rec(11, (400, 800, 0, 0, 0, 0, 300, 0), [0, 2700], 3)   # pie slice 0..270 degrees
rec(17, (), [2]); rec(16, (5, 0))
rec(11, (1500, 800, 0, 0, 0, 0, 300, 0), [450, 1800], 2) # arc 45..180
rec(17, (), [1]); rec(16, (1, 0))
rec(11, (1000, 300, 1900, 600), (), 8)           # rounded box outline
rec(12, (0, 40)); rec(22, (), [1]); rec(39, (), [1, 0])  # centred text
text(1450, 420, 'centred')
rec(23, (), [0]); rec(17, (), [9])
rec(9, (200, 300, 500, 600, 800, 300))           # hollow triangle (perimeter)
rec(7, (300, 1500, 400, 1500, 500, 1500))        # markers
data = b''.join(struct.pack('<h', v) for v in hdr) + b''.join(recs) + struct.pack('<h', -1)
open(sys.argv[1], 'wb').write(data)
print(len(data), 'bytes')
