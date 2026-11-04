/* dec_bmp.c - Windows and OS/2 bitmaps (.BMP, .DIB).
 * 1, 4 and 8 bit palette pictures, RLE4 and RLE8, 16 bit (5-5-5 or bit
 * fields), 24 bit, 32 bit (with bit fields and an alpha mask, composited
 * onto the background colour), bottom-up and top-down rows; OS/2 1.x and
 * 2.x headers and Windows V3/V4/V5 headers.                            */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "rd.h"

/* signed 32 bit little-endian (heights are negative for top-down rows) */
static long le32(const u8 *p)
{
    u32 v = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
    v &= 0xFFFFFFFFUL;
    return (v & 0x80000000UL) ? -(long)((~v + 1) & 0xFFFFFFFFUL) : (long)v;
}
static u32 ule32(const u8 *p)
{
    return ((u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24)) & 0xFFFFFFFFUL;
}
static int le16(const u8 *p) { return p[0] | (p[1] << 8); }

/* a bit field: shift and width of a channel mask */
typedef struct { int shift, bits; u32 mask; } FIELD;

static void field_of(u32 m, FIELD *f)
{
    f->mask = m;
    f->shift = f->bits = 0;
    if (!m)
        return;
    while (!(m & 1)) { m >>= 1; f->shift++; }
    while (m & 1) { m >>= 1; f->bits++; }
}

static int field_val(u32 px, const FIELD *f)
{
    u32 v;
    if (!f->bits)
        return 0;
    v = (px & f->mask) >> f->shift;
    if (f->bits >= 8)
        return (int)(v >> (f->bits - 8));
    /* scale up, e.g. 5 bits 31 -> 255 */
    return (int)(v * 255 / ((1UL << f->bits) - 1));
}

int dec_bmp(const char *path, IMAGE *im, const PREFS *pf, PROG *pg)
{
    RD r;
    u8 fh[14], ih[124];
    long offs, hsize, w, h, y, x, xppm = 0, yppm = 0, at;
    int e, bpp, comp = 0, topdown = 0, ncol = 0, i, os2;
    u32 masks[4] = { 0, 0, 0, 0 };
    FIELD fr, fg, fb, fa;
    int bv = bg_value(pf), anyalpha = 0;
    u8 *alpha = 0;                               /* kept until we know it's used */

    e = rd_open(&r, path, pg);
    if (e)
        return e;
    if (rd_read(&r, fh, 14) != 14 || fh[0] != 'B' || fh[1] != 'M') {
        rd_close(&r);
        return E_FORMAT;
    }
    offs = le32(fh + 10);
    if (rd_read(&r, ih, 4) != 4) {
        rd_close(&r);
        return E_FORMAT;
    }
    hsize = le32(ih);
    if (hsize < 12 || hsize > 124 || rd_read(&r, ih + 4, hsize - 4) != hsize - 4) {
        rd_close(&r);
        return E_FORMAT;
    }
    at = 14 + hsize;
    os2 = hsize == 12;
    if (os2) {                                   /* OS/2 1.x core header */
        w = le16(ih + 4);
        h = (short)le16(ih + 6);
        bpp = le16(ih + 10);
    } else {
        w = le32(ih + 4);
        h = le32(ih + 8);
        bpp = le16(ih + 14);
        if (hsize >= 20) comp = (int)le32(ih + 16);
        if (hsize >= 32) { xppm = le32(ih + 24); yppm = le32(ih + 28); }
        if (hsize >= 36) ncol = (int)le32(ih + 32);
        if (hsize >= 52) { masks[0] = ule32(ih + 40); masks[1] = ule32(ih + 44); masks[2] = ule32(ih + 48); }
        if (hsize >= 56) masks[3] = ule32(ih + 52);
        /* OS/2 2.x uses 3 and 4 for Huffman and RLE24: not supported */
        if (hsize == 64 && comp >= 3) {
            rd_close(&r);
            sprintf(fv_errdetail, "OS/2 BMP compression %d", comp);
            return E_UNSUP;
        }
    }
    if (h < 0) {
        topdown = 1;
        h = -h;
    }
    if (w <= 0 || h <= 0 || w > 32000 || h > 32000) {
        rd_close(&r);
        return E_FORMAT;
    }
    if (!(bpp == 1 || bpp == 4 || bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32) ||
        comp == 4 || comp == 5 || comp > 6 ||
        (comp == 1 && bpp != 8) || (comp == 2 && bpp != 4) ||
        ((comp == 3 || comp == 6) && bpp != 16 && bpp != 32)) {
        rd_close(&r);
        sprintf(fv_errdetail, "BMP, %d bit, compression %d", bpp, comp);
        return E_UNSUP;
    }

    /* bit fields stored after a 40 byte header */
    if ((comp == 3 || comp == 6) && hsize == 40) {
        u8 m[16];
        int n = comp == 6 ? 16 : 12;
        if (rd_read(&r, m, n) != n) {
            rd_close(&r);
            return E_FORMAT;
        }
        at += n;
        masks[0] = ule32(m); masks[1] = ule32(m + 4); masks[2] = ule32(m + 8);
        if (comp == 6) masks[3] = ule32(m + 12);
    }
    if (comp != 3 && comp != 6) {               /* default layouts */
        if (bpp == 16) { masks[0] = 0x7C00; masks[1] = 0x03E0; masks[2] = 0x001F; masks[3] = 0; }
        if (bpp == 32) { masks[0] = 0xFF0000L; masks[1] = 0xFF00; masks[2] = 0xFF;
                         if (hsize < 56) masks[3] = 0; }
    }
    field_of(masks[0], &fr); field_of(masks[1], &fg);
    field_of(masks[2], &fb); field_of(masks[3], &fa);

    im->w = w;
    im->h = h;
    im->type = bpp <= 8 ? IT_INDEXED : IT_RGB;
    im->orig_bits = bpp;
    if (xppm > 0 && yppm > 0 && xppm != yppm && xppm < yppm * 4 && yppm < xppm * 4) {
        /* pixel width is 1/xppm: shape w:h = yppm:xppm, kept small */
        long a = yppm, b = xppm;
        while (a > 1000 || b > 1000) { a /= 2; b /= 2; }
        im->aspx = (int)(a ? a : 1);
        im->aspy = (int)(b ? b : 1);
    }

    if (bpp <= 8) {                              /* palette */
        int n = 1 << bpp, esz = os2 ? 3 : 4;
        u8 pe[4];
        if (ncol > 0 && ncol < n) n = ncol;
        memset(im->pal, 0, sizeof im->pal);
        for (i = 0; i < n; i++) {
            if (at + esz > offs && offs > 0)
                break;                           /* palette cut short */
            if (rd_read(&r, pe, esz) != esz)
                break;
            at += esz;
            im->pal[i * 3]     = pe[2];
            im->pal[i * 3 + 1] = pe[1];
            im->pal[i * 3 + 2] = pe[0];
        }
        if (i == 0)                              /* no palette: greys */
            for (i = 0; i < n; i++)
                im->pal[i * 3] = im->pal[i * 3 + 1] = im->pal[i * 3 + 2] =
                    (u8)(i * 255 / (n - 1));
        im->ncolors = 1 << bpp;
    }
    if (offs > at)
        rd_skip(&r, offs - at);

    e = img_alloc_px(im, RB_ORIG);
    if (e) {
        rd_close(&r);
        return e;
    }
    memset(im->px, 0, im->stride * h);
    sprintf(im->fmtinfo, "BMP, %d bit%s%s", bpp, comp == 1 || comp == 2 ? ", RLE" : "",
            fa.bits ? ", alpha" : "");

    if (comp == 1 || comp == 2) {
        /* RLE8 / RLE4: pairs of count, value; 0 escapes */
        long px = 0, row = 0;
        for (;;) {
            int c = rd_byte(&r), v;
            u8 *out;
            if (c < 0)
                break;
            v = rd_byte(&r);
            if (v < 0)
                break;
            if (row >= h)
                break;
            out = im->px + (topdown ? row : h - 1 - row) * im->stride;
            if (c > 0) {                         /* run */
                for (i = 0; i < c && px < w; i++, px++)
                    out[px] = (u8)(comp == 1 ? v : (i & 1) ? v & 15 : v >> 4);
            } else if (v == 0) {                 /* end of line */
                px = 0;
                row++;
                if ((row & 15) == 0 && rd_cancelled(&r)) {
                    rd_close(&r);
                    return E_CANCEL;
                }
            } else if (v == 1) {                 /* end of picture */
                break;
            } else if (v == 2) {                 /* delta */
                int dx = rd_byte(&r), dy = rd_byte(&r);
                if (dx < 0 || dy < 0)
                    break;
                px += dx;
                row += dy;
            } else {                             /* v literal pixels */
                int nbytes = comp == 1 ? v : (v + 1) / 2, k = 0, b = 0;
                for (i = 0; i < nbytes; i++) {
                    b = rd_byte(&r);
                    if (b < 0)
                        break;
                    if (comp == 1) {
                        if (px < w) out[px] = (u8)b;
                        px++;
                    } else {
                        if (k < v) { if (px < w) out[px] = (u8)(b >> 4); px++; k++; }
                        if (k < v) { if (px < w) out[px] = (u8)(b & 15); px++; k++; }
                    }
                }
                if (nbytes & 1)
                    rd_byte(&r);                 /* word alignment */
            }
        }
    } else {
        long rowb = ((w * bpp + 31) / 32) * 4;
        u8 *line = malloc(rowb + 4);
        if (fa.bits)
            alpha = malloc(w * h);
        if (!line) {
            free(alpha);
            rd_close(&r);
            return E_NOMEM;
        }
        for (y = 0; y < h; y++) {
            u8 *out = im->px + (topdown ? y : h - 1 - y) * im->stride;
            if (rd_read(&r, line, rowb) != rowb)
                break;                           /* show what we have */
            switch (bpp) {
            case 1:
                for (x = 0; x < w; x++) out[x] = (line[x >> 3] >> (7 - (x & 7))) & 1;
                break;
            case 4:
                for (x = 0; x < w; x++) out[x] = (x & 1) ? line[x >> 1] & 15 : line[x >> 1] >> 4;
                break;
            case 8:
                memcpy(out, line, w);
                break;
            case 24:
                for (x = 0; x < w; x++) {
                    out[x * 3]     = line[x * 3 + 2];
                    out[x * 3 + 1] = line[x * 3 + 1];
                    out[x * 3 + 2] = line[x * 3];
                }
                break;
            default: {                           /* 16 or 32 bit fields */
                for (x = 0; x < w; x++) {
                    u32 p = bpp == 16 ? (u32)le16(line + x * 2) : ule32(line + x * 4);
                    out[x * 3]     = (u8)field_val(p, &fr);
                    out[x * 3 + 1] = (u8)field_val(p, &fg);
                    out[x * 3 + 2] = (u8)field_val(p, &fb);
                    if (alpha) {
                        int a = field_val(p, &fa);
                        alpha[(out - im->px) / im->stride * w + x] = (u8)a;
                        if (a) anyalpha = 1;
                    }
                }
                break;
            }
            }
            if ((y & 15) == 0 && rd_cancelled(&r)) {
                free(line);
                free(alpha);
                rd_close(&r);
                return E_CANCEL;
            }
        }
        free(line);
    }
    rd_close(&r);
    /* composite onto the background, unless every alpha value is zero
       (many writers declare an alpha mask and leave it empty)          */
    if (alpha && anyalpha) {
        long n = w * h, k;
        u8 *q = im->px;
        for (k = 0; k < n; k++, q += 3) {
            int a = alpha[k];
            q[0] = (u8)((q[0] * a + bv * (255 - a)) / 255);
            q[1] = (u8)((q[1] * a + bv * (255 - a)) / 255);
            q[2] = (u8)((q[2] * a + bv * (255 - a)) / 255);
        }
    } else if (alpha)
        strcpy(im->fmtinfo + strlen(im->fmtinfo) - 7, "");    /* drop ", alpha" */
    free(alpha);
    if (im->type == IT_RGB)
        img_reduce(im, RB_ORIG);
    else
        img_compact(im);
    return E_OK;
}
