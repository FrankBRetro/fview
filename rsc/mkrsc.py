#!/usr/bin/env python3
"""
mkrsc.py - builds the GEM resource files of FView and FConv, one per
language, from the texts in rsc/lang/*.txt.

    python3 rsc/mkrsc.py          (from the project folder; make rsc)

Writes
    rsc/fview.rsc  rsc/fviewde.rsc ... (English, German, French, Italian,
    rsc/fconv.rsc  rsc/fconvde.rsc ...  Spanish, Czech)
    src/fvrsc.h  src/fcrsc.h          object and string numbers for the C code
    src/fvprefs.inc                   what each Preferences object sets

Every language gets the same trees with the same object numbers; only the
texts, and the positions and widths that follow from them, differ.

The files are plain (pre-AES 4) RSC files that every TOS loads, and the
script refuses anything that could upset TOS 1.x-4.x or EmuTOS:
  - a file over 32000 bytes (rsrc_load offsets are 16 bit; keep clear of
    the signed range);
  - alerts with more than 5 lines of 32 characters or 3 buttons of 10
    (the TOS form_alert limits; %.30s counts as 30, %ld as 8, %d as 5);
  - drop-down menus wider than 32 characters, longer than 20 items, or
    bigger than the AES's save buffer (a quarter of a 640x200 screen);
  - a menu bar wider than 79 characters;
  - dialogs that don't fit a 640x400 or 640x200 desktop in a window
    (also with MagiC's wider check boxes);
  - translations whose printf codes differ from the English ones;
  - characters the Atari character set doesn't have (Czech is written
    without accents: the ST system font has no Czech letters).
"""
import os, re, sys, struct, unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(HERE)

LANGS = [('en', ''), ('de', 'DE'), ('fr', 'FR'), ('it', 'IT'), ('es', 'ES'), ('cs', 'CZ')]

# ------------------------------------------------------------ AES values
G_BOX, G_TEXT, G_BOXTEXT, G_IMAGE, G_IBOX, G_BUTTON = 20, 21, 22, 23, 25, 26
G_STRING, G_FTEXT, G_FBOXTEXT, G_ICON, G_TITLE = 28, 29, 30, 31, 32
SELECTABLE, DEFAULT, EXIT, EDITABLE, RBUTTON, LASTOB = 1, 2, 4, 8, 16, 32
HIDETREE = 128
SELECTED, DISABLED, OUTLINED = 1, 8, 16

# Atari ST character set, 0x80-0xFF (Wikipedia, "Atari ST character set")
ATARI_HIGH = ("ÇüéâäàåçêëèïîìÄÅÉæÆôöòûùÿÖÜ¢£¥ßƒáíóúñÑªº¿⌐¬½¼¡«»ãõØøœŒÀÃÕ¨´†¶©®™"
              "ĳĲאבגדהוזחטיכלמנסעפצקרשתןךםףץ§∧∞αβΓπΣσµτΦΘΩδ∮ϕ∈∩≡±≥≤⌠⌡÷≈°•·√ⁿ²³¯")
assert len(ATARI_HIGH) == 128
ATARI = {c: 0x80 + i for i, c in enumerate(ATARI_HIGH)}


def encode(text, lang, where):
    if lang == 'cs':          # no Czech letters in the ST font: no accents
        text = ''.join(c for c in unicodedata.normalize('NFD', text)
                       if unicodedata.category(c) != 'Mn')
    out = bytearray()
    for c in text:
        o = ord(c)
        if o < 0x80:
            out.append(o)
        elif c in ATARI:
            out.append(ATARI[c])
        else:
            die('%s: %r has no Atari character (%s)' % (where, c, lang))
    return bytes(out)


ERRORS = []


def die(msg):
    ERRORS.append(msg)


# ------------------------------------------------------------- the texts
def load_texts(lang):
    t = {}
    path = os.path.join(HERE, 'lang', lang + '.txt')
    with open(path, encoding='utf-8') as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip('\n')
            if not line.strip() or line.lstrip().startswith('#'):
                continue
            m = re.match(r'^\s*(\w+)\s*=\s*"(.*)"\s*$', line)
            if not m:
                die('%s:%d: not key = "text"' % (path, n))
                continue
            v = m.group(2).replace('\\"', '"').replace('\\\\', '\\')
            t[m.group(1)] = v
    return t


SPEC = re.compile(r'%(?:%|[-+ #0]*(\d+)?(?:\.(\d+))?(l?)([sdxuc]))')


def specs(s):
    return [m.group(0) for m in SPEC.finditer(s) if m.group(0) != '%%']


def shown_len(s):
    """longest length of s once formatted (for the limit checks)"""
    n, pos = 0, 0
    for m in SPEC.finditer(s):
        n += m.start() - pos
        pos = m.end()
        if m.group(0) == '%%':
            n += 1
        elif m.group(4) == 's':
            n += int(m.group(2)) if m.group(2) else 32
        else:
            # a width says how wide it is (%3d: 3); otherwise a guess
            n += int(m.group(1)) if m.group(1) else (8 if m.group(3) else 5)
    return n + len(s) - pos


def check_alert(key, s, lang):
    m = re.match(r'^\[(\d)\]\[(.*)\]\[(.*)\]$', s)
    if not m:
        die('%s %s: not an alert: %s' % (lang, key, s))
        return
    lines, buttons = m.group(2).split('|'), m.group(3).split('|')
    if len(lines) > 5:
        die('%s %s: %d lines (5 at most)' % (lang, key, len(lines)))
    for l in lines:
        if shown_len(l) > 32:
            die('%s %s: line "%s" can be %d characters (32 at most)' % (lang, key, l, shown_len(l)))
    if len(buttons) > 3:
        die('%s %s: %d buttons (3 at most)' % (lang, key, len(buttons)))
    for b in buttons:
        if shown_len(b) > 10:
            die('%s %s: button "%s" can be %d characters (10 at most)' % (lang, key, b, shown_len(b)))
    if len(s) > 255:
        die('%s %s: alert longer than 255 characters' % (lang, key))


# -------------------------------------------------------------- trees
def ch(c, px=0):
    """object coordinate: characters in the low byte, pixels in the high"""
    return (c & 0xFF) | ((px & 0xFF) << 8)


class Obj:
    def __init__(self, typ, flags, state, spec, x, y, w, h, name):
        self.typ, self.flags, self.state, self.spec = typ, flags, state, spec
        self.x, self.y, self.w, self.h = x, y, w, h
        self.name = name
        self.next = self.head = self.tail = -1


class Tree:
    def __init__(self, name):
        self.name = name
        self.objs = []

    def add(self, parent, typ, spec, x, y, w, h, flags=0, state=0, name=None):
        o = Obj(typ, flags, state, spec, x, y, w, h, name)
        idx = len(self.objs)
        self.objs.append(o)
        if parent is not None:
            p = self.objs[parent]
            if p.head < 0:
                p.head = idx
            else:
                self.objs[p.tail].next = idx
            p.tail = idx
            o.next = parent
        return idx

    def finish(self):
        self.objs[-1].flags |= LASTOB
        return self


class Ted:
    def __init__(self, text, tmpl, valid, font=3, just=0, color=0x1180, thick=-1):
        self.text, self.tmpl, self.valid = text, tmpl, valid
        self.font, self.just, self.color, self.thick = font, just, color, thick


class Bits:                       # a 32x32 picture, 1 = black
    def __init__(self):
        self.px = [[0] * 32 for _ in range(32)]

    def set(self, x, y):
        if 0 <= x < 32 and 0 <= y < 32:
            self.px[y][x] = 1

    def words(self):
        out = b''
        for row in self.px:
            v = 0
            for b in row:
                v = (v << 1) | b
            out += struct.pack('>I', v)
        return out


class Icon:
    def __init__(self, data, mask, text):
        self.data, self.mask, self.text = data, mask, text


# ------------------------------------------------------------- pictures
FONT5x7 = {
    '!': ["..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."],
    '?': [".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."],
}


def glyph(b, ch_, x0, y0, scale):
    for r, row in enumerate(FONT5x7[ch_]):
        for c, v in enumerate(row):
            if v == '#':
                for dy in range(scale):
                    for dx in range(scale):
                        b.set(x0 + c * scale + dx, y0 + r * scale + dy)


def alert_icons():
    note, quest, stop = Bits(), Bits(), Bits()
    for y in range(3, 30):                       # note: triangle with !
        half = (y - 3) * 14 // 26
        for x in (16 - half, 16 + half, 15 - half, 17 + half):
            note.set(x, y)
    for x in range(2, 31):
        note.set(x, 29); note.set(x, 30)
    for r, row in enumerate(FONT5x7['!']):
        for c, v in enumerate(row):
            if v == '#':
                for dy in range(3):
                    for dx in range(2):
                        note.set(12 + c * 2 + dx, 9 + r * 3 + dy)
    for i in range(32):                          # question: box with ?
        for t in (1, 2):
            quest.set(i, t); quest.set(i, 31 - t + 1); quest.set(t, i); quest.set(31 - t + 1, i)
    glyph(quest, '?', 9, 6, 3)
    import math                                  # stop: octagon and a bar
    pts = []
    for k in range(8):
        a = math.pi / 8 + k * math.pi / 4
        pts.append((16 + 15 * math.cos(a), 16 + 15 * math.sin(a)))
    for y in range(32):
        for x in range(32):
            inside = all((pts[(k + 1) % 8][0] - pts[k][0]) * (y + .5 - pts[k][1]) -
                         (pts[(k + 1) % 8][1] - pts[k][1]) * (x + .5 - pts[k][0]) >= 0
                         for k in range(8))
            if inside:
                stop.set(x, y)
    for y in range(13, 19):
        for x in range(7, 25):
            stop.px[y][x] = 0
    return note, quest, stop


def fview_icon():
    d = Bits()
    for x in range(2, 30):
        for y in (3, 4, 27, 28):
            d.set(x, y)
    for y in range(3, 29):
        for x in (2, 3, 28, 29):
            d.set(x, y)
    for y in range(-3, 4):
        for x in range(-3, 4):
            if x * x + y * y <= 10:
                d.set(21 + x, 10 + y)
    for y in range(12):
        for x in range(y + 1):
            d.set(11 - x, 14 + y); d.set(11 + x, 14 + y)
            if y > 4:
                d.set(19 - (x - 5), 14 + y); d.set(19 + (x - 5), 14 + y)
    m = Bits()
    for y in range(3, 29):
        for x in range(2, 30):
            m.set(x, y)
    return d, m


def fconv_icon():
    d = Bits()
    for x in range(1, 15):
        d.set(x, 6); d.set(x, 19)
    for y in range(6, 20):
        d.set(1, y); d.set(14, y)
    for y in range(8):
        for x in range(y + 1):
            d.set(7 - x, 11 + y); d.set(7 + x, 11 + y)
    for x, y in ((11, 9), (12, 9), (11, 10), (12, 10)):
        d.set(x, y)
    for x in range(15, 20):
        d.set(x, 13)
    for y in range(4):
        d.set(19 - y, 10 + y); d.set(19 - y, 16 - y)
    for y in range(4, 28):
        d.set(21, y); d.set(30, y)
    for x in range(21, 31):
        d.set(x, 4); d.set(x, 27)
    for y in range(8, 25, 3):
        for x in range(23, 29):
            d.set(x, y)
    m = Bits()
    for y in range(4, 28):
        for x in range(1, 31):
            m.set(x, y)
    return d, m


# ----------------------------------------------------------------- menus
def menu_tree(T, titles, menus, lang, tname):
    """titles: keys; menus: lists of (key | '-' | ('$text',)) , shortcut).
    The layout of build_menu() in the old tree.c: root, bar, active area,
    titles, screen area, then each drop-down's box and items."""
    t = Tree(tname)
    tt = [' ' + T(k) + ' ' for k in titles]
    width = sum(len(s) for s in tt)
    if width + 2 > 79:
        die('%s %s: menu bar is %d characters wide' % (lang, tname, width + 2))
    root = t.add(None, G_IBOX, 0, 0, 0, 80, 25)
    bar = t.add(root, G_BOX, 0x00001100, 0, 0, 80, ch(0, 2) | 1)
    act = t.add(bar, G_IBOX, 0, 2, 0, width, ch(1, 3))
    x, tx = 0, []
    for i, s in enumerate(tt):
        tx.append(x)
        t.add(act, G_TITLE, s, x, 0, len(s), ch(1, 3), name='MT_%d' % i)
        x += len(s)
    scr = t.add(root, G_IBOX, 0, 0, ch(1, 3), 80, 19)
    items = {}
    for i, m in enumerate(menus):
        rows = []
        for key, short in m:
            if key == '-':
                rows.append(None)
            else:
                label = key[1:] if key.startswith('$') else T(key)
                rows.append((key, label, short))
        need = max(2 + shown_len(r[1]) + 2 + len(r[2]) + 1 for r in rows if r)
        if i == 0:
            need = max(need, 21)
        w = need
        texts = []
        for r in rows:
            if r is None:
                texts.append(('-', '-' * w, DISABLED))
                continue
            key, label, short = r
            pad = w - 2 - shown_len(label) - len(short) - 1
            texts.append((key, '  ' + label + ' ' * pad + short + ' ', 0))
        if w > 32:
            die('%s %s: menu %d is %d characters wide (32 at most)' % (lang, tname, i, w))
        if len(rows) > 20:
            die('%s %s: menu %d has %d items (20 at most)' % (lang, tname, i, len(rows)))
        # TOS saves the screen under a drop-down in a buffer of a quarter
        # of the screen: 640x200 in 2 planes is 8000 bytes = 32000 pixels
        if (w * 8 + 2) * (len(rows) * 8 + 2) > 32000:
            die('%s %s: menu %d is too big for the AES menu buffer' % (lang, tname, i))
        bx = 2 + tx[i]
        if bx + w > 80:
            bx = 80 - w
        box = t.add(scr, G_BOX, 0x00FF1100, bx, 0, w, len(rows), name='MB_%d' % i)
        for k, (key, text, st) in enumerate(texts):
            nm = None if key == '-' else ('MI_' + key.split('_', 1)[1].upper() if not key.startswith('$') else None)
            o = t.add(box, G_STRING, text, 0, k, w, 1, state=st, name=nm)
            items.setdefault(i, []).append(o)
    return t.finish(), items


# --------------------------------------------------------- flow dialogs
MAXW = 78        # root width in characters: 624 pixels, fits 640 with the frame
MAXH = 21        # rows: 336 + window frame fits 640x400; 168 fits 640x200


def check_dialog(t, lang, magic_right=0):
    """magic_right: the right edge of the widest row once MagiC has made
    its check boxes 1.5 characters wider each (tree_finish in wdlg.c)"""
    root = t.objs[0]
    w, h = root.w & 0xFF, root.h & 0xFF
    if max(w, magic_right + 1) > MAXW:
        die('%s %s: dialog is %d characters wide (%.1f with MagiC check boxes)'
            % (lang, t.name, w, magic_right + 1))
    if h > MAXH:
        die('%s %s: dialog is %d rows high (%d at most)' % (lang, t.name, h, MAXH))


def prefs_tree(T, lang, rows):
    t = Tree('PREFS')
    root = t.add(None, G_BOX, 0x00021100, 0, 0, 10, 10, state=OUTLINED)
    labw = max(len(T(r[0])) for r in rows) + 4
    y, right, most = 1, 0, 0
    table = []
    for label, items in rows:
        t.add(root, G_STRING, T(label), 2, y, len(T(label)), 1)
        table.append(('-1', '0', '0'))
        x, n = labw, 0
        for key, field, val in items:
            text = key[1:] if key.startswith('=') else T(key)
            w = len(text) + 2
            if n and x + w + 1.5 * (n + 1) + 1 > MAXW:     # wrap
                y += 1
                x, n = labw, 0
            t.add(root, G_BUTTON, text, x, y, w, 1,
                  flags=SELECTABLE | (RBUTTON if val != '-1' else 0))
            table.append(('PF(%s)' % field, val, '0'))
            x += w + 1
            n += 1
            right = max(right, x)
            most = max(most, x - 1 + 1.5 * n)
        y += 1
    bw = max(len(T(k)) for k in ('b_save', 'b_apply', 'b_close')) + 4
    bx = labw + 2
    for k, kind in (('b_save', 'PB_SAVE'), ('b_apply', 'PB_APPLY'), ('b_close', 'PB_CLOSE')):
        t.add(root, G_BUTTON, T(k), bx, y, bw, 1,
              flags=SELECTABLE | EXIT | (DEFAULT if kind == 'PB_SAVE' else 0))
        table.append(('-1', '0', kind))
        bx += bw + 2
        right = max(right, bx)
    t.objs[0].w = right + 1
    t.objs[0].h = y + 2
    check_dialog(t, lang, most)
    return t.finish(), table


def row_buttons(t, root, T, keys, x, y, exit_=True, default=None, gap=1, minw=8):
    out = []
    for k in keys:
        w = max(len(T(k)) + 2, minw)
        o = t.add(root, G_BUTTON, T(k), x, y, w, 1,
                  flags=SELECTABLE | (EXIT if exit_ else 0) | (DEFAULT if k == default else 0),
                  name=None)
        out.append(o)
        x += w + gap
    return out, x


def dither_tree(T, lang):
    t = Tree('DITHER')
    root = t.add(None, G_BOX, 0x00021100, 0, 0, 10, 10, state=OUTLINED, name='D_ROOT')
    labw = max(len(T(k)) for k in ('d_dither', 'p_palette', 'd_colours')) + 4
    t.add(root, G_STRING, '', 2, 1, 48, 1, name='D_TITLE')
    y = 3
    t.add(root, G_STRING, T('d_dither'), 2, y, len(T('d_dither')), 1, name='D_LDITH')
    x, right, most = labw, 0, 0
    for k, nm in (('p_fs', 'D_FS'), ('p_atkinson', 'D_ATK'), ('p_ordered', 'D_ORD'), ('p_none', 'D_NONE')):
        w = len(T(k)) + 2
        t.add(root, G_BUTTON, T(k), x, y, w, 1, flags=SELECTABLE | RBUTTON, name=nm)
        x += w + 1
    right, most = x, x - 1 + 1.5 * 4
    y += 1
    x = labw
    for k, nm in (('p_serp', 'D_SERP'), ('p_smooth', 'D_SMOOTH')):
        w = len(T(k)) + 2
        t.add(root, G_BUTTON, T(k), x, y, w, 1, flags=SELECTABLE, name=nm)
        x += w + 1
    right = max(right, x)
    most = max(most, x - 1 + 3)
    y += 2
    t.add(root, G_STRING, T('p_palette'), 2, y, len(T('p_palette')), 1, name='D_LPAL')
    x = labw
    for k, nm in (('d_own', 'D_OWN'), ('p_system', 'D_SYS')):
        w = len(T(k)) + 2
        t.add(root, G_BUTTON, T(k), x, y, w, 1, flags=SELECTABLE | RBUTTON, name=nm)
        x += w + 1
    notew = max(len(T('d_sharing')), len(T('d_truecol'))) + 1
    t.add(root, G_STRING, '', x + 3, y, notew, 1, name='D_NOTE')
    right = max(right, x + 3 + notew)
    most = max(most, x + 3 + notew + 3)
    y += 2
    t.add(root, G_STRING, T('d_colours'), 2, y, len(T('d_colours')), 1, name='D_LCOL')
    t.add(root, G_STRING, '', labw, y, 40, 1, name='D_COUNTS')
    right = max(right, labw + 40)
    y += 2
    x = 2
    for k, nm, fl in (('b_count', 'D_COUNT', 0), ('b_reduce', 'D_REDUCE', 0), ('b_resize', 'D_RESIZE', 0),
                      ('b_apply', 'D_APPLY', DEFAULT), ('b_close', 'D_CLOSE', 0)):
        w = max(len(T(k)) + 2, 7)
        t.add(root, G_BUTTON, T(k), x, y, w, 1, flags=SELECTABLE | EXIT | fl, name=nm)
        x += w + 1
    right = max(right, x)
    t.objs[0].w = max(right + 1, 52)
    t.objs[0].h = y + 2
    t.objs[1].w = t.objs[0].w - 4
    check_dialog(t, lang, most)
    return t.finish()


def field_ted():
    return Ted('', '_____', '99999')


def reduce_tree(T, lang):
    t = Tree('REDUCE')
    root = t.add(None, G_BOX, 0x00021100, 0, 0, 10, 10, state=OUTLINED, name='R_ROOT')
    lw = len(T('n_colours'))
    t.add(root, G_STRING, '', 2, 1, 36, 1, name='R_TITLE')
    t.add(root, G_STRING, T('n_colours'), 2, 3, lw, 1, name='R_LABEL')
    t.add(root, G_FBOXTEXT, field_ted(), lw + 4, 3, 7, 1, flags=EDITABLE, name='R_FIELD')
    bw = max(len(T('b_ok')), len(T('b_cancel'))) + 4
    w = max(40, lw + 13)
    x = w - 2 - 2 * bw - 2
    t.add(root, G_BUTTON, T('b_ok'), x, 5, bw, 1, flags=SELECTABLE | EXIT | DEFAULT, name='R_OK')
    t.add(root, G_BUTTON, T('b_cancel'), x + bw + 2, 5, bw, 1, flags=SELECTABLE | EXIT, name='R_CANCEL')
    t.objs[0].w, t.objs[0].h = w, 7
    t.objs[1].w = w - 4
    check_dialog(t, lang)
    return t.finish()


def resize_tree(T, lang):
    t = Tree('RESIZE')
    root = t.add(None, G_BOX, 0x00021100, 0, 0, 10, 10, state=OUTLINED, name='Z_ROOT')
    lw = max(len(T('n_width')), len(T('n_height')))
    t.add(root, G_STRING, '', 2, 1, 40, 1, name='Z_TITLE')
    t.add(root, G_STRING, T('n_width'), 2, 3, lw, 1, name='Z_LW')
    t.add(root, G_FBOXTEXT, field_ted(), lw + 4, 3, 7, 1, flags=EDITABLE, name='Z_FW')
    t.add(root, G_STRING, T('n_height'), 2, 5, lw, 1, name='Z_LH')
    t.add(root, G_FBOXTEXT, field_ted(), lw + 4, 5, 7, 1, flags=EDITABLE, name='Z_FH')
    sw = max(len(T('n_keep')), len(T('n_smooth'))) + 2
    t.add(root, G_BUTTON, T('n_keep'), 2, 7, len(T('n_keep')) + 2, 1, flags=SELECTABLE, name='Z_KEEP')
    t.add(root, G_BUTTON, T('n_smooth'), 2, 8, len(T('n_smooth')) + 2, 1, flags=SELECTABLE, name='Z_SMOOTH')
    bw = max(len(T('b_ok')), len(T('b_cancel'))) + 4
    w = max(44, lw + 13, sw + 6)
    x = w - 2 - 2 * bw - 2
    t.add(root, G_BUTTON, T('b_ok'), x, 10, bw, 1, flags=SELECTABLE | EXIT | DEFAULT, name='Z_OK')
    t.add(root, G_BUTTON, T('b_cancel'), x + bw + 2, 10, bw, 1, flags=SELECTABLE | EXIT, name='Z_CANCEL')
    t.objs[0].w, t.objs[0].h = w, 12
    t.objs[1].w = w - 4
    check_dialog(t, lang, max(len(T('n_keep')), len(T('n_smooth'))) + 3 + 1)
    return t.finish()


def alert_tree(icons):
    """laid out by the program (wdlg.c) for each alert"""
    t = Tree('ALERT')
    root = t.add(None, G_BOX, 0x00021100, 0, 0, 40, 9, state=OUTLINED, name='A_ROOT')
    for nm, b in zip(('A_NOTE', 'A_QUEST', 'A_STOP'), icons):
        t.add(root, G_IMAGE, ('bitblk', b), 2, 1, ch(0, 32), ch(0, 32), flags=HIDETREE, name=nm)
    for i in range(5):
        t.add(root, G_STRING, '', 9, 1 + i, 32, 1, name='A_L%d' % (i + 1))
    for i in range(3):
        t.add(root, G_BUTTON, '', 9 + i * 12, 7, 10, 1, flags=SELECTABLE | EXIT, name='A_B%d' % (i + 1))
    return t.finish()


def progress_tree(T):
    """one document's progress panel; laid out by win.c"""
    t = Tree('PROGRESS')
    root = t.add(None, G_BOX, 0x00011100, 0, 0, 40, 6)
    t.add(root, G_STRING, '', 1, 1, 34, 1)
    trough = t.add(root, G_BOX, 0x00FF1100, 1, 2, 34, ch(0, 12))
    t.add(trough, G_BOX, 0x00001171, 0, 0, 0, ch(0, 12))
    t.add(root, G_STRING, '', 1, 3, 20, 1)
    t.add(root, G_BUTTON, T('b_cancel'), 25, 3, 10, ch(1, 4), flags=SELECTABLE | EXIT | DEFAULT)
    return t.finish()


def fc_progress_tree(T):
    """FConv's progress dialog; laid out by fconv.c"""
    t = Tree('FPROGRESS')
    root = t.add(None, G_BOX, 0x00011100, 0, 0, 44, 10, name='P_ROOT')
    t.add(root, G_STRING, '', 2, 1, 40, 1, name='P_TOTAL')
    b = t.add(root, G_BOX, 0x00FF1100, 2, 2, 40, ch(0, 12), name='P_TBOX')
    t.add(b, G_BOX, 0x00001171, 0, 0, 0, ch(0, 12), name='P_TBAR')
    t.add(root, G_STRING, '', 2, 4, 40, 1, name='P_FILE')
    b = t.add(root, G_BOX, 0x00FF1100, 2, 5, 40, ch(0, 12), name='P_FBOX')
    t.add(b, G_BOX, 0x00001171, 0, 0, 0, ch(0, 12), name='P_FBAR')
    t.add(root, G_STRING, '', 2, 7, 28, 1, name='P_STAT')
    t.add(root, G_BUTTON, T('b_cancel'), 31, 7, 11, 1, flags=SELECTABLE | EXIT | DEFAULT, name='P_BTN')
    return t.finish()


def icon_tree(name, bits, text):
    d, m = bits
    t = Tree(name)
    root = t.add(None, G_IBOX, 0, 0, 0, ch(0, 64), ch(0, 40))
    t.add(root, G_ICON, Icon(d, m, text), 0, 0, ch(0, 64), ch(0, 40))
    return t.finish()


# ------------------------------------------------------------ the writer
def write_rsc(path, trees, frstrs, lang):
    strings = bytearray()
    sref = {}

    def addstr(b):
        off = 36 + len(strings)
        strings.extend(b + b'\0')
        return off

    # pass 1: strings
    tedlist, iconlist, bitlist = [], [], []
    for t in trees:
        for o in t.objs:
            s = o.spec
            if isinstance(s, str):
                o._soff = addstr(encode(s, lang, t.name))
            elif isinstance(s, Ted):
                s._t = addstr(encode(s.text, lang, t.name).ljust(len(s.tmpl), b'\0')[:len(s.tmpl)])
                s._m = addstr(s.tmpl.encode())
                s._v = addstr(s.valid.encode())
                tedlist.append(s)
            elif isinstance(s, Icon):
                s._txt = addstr(encode(s.text, lang, t.name).ljust(12, b'\0'))
                iconlist.append(s)
            elif isinstance(s, tuple) and s[0] == 'bitblk':
                bitlist.append(s[1])
    froffs = [addstr(encode(s, lang, 'free string')) for s in frstrs]
    if len(strings) & 1:
        strings.append(0)
    pos = 36 + len(strings)
    # image data
    imdata = bytearray()
    for ic in iconlist:
        ic._m = pos + len(imdata); imdata += ic.mask.words()
        ic._d = pos + len(imdata); imdata += ic.data.words()
    for b in bitlist:
        b._d = pos + len(imdata); imdata += b.words()
    o_imdata = pos
    pos += len(imdata)
    o_frstr = pos
    frtab = b''.join(struct.pack('>I', x) for x in froffs)
    pos += len(frtab)
    o_bitblk = pos
    bits = b''
    for b in bitlist:
        b._off = pos + len(bits)
        bits += struct.pack('>Ihhhhh', b._d, 4, 32, 0, 0, 1)
    pos += len(bits)
    o_frimg = pos
    o_ted = pos
    teds = b''
    for s in tedlist:
        s._off = pos + len(teds)
        teds += struct.pack('>IIIhhhhhhhh', s._t, s._m, s._v, s.font, 0, s.just, s.color, 0,
                            s.thick, len(s.tmpl) + 1, len(s.tmpl) + 1)
    pos += len(teds)
    o_icon = pos
    icons = b''
    for ic in iconlist:
        ic._off = pos + len(icons)
        icons += struct.pack('>IIIhhhhhhhhhhh', ic._m, ic._d, ic._txt, 0x1000, 0, 0,
                             16, 0, 32, 32, 0, 32, 64, 8)
    pos += len(icons)
    o_obj = pos
    objs = b''
    nobs = 0
    roots = []
    for t in trees:
        roots.append(pos + len(objs))
        for o in t.objs:
            s = o.spec
            if isinstance(s, str):
                spec = o._soff
            elif isinstance(s, Ted):
                spec = s._off
            elif isinstance(s, Icon):
                spec = s._off
            elif isinstance(s, tuple):
                spec = s[1]._off
            else:
                spec = s & 0xFFFFFFFF
            objs += struct.pack('>hhhHHHIHHHH', o.next, o.head, o.tail, o.typ, o.flags,
                                o.state, spec, o.x, o.y, o.w, o.h)
            nobs += 1
    pos += len(objs)
    o_tree = pos
    trtab = b''.join(struct.pack('>I', r) for r in roots)
    pos += len(trtab)
    size = pos
    hdr = struct.pack('>18H', 0, o_obj, o_ted, o_icon, o_bitblk, o_frstr, 36, o_imdata,
                      o_frimg, o_tree, nobs, len(trees), len(tedlist), len(iconlist),
                      len(bitlist), len(frstrs), 0, size)
    data = hdr + bytes(strings) + bytes(imdata) + frtab + bits + teds + icons + objs + trtab
    assert len(data) == size
    if size > 32000:
        die('%s: %d bytes (32000 at most)' % (os.path.basename(path), size))
    with open(path, 'wb') as f:
        f.write(data)
    return size


# ------------------------------------------------------- the programs
FV_MENUS = (
    ['m_fview', 'm_file', 'm_view', 'm_picture', 'm_options'],
    [
        [('mi_about', ''), ('-', ''), ('$Desk Accessory 1', ''), ('$Desk Accessory 2', ''),
         ('$Desk Accessory 3', ''), ('$Desk Accessory 4', ''), ('$Desk Accessory 5', ''),
         ('$Desk Accessory 6', '')],
        [('mi_open', '^O'), ('mi_close', '^U'), ('-', ''), ('mi_saveorig', '^S'),
         ('mi_savedith', '\x01^S'), ('-', ''), ('mi_print', '^P'), ('-', ''), ('mi_quit', '^Q')],
        [('mi_real', '^1'), ('mi_fit', '^2'), ('mi_zoomin', '+'), ('mi_zoomout', '-'), ('-', ''),
         ('mi_prevpg', '^B'), ('mi_nextpg', '^N'), ('mi_firstpg', 'Clr/Home'),
         ('mi_lastpg', '\x01Clr/Home'), ('-', ''), ('mi_cycle', '^W')],
        [('mi_dup', '^D'), ('mi_dither', '^T'), ('mi_reduce', '^K'), ('mi_resize', '^R'),
         ('mi_count', '^I'), ('-', ''), ('mi_loadpal', '^L'), ('mi_savepal', '\x01^L'), ('-', ''),
         ('mi_sharepal', '^G')],
        [('mi_prefs', '^E')],
    ])

# label, [(text key or =literal, PREFS field, value or -1 for on/off)]
FV_PREFS = [
    ('p_general', [('p_iconified', 'start_iconified', '-1'), ('p_workers', 'use_workers', '-1')]),
    ('p_dither', [('p_fs', 'dither', 'DITH_FS'), ('p_atkinson', 'dither', 'DITH_ATKINSON'),
                  ('p_ordered', 'dither', 'DITH_ORDERED'), ('p_none', 'dither', 'DITH_NONE'),
                  ('p_serp', 'serpentine', '-1')]),
    ('p_openat', [('p_realsize', 'zoom', 'ZOOM_REAL'), ('p_fitwin', 'zoom', 'ZOOM_FIT'),
                  ('p_smooth', 'smooth', '-1')]),
    ('p_palette', [('p_perpic', 'palmode', 'PAL_OPTIMISE'), ('p_system', 'palmode', 'PAL_SYSTEM'),
                   ('p_keepgem', 'keep_sys', '-1')]),
    ('p_backgr', [('p_white', 'bg', 'BG_WHITE'), ('p_grey', 'bg', 'BG_GREY'), ('p_black', 'bg', 'BG_BLACK')]),
    ('p_gifpng', [('p_giftrans', 'gif_trans', '-1'), ('p_pngalpha', 'png_alpha', '-1')]),
    ('p_iffjpeg', [('p_iffmask', 'iff_mask', '-1'), ('p_ham', 'iff_ham', '-1'),
                   ('p_jpggrey', 'jpg_grey_mono', '-1')]),
    ('p_gemimg', [('p_imgsys', 'img_syspal', '-1'), ('p_vdiorder', 'img_vdiorder', '-1')]),
    ('p_text', [('p_tab4', 'tabw', '4'), ('p_tab8', 'tabw', '8'), ('p_wrap', 'wrap', '-1')]),
    ('p_8bit', [('=Atari', 'charset', 'CS_ATARI'), ('=Latin-1', 'charset', 'CS_LATIN1'),
                ('=CP1252', 'charset', 'CS_CP1252')]),
    ('p_mdrtf', [('p_markup', 'md_markup', '-1'), ('p_rtfpict', 'rtf_pict', '-1')]),
    ('p_pdf', [('p_fitwidth', 'pdf_zoom', 'PDFZ_FITW'), ('=100%', 'pdf_zoom', 'PDFZ_100'),
               ('p_graphics', 'pdf_gfx', '-1'), ('p_images', 'pdf_imgbox', '-1')]),
    ('p_prtsize', [('p_fitpage', 'prt_scale', 'PRS_FIT'), ('p_realsize', 'prt_scale', 'PRS_REAL'),
                   ('p_header', 'prt_header', '-1')]),
    ('p_prtdith', [('p_fs', 'prt_dither', 'DITH_FS'), ('p_atkinson', 'prt_dither', 'DITH_ATKINSON'),
                   ('p_ordered', 'prt_dither', 'DITH_ORDERED')]),
    ('p_palpics', [('p_ditherpal', 'dither_pal', '-1')]),
    ('p_menucol', [('p_uipens', 'ui_pens', '-1')]),
    ('p_language', [('=EN', 'lang', '0'), ('=DE', 'lang', '1'), ('=FR', 'lang', '2'),
                    ('=IT', 'lang', '3'), ('=ES', 'lang', '4'), ('=CZ', 'lang', '5')]),
]

FC_MENUS = (
    ['fm_fconv', 'fm_file', 'fm_format', 'fm_dither'],
    [
        [('fi_about', ''), ('-', ''), ('$Desk Accessory 1', ''), ('$Desk Accessory 2', ''),
         ('$Desk Accessory 3', ''), ('$Desk Accessory 4', ''), ('$Desk Accessory 5', ''),
         ('$Desk Accessory 6', '')],
        [('fi_convert', '^O'), ('fi_dest', '^D'), ('-', ''), ('fi_quit', '^Q')],
        [('fi_img', ''), ('fi_pdf', ''), ('-', ''), ('fi_orig', ''), ('fi_true', ''),
         ('fi_screen', ''), ('fi_mono', ''), ('-', ''), ('fi_fit', ''), ('fi_replace', ''),
         ('fi_subdirs', '')],
        [('fi_fs', ''), ('fi_atk', ''), ('fi_ord', ''), ('fi_none', ''), ('-', ''),
         ('fi_serp', ''), ('fi_smooth', ''), ('-', ''), ('fi_perpic', ''), ('fi_syspal', '')],
    ])

# free strings in each program: everything in en.txt except what the trees use
FV_TREE_ONLY = re.compile(r'^(m_|mi_|p_|fm_|fi_|fc_)')
FC_KEYS = re.compile(r'^(fc_|e_|b_ok$|b_cancel$|b_close$)')


def header_name(key):
    return 'S_' + key.upper()


def build(lang, texts, en):
    def T(k):
        if k not in texts:
            if lang != 'en':
                print('  %s: %s not translated, English used' % (lang, k))
            return en[k]
        return texts[k]

    # printf codes and alerts
    for k, v in texts.items():
        if k not in en:
            die('%s: unknown key %s' % (lang, k))
        elif specs(v) != specs(en[k]):
            die('%s %s: printf codes %s, English has %s' % (lang, k, specs(v), specs(en[k])))
    for k in en:
        if T(k).startswith('[') and T(k)[1:2].isdigit():
            check_alert(k, T(k), lang)

    note, quest, stop = alert_icons()
    out = {}
    # ---- FView
    menu, _ = menu_tree(T, FV_MENUS[0], FV_MENUS[1], lang, 'MENU')
    prefs, ptable = prefs_tree(T, lang, FV_PREFS)
    trees = [menu, prefs, dither_tree(T, lang), reduce_tree(T, lang), resize_tree(T, lang),
             alert_tree((note, quest, stop)), progress_tree(T), icon_tree('ICON', fview_icon(), 'FView')]
    fv_strings = [k for k in en if not FV_TREE_ONLY.match(k)]
    # FView's Format menu needs no fi_ texts; FConv's screen-colour item
    # texts are free strings too (filled in at run time)
    fv_frs = [T(k) for k in fv_strings]
    suffix = dict(LANGS)[lang]
    size = write_rsc(os.path.join(HERE, 'fview%s.rsc' % suffix.lower()), trees, fv_frs, lang)
    out['fview'] = (trees, fv_strings, ptable, size)
    # ---- FConv
    fmenu, fitems = menu_tree(T, FC_MENUS[0], FC_MENUS[1], lang, 'MENU')
    # the screen-colours item is filled in at run time: its texts padded
    # to the menu's width
    w = fmenu.objs[fitems[2][0]].w
    scr = {}
    for k in ('fi_screen', 'fi_screen24'):
        s = '  ' + T(k)
        scr[k] = s + ' ' * (w - shown_len(s))
    fc_trees = [fmenu, fc_progress_tree(T), alert_tree((note, quest, stop)),
                icon_tree('ICON', fconv_icon(), 'FConv')]
    fc_strings = [k for k in en if FC_KEYS.match(k)] + ['fi_screen', 'fi_screen24']
    fc_frs = [T(k) if k not in scr else scr[k] for k in fc_strings]
    size2 = write_rsc(os.path.join(HERE, 'fconv%s.rsc' % suffix.lower()), fc_trees, fc_frs, lang)
    out['fconv'] = (fc_trees, fc_strings, None, size2)
    return out


def write_header(path, prog, trees, strings, extra=''):
    lines = ['/* %s - generated by rsc/mkrsc.py: do not edit (edit rsc/lang/xx.txt) */' % os.path.basename(path),
             '#ifndef %s_H' % prog.upper(), '#define %s_H' % prog.upper(), '']
    for i, t in enumerate(trees):
        lines.append('#define T_%-12s %d' % (t.name, i))
    lines.append('')
    for t in trees:
        named = [(o.name, i) for i, o in enumerate(t.objs) if o.name]
        if named:
            lines.append('/* %s */' % t.name)
            for nm, i in named:
                lines.append('#define %-14s %d' % (nm, i))
        lines.append('#define %-14s %d' % (t.name + '_NOBJ', len(t.objs)))
        lines.append('')
    for i, k in enumerate(strings):
        lines.append('#define %-18s %d' % (header_name(k), i))
    lines.append('#define %-18s %d' % ('S_COUNT', len(strings)))
    lines += ['', extra, '#endif', '']
    with open(path, 'w') as f:
        f.write('\n'.join(lines))


def main():
    en = load_texts('en')
    results = {}
    for lang, suf in LANGS:
        texts = load_texts(lang) if os.path.exists(os.path.join(HERE, 'lang', lang + '.txt')) else {}
        results[lang] = build(lang, texts, en)
    if ERRORS:
        print('\n'.join(ERRORS))
        print('%d problem(s): the resource files are not usable' % len(ERRORS))
        sys.exit(1)
    # the headers from English (every language has the same numbers)
    for lang in results:
        for prog in ('fview', 'fconv'):
            a = [[(o.typ, o.flags & ~DEFAULT, o.name) for o in t.objs] for t in results[lang][prog][0]]
            b = [[(o.typ, o.flags & ~DEFAULT, o.name) for o in t.objs] for t in results['en'][prog][0]]
            if a != b:
                print('%s %s: trees differ from English' % (lang, prog))
                sys.exit(1)
    trees, strings, ptable, _ = results['en']['fview']
    write_header(os.path.join(TOP, 'src', 'fvrsc.h'), 'fvrsc', trees, strings)
    with open(os.path.join(TOP, 'src', 'fvprefs.inc'), 'w') as f:
        f.write('/* fvprefs.inc - generated by rsc/mkrsc.py: what each object of the\n'
                '   Preferences tree sets (object n is line n), see tree.c */\n')
        f.write('    { -1, 0, 0 },          /* the root */\n')
        for fld, val, kind in ptable:
            f.write('    { %s, %s, %s },\n' % (fld, val, kind))
    trees, strings, _, _ = results['en']['fconv']
    write_header(os.path.join(TOP, 'src', 'fcrsc.h'), 'fcrsc', trees, strings)
    for lang, suf in LANGS:
        print('%-2s  fview%s.rsc %5d bytes   fconv%s.rsc %5d bytes' % (
            lang, suf.lower().ljust(2), results[lang]['fview'][3], suf.lower().ljust(2), results[lang]['fconv'][3]))


if __name__ == '__main__':
    main()
