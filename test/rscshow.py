#!/usr/bin/env python3
"""
rscshow.py - reads a GEM resource file the way rsrc_load does and draws
its trees into PNG pictures (needs Pillow), to look at a translation
without an Atari. It also reports objects that overlap their neighbours.

    python3 test/rscshow.py rsc/FVIEWDE.RSC out/      [--magic] [--cell 8x16]

--magic draws on/off and radio buttons the way MagiC does (check boxes,
round buttons) after making them wider like wdlg.c's tree_finish.
"""
import struct, sys, os

ATARI_HIGH = ("ÇüéâäàåçêëèïîìÄÅÉæÆôöòûùÿÖÜ¢£¥ßƒáíóúñÑªº¿⌐¬½¼¡«»ãõØøœŒÀÃÕ¨´†¶©®™"
              "ĳĲאבגדהוזחטיכלמנסעפצקרשתןךםףץ§∧∞αβΓπΣσµτΦΘΩδ∮ϕ∈∩≡±≥≤⌠⌡÷≈°•·√ⁿ²³¯")


def text(data, off):
    end = data.index(b'\0', off)
    return ''.join(chr(b) if b < 0x80 else ATARI_HIGH[b - 0x80] for b in data[off:end]).replace('\x01', '⇧')


def load(path, cw, chh):
    d = open(path, 'rb').read()
    h = struct.unpack('>18H', d[:36])
    (vrsn, o_obj, o_ted, o_icon, o_bb, o_frstr, o_str, o_im, o_frimg, o_tree,
     nobs, ntree, nted, nib, nbb, nstr, nimg, size) = h
    assert size == len(d), 'rsh_rssize %d, file %d' % (size, len(d))
    roots = [struct.unpack('>I', d[o_tree + 4 * i:o_tree + 4 * i + 4])[0] for i in range(ntree)]
    objs = []
    for i in range(nobs):
        o = struct.unpack('>hhhHHHIHHHH', d[o_obj + 24 * i:o_obj + 24 * i + 24])
        objs.append(list(o))
    trees = []
    for r in roots:
        first = (r - o_obj) // 24
        t, k = [], first
        while True:
            nx, hd, tl, typ, fl, st, spec, x, y, w, hh = objs[k]
            fix = lambda v, c: (v & 0xFF) * c + (v >> 8)
            o = dict(next=nx, head=hd, tail=tl, type=typ, flags=fl, state=st,
                     x=fix(x, cw), y=fix(y, chh), w=fix(w, cw), h=fix(hh, chh), text='')
            if typ in (26, 28, 32):
                o['text'] = text(d, spec)
            elif typ in (21, 22, 29, 30):
                pt, pm, pv = struct.unpack('>III', d[spec:spec + 12])
                o['text'] = text(d, pm)
            elif typ == 23:
                pd, wb, hl = struct.unpack('>Ihh', d[spec:spec + 8])
                o['bits'] = (d[pd:pd + wb * hl], wb, hl)
            elif typ == 31:
                pm, pd = struct.unpack('>II', d[spec:spec + 8])
                o['bits'] = (d[pd:pd + 128], 4, 32)
            else:
                o['spec'] = spec
            t.append(o)
            if fl & 32:
                break
            k += 1
        trees.append(t)
    frs = [text(d, struct.unpack('>I', d[o_frstr + 4 * i:o_frstr + 4 * i + 4])[0]) for i in range(nstr)]
    return trees, frs, size


def magic(t, cw, chh):
    """what tree_finish does for MagiC check boxes"""
    for o in t[1:]:
        if o['type'] == 26 and o['flags'] & 1 and not o['flags'] & (2 | 4 | 64):
            o['magic'] = 1
            for p in t[1:]:
                if p['y'] // chh == o['y'] // chh and p['x'] > o['x']:
                    p['x'] += cw + cw // 2
            o['w'] += cw


def abspos(t):
    pos = {0: (t[0]['x'], t[0]['y'])}

    def walk(i):
        c = t[i]['head']
        while c != -1 and c != i:
            pos[c] = (pos[i][0] + t[c]['x'], pos[i][1] + t[c]['y'])
            walk(c)
            c = t[c]['next']
    walk(0)
    return pos


def overlaps(t, name):
    pos = abspos(t)
    bad = []
    kids = {}
    for i, o in enumerate(t):
        if i in pos:
            par = None
            for j, p in enumerate(t):
                c = p['head']
                while c != -1 and c != j:
                    if c == i:
                        par = j
                    c = t[c]['next']
            kids.setdefault(par, []).append(i)
    for par, ks in kids.items():
        for a in ks:
            for b in ks:
                if a < b and not (t[a]['flags'] & 128 or t[b]['flags'] & 128):
                    ax, ay = pos[a]; bx, by = pos[b]
                    A, B = t[a], t[b]
                    if A['w'] and B['w'] and ax < bx + B['w'] and bx < ax + A['w'] and ay < by + B['h'] and by < ay + A['h']:
                        if par == 0 and name == 'MENU':
                            continue
                        bad.append((a, b))
    return bad


def draw(t, path, cw, chh, menu=False):
    from PIL import Image, ImageDraw, ImageFont
    pos = abspos(t)
    W = max(pos[i][0] + o['w'] for i, o in enumerate(t) if i in pos) + 8
    H = max(pos[i][1] + o['h'] for i, o in enumerate(t) if i in pos) + 8
    if menu:
        H = max(pos[i][1] + o['h'] for i, o in enumerate(t) if i in pos and o['type'] != 25) + 8
    im = Image.new('RGB', (W, H), 'white')
    g = ImageDraw.Draw(im)
    try:
        f = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf', int(chh * 0.8))
    except Exception:
        f = ImageFont.load_default()
    for i, o in enumerate(t):
        if i not in pos or o['flags'] & 128:
            continue
        x, y = pos[i]
        w, h = o['w'], o['h']
        typ = o['type']
        if typ == 20:
            if o['state'] & 16 or (o.get('spec', 0) >> 16) & 0xFF:
                g.rectangle([x, y, x + w - 1, y + h - 1], outline='black')
        elif typ == 28 or typ == 32:
            g.text((x, y), o['text'], font=f, fill='grey' if o['state'] & 8 else 'black')
        elif typ == 26:
            if o.get('magic'):
                b = h - 2
                if o['flags'] & 16:
                    g.ellipse([x, y + 1, x + b, y + 1 + b], outline='black')
                else:
                    g.rectangle([x, y + 1, x + b, y + 1 + b], outline='black')
                g.text((x + 3 * cw, y), o['text'], font=f, fill='black')
            else:
                bw = 1 + (1 if o['flags'] & 2 else 0) + (1 if o['flags'] & 4 else 0)
                g.rectangle([x - bw, y - bw, x + w - 1 + bw, y + h - 1 + bw], fill='black')
                g.rectangle([x, y, x + w - 1, y + h - 1], fill='white')
                tw = g.textlength(o['text'], font=f)
                g.text((x + (w - tw) / 2, y), o['text'], font=f, fill='black')
        elif typ in (29, 30):
            g.rectangle([x - 1, y - 1, x + w, y + h], outline='black')
            g.text((x, y), o['text'], font=f, fill='black')
        elif typ in (23, 31) and 'bits' in o:
            bits, wb, hl = o['bits']
            ox = x + (16 if typ == 31 else 0)
            for yy in range(hl):
                for xx in range(wb * 8):
                    if bits[yy * wb + xx // 8] & (0x80 >> (xx & 7)):
                        g.point((ox + xx, y + yy), fill='black')
    im.save(path)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    cw, chh = 8, 16
    for a in sys.argv[1:]:
        if a.startswith('--cell'):
            cw, chh = map(int, a.split('=')[1].split('x'))
    path, out = args[0], args[1] if len(args) > 1 else None
    trees, frs, size = load(path, cw, chh)
    names = ['MENU', 'PREFS', 'DITHER', 'REDUCE', 'RESIZE', 'ALERT', 'PROGRESS', 'ICON'] \
        if 'FVIEW' in os.path.basename(path).upper() else ['MENU', 'FPROGRESS', 'ALERT', 'ICON']
    print('%s: %d bytes, %d trees, %d free strings' % (path, size, len(trees), len(frs)))
    for name, t in zip(names, trees):
        if '--magic' in sys.argv and name not in ('MENU',):
            magic(t, cw, chh)
        bad = overlaps(t, name) if name in ('PREFS', 'DITHER', 'REDUCE', 'RESIZE') else []
        if bad:
            print('  %s: overlapping objects %s' % (name, bad))
        if out:
            os.makedirs(out, exist_ok=True)
            if name == 'MENU':
                # open every drop-down at once
                draw(t, os.path.join(out, name + '.png'), cw, chh, menu=True)
            elif name in ('PREFS', 'DITHER', 'REDUCE', 'RESIZE', 'ICON'):
                draw(t, os.path.join(out, name + '.png'), cw, chh)


if __name__ == '__main__':
    main()
