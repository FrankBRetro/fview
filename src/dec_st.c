/*
 * dec_st.c - Atari ST picture formats.
 *
 *   Degas / Degas Elite      PI1 PI2 PI3 (plain), PC1 PC2 PC3 (PackBits)
 *   NEOchrome                NEO, with NEOchrome Master's RST raster file
 *                            and its 640x400 "big picture" variant
 *   Spectrum 512             SPU (plain), SPC (compressed), SPS (smooshed)
 *   Photochrome              PCS (one or two interlaced screens)
 *   QuantumPaint             PBX (all four modes, plain or packed)
 *
 * Single-palette pictures come out as indexed images with their own
 * palette; the multi-palette ones (Spectrum, Photochrome, QuantumPaint
 * 512, rasters) as RGB, reduced to indexed again when 256 colours or
 * fewer are used. Medium resolution pictures are marked as having
 * pixels twice as tall as wide, so they show in proportion.
 *
 * File layouts checked against RECOIL (recoil.sourceforge.net, MIT).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rd.h"

/* --------------------------------------------------------- helpers */

static unsigned be16(const u8 *p) { return ((unsigned)p[0] << 8) | p[1]; }
static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

/* palette words: STE 4-bit channels have their low bit in bit 3 */
static int ste_palette(const u8 *p, int n)
{
    while (n-- > 0) {
        if ((p[0] & 8) || (p[1] & 0x88))
            return 1;
        p += 2;
    }
    return 0;
}

static void st_rgb(unsigned w, int ste, u8 *o)
{
    int c, sh;
    for (c = 0, sh = 8; c < 3; c++, sh -= 4) {
        int n = (w >> sh) & 15;
        if (ste)
            o[c] = (u8)((((n & 7) << 1) | (n >> 3)) * 17);
        else
            o[c] = (u8)(((n & 7) * 255 + 3) / 7);
    }
}

/* pixel x of a line in ST interleaved format (4, 2 or 1 planes) */
static int ilv_pix(const u8 *line, int x, int planes)
{
    const u8 *g = line + (x >> 4) * planes * 2;
    int bit = 15 - (x & 15), p, v = 0;
    for (p = 0; p < planes; p++)
        if (be16(g + p * 2) & (1 << bit))
            v |= 1 << p;
    return v;
}

/* pixel x of a line stored as separate planes of plen bytes each */
static int sep_pix(const u8 *line, int x, long plen)
{
    int p, v = 0;
    for (p = 0; p < 4; p++)
        if (line[p * plen + (x >> 3)] & (0x80 >> (x & 7)))
            v |= 1 << p;
    return v;
}

/* ---------------------------------------------------- RLE streams */

enum { RL_PACKBITS, RL_SPC, RL_SPS, RL_PCS };

typedef struct {
    const u8 *p;
    long pos, end;
    int  kind;
    long count;          /* left in the current block                */
    int  value;          /* repeat value, -1 = literals               */
    int  words;          /* PCS palettes: values are 16-bit words      */
    long cmds;           /* PCS: commands left in the block           */
} RLE;

static int rl_byte(RLE *r) { return r->pos < r->end ? r->p[r->pos++] : -1; }

static int rl_val(RLE *r)
{
    if (r->words) {
        int v;
        if (r->pos + 1 >= r->end)
            return -1;
        v = (int)be16(r->p + r->pos);
        r->pos += 2;
        return v;
    }
    return rl_byte(r);
}

static int rl_cmd(RLE *r)
{
    int b;
    if (r->kind == RL_PCS) {
        if (r->cmds <= 0)
            return 0;
        r->cmds--;
    }
    b = rl_byte(r);
    if (b < 0)
        return 0;
    switch (r->kind) {
    case RL_PACKBITS:
        if (b < 128) { r->count = b + 1; r->value = -1; }
        else if (b == 128) { r->count = 0; r->value = -1; }  /* no-op */
        else { r->count = 257 - b; r->value = rl_val(r); }
        break;
    case RL_SPC:
        if (b < 128) { r->count = b + 1; r->value = -1; }
        else { r->count = 258 - b; r->value = rl_byte(r); }
        break;
    case RL_SPS:
        if (b < 128) { r->count = b + 3; r->value = rl_byte(r); }
        else { r->count = b - 127; r->value = -1; }
        break;
    default:           /* PCS */
        if (b < 128) {
            if (b == 0 || b == 1) {
                if (r->pos + 1 >= r->end)
                    return 0;
                r->count = be16(r->p + r->pos);
                r->pos += 2;
            } else
                r->count = b;
            r->value = b == 1 ? -1 : rl_val(r);
        } else {
            r->count = 256 - b;
            r->value = -1;
        }
    }
    return 1;
}

/* next decompressed value, -1 at end of data */
static int rl_get(RLE *r)
{
    while (r->count == 0)
        if (!rl_cmd(r))
            return -1;
    r->count--;
    return r->value >= 0 ? r->value : rl_val(r);
}

static void rl_init(RLE *r, const u8 *p, long pos, long end, int kind)
{
    memset(r, 0, sizeof *r);
    r->p = p; r->pos = pos; r->end = end; r->kind = kind;
}

/* --------------------------------------------- output helpers */

static int start_indexed(IMAGE *im, long w, long h, int planes, const u8 *pal, int ste)
{
    int e, i, n = 1 << planes;
    im->w = w; im->h = h;
    im->type = IT_INDEXED;
    e = img_alloc_px(im, RB_ORIG);
    if (e)
        return e;
    im->ncolors = n;
    im->orig_bits = planes;
    if (planes == 1) {               /* ST high: 0 white, 1 black */
        memset(im->pal, 0, 6);
        im->pal[0] = im->pal[1] = im->pal[2] = 255;
    } else
        for (i = 0; i < n; i++)
            st_rgb(be16(pal + i * 2), ste, im->pal + i * 3);
    return E_OK;
}

static int start_rgb(IMAGE *im, long w, long h)
{
    im->w = w; im->h = h;
    im->type = IT_RGB;
    im->orig_bits = 12;
    return img_alloc_px(im, RB_ORIG);
}

/* screen of interleaved planes -> indexed image */
static int put_screen(IMAGE *im, const u8 *bits, long w, long h, int planes, PROG *pg)
{
    long y, x, bpl = (w + 15) / 16 * planes * 2;
    for (y = 0; y < h; y++) {
        const u8 *line = bits + y * bpl;
        u8 *o = im->px + y * im->stride;
        for (x = 0; x < w; x++)
            o[x] = (u8)ilv_pix(line, (int)x, planes);
        if ((y & 15) == 0 && prog_step(pg, y, h))
            return E_CANCEL;
    }
    return E_OK;
}

static const int mode_w[3] = { 320, 640, 640 }, mode_h[3] = { 200, 200, 400 },
                 mode_planes[3] = { 4, 2, 1 };

static void mode_aspect(IMAGE *im, int mode)
{
    if (mode == 1) {                 /* medium: pixels twice as tall */
        im->aspx = 1;
        im->aspy = 2;
    }
}

/* -------------------------------------------------------- Degas */

static int do_degas(const u8 *f, long n, IMAGE *im, PROG *pg)
{
    int mode = f[1], e;
    long h = 200, w;
    if (n < 32034 || f[0] != 0 || mode > 2)
        return E_FORMAT;
    if (mode == 0) {                 /* a few known taller low-res sizes */
        if (n == 38434) h = 240;
        else if (n == 44834) h = 280;
        else if (n == 64034) h = 400;
    }
    if (mode == 0 && n == 116514) {  /* 416x560 */
        w = 416; h = 560;
    } else {
        w = mode_w[mode];
        h = mode == 0 ? h : mode_h[mode];
    }
    if (34 + (w + 15) / 16 * mode_planes[mode] * 2 * h > n)
        return E_FORMAT;
    e = start_indexed(im, w, h, mode_planes[mode], f + 2, ste_palette(f + 2, 16));
    if (e) return e;
    mode_aspect(im, mode);
    sprintf(im->fmtinfo, "Degas, %s resolution", mode == 0 ? "low" : mode == 1 ? "medium" : "high");
    return put_screen(im, f + 34, w, h, mode_planes[mode], pg);
}

static int do_degas_c(const u8 *f, long n, IMAGE *im, PROG *pg)
{
    int mode = f[1] & 3, planes, p, e;
    long y, h, bpp, bpl, i;
    u8 *scr;
    RLE r;
    if (n < 68 || f[0] != 0x80 || mode > 2)
        return E_FORMAT;
    planes = mode_planes[mode];
    h = mode_h[mode];
    bpp = mode_w[mode] / 8;                    /* bytes per plane line */
    bpl = bpp * planes;
    scr = calloc(bpl, h);
    if (!scr)
        return E_NOMEM;
    rl_init(&r, f, 34, n, RL_PACKBITS);
    for (y = 0; y < h; y++) {
        u8 *line = scr + y * bpl;
        for (p = 0; p < planes; p++)
            for (i = 0; i < bpp; i++) {
                int b = rl_get(&r);
                if (b < 0)
                    goto done;                 /* short file: show the rest blank */
                line[(i >> 1) * planes * 2 + p * 2 + (i & 1)] = (u8)b;
            }
    }
done:
    e = start_indexed(im, mode_w[mode], h, planes, f + 2, ste_palette(f + 2, 16));
    if (!e) {
        mode_aspect(im, mode);
        sprintf(im->fmtinfo, "Degas Elite, %s resolution",
                mode == 0 ? "low" : mode == 1 ? "medium" : "high");
        e = put_screen(im, scr, mode_w[mode], h, planes, pg);
    }
    free(scr);
    return e;
}

/* ----------------------------------------------------- NEOchrome */

/* NEOchrome Master keeps raster palettes in a .RST file next to the
   picture: up to 200 records of a line number and 16 colours.          */
/* unused RST records are left zero: line 0, all black - skip them */
static int all_zero(const u8 *p, int n)
{
    while (n--)
        if (*p++)
            return 0;
    return 1;
}

static u8 *read_rst(const char *path, long *len)
{
    char p[256], *d;
    static const char *ext[] = { "RST", "rst" };
    int k, e;
    for (k = 0; k < 2; k++) {
        u8 *m;
        strncpy(p, path, sizeof p - 5);
        p[sizeof p - 5] = 0;
        d = strrchr(p, '.');
        if (!d)
            return 0;
        strcpy(d + 1, ext[k]);
        m = rd_slurp(p, len, 0, &e);
        if (m)
            return m;
    }
    return 0;
}

static int do_neo(const char *path, const u8 *f, long n, IMAGE *im, PROG *pg)
{
    int mode, e;
    long rlen = 0, y, x;
    u8 *rst;
    if (n == 128128 && f[0] == 0xBA && f[1] == 0xBE) {
        /* NEOchrome Master big picture: 640x400 in low resolution */
        e = start_indexed(im, 640, 400, 4, f + 4, ste_palette(f + 4, 16));
        if (e) return e;
        strcpy(im->fmtinfo, "NEOchrome Master, 640x400");
        return put_screen(im, f + 128, 640, 400, 4, pg);
    }
    if (n < 32128 || f[0] != 0 || f[1] != 0)
        return E_FORMAT;
    mode = f[3];
    if (mode > 2)
        return E_FORMAT;
    rst = mode == 0 ? read_rst(path, &rlen) : 0;
    if (rst && rlen >= 34) {
        /* rasters: the palette changes on the lines the RST file lists */
        u8 pal[32];
        int ste = ste_palette(f + 4, 16);
        long k;
        for (k = 0; k + 34 <= rlen; k += 34)
            ste |= ste_palette(rst + k + 2, 16);
        e = start_rgb(im, 320, 200);
        if (e) { free(rst); return e; }
        memcpy(pal, f + 4, 32);
        for (y = 0; y < 200; y++) {
            u8 rgb[16 * 3], *o = im->px + y * im->stride;
            const u8 *line = f + 128 + y * 160;
            for (k = 0; k + 34 <= rlen; k += 34)       /* not sorted by line */
                if ((long)be16(rst + k) == y && !all_zero(rst + k + 2, 32)) {
                    memcpy(pal, rst + k + 2, 32);
                    break;
                }
            for (x = 0; x < 16; x++)
                st_rgb(be16(pal + x * 2), ste, rgb + x * 3);
            for (x = 0; x < 320; x++, o += 3)
                memcpy(o, rgb + ilv_pix(line, (int)x, 4) * 3, 3);
            if ((y & 15) == 0 && prog_step(pg, y, 200)) {
                free(rst);
                return E_CANCEL;
            }
        }
        free(rst);
        strcpy(im->fmtinfo, "NEOchrome + rasters");
        img_reduce(im, RB_ORIG);
        return E_OK;
    }
    free(rst);
    e = start_indexed(im, mode_w[mode], mode_h[mode], mode_planes[mode], f + 4,
                      ste_palette(f + 4, 16));
    if (e) return e;
    mode_aspect(im, mode);
    sprintf(im->fmtinfo, "NEOchrome, %s resolution", mode == 0 ? "low" : mode == 1 ? "medium" : "high");
    return put_screen(im, f + 128, mode_w[mode], mode_h[mode], mode_planes[mode], pg);
}

/* --------------------------------------------------- Spectrum 512 */

/* unpacked layout shared by SPU/SPC/SPS: 32000 bytes of screen (line 0
   blank) and 199 lines x 48 colour words                              */
#define SPU_LEN 51104

/* Spectrum: which of the line's 48 colours a pixel of colour c at x uses */
static int spec_index(int c, int x)
{
    int x1 = c * 10 + 1 - (c & 1) * 6;
    if (x >= x1 + 160)
        return c + 32;
    if (x >= x1)
        return c + 16;
    return c;
}

static int spu_image(const u8 *u, IMAGE *im, const char *what, PROG *pg)
{
    long y, x;
    int ste = ste_palette(u + 32000, 199 * 48), e;
    e = start_rgb(im, 320, 199);
    if (e) return e;
    for (y = 0; y < 199; y++) {
        const u8 *line = u + 160 + y * 160, *pal = u + 32000 + y * 96;
        u8 rgb[48 * 3], *o = im->px + y * im->stride;
        for (x = 0; x < 48; x++)
            st_rgb(be16(pal + x * 2), ste, rgb + x * 3);
        for (x = 0; x < 320; x++, o += 3)
            memcpy(o, rgb + spec_index(ilv_pix(line, (int)x, 4), (int)x) * 3, 3);
        if ((y & 15) == 0 && prog_step(pg, y, 199))
            return E_CANCEL;
    }
    strcpy(im->fmtinfo, what);
    img_reduce(im, RB_ORIG);
    return E_OK;
}

/* SPC/SPS bitmap order: plane by plane, every word of the screen */
static int spc_bitmap(RLE *r, u8 *u)
{
    int plane;
    long off;
    for (plane = 0; plane < 8; plane += 2)
        for (off = 160 + plane; off < 32000; off += 8) {
            int a = rl_get(r), b = rl_get(r);
            if (a < 0 || b < 0)
                return 0;
            u[off] = (u8)a;
            u[off + 1] = (u8)b;
        }
    return 1;
}

static int do_spc(const u8 *f, long n, IMAGE *im, PROG *pg)
{
    u8 *u;
    RLE r;
    long po, uo;
    int e;
    if (n < 12 || f[0] != 'S' || f[1] != 'P')
        return E_FORMAT;
    u = calloc(SPU_LEN, 1);
    if (!u) return E_NOMEM;
    rl_init(&r, f, 12, n, RL_SPC);
    if (!spc_bitmap(&r, u)) { free(u); return E_FORMAT; }
    po = 12 + (long)be32(f + 4);
    if (po < 12 || po > n) { free(u); return E_FORMAT; }
    /* palettes: a 15-bit mask of the colours present, then those words */
    for (uo = 32000; uo < SPU_LEN; ) {
        unsigned got;
        int i;
        if (po + 1 >= n) { free(u); return E_FORMAT; }
        got = ((f[po] & 0x7f) << 8) | f[po + 1];
        po += 2;
        for (i = 0; i < 16; i++, uo += 2) {
            if (got >> i & 1) {
                if (po + 1 >= n) { free(u); return E_FORMAT; }
                u[uo] = f[po]; u[uo + 1] = f[po + 1];
                po += 2;
            }
        }
    }
    e = spu_image(u, im, "Spectrum 512 (SPC)", pg);
    free(u);
    return e;
}

/* SPS palettes are a bit stream: 14-bit mask, then 9-bit colours */
typedef struct { const u8 *p; long pos, end; int bit; } BITS;
static int bits_get(BITS *b, int n)
{
    int v = 0;
    while (n--) {
        if (b->pos >= b->end)
            return -1;
        v = (v << 1) | ((b->p[b->pos] >> (7 - b->bit)) & 1);
        if (++b->bit == 8) { b->bit = 0; b->pos++; }
    }
    return v;
}

static int do_sps(const u8 *f, long n, IMAGE *im, PROG *pg)
{
    u8 *u;
    RLE r;
    BITS b;
    long uo;
    int e;
    if (n < 13 || f[0] != 'S' || f[1] != 'P' || f[2] || f[3])
        return E_FORMAT;
    u = calloc(SPU_LEN, 1);
    if (!u) return E_NOMEM;
    rl_init(&r, f, 12, n, RL_SPS);
    if (!(f[n - 1] & 1)) {
        /* bitmap in columns: plane, byte column, all lines */
        int plane, x;
        long y;
        for (plane = 0; plane < 8; plane += 2)
            for (x = 0; x < 40; x++)
                for (y = 160 + ((x & ~1) << 2) + plane + (x & 1); y < 32000; y += 160) {
                    int v = rl_get(&r);
                    if (v < 0) { free(u); return E_FORMAT; }
                    u[y] = (u8)v;
                }
    } else if (!spc_bitmap(&r, u)) {
        free(u);
        return E_FORMAT;
    }
    b.p = f; b.pos = 12 + (long)be32(f + 4); b.end = n; b.bit = 0;
    if (b.pos < 12 || b.pos > n) { free(u); return E_FORMAT; }
    for (uo = 32000; uo < SPU_LEN; ) {
        int got = bits_get(&b, 14), i;
        if (got < 0) { free(u); return E_FORMAT; }
        got <<= 1;
        for (i = 15; i >= 0; i--, uo += 2) {
            int rgb = 0;
            if (got >> i & 1) {
                rgb = bits_get(&b, 9);
                if (rgb < 0) { free(u); return E_FORMAT; }
            }
            u[uo] = (u8)(rgb >> 6);                       /* RRRGGGBBB */
            u[uo + 1] = (u8)(((rgb >> 3) & 7) << 4 | (rgb & 7));
        }
    }
    e = spu_image(u, im, "Spectrum 512 (SPS)", pg);
    free(u);
    return e;
}

/* ---------------------------------------------------- Photochrome */

#define PCS_LEN (32000 + (199 * 3 + 1) * 32)

static int pcs_block(RLE *r)
{
    if (r->pos + 1 >= r->end)
        return 0;
    r->cmds = be16(r->p + r->pos);
    r->pos += 2;
    r->count = 0;
    return 1;
}

static int pcs_end(RLE *r)         /* skip what is left of the block */
{
    while (r->count > 0 || r->cmds > 0)
        if (rl_get(r) < 0)
            return 0;
    return 1;
}

static int pcs_unpack(RLE *r, u8 *u)
{
    long i;
    r->words = 0;
    if (!pcs_block(r))
        return 0;
    for (i = 0; i < 32000; i++) {
        int v = rl_get(r);
        if (v < 0) return 0;
        u[i] = (u8)v;
    }
    if (!pcs_end(r))
        return 0;
    r->words = 1;
    if (!pcs_block(r))
        return 0;
    for (i = 32000; i < PCS_LEN; i += 2) {
        int v = rl_get(r);
        if (v < 0) return 0;
        u[i] = (u8)(v >> 8);
        u[i + 1] = (u8)v;
    }
    return pcs_end(r);
}

/* colours of one Photochrome screen into rgb (320 x 199 x 3) */
static void pcs_screen(const u8 *u, u8 *rgb, int ste)
{
    long y, x;
    for (y = 0; y < 199; y++) {
        const u8 *line = u + 40 + y * 40, *pal = u + 32000 + y * 96;
        for (x = 0; x < 320; x++, rgb += 3) {
            int c = sep_pix(line, (int)x, 8000) << 1;   /* byte offset */
            if (x >= c * 2) {
                if (c < 28) {
                    if (x >= c * 2 + 76) {
                        if (x >= 176 + c * 5 - (c & 2) * 3)
                            c += 32;
                        c += 32;
                    }
                } else if (x >= c * 2 + 92)
                    c += 32;
                c += 32;
            }
            st_rgb(be16(pal + c), ste, rgb);
        }
    }
}

static int do_pcs(const u8 *f, long n, IMAGE *im, PROG *pg)
{
    u8 *u1, *u2 = 0;
    RLE r;
    int e, ste;
    long i;
    if (n < 18 || f[0] != 0x01 || f[1] != 0x40 || f[2] != 0x00 || f[3] != 0xC8)
        return E_FORMAT;
    u1 = malloc(PCS_LEN);
    if (!u1) return E_NOMEM;
    rl_init(&r, f, 6, n, RL_PCS);
    if (!pcs_unpack(&r, u1)) { free(u1); return E_FORMAT; }
    prog_step(pg, 1, 3);
    ste = ste_palette(u1 + 32000, 199 * 48);
    if (f[4]) {                      /* second screen, shown alternately */
        u2 = malloc(PCS_LEN);
        if (!u2 || !pcs_unpack(&r, u2)) {
            free(u1); free(u2);
            return u2 ? E_FORMAT : E_NOMEM;
        }
        if (!(f[4] & 1))             /* stored as a difference */
            for (i = 0; i < 32000; i++) u2[i] ^= u1[i];
        if (!(f[4] & 2))
            for (i = 32000; i < PCS_LEN; i++) u2[i] ^= u1[i];
        ste |= ste_palette(u2 + 32000, 199 * 48);
    }
    if (prog_step(pg, 2, 3)) {
        free(u1); free(u2);
        return E_CANCEL;
    }
    e = start_rgb(im, 320, 199);
    if (!e) {
        pcs_screen(u1, im->px, ste);
        if (u2) {                    /* blend the two, as the eye does */
            u8 *t = malloc(320L * 199 * 3);
            if (t) {
                pcs_screen(u2, t, ste);
                for (i = 0; i < 320L * 199 * 3; i++)
                    im->px[i] = (u8)((im->px[i] + t[i] + 1) >> 1);
                free(t);
            }
        }
        strcpy(im->fmtinfo, u2 ? "Photochrome, interlaced" : "Photochrome");
        img_reduce(im, RB_ORIG);
    }
    free(u1);
    free(u2);
    return e;
}

/* --------------------------------------------------- QuantumPaint */

/* a PBX is either the raw layout or packed: the header area PackBits'd
   in order, then the bitmap word column by word column                 */
static u8 *pbx_unpack(const u8 *f, long n, long bmoff, int bp16, long ulen)
{
    u8 *u;
    RLE r;
    long o;
    int plane;
    if (!(f[4] == 0x80 && f[5] == 1)) {
        if (n < ulen)
            return 0;
        u = malloc(ulen);
        if (u) memcpy(u, f, ulen);
        return u;
    }
    u = calloc(ulen, 1);
    if (!u)
        return 0;
    memcpy(u, f, 128);
    rl_init(&r, f, 128, n, RL_PACKBITS);
    for (o = 128; o < bmoff; o++) {
        int v = rl_get(&r);
        if (v < 0) { free(u); return 0; }
        u[o] = (u8)v;
    }
    for (plane = 0; plane < bp16; plane += 2) {
        long x;
        for (x = plane; x < 160; x += bp16)
            for (o = bmoff + x; o < ulen; o += 160) {
                int a = rl_get(&r), b = rl_get(&r);
                if (a < 0 || b < 0) { free(u); return 0; }
                u[o] = (u8)a;
                u[o + 1] = (u8)b;
            }
    }
    return u;
}

/* 512-colour modes: 32 colours per line, switched part way along */
static void pbx8_screen(const u8 *u, long poff, long boff, u8 *rgb, int ste)
{
    long y, x;
    for (y = 0; y < 200; y++) {
        const u8 *line = u + boff + y * 160, *pal = u + poff + y * 64;
        for (x = 0; x < 320; x++, rgb += 3) {
            int c = ilv_pix(line, (int)x, 4);
            if (x >= (c > 7 ? 88 : 76) + c * 10 - (c & 1) * 6)
                c += 16;
            st_rgb(be16(pal + c * 2), ste, rgb);
        }
    }
}

static int do_pbx(const u8 *f, long n, IMAGE *im, PROG *pg)
{
    int mode, e = E_OK;
    u8 *u;
    long i;
    if (n < 128 || f[0] || f[1] || f[2])
        return E_FORMAT;
    mode = f[3];
    if (mode == 0 || mode == 1) {
        /* 16 or 4 colours, up to 8 palettes changing at given lines */
        int planes = mode ? 2 : 4, ste = 0;
        long w = mode ? 640 : 320, y, x, po = 128;
        u8 rgb[16 * 3];
        u = pbx_unpack(f, n, 512, planes * 2, 32512);
        if (!u) return E_FORMAT;
        for (i = 128; i < 512; i += 48)
            ste |= ste_palette(u + i, 16);
        e = start_rgb(im, w, 200);
        if (e) { free(u); return e; }
        memset(rgb, 0, sizeof rgb);
        for (y = 0; y < 200; y++) {
            u8 *o = im->px + y * im->stride;
            const u8 *line = u + 512 + y * 160;
            if (po < 512 && y == u[po + 33]) {
                for (x = 0; x < (1 << planes); x++)
                    st_rgb(be16(u + po + x * 2), ste, rgb + x * 3);
                do
                    po += 48;
                while (po < 512 && u[po + 34] == 0 && u[po + 35] == 0);
            }
            for (x = 0; x < w; x++, o += 3)
                memcpy(o, rgb + ilv_pix(line, (int)x, planes) * 3, 3);
            if ((y & 15) == 0 && prog_step(pg, y, 200)) {
                free(u);
                return E_CANCEL;
            }
        }
        if (mode == 1) { im->aspx = 1; im->aspy = 2; }
        strcpy(im->fmtinfo, mode ? "QuantumPaint, medium" : "QuantumPaint, 16 colours");
    } else if (mode == 0x80 || mode == 0x81) {
        long boff = mode == 0x80 ? 0x3280 : 0x6480, ulen = mode == 0x80 ? 44928 : 57728;
        int ste;
        u = pbx_unpack(f, n, boff, 8, ulen);
        if (!u) return E_FORMAT;
        ste = ste_palette(u + 128, (int)((boff - 128) / 2));
        if (prog_step(pg, 1, 2)) { free(u); return E_CANCEL; }
        e = start_rgb(im, 320, 200);
        if (e) { free(u); return e; }
        pbx8_screen(u, 128, boff, im->px, ste);
        if (mode == 0x81) {          /* two palettes, shown alternately */
            u8 *t = malloc(320L * 200 * 3);
            if (t) {
                pbx8_screen(u, 0x3280, boff, t, ste);
                for (i = 0; i < 320L * 200 * 3; i++)
                    im->px[i] = (u8)((im->px[i] + t[i] + 1) >> 1);
                free(t);
            }
        }
        strcpy(im->fmtinfo, mode == 0x80 ? "QuantumPaint, 512 colours" : "QuantumPaint, 4096 colours");
    } else
        return E_UNSUP;
    free(u);
    img_reduce(im, RB_ORIG);
    return e;
}

/* --------------------------------------------------------- entry */

int dec_st(const char *path, int ftype, IMAGE *im, const PREFS *pf, PROG *pg)
{
    long n;
    int e;
    u8 *f = rd_slurp(path, &n, pg, &e);
    (void)pf;
    if (!f)
        return e;
    switch (ftype) {
    case FT_DEGAS:  e = f[0] == 0x80 ? do_degas_c(f, n, im, pg) : do_degas(f, n, im, pg); break;
    case FT_DEGASC: e = f[0] == 0x80 ? do_degas_c(f, n, im, pg) : do_degas(f, n, im, pg); break;
    case FT_NEO:    e = do_neo(path, f, n, im, pg); break;
    case FT_SPU:    e = n >= SPU_LEN ? spu_image(f, im, "Spectrum 512", pg) : E_FORMAT; break;
    case FT_SPC:    e = do_spc(f, n, im, pg); break;
    case FT_SPS:    e = do_sps(f, n, im, pg); break;
    case FT_PCS:    e = do_pcs(f, n, im, pg); break;
    case FT_PBX:    e = do_pbx(f, n, im, pg); break;
    default:        e = E_UNSUP;
    }
    free(f);
    return e;
}
