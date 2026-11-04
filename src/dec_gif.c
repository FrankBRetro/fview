/* dec_gif.c - GIF 87a/89a decoder (first frame, composited on the
 * logical screen, interlace and transparency supported). */
#include <stdlib.h>
#include <string.h>
#include "rd.h"

typedef struct {
    RD  *r;
    int  blen;           /* bytes left in current sub-block */
    u32  bits;
    int  nbits;
    int  end;
} GBITS;

static int gb_byte(GBITS *g)
{
    if (g->end)
        return -1;
    while (g->blen == 0) {
        int n = rd_byte(g->r);
        if (n <= 0) {
            g->end = 1;
            return -1;
        }
        g->blen = n;
    }
    g->blen--;
    return rd_byte(g->r);
}

static int gb_code(GBITS *g, int size)
{
    int c;
    while (g->nbits < size) {
        int b = gb_byte(g);
        if (b < 0)
            return -1;
        g->bits |= (u32)b << g->nbits;
        g->nbits += 8;
    }
    c = g->bits & ((1 << size) - 1);
    g->bits >>= size;
    g->nbits -= size;
    return c;
}

int dec_gif(const char *path, IMAGE *im, const PREFS *pf, PROG *pg)
{
    RD r;
    int e, flags, gctsize = 0, bgidx, trans = -1, ver89;
    u8 gct[256 * 3], lct[256 * 3], *pal;
    long sw, sh;

    e = rd_open(&r, path, pg);
    if (e)
        return e;
    {
        u8 h[13];
        if (rd_read(&r, h, 13) != 13 || memcmp(h, "GIF8", 4)) {
            rd_close(&r);
            return E_FORMAT;
        }
        ver89 = h[4] == '9';
        sw = h[6] | (h[7] << 8);
        sh = h[8] | (h[9] << 8);
        flags = h[10];
        bgidx = h[11];
        if (flags & 0x80) {
            gctsize = 2 << (flags & 7);
            rd_read(&r, gct, gctsize * 3);
        }
    }

    for (;;) {
        int c = rd_byte(&r);
        if (c < 0 || c == 0x3B) {
            rd_close(&r);
            return E_FORMAT;          /* no image found */
        }
        if (c == 0x21) {              /* extension */
            int label = rd_byte(&r), n;
            if (label == 0xF9) {      /* graphic control */
                u8 gce[6];
                rd_read(&r, gce, 6);
                if ((gce[1] & 1) && pf->gif_trans)
                    trans = gce[4];
                continue;             /* gce[5] is the terminator */
            }
            while ((n = rd_byte(&r)) > 0)
                rd_skip(&r, n);
            continue;
        }
        if (c != 0x2C)
            continue;                 /* tolerate junk */

        /* image descriptor */
        {
            u8 d[9];
            long ix, iy, iw, ih, x, y, npix;
            int ifl, ncol, mincode, inter, pass = 0;
            static const int pstart[4] = { 0, 4, 2, 1 }, pstep[4] = { 8, 8, 4, 2 };
            /* LZW tables */
            u16 *prefix;
            u8 *suffix, *stack;
            GBITS gb;
            int clear, eoi, avail, codesize, codemask, old = -1, first = 0, sp = 0;

            if (rd_read(&r, d, 9) != 9) {
                rd_close(&r);
                return E_FORMAT;
            }
            ix = d[0] | (d[1] << 8); iy = d[2] | (d[3] << 8);
            iw = d[4] | (d[5] << 8); ih = d[6] | (d[7] << 8);
            ifl = d[8];
            inter = (ifl & 0x40) != 0;
            if (ifl & 0x80) {
                ncol = 2 << (ifl & 7);
                rd_read(&r, lct, ncol * 3);
                pal = lct;
            } else {
                ncol = gctsize;
                pal = gct;
                if (!ncol) {          /* no palette: grey ramp */
                    ncol = 256;
                    for (x = 0; x < 256; x++)
                        gct[x * 3] = gct[x * 3 + 1] = gct[x * 3 + 2] = (u8)x;
                }
            }
            if (sw < ix + iw) sw = ix + iw;
            if (sh < iy + ih) sh = iy + ih;

            im->w = sw; im->h = sh;
            im->type = IT_INDEXED;
            e = img_alloc_px(im, RB_ORIG);
            if (e) {
                rd_close(&r);
                return e;
            }
            memcpy(im->pal, pal, ncol * 3);
            im->ncolors = ncol;
            {   /* bits from the palette size actually used */
                int b = 1;
                while ((1 << b) < ncol) b++;
                im->orig_bits = b;
            }
            /* background: transparent areas get the preference colour */
            if (trans >= 0 && trans < ncol) {
                int bv = bg_value(pf);
                im->pal[trans * 3] = im->pal[trans * 3 + 1] = im->pal[trans * 3 + 2] = (u8)bv;
                im->trans = trans;
                memset(im->px, trans, im->stride * im->h);
            } else
                memset(im->px, bgidx < ncol ? bgidx : 0, im->stride * im->h);
            sprintf(im->fmtinfo, "GIF %s, %d colours%s", ver89 ? "89a" : "87a",
                    ncol, inter ? ", interlaced" : "");

            mincode = rd_byte(&r);
            if (mincode < 2 || mincode > 11) {
                rd_close(&r);
                return E_FORMAT;
            }
            prefix = malloc(4096 * sizeof(u16));
            suffix = malloc(4096);
            stack = malloc(4097);
            if (!prefix || !suffix || !stack) {
                free(prefix); free(suffix); free(stack);
                rd_close(&r);
                return E_NOMEM;
            }
            memset(&gb, 0, sizeof gb);
            gb.r = &r;
            clear = 1 << mincode;
            eoi = clear + 1;
            avail = clear + 2;
            codesize = mincode + 1;
            codemask = (1 << codesize) - 1;
            for (x = 0; x < clear; x++) {
                prefix[x] = 0;
                suffix[x] = (u8)x;
            }

            x = 0; y = 0; npix = iw * ih;
            while (npix > 0) {
                int code = gb_code(&gb, codesize), in;
                if (code < 0 || code == eoi)
                    break;
                if (code == clear) {
                    codesize = mincode + 1;
                    codemask = (1 << codesize) - 1;
                    avail = clear + 2;
                    old = -1;
                    continue;
                }
                if (old == -1) {
                    if (code >= clear)
                        break;
                    stack[sp++] = suffix[code];
                    old = code;
                    first = code;
                } else {
                    in = code;
                    if (code >= avail) {        /* KwKwK case */
                        if (code > avail)
                            break;
                        stack[sp++] = (u8)first;
                        code = old;
                    }
                    while (code >= clear) {
                        if (sp >= 4096)
                            break;
                        stack[sp++] = suffix[code];
                        code = prefix[code];
                    }
                    first = suffix[code];
                    stack[sp++] = (u8)first;
                    if (avail < 4096) {
                        prefix[avail] = old;
                        suffix[avail] = (u8)first;
                        avail++;
                        if ((avail & codemask) == 0 && avail < 4096) {
                            codesize++;
                            codemask = (1 << codesize) - 1;
                        }
                    }
                    old = in;
                }
                /* output stack */
                while (sp > 0 && npix > 0) {
                    u8 v = stack[--sp];
                    long px = ix + x, py = iy + y;
                    if (!(trans >= 0 && v == trans) && px < sw && py < sh)
                        im->px[py * im->stride + px] = v;
                    npix--;
                    if (++x >= iw) {
                        x = 0;
                        if (inter) {
                            y += pstep[pass];
                            while (y >= ih && pass < 3) {
                                pass++;
                                y = pstart[pass];
                            }
                        } else
                            y++;
                        if ((y & 15) == 0 && rd_cancelled(&r)) {
                            free(prefix); free(suffix); free(stack);
                            rd_close(&r);
                            return E_CANCEL;
                        }
                    }
                }
                sp = 0;
            }
            free(prefix); free(suffix); free(stack);
            rd_close(&r);
            return E_OK;    /* a truncated image still shows what we got */
        }
    }
}
