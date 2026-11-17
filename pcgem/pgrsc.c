/* Copyright (c) 2026 Francis C Bullen. SPDX-License-Identifier: MIT (see LICENSE) */
/*
 * pgrsc.c - rsrc_load / rsrc_gaddr / rsrc_free for PC GEM
 *
 * The resource files are Atari ones (big-endian,
 * from rsc/mkrsc.py). Rather than have PC GEM load them, they're read here
 * into normal 32-bit memory with the byte order swapped (but not in the
 * images: see below) and the offsets
 * made into pointers, and the coordinates fixed (rsrc_obfix) for the
 * system font. The trees then live where the front end can change them
 * freely; pgaes.c copies a tree down to GEM's memory when GEM needs it.
 *
 * The same .RSC files serve the Atari and the PC.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gem.h>

extern short pg_wchar, pg_hchar, pg_scr_w, pg_scr_h;   /* pgaes.c, from graf_handle() and the desk */

static unsigned char *raw;     /* the file as read */
static long rawlen;
static OBJECT  *objs;
static TEDINFO *teds;
static BITBLK  *bits;
static short   *imgs;          /* image data, in the file's byte order */
static OBJECT **trees;
static ICONBLK *icons;
static char **frstr;          /* free strings (R_FRSTR) */
static BITBLK **frimg;        /* free images (R_FRIMG) */
static int nobj, nted, nbb, ntree, nib, nfrstr, nfrimg;

static unsigned short be16(long o) { return (unsigned short)((raw[o] << 8) | raw[o + 1]); }
static long be32(long o) { return ((long)raw[o] << 24) | ((long)raw[o + 1] << 16) | ((long)raw[o + 2] << 8) | raw[o + 3]; }

/* as the AES does it: low byte characters, high byte (signed) pixels;
 * a width of 80 characters is the whole screen */
static short fix(unsigned short v, int horiz)
{
    int c = v & 0xff, px = (v >> 8) & 0xff;
    if (px > 128) px -= 256;
    if (horiz && c == 80) c = pg_scr_w;
    else c *= horiz ? pg_wchar : pg_hchar;
    return (short)(c + px);
}

/* one object's coordinates from characters to pixels, as the AES does */
short rsrc_obfix(OBJECT *tree, short obj)
{
    OBJECT *o = &tree[obj];
    o->ob_x = fix((unsigned short)o->ob_x, 1);
    o->ob_y = fix((unsigned short)o->ob_y, 0);
    o->ob_width = fix((unsigned short)o->ob_width, 1);
    o->ob_height = fix((unsigned short)o->ob_height, 0);
    return 1;
}

/* the overlap of two rectangles into dst; 0 when they don't overlap */
short rc_intersect(const GRECT *a, GRECT *b)
{
    short x1 = a->g_x > b->g_x ? a->g_x : b->g_x;
    short y1 = a->g_y > b->g_y ? a->g_y : b->g_y;
    short x2 = (a->g_x + a->g_w < b->g_x + b->g_w) ? a->g_x + a->g_w : b->g_x + b->g_w;
    short y2 = (a->g_y + a->g_h < b->g_y + b->g_h) ? a->g_y + a->g_h : b->g_y + b->g_h;
    b->g_x = x1; b->g_y = y1; b->g_w = x2 - x1; b->g_h = y2 - y1;
    return x2 > x1 && y2 > y1;
}

void pg_rsrc_freeall(void)
{
    free(raw); free(objs); free(teds); free(bits); free(imgs); free(trees);
    free(icons); free(frstr); free(frimg);
    raw = NULL; objs = NULL; teds = NULL; bits = NULL; imgs = NULL; trees = NULL;
    icons = NULL; frstr = NULL; frimg = NULL;
    nobj = nted = nbb = ntree = nib = nfrstr = nfrimg = 0;
}

short rsrc_free(void) { pg_rsrc_freeall(); return 1; }

/* a pointer into the file's string area, or NULL if the offset is bad */
static char *strat(long off)
{
    if (off <= 0 || off >= rawlen) return NULL;
    return (char *)raw + off;
}

short rsrc_load(const char *name)
{
    FILE *f;
    long o_obj, o_ted, o_ib, o_bb, o_frstr, o_frimg, o_imd, o_tree, n_imd, i;
    pg_rsrc_freeall();
    if (!(f = fopen(name, "rb"))) return 0;
    fseek(f, 0, SEEK_END); rawlen = ftell(f); fseek(f, 0, SEEK_SET);
    if (rawlen < 36 || !(raw = (unsigned char *)malloc(rawlen + 1))) { fclose(f); pg_rsrc_freeall(); return 0; }
    if (fread(raw, 1, rawlen, f) != (size_t)rawlen) { fclose(f); pg_rsrc_freeall(); return 0; }
    fclose(f);
    raw[rawlen] = 0;
    o_obj = be16(2); o_ted = be16(4); o_ib = be16(6); o_bb = be16(8); o_frstr = be16(10);
    o_imd = be16(14); o_frimg = be16(16); o_tree = be16(18);
    nobj = be16(20); ntree = be16(22); nted = be16(24); nib = be16(26); nbb = be16(28);
    nfrstr = be16(30); nfrimg = be16(32);
    if (o_ib + 34L * nib > rawlen) nib = 0;
    if (o_frstr + 4L * nfrstr > rawlen) nfrstr = 0;
    if (o_frimg + 4L * nfrimg > rawlen) nfrimg = 0;
    if (be16(34) > rawlen || o_obj + 24L * nobj > rawlen || o_ted + 28L * nted > rawlen ||
        o_bb + 14L * nbb > rawlen || o_tree + 4L * ntree > rawlen) { pg_rsrc_freeall(); return 0; }
    n_imd = (o_obj < o_ted ? o_obj : o_ted) - o_imd;     /* image data runs up to the next section */
    if (n_imd < 0 || o_imd + n_imd > rawlen) n_imd = 0;
    objs  = (OBJECT *)calloc(nobj ? nobj : 1, sizeof(OBJECT));
    teds  = (TEDINFO *)calloc(nted ? nted : 1, sizeof(TEDINFO));
    bits  = (BITBLK *)calloc(nbb ? nbb : 1, sizeof(BITBLK));
    imgs  = (short *)calloc(n_imd / 2 + 1, sizeof(short));
    trees = (OBJECT **)calloc(ntree ? ntree : 1, sizeof(OBJECT *));
    icons = (ICONBLK *)calloc(nib ? nib : 1, sizeof(ICONBLK));
    frstr = (char **)calloc(nfrstr ? nfrstr : 1, sizeof(char *));
    frimg = (BITBLK **)calloc(nfrimg ? nfrimg : 1, sizeof(BITBLK *));
    if (!objs || !teds || !bits || !imgs || !trees || !icons || !frstr || !frimg) { pg_rsrc_freeall(); return 0; }

    /* image data stays in the file's (Atari) byte order: PC GEM's AES and
     * VDI read bitmaps a byte at a time, leftmost pixels first, as the ST
     * does - swapped, each 16 pixels came out with their halves exchanged */
    memcpy(imgs, raw + o_imd, (size_t)(n_imd & ~1L));
    for (i = 0; i < nted; i++) {
        long p = o_ted + 28 * i;
        TEDINFO *t = &teds[i];
        t->te_ptext = strat(be32(p)); t->te_ptmplt = strat(be32(p + 4)); t->te_pvalid = strat(be32(p + 8));
        t->te_font = (short)be16(p + 12); t->te_fontid = (short)be16(p + 14); t->te_just = (short)be16(p + 16);
        t->te_color = (short)be16(p + 18); t->te_fontsize = (short)be16(p + 20); t->te_thickness = (short)be16(p + 22);
        t->te_txtlen = (short)be16(p + 24); t->te_tmplen = (short)be16(p + 26);
    }
    for (i = 0; i < nbb; i++) {
        long p = o_bb + 14 * i, d = be32(p);
        BITBLK *b = &bits[i];
        b->bi_pdata = (d >= o_imd && d < o_imd + n_imd) ? imgs + (d - o_imd) / 2 : NULL;
        b->bi_wb = (short)be16(p + 4); b->bi_hl = (short)be16(p + 6); b->bi_x = (short)be16(p + 8);
        b->bi_y = (short)be16(p + 10); b->bi_color = (short)be16(p + 12);
    }
    for (i = 0; i < nib; i++) {
        long p = o_ib + 34 * i, m = be32(p), d = be32(p + 4);
        ICONBLK *b = &icons[i];
        b->ib_pmask = (m >= o_imd && m < o_imd + n_imd) ? imgs + (m - o_imd) / 2 : NULL;
        b->ib_pdata = (d >= o_imd && d < o_imd + n_imd) ? imgs + (d - o_imd) / 2 : NULL;
        b->ib_ptext = strat(be32(p + 8));
        b->ib_char = (short)be16(p + 12);
        b->ib_xchar = (short)be16(p + 14); b->ib_ychar = (short)be16(p + 16);
        b->ib_xicon = (short)be16(p + 18); b->ib_yicon = (short)be16(p + 20);
        b->ib_wicon = (short)be16(p + 22); b->ib_hicon = (short)be16(p + 24);
        b->ib_xtext = (short)be16(p + 26); b->ib_ytext = (short)be16(p + 28);
        b->ib_wtext = (short)be16(p + 30); b->ib_htext = (short)be16(p + 32);
    }
    for (i = 0; i < nfrstr; i++) frstr[i] = strat(be32(o_frstr + 4 * i));
    for (i = 0; i < nfrimg; i++) {
        long b = be32(o_frimg + 4 * i);
        frimg[i] = (b >= o_bb && b < o_bb + 14L * nbb && (b - o_bb) % 14 == 0) ? &bits[(b - o_bb) / 14] : NULL;
    }
    for (i = 0; i < nobj; i++) {
        long p = o_obj + 24 * i, spec = be32(p + 12);
        OBJECT *o = &objs[i];
        o->ob_next = (short)be16(p); o->ob_head = (short)be16(p + 2); o->ob_tail = (short)be16(p + 4);
        o->ob_type = be16(p + 6); o->ob_flags = be16(p + 8); o->ob_state = be16(p + 10);
        switch (o->ob_type & 0xff) {
        case G_TEXT: case G_BOXTEXT: case G_FTEXT: case G_FBOXTEXT:
            o->ob_spec.tedinfo = (spec >= o_ted && spec < o_ted + 28L * nted && (spec - o_ted) % 28 == 0)
                               ? &teds[(spec - o_ted) / 28] : NULL;
            break;
        case G_IMAGE:
            o->ob_spec.bitblk = (spec >= o_bb && spec < o_bb + 14L * nbb && (spec - o_bb) % 14 == 0)
                              ? &bits[(spec - o_bb) / 14] : NULL;
            break;
        case G_BUTTON: case G_STRING: case G_TITLE:
            o->ob_spec.free_string = strat(spec);
            break;
        case G_ICON:
            o->ob_spec.iconblk = (spec >= o_ib && spec < o_ib + 34L * nib && (spec - o_ib) % 34 == 0)
                               ? &icons[(spec - o_ib) / 34] : NULL;
            break;
        default:                                   /* G_BOX, G_IBOX, G_BOXCHAR: the spec is a value */
            o->ob_spec.index = spec;
        }
        o->ob_x = fix(be16(p + 16), 1); o->ob_y = fix(be16(p + 18), 0);
        o->ob_width = fix(be16(p + 20), 1); o->ob_height = fix(be16(p + 22), 0);
    }
    for (i = 0; i < ntree; i++) {
        long t = be32(o_tree + 4 * i);
        trees[i] = (t >= o_obj && t < o_obj + 24L * nobj && (t - o_obj) % 24 == 0) ? &objs[(t - o_obj) / 24] : NULL;
    }
    return 1;
}

short rsrc_gaddr(short type, short index, void *addr)
{
    void **out = (void **)addr;
    *out = NULL;
    switch (type) {
    case R_TREE:    if (index >= 0 && index < ntree) *out = trees[index]; break;
    case R_OBJECT:  if (index >= 0 && index < nobj) *out = &objs[index]; break;
    case R_TEDINFO: if (index >= 0 && index < nted) *out = &teds[index]; break;
    case R_BITBLK:  if (index >= 0 && index < nbb) *out = &bits[index]; break;
    case R_ICONBLK: if (index >= 0 && index < nib) *out = &icons[index]; break;
    case R_FRSTR:   if (index >= 0 && index < nfrstr) *out = &frstr[index]; break;   /* char ** */
    case R_FRIMG:   if (index >= 0 && index < nfrimg) *out = &frimg[index]; break;  /* BITBLK ** */
    }
    return *out != NULL;
}
