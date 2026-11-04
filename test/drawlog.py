"""Draw a VDI call log from rendertest into a PNG (8x16-ish monospace font,
Atari charset approximated through cp437)."""
import sys
from PIL import Image, ImageDraw, ImageFont

PAL = [(255,255,255),(0,0,0),(255,0,0),(0,255,0),(0,0,255),(0,255,255),(255,255,0),
       (255,0,255),(192,192,192),(128,128,128),(128,0,0),(0,128,0),(0,0,128),(0,128,128),
       (128,128,0),(128,0,128)]
lines = open(sys.argv[1], encoding='latin-1').read().splitlines()
w, h = map(int, lines[0].split()[1:3])
im = Image.new('RGB', (w + 16, h + 16), (255, 255, 255))
d = ImageDraw.Draw(im)
fonts = {}
def font(px):
    px = max(6, px)
    if px not in fonts:
        try:
            fonts[px] = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf', px)
        except Exception:
            fonts[px] = ImageFont.load_default()
    return fonts[px]
for ln in lines[1:]:
    p = ln.split(' ')
    if p[0] == 'T':
        x, y, tw, ch, col, eff, valign, hgt, fid, fpx = map(int, p[1:11])
        s = ' '.join(p[11:]).encode('latin-1').decode('cp437', 'replace')
        if fid > 1 and fid != 10:
            ttf = '/usr/share/fonts/truetype/dejavu/DejaVu%s%s.ttf' % ('Serif' if fid >= 5 else 'Sans',
                  '-Bold' if fid in (3, 6, 9) or eff & 1 else '')
            f = ImageFont.truetype(ttf, max(6, fpx))
            d.text((x, y), s, fill=PAL[col % 16], font=f, anchor='ls' if valign == 0 else 'lt')
            continue
        f = font(int(ch * 0.8))
        top = y if valign == 5 else y - int(ch * 0.75)
        # spread characters over the given width like v_justified
        n = max(1, len(s))
        for i, c in enumerate(s):
            cx = x + tw * i / n
            d.text((cx, top), c, fill=PAL[col % 16], font=f)
        if eff & 8:
            d.line((x, top + ch - 2, x + tw, top + ch - 2), fill=PAL[col % 16])
        if eff & 1:
            for i, c in enumerate(s):
                d.text((x + tw * i / n + 1, top), c, fill=PAL[col % 16], font=f)
    elif p[0] == 'L':
        col, wid, n = map(int, p[1:4])
        pts = list(map(int, p[4:4 + n * 2]))
        d.line(pts, fill=PAL[col % 16], width=wid)
    elif p[0] == 'B':
        x, y, bw, bh, trans = map(int, p[1:6])
        b = Image.open(p[6]).convert('RGB')
        if trans:
            m = Image.eval(b.convert('L'), lambda v: 255 if v < 128 else 0)
            im.paste((0,0,0), (x, y), m)
        else:
            im.paste(b, (x, y))
    elif p[0] == 'F':
        col, inter, style, n = map(int, p[1:5])
        pts = list(map(int, p[5:5 + n * 2]))
        if inter == 2 or inter == 3:          # pattern: draw a dot/line texture in the colour
            mask = Image.new('L', im.size, 0); ImageDraw.Draw(mask).polygon(pts, fill=255)
            tex = Image.new('L', im.size, 0); tp = tex.load()
            step = 2 if inter == 2 else 6
            for yy in range(0, im.size[1]):
                for xx in range((yy % 2) if inter == 2 else 0, im.size[0], step):
                    if inter == 3 and (xx + yy) % 6: continue
                    tp[xx, yy] = 255
            from PIL import ImageChops
            im.paste(PAL[col % 16], (0, 0), ImageChops.multiply(mask, tex))
        else:
            d.polygon(pts, fill=PAL[col % 16])
im.save(sys.argv[2])
