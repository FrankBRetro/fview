/* dec_iff.c - IFF ILBM / PBM decoder.
 * Supports 1-8 bitplanes, EHB, HAM6, HAM8, 24/32-bit deep ILBM, ByteRun1,
 * Deluxe Paint ST "VDAT" vertical compression, mask plane and transparent
 * colour, Deluxe Paint PC "PBM" chunky files. */
#include <stdlib.h>
#include <string.h>
#include "rd.h"

#define CAMG_HAM 0x800
#define CAMG_EHB 0x80

static int unpack_row(RD *r, u8 *dst, long n, int comp)
{
    if (!comp)
        return rd_read(r, dst, n) == n;
    while (n > 0) {
        int c = rd_byte(r);
        if (c < 0)
            return 0;
        if (c < 128) {
            long k = c + 1;
            if (k > n) {           /* malformed: read what fits */
                rd_read(r, dst, n);
                rd_skip(r, k - n);
                return 1;
            }
            if (rd_read(r, dst, k) != k)
                return 0;
            dst += k; n -= k;
        } else if (c > 128) {
            long k = 257 - c;
            int v = rd_byte(r);
            if (v < 0)
                return 0;
            if (k > n) k = n;
            memset(dst, v, k);
            dst += k; n -= k;
        }
        /* 128 is a no-op */
    }
    return 1;
}

/* Deluxe Paint ST compression 2: BODY holds one VDAT chunk per bitplane.
 * Each chunk is a word count of command bytes (+2), the command bytes, then
 * data words. Words fill a 16 pixel column top to bottom, then the next
 * column. Commands: 0 = literal run (count word), 1 = repeat (count word),
 * 2..127 = repeat next word n times, 128..255 = 256-n literal words.
 * Output is ILBM interleaved rows of linelen bytes, plane p at p*rowb.  */
static void vdat_plane(const u8 *c, long len, u8 *out, long rowb, long linelen,
                       long h, int plane)
{
    long ncmd, i, pos, col = 0, y = 0, ncol = rowb / 2;
    const u8 *end = c + len;
    u8 *base = out + rowb * plane;
    if (len < 2)
        return;
    ncmd = ((c[0] << 8) | c[1]) - 2;
    if (ncmd < 1 || ncmd > len - 2)
        return;
    pos = 2 + ncmd;
#define VPUT(hi, lo) do { if (col < ncol) { u8 *d = base + y * linelen + col * 2; \
        d[0] = (hi); d[1] = (lo); if (++y >= h) { y = 0; col++; } } } while (0)
    for (i = 0; i < ncmd && col < ncol; i++) {
        int cmd = c[2 + i];
        long k, n;
        if (c + pos + 2 > end)
            break;
        if (cmd == 0 || cmd == 1) {
            n = (c[pos] << 8) | c[pos + 1];
            pos += 2;
        } else
            n = cmd >= 128 ? 256 - cmd : cmd;
        if (cmd == 0 || cmd >= 128) {               /* literal words */
            for (k = 0; k < n && c + pos + 2 <= end; k++, pos += 2)
                VPUT(c[pos], c[pos + 1]);
        } else {                                    /* repeated word */
            u8 hi, lo;
            if (c + pos + 2 > end)
                break;
            hi = c[pos]; lo = c[pos + 1];
            pos += 2;
            for (k = 0; k < n && col < ncol; k++)
                VPUT(hi, lo);
        }
    }
#undef VPUT
}

static u8 *vdat_body(RD *r, long bodylen, long rowb, long linelen, long h, int planes)
{
    u8 *buf = calloc(1, linelen * h + 1), *ck;
    long got = 0;
    int plane = 0;
    if (!buf)
        return 0;
    while (got + 8 <= bodylen && plane < planes) {
        u8 id[4];
        long len;
        if (rd_read(r, id, 4) != 4)
            break;
        len = rd_u32be(r);
        got += 8;
        if (len < 0 || len > bodylen - got)
            break;
        if (memcmp(id, "VDAT", 4)) {
            rd_skip(r, len + (len & 1));
        } else {
            ck = malloc(len + 1);
            if (!ck)
                break;
            if (rd_read(r, ck, len) == len)
                vdat_plane(ck, len, buf, rowb, linelen, h, plane);
            free(ck);
            rd_skip(r, len & 1);
            plane++;
        }
        got += len + (len & 1);
        if (rd_cancelled(r))
            break;
    }
    return buf;
}

int dec_iff(const char *path, IMAGE *im, const PREFS *pf, PROG *pg)
{
    RD r;
    int e, pbm = 0, planes = 0, masking = 0, comp = 0, transcol = -1;
    long w = 0, h = 0, camg = 0, formlen, ncmap = 0, bodylen = 0;
    u8 cmap[256 * 3];
    int have_bmhd = 0, xasp = 0, yasp = 0, have_camg = 0;
    long pageh = 0;

    e = rd_open(&r, path, pg);
    if (e)
        return e;
    {
        u8 hd[12];
        if (rd_read(&r, hd, 12) != 12 || memcmp(hd, "FORM", 4)) {
            rd_close(&r);
            return E_FORMAT;
        }
        formlen = ((long)hd[4] << 24) | ((long)hd[5] << 16) | (hd[6] << 8) | hd[7];
        (void)formlen;
        if (!memcmp(hd + 8, "PBM ", 4))
            pbm = 1;
        else if (memcmp(hd + 8, "ILBM", 4)) {
            rd_close(&r);
            return E_UNSUP;
        }
    }

    for (;;) {
        u8 id[4];
        long len;
        if (rd_read(&r, id, 4) != 4)
            break;
        len = rd_u32be(&r);
        if (len < 0)
            break;
        if (!memcmp(id, "BMHD", 4)) {
            u8 b[20];
            rd_read(&r, b, 20);
            rd_skip(&r, len - 20 + (len & 1));
            w = (b[0] << 8) | b[1];
            h = (b[2] << 8) | b[3];
            planes = b[8];
            masking = b[9];
            comp = b[10];
            transcol = (b[12] << 8) | b[13];
            xasp = b[14];
            yasp = b[15];
            pageh = (b[18] << 8) | b[19];
            have_bmhd = 1;
        } else if (!memcmp(id, "CMAP", 4)) {
            long n = len > 768 ? 768 : len;
            rd_read(&r, cmap, n);
            rd_skip(&r, len - n + (len & 1));
            ncmap = n / 3;
        } else if (!memcmp(id, "CAMG", 4)) {
            camg = rd_u32be(&r);
            have_camg = 1;
            rd_skip(&r, len - 4 + (len & 1));
        } else if (!memcmp(id, "BODY", 4)) {
            bodylen = len;
            break;
        } else
            rd_skip(&r, len + (len & 1));
    }
    if (!have_bmhd || w <= 0 || h <= 0 || comp > 2 || (comp == 2 && pbm)) {
        rd_close(&r);
        if (comp > 1)
            sprintf(fv_errdetail, "IFF compression type %d", comp);
        return comp > 1 ? E_UNSUP : E_FORMAT;
    }

    /* old OCS palettes store 4 bit values in the high nibble */
    {
        int i, low = 0;
        for (i = 0; i < ncmap * 3; i++)
            low |= cmap[i] & 0x0F;
        if (!low)
            for (i = 0; i < ncmap * 3; i++)
                cmap[i] |= cmap[i] >> 4;
    }

    {
        int ham = pf->iff_ham && (camg & CAMG_HAM) && (planes == 6 || planes == 8) && !pbm;
        int ehb = pf->iff_ham && (camg & CAMG_EHB) && planes == 6 && !ham && !pbm;
        int deep = !pbm && planes > 8;
        long rowb = ((w + 15) / 16) * 2, y, x;
        long linelen = pbm ? (w + 1) & ~1L : rowb * (planes + (masking == 1));
        u8 *line = malloc(linelen + 16), *vbuf = 0;
        int bv = bg_value(pf);

        if (!line) {
            rd_close(&r);
            return E_NOMEM;
        }
        im->w = w; im->h = h;
        im->type = (ham || deep) ? IT_RGB : IT_INDEXED;
        e = img_alloc_px(im, RB_ORIG);
        if (e) {
            free(line);
            rd_close(&r);
            return e;
        }
        im->orig_bits = deep ? 24 : ham ? 24 : (ehb ? 6 : planes);
        /* pixel shape. The display mode in CAMG says how the picture was
           shown. A lores pixel on a 4:3 monitor is 10:11 on NTSC (200
           lines) but 16:15 on PAL (256 lines fill the same height, so
           PAL pixels are a little wider than tall); hires halves the
           width, superhires quarters it, interlace halves the height.
           PAL or NTSC comes from the monitor ID in CAMG, or else from the
           height of the picture. Other monitors (VGA, Multiscan, A2024
           ...) and pictures without CAMG use the BMHD aspect fields.    */
        {
            long mon = camg & 0xFFFF1000L;
            int lace = (camg & 0x0004) != 0;
            long tall = pageh > h ? pageh : h;
            int pal = -1;
            if (mon == 0x00021000L || mon == 0x000A1000L)       /* PAL, DblPAL   */
                pal = 1;
            else if (mon == 0x00011000L || mon == 0x00091000L)  /* NTSC, DblNTSC */
                pal = 0;
            else if (!(camg & 0xFFFF0000L))                     /* default monitor */
                pal = tall > (lace ? 400 : 200);
            if (have_camg && !pbm && pal >= 0) {
                long ax = pal ? 64 : 40, ay = pal ? 60 : 44, a, b, t;
                if (camg & 0x0020) ax /= 4;                     /* SUPERHIRES */
                else if (camg & 0x8000) ax /= 2;                /* HIRES      */
                if (lace) ay /= 2;                              /* LACE       */
                for (a = ax, b = ay; b; t = a % b, a = b, b = t)
                    ;
                im->aspx = (int)(ax / a);
                im->aspy = (int)(ay / a);
            } else if (xasp > 0 && yasp > 0 && xasp <= yasp * 4 && yasp <= xasp * 4) {
                im->aspx = xasp;
                im->aspy = yasp;
            }
        }
        if (deep && planes != 24 && planes != 32) {
            free(line);
            rd_close(&r);
            return E_UNSUP;
        }

        if (!deep && !ham) {
            int ncol = pbm ? 256 : 1 << planes, i;
            if (ncol > 256) ncol = 256;
            memset(im->pal, 0, sizeof im->pal);
            for (i = 0; i < ncol && i < ncmap; i++)
                memcpy(im->pal + i * 3, cmap + i * 3, 3);
            if (ncmap == 0)                 /* no palette: grey ramp */
                for (i = 0; i < ncol; i++)
                    im->pal[i * 3] = im->pal[i * 3 + 1] = im->pal[i * 3 + 2] =
                        (u8)(i * 255 / (ncol - 1 ? ncol - 1 : 1));
            if (ehb)
                for (i = 0; i < 32; i++) {
                    im->pal[(i + 32) * 3]     = im->pal[i * 3] >> 1;
                    im->pal[(i + 32) * 3 + 1] = im->pal[i * 3 + 1] >> 1;
                    im->pal[(i + 32) * 3 + 2] = im->pal[i * 3 + 2] >> 1;
                }
            im->ncolors = ncol;
            if (pf->iff_mask && masking == 2 && transcol < ncol) {
                im->pal[transcol * 3] = im->pal[transcol * 3 + 1] =
                    im->pal[transcol * 3 + 2] = (u8)bv;
                im->trans = transcol;
            }
        }
        sprintf(im->fmtinfo, "IFF %s, %d planes%s%s", pbm ? "PBM" : "ILBM",
                planes, ham ? ", HAM" : "", ehb ? ", EHB" : "");
        if (comp == 2) {                    /* Deluxe Paint ST: unpack it all */
            if (h > 0x7FFFFFL / linelen ||
                !(vbuf = vdat_body(&r, bodylen, rowb, linelen, h, planes + (masking == 1)))) {
                free(line);
                rd_close(&r);
                return E_NOMEM;
            }
            strcat(im->fmtinfo, ", DPaint ST");
        }

        for (y = 0; y < h; y++) {
            u8 *out = im->px + y * im->stride;
            if (vbuf)
                memcpy(line, vbuf + y * linelen, linelen);
            else if (!unpack_row(&r, line, linelen, comp))
                break;                       /* show what we have */
            if (pbm) {
                memcpy(out, line, w);
            } else if (deep) {
                int ch, b;
                memset(out, 0, w * 3);
                for (ch = 0; ch < 3; ch++)
                    for (b = 0; b < 8; b++) {
                        const u8 *pl = line + rowb * (ch * 8 + b);
                        for (x = 0; x < w; x++)
                            if (pl[x >> 3] & (0x80 >> (x & 7)))
                                out[x * 3 + ch] |= 1 << b;
                    }
            } else {
                /* planar -> chunky */
                u8 tmp[4096];
                u8 *pix = w <= 4096 ? tmp : out;
                int p;
                if (ham && w > 4096) {
                    free(line);
                    free(vbuf);
                    rd_close(&r);
                    return E_UNSUP;
                }
                memset(pix, 0, w);
                for (p = 0; p < planes; p++) {
                    const u8 *pl = line + rowb * p;
                    u8 bit = 1 << p;
                    for (x = 0; x < w; x++)
                        if (pl[x >> 3] & (0x80 >> (x & 7)))
                            pix[x] |= bit;
                }
                if (masking == 1 && pf->iff_mask) {
                    const u8 *ml = line + rowb * planes;
                    for (x = 0; x < w; x++)
                        if (!(ml[x >> 3] & (0x80 >> (x & 7))))
                            pix[x] = 0xFF;       /* marker, fixed below */
                }
                if (ham) {
                    int sh = planes == 6 ? 4 : 6, r0 = 0, g0 = 0, b0 = 0;
                    int pmask = (1 << sh) - 1;
                    for (x = 0; x < w; x++) {
                        int v = pix[x], ctl = v >> sh, d = v & pmask;
                        int dv = sh == 4 ? (d << 4) | d : (d << 2) | (d >> 4);
                        if (v == 0xFF && masking == 1 && pf->iff_mask) {
                            out[x * 3] = out[x * 3 + 1] = out[x * 3 + 2] = (u8)bv;
                            continue;
                        }
                        switch (ctl) {
                        case 0:
                            if (d < ncmap) {
                                r0 = cmap[d * 3]; g0 = cmap[d * 3 + 1]; b0 = cmap[d * 3 + 2];
                            }
                            break;
                        case 1: b0 = dv; break;
                        case 2: r0 = dv; break;
                        case 3: g0 = dv; break;
                        }
                        out[x * 3] = r0; out[x * 3 + 1] = g0; out[x * 3 + 2] = b0;
                    }
                } else {
                    if (pix != out)
                        memcpy(out, pix, w);
                    if (masking == 1 && pf->iff_mask) {
                        /* need a palette slot for the background */
                        int slot = im->ncolors < 256 ? im->ncolors : 255;
                        for (x = 0; x < w; x++)
                            if (out[x] == 0xFF && planes < 8)
                                out[x] = slot;
                        if (planes < 8) {
                            im->pal[slot * 3] = im->pal[slot * 3 + 1] =
                                im->pal[slot * 3 + 2] = (u8)bv;
                            if (im->ncolors <= slot)
                                im->ncolors = slot + 1;
                            im->trans = slot;
                        }
                    }
                }
            }
            if ((y & 7) == 0 && rd_cancelled(&r)) {
                free(line);
                free(vbuf);
                rd_close(&r);
                return E_CANCEL;
            }
        }
        free(line);
        free(vbuf);
    }
    rd_close(&r);
    if (im->type == IT_RGB)
        img_reduce(im, RB_ORIG);
    return E_OK;
}
