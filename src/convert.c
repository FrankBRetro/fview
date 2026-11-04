/*
 * convert.c - decode a picture and write it as a GEM IMG (used by FConv,
 * in FVWORK.TTP or cooperatively). No GEM or MiNT calls in here.
 *
 *   XF_ORIGINAL  the picture's own depth: 1/2/4/8-plane XIMG with its
 *                palette, or 24-bit for true colour pictures
 *   XF_TRUE      24-bit true colour
 *   XF_SCREEN    dithered to the screen's colours with the dither and
 *                palette preferences (true colour screens: 24-bit)
 *   XF_MONO      dithered to black and white
 *
 * Progress: phase 0 reading, 1 converting (dithering), 2 writing.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "fview.h"

int decode_picture(const char *path, int ftype, IMAGE *im, const PREFS *pf,
                   const DISPINFO *d, PROG *pg)
{
    img_init(im);
    switch (ftype) {
    case FT_GIF:  return dec_gif(path, im, pf, pg);
    case FT_IFF:  return dec_iff(path, im, pf, pg);
    case FT_BMP:  return dec_bmp(path, im, pf, pg);
    case FT_IMG:  return dec_img(path, im, pf, d, pg);
    case FT_DEGAS: case FT_DEGASC: case FT_NEO: case FT_SPU: case FT_SPC:
    case FT_SPS: case FT_PCS: case FT_PBX:
                  return dec_st(path, ftype, im, pf, pg);
    case FT_PNG: case FT_JPEG:
                  return dec_stb(path, im, pf, d, pg);
    }
    strcpy(fv_errdetail, "not a picture");
    return E_UNSUP;
}

/* rows for the 24-bit writer, shrunk with a box filter when needed */
typedef struct {
    const IMAGE *im;
    long tw, th;
    u8 *src;                   /* one source row of RGB                   */
    u32 *acc;                  /* sums for one target row                 */
    u16 *cnt;
} RGBCTX;

static void rgb_row(void *ctx, long y, u8 *out)
{
    RGBCTX *c = ctx;
    const IMAGE *im = c->im;
    long x, sy, sy0, sy1;
    if (c->tw == im->w && c->th == im->h) {
        img_get_rgb_row(im, y, out);
        return;
    }
    sy0 = y * im->h / c->th;
    sy1 = (y + 1) * im->h / c->th;
    if (sy1 <= sy0) sy1 = sy0 + 1;
    memset(c->acc, 0, c->tw * 3 * sizeof(u32));
    memset(c->cnt, 0, c->tw * sizeof(u16));
    for (sy = sy0; sy < sy1 && sy < im->h; sy++) {
        img_get_rgb_row(im, sy, c->src);
        for (x = 0; x < im->w; x++) {
            long tx = x * c->tw / im->w;
            c->acc[tx * 3]     += c->src[x * 3];
            c->acc[tx * 3 + 1] += c->src[x * 3 + 1];
            c->acc[tx * 3 + 2] += c->src[x * 3 + 2];
            c->cnt[tx]++;
        }
    }
    for (x = 0; x < c->tw; x++) {
        unsigned n = c->cnt[x] ? c->cnt[x] : 1;
        out[x * 3]     = (u8)(c->acc[x * 3] / n);
        out[x * 3 + 1] = (u8)(c->acc[x * 3 + 1] / n);
        out[x * 3 + 2] = (u8)(c->acc[x * 3 + 2] / n);
    }
}

/* ----------------------------------------------------------- PDF output */

/* pack one row of pixel values into bpc bits each */
static void pack_row(const u8 *v, long w, int bpc, u8 *out, long rowbytes)
{
    long x;
    memset(out, 0, rowbytes);
    if (bpc == 8) {
        memcpy(out, v, w);
        return;
    }
    for (x = 0; x < w; x++) {
        long bit = x * bpc;
        out[bit >> 3] |= (u8)((v[x] & ((1 << bpc) - 1)) << (8 - bpc - (bit & 7)));
    }
}

static int bpc_for(int ncolors)
{
    return ncolors <= 2 ? 1 : ncolors <= 4 ? 2 : ncolors <= 16 ? 4 : 8;
}

/* palette picture at its own depth */
static int pdf_indexed(const char *out, const IMAGE *im, PROG *pg)
{
    int e, bpc = bpc_for(im->ncolors);
    long y;
    u8 *row;
    PDFW *p = pdfw_open(out, im->w, im->h, 1, bpc, im->pal, im->ncolors,
                        im->aspx, im->aspy, &e);
    if (!p)
        return e;
    row = malloc(pdfw_rowbytes(p) + 4);
    if (!row)
        return pdfw_close(p, out, 1), E_NOMEM;
    for (y = 0; y < im->h && !e; y++) {
        pack_row(im->px + y * im->stride, im->w, bpc, row, pdfw_rowbytes(p));
        e = pdfw_row(p, row);
        if ((y & 15) == 0 && prog_step(pg, y, im->h)) {
            free(row);
            return pdfw_close(p, out, 1);
        }
    }
    free(row);
    return e ? (pdfw_close(p, out, 1), e) : pdfw_close(p, out, 0);
}

/* dithered: banded standard planes with a palette by pixel value */
static int pdf_planes(const char *out, const u16 *std, long w, long h, long wdw,
                      int planes, const u8 *pal, int aspx, int aspy, PROG *pg)
{
    int e, bpc = planes >= 8 ? 8 : planes >= 4 ? 4 : planes >= 2 ? 2 : 1;
    long y, x, psize;
    u8 *row, *val;
    PDFW *p = pdfw_open(out, w, h, 1, bpc, pal, 1 << bpc, aspx, aspy, &e);
    if (!p)
        return e;
    row = malloc(pdfw_rowbytes(p) + 4);
    val = malloc(w + 16);
    if (!row || !val) {
        free(row);
        free(val);
        return pdfw_close(p, out, 1), E_NOMEM;
    }
    for (y = 0; y < h && !e; y++) {
        const u16 *r = std_row((u16 *)std, y, h, wdw, planes, &psize);
        int pl;
        memset(val, 0, w);
        for (pl = 0; pl < planes; pl++) {
            const u16 *src = r + pl * psize;
            for (x = 0; x < w; x++)
                if (src[x >> 4] & (0x8000 >> (x & 15)))
                    val[x] |= (u8)(1 << pl);
        }
        pack_row(val, w, bpc, row, pdfw_rowbytes(p));
        e = pdfw_row(p, row);
        if ((y & 15) == 0 && prog_step(pg, y, h)) {
            free(row);
            free(val);
            return pdfw_close(p, out, 1);
        }
    }
    free(row);
    free(val);
    return e ? (pdfw_close(p, out, 1), e) : pdfw_close(p, out, 0);
}

static void rgb_row(void *ctx, long y, u8 *out);

static int write_rgb(const char *out, const IMAGE *im, long tw, long th, PROG *pg);

/* 24-bit, shrunk to tw x th if needed */
static int pdf_rgb(const char *out, const IMAGE *im, long tw, long th, PROG *pg);

static int write_rgb_any(const char *out, const IMAGE *im, long tw, long th,
                         int pdf, PROG *pg)
{
    return pdf ? pdf_rgb(out, im, tw, th, pg) : write_rgb(out, im, tw, th, pg);
}

static int write_rgb(const char *out, const IMAGE *im, long tw, long th, PROG *pg)
{
    RGBCTX c;
    int e;
    memset(&c, 0, sizeof c);
    c.im = im;
    c.tw = tw;
    c.th = th;
    c.src = malloc(im->w * 3 + 4);
    c.acc = malloc(tw * 3 * sizeof(u32) + 4);
    c.cnt = malloc(tw * sizeof(u16) + 4);
    if (!c.src || !c.acc || !c.cnt)
        e = E_NOMEM;
    else
        e = img_save_rgbrows(out, tw, th, rgb_row, &c, pg);
    free(c.src);
    free(c.acc);
    free(c.cnt);
    return e;
}

static int pdf_rgb(const char *out, const IMAGE *im, long tw, long th, PROG *pg)
{
    RGBCTX c;
    PDFW *p;
    int e;
    long y;
    u8 *row;
    memset(&c, 0, sizeof c);
    c.im = im;
    c.tw = tw;
    c.th = th;
    c.src = malloc(im->w * 3 + 4);
    c.acc = malloc(tw * 3 * sizeof(u32) + 4);
    c.cnt = malloc(tw * sizeof(u16) + 4);
    row = malloc(tw * 3 + 4);
    if (!c.src || !c.acc || !c.cnt || !row) {
        e = E_NOMEM;
        goto done;
    }
    p = pdfw_open(out, tw, th, 3, 8, 0, 0, im->aspx, im->aspy, &e);
    if (!p)
        goto done;
    for (y = 0; y < th && !e; y++) {
        rgb_row(&c, y, row);
        e = pdfw_row(p, row);
        if ((y & 15) == 0 && prog_step(pg, y, th)) {
            e = pdfw_close(p, out, 1);
            goto done;
        }
    }
    e = e ? (pdfw_close(p, out, 1), e) : pdfw_close(p, out, 0);
done:
    free(c.src);
    free(c.acc);
    free(c.cnt);
    free(row);
    return e;
}

int convert_picture(const char *path, int ftype, const char *out, int xfmt,
                    int pdf, long maxw, long maxh, const PREFS *pf0,
                    const DISPINFO *d0, PROG *pg)
{
    IMAGE im;
    PREFS pf = *pf0;
    DISPINFO d = *d0;
    long tw, th;
    int e, dith;

    fv_errdetail[0] = 0;
    dith = xfmt == XF_MONO || (xfmt == XF_SCREEN && !d.truecolor);
    pg->phases = dith ? 3 : 2;
    pg->phase = 0;
    e = decode_picture(path, ftype, &im, &pf, &d, pg);
    if (e) {
        res_free(RB_ORIG);
        return e;
    }
    tw = im.w;
    th = im.h;
    if (xfmt != XF_ORIGINAL && maxw > 0 && maxh > 0 && (tw > maxw || th > maxh))
        img_fit(im.w, im.h, maxw, maxh, &tw, &th);
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    /* the shrunk picture keeps the source's pixel shape */
    img_set_pixel_shape(im.aspx, im.aspy);

    if (xfmt == XF_ORIGINAL) {
        pg->phase = 1;
        if (!pdf)
            e = img_save(out, &im, &pf, pg);
        else if (im.type == IT_RGB)
            e = pdf_rgb(out, &im, im.w, im.h, pg);
        else
            e = pdf_indexed(out, &im, pg);
    } else if (!dith) {                         /* 24-bit */
        pg->phase = 1;
        e = write_rgb_any(out, &im, tw, th, pdf, pg);
    } else {
        u8 pal[256 * 3], *buf = 0;
        long wdw = 0;
        if (xfmt == XF_MONO) {
            memset(&d, 0, sizeof d);
            d.planes = 1;
            d.fixed_palette = 1;
            d.palmode = PAL_SYSTEM;
            d.sysrgb[0] = d.sysrgb[1] = d.sysrgb[2] = 255;      /* white */
        } else {
            d.palmode = pf.palmode;
            memset(d.reserved, 0, sizeof d.reserved);  /* a file needs no UI colours */
        }
        d.chunky = 0;                           /* we want standard planes */
        pg->phase = 1;
        e = render_display(&im, tw, th, &d, &pf, pal, &buf, &wdw, pg);
        res_free(RB_ORIG);                      /* the original is done with */
        if (!e) {
            pg->phase = 2;
            img_set_pixel_shape(im.aspx, im.aspy);
            if (pdf)
                e = pdf_planes(out, (const u16 *)buf, tw, th, wdw, d.planes, pal,
                               im.aspx, im.aspy, pg);
            else
                e = img_save_planes(out, (const u16 *)buf, tw, th, wdw, d.planes,
                                    pal, &pf, pg);
        }
        res_free(RB_DISP);
        img_set_pixel_shape(0, 0);
        return e;
    }
    res_free(RB_ORIG);
    img_set_pixel_shape(0, 0);
    return e;
}
