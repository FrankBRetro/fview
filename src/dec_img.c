/* dec_img.c - GEM (X)IMG decoder: 1-8 planes with XIMG RGB palettes,
 * 24-bit true colour IMG (chunky RGB scanlines), vertical replication,
 * pattern runs, solid runs and bit strings. */
#include <stdlib.h>
#include <string.h>
#include "rd.h"

/* default VDI colours 0..15 (pen order), 0..1000 scale converted to 0..255 */
static const u8 vdi16[16 * 3] = {
    255,255,255,  0,0,0,  255,0,0,  0,255,0,  0,0,255,  0,255,255,
    255,255,0,  255,0,255,  192,192,192,  128,128,128,  128,0,0,
    0,128,0,  0,0,128,  0,128,128,  128,128,0,  128,0,128
};

/* decode one compressed plane line (bytes) */
static int img_line(RD *r, u8 *dst, long n, int patlen)
{
    long o = 0;
    while (o < n) {
        int c = rd_byte(r);
        if (c < 0)
            return 0;
        if (c == 0) {                 /* pattern run */
            int cnt = rd_byte(r), i;
            u8 pat[16];
            if (cnt < 0) return 0;
            if (cnt == 0) return -1;  /* unexpected vertical replication */
            if (rd_read(r, pat, patlen) != patlen) return 0;
            while (cnt--)
                for (i = 0; i < patlen; i++)
                    if (o < n) dst[o++] = pat[i];
        } else if (c == 0x80) {       /* bit string */
            int cnt = rd_byte(r);
            if (cnt < 0) return 0;
            while (cnt--) {
                int v = rd_byte(r);
                if (v < 0) return 0;
                if (o < n) dst[o++] = (u8)v;
            }
        } else {                      /* solid run */
            int cnt = c & 0x7F;
            u8 v = (c & 0x80) ? 0xFF : 0x00;
            while (cnt--)
                if (o < n) dst[o++] = v;
        }
    }
    return 1;
}

int dec_img(const char *path, IMAGE *im, const PREFS *pf, const DISPINFO *d, PROG *pg)
{
    RD r;
    int e, hdrw, planes, patlen, i;
    long w, h, y, x, bpl, linebytes;
    int ximg = 0, tc;
    u16 hdr[8];
    u8 *line;

    e = rd_open(&r, path, pg);
    if (e)
        return e;
    for (i = 0; i < 8; i++)
        hdr[i] = rd_u16be(&r);
    hdrw = hdr[1];
    planes = hdr[2];
    patlen = hdr[3];
    w = hdr[6];
    h = hdr[7];
    if (hdr[4] > 0 && hdr[5] > 0 && hdr[4] <= hdr[5] * 4 && hdr[5] <= hdr[4] * 4) {
        im->aspx = hdr[4];              /* pixel size in micrometres */
        im->aspy = hdr[5];
    }
    if (hdrw < 8 || planes < 1 || w <= 0 || h <= 0 || patlen < 1 || patlen > 16) {
        rd_close(&r);
        return E_FORMAT;
    }
    tc = planes > 8;
    if (tc && planes != 24 && planes != 32) {
        rd_close(&r);
        return E_UNSUP;
    }
    im->w = w; im->h = h;
    im->type = tc ? IT_RGB : IT_INDEXED;
    im->orig_bits = tc ? 24 : planes;
    e = img_alloc_px(im, RB_ORIG);
    if (e) {
        rd_close(&r);
        return e;
    }

    /* palette */
    if (!tc) {
        int ncol = 1 << planes;
        im->ncolors = ncol;
        if (hdrw >= 11) {
            u8 tag[4];
            int fmt;
            rd_read(&r, tag, 4);
            fmt = rd_u16be(&r);
            if (!memcmp(tag, "XIMG", 4) && fmt == 0 && hdrw >= 11 + ncol * 3) {
                ximg = 1;
                for (i = 0; i < ncol; i++) {
                    int c, pix = pf->img_vdiorder ? vdi2pix(i, planes) : i;
                    for (c = 0; c < 3; c++) {
                        long v = rd_u16be(&r);
                        if (v > 1000) v = 1000;
                        im->pal[pix * 3 + c] = (u8)((v * 255 + 500) / 1000);
                    }
                }
                rd_skip(&r, (hdrw - 11 - ncol * 3) * 2L);
            } else
                rd_skip(&r, (hdrw - 11) * 2L);
        } else
            rd_skip(&r, (hdrw - 8) * 2L);
        if (!ximg) {
            if (planes == 1) {
                im->pal[0] = im->pal[1] = im->pal[2] = 255;
                im->pal[3] = im->pal[4] = im->pal[5] = 0;
            } else if (pf->img_syspal && d && !d->truecolor &&
                       (1 << d->planes) >= ncol) {
                memcpy(im->pal, d->sysrgb, ncol * 3);
            } else {
                for (i = 0; i < ncol; i++) {
                    int pix = vdi2pix(i < 16 ? i : i, planes);
                    if (i < 16)
                        memcpy(im->pal + pix * 3, vdi16 + i * 3, 3);
                    else
                        im->pal[pix * 3] = im->pal[pix * 3 + 1] =
                            im->pal[pix * 3 + 2] = (u8)i;
                }
            }
        }
    } else
        rd_skip(&r, (hdrw - 8) * 2L);

    sprintf(im->fmtinfo, "%s, %d plane%s", ximg ? "XIMG" : "GEM IMG",
            planes, planes > 1 ? "s" : "");

    bpl = tc ? w * (planes / 8) : (w + 7) / 8;      /* bytes per plane line */
    linebytes = tc ? bpl : bpl * planes;
    line = malloc(linebytes + 2);
    if (!line) {
        rd_close(&r);
        return E_NOMEM;
    }

    y = 0;
    while (y < h) {
        int rep = 1, p, ok = 1;
        /* vertical replication record: 00 00 FF n */
        if (rd_peek(&r, 0) < 0)
            break;
        if (rd_peek(&r, 0) == 0 && rd_peek(&r, 1) == 0) {
            rd_skip(&r, 2);
            if (rd_byte(&r) != 0xFF)
                break;
            rep = rd_byte(&r);
            if (rep < 0)
                break;
        }

        for (p = 0; p < (tc ? 1 : planes) && ok; p++)
            ok = img_line(&r, line + p * bpl, bpl, patlen) == 1;
        if (!ok)
            break;
        while (rep-- > 0 && y < h) {
            u8 *out = im->px + y * im->stride;
            if (tc) {
                int bpp = planes / 8;
                for (x = 0; x < w; x++) {
                    const u8 *s = line + x * bpp + (bpp == 4 ? 1 : 0);
                    out[x * 3] = s[0]; out[x * 3 + 1] = s[1]; out[x * 3 + 2] = s[2];
                }
            } else {
                memset(out, 0, w);
                for (p = 0; p < planes; p++) {
                    const u8 *pl = line + p * bpl;
                    u8 bit = 1 << p;
                    for (x = 0; x < w; x++)
                        if (pl[x >> 3] & (0x80 >> (x & 7)))
                            out[x] |= bit;
                }
            }
            y++;
        }
        if ((y & 7) == 0 && rd_cancelled(&r)) {
            free(line);
            rd_close(&r);
            return E_CANCEL;
        }
    }
    free(line);
    rd_close(&r);
    if (tc)
        img_reduce(im, RB_ORIG);
    return E_OK;
}
