/*
 * dither.c - colour quantisation, scaling and dithering.
 *
 * Pipeline (streaming, one output row at a time, so memory use is tiny):
 *
 *   source row(s) --> scale (box filter when shrinking, nearest when
 *   enlarging) --> error diffusion / ordered dither against the target
 *   palette --> pixel values (palette displays) or packed pixels.
 *
 * Palette choice for palette displays:
 *   - PAL_SYSTEM: dither onto the current system palette (no palette
 *     switching at all).
 *   - PAL_OPTIMISE: median cut on a 15-bit histogram picks the free
 *     colours; reserved GEM colours stay as they are and are still used
 *     by the ditherer. Images that already fit get their exact colours.
 *
 * Dithering defaults to serpentine Floyd-Steinberg in integer arithmetic,
 * which gives the best quality per CPU cycle on a 68000. Atkinson (good on
 * mono), ordered 8x8 Bayer (fastest) and none are available as options.
 * Nearest-colour searches are cached in a lazily filled 32K table.
 */
#include <stdlib.h>
#include <string.h>
#include "fview.h"

struct DITHER {
    const IMAGE *im;
    const DISPINFO *d;
    long tw, th;
    int  method, serp, smooth;
    int  npal;                 /* number of usable target colours          */
    u8   prgb[256 * 3];        /* target colours                           */
    u8   ppix[256];            /* pixel value for each target colour       */
    u8  *cache;                /* 32K: 15-bit colour -> target index       */
    u8  *cvalid;               /* 4K bitset                                */
    short *err0, *err1, *err2; /* error rows (w+4)*3                       */
    u8  *row;                  /* scaled RGB row                           */
    u8  *srow;                 /* source RGB row                           */
    u32 *acc;                  /* box filter accumulators (tw*3)           */
    u16 *cnt;
    long *xmap;                /* scale: target x -> source x start        */
    u32 lut[3][256];           /* true colour packing                      */
    int exact;                 /* every source colour is in the palette    */
    u8  exmap[256];            /* exact: source index -> target index      */
    long lasty;
};

/* ---------------------------------------------------------- median cut */

typedef struct { u8 lo[3], hi[3]; long n; } BOX;

static long box_count(const u32 *hist, BOX *b)
{
    int r, g, bl;
    long n = 0;
    u8 lo[3] = { 31, 31, 31 }, hi[3] = { 0, 0, 0 };
    for (r = b->lo[0]; r <= b->hi[0]; r++)
        for (g = b->lo[1]; g <= b->hi[1]; g++)
            for (bl = b->lo[2]; bl <= b->hi[2]; bl++) {
                u32 c = hist[(r << 10) | (g << 5) | bl];
                if (c) {
                    n += c;
                    if (r < lo[0]) lo[0] = r;
                    if (r > hi[0]) hi[0] = r;
                    if (g < lo[1]) lo[1] = g;
                    if (g > hi[1]) hi[1] = g;
                    if (bl < lo[2]) lo[2] = bl;
                    if (bl > hi[2]) hi[2] = bl;
                }
            }
    if (n) {
        memcpy(b->lo, lo, 3);
        memcpy(b->hi, hi, 3);
    }
    b->n = n;
    return n;
}

static int median_cut(const u32 *hist, int want, u8 *out, PROG *pg)
{
    BOX *bx = malloc(sizeof(BOX) * (want + 1));
    int nb = 1, i, c;
    if (!bx)
        return 0;
    bx[0].lo[0] = bx[0].lo[1] = bx[0].lo[2] = 0;
    bx[0].hi[0] = bx[0].hi[1] = bx[0].hi[2] = 31;
    if (!box_count(hist, &bx[0])) {
        free(bx);
        return 0;
    }
    while (nb < want) {
        /* split the box with the largest population * extent, weighting
           green over red over blue the way the eye does                  */
        long best = -1, score;
        int bi = -1, axis = 0, len, l2;
        for (i = 0; i < nb; i++) {
            int dr = (bx[i].hi[0] - bx[i].lo[0]) * 3;
            int dg = (bx[i].hi[1] - bx[i].lo[1]) * 4;
            int db = (bx[i].hi[2] - bx[i].lo[2]) * 2;
            len = dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
            if (len == 0)
                continue;
            score = bx[i].n * len;
            if (score > best)
                best = score, bi = i;
        }
        if (bi < 0)
            break;
        {
            BOX *b = &bx[bi];
            int dr = (b->hi[0] - b->lo[0]) * 3, dg = (b->hi[1] - b->lo[1]) * 4,
                db = (b->hi[2] - b->lo[2]) * 2;
            long half, run = 0, sl[32];
            int v, cut;
            axis = (dg >= dr && dg >= db) ? 1 : (dr >= db ? 0 : 2);
            /* histogram along the axis */
            memset(sl, 0, sizeof sl);
            {
                int r, g, bl;
                for (r = b->lo[0]; r <= b->hi[0]; r++)
                    for (g = b->lo[1]; g <= b->hi[1]; g++)
                        for (bl = b->lo[2]; bl <= b->hi[2]; bl++) {
                            u32 n = hist[(r << 10) | (g << 5) | bl];
                            v = axis == 0 ? r : axis == 1 ? g : bl;
                            sl[v] += n;
                        }
            }
            half = b->n / 2;
            cut = b->lo[axis];
            for (v = b->lo[axis]; v < b->hi[axis]; v++) {
                run += sl[v];
                cut = v;
                if (run >= half)
                    break;
            }
            bx[nb] = *b;
            b->hi[axis] = cut;
            bx[nb].lo[axis] = cut + 1;
            l2 = box_count(hist, b) != 0;
            if (box_count(hist, &bx[nb]) && l2)
                nb++;
            else if (!l2) {             /* keep the non-empty half */
                *b = bx[nb];
            }
        }
        if (prog_step(pg, nb, want)) {
            free(bx);
            return -1;
        }
    }
    /* average colour of each box */
    for (i = 0; i < nb; i++) {
        double s[3] = { 0, 0, 0 };
        long n = 0;
        int r, g, bl;
        for (r = bx[i].lo[0]; r <= bx[i].hi[0]; r++)
            for (g = bx[i].lo[1]; g <= bx[i].hi[1]; g++)
                for (bl = bx[i].lo[2]; bl <= bx[i].hi[2]; bl++) {
                    u32 h = hist[(r << 10) | (g << 5) | bl];
                    if (h) {
                        s[0] += (double)h * r;
                        s[1] += (double)h * g;
                        s[2] += (double)h * bl;
                        n += h;
                    }
                }
        for (c = 0; c < 3; c++) {
            int v = n ? (int)(s[c] / n * 255.0 / 31.0 + 0.5) : 0;
            out[i * 3 + c] = v > 255 ? 255 : v;
        }
    }
    free(bx);
    return nb;
}

/* ---------------------------------------------------- nearest colour */

static int nearest(DITHER *dt, int r, int g, int b)
{
    unsigned key = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
    int i, best = 0;
    long bd = 0x7FFFFFFFL;
    if (dt->cvalid[key >> 3] & (1 << (key & 7)))
        return dt->cache[key];
    /* search with the cell centre so the cache is order independent */
    r = (r & 0xF8) | 4; g = (g & 0xF8) | 4; b = (b & 0xF8) | 4;
    for (i = 0; i < dt->npal; i++) {
        const u8 *p = dt->prgb + i * 3;
        long d = (long)(p[1] - g) * (p[1] - g) * 4;
        if (d >= bd)
            continue;
        d += (long)(p[0] - r) * (p[0] - r) * 3;
        if (d >= bd)
            continue;
        d += (long)(p[2] - b) * (p[2] - b) * 2;
        if (d < bd)
            bd = d, best = i;
    }
    dt->cache[key] = best;
    dt->cvalid[key >> 3] |= 1 << (key & 7);
    return best;
}

/* -------------------------------------------------------------- setup */

static void add_hist(u32 *hist, const u8 *rgb, long n)
{
    while (n--) {
        hist[((rgb[0] >> 3) << 10) | ((rgb[1] >> 3) << 5) | (rgb[2] >> 3)]++;
        rgb += 3;
    }
}

/* 15-bit histogram of a picture, every other pixel on big ones; 0 on
   failure or cancel                                                   */
static u32 *make_hist(const IMAGE *im, PROG *pg)
{
    u32 *hist = calloc(32768L, sizeof(u32));
    long x, y, step;
    if (!hist)
        return 0;
    step = (im->w * im->h > 200000L) ? 2 : 1;
    for (y = 0; y < im->h; y += step) {
        if (im->type == IT_INDEXED) {
            const u8 *s = im->px + y * im->stride;
            for (x = 0; x < im->w; x += step) {
                const u8 *c = im->pal + s[x] * 3;
                hist[((c[0] >> 3) << 10) | ((c[1] >> 3) << 5) | (c[2] >> 3)]++;
            }
        } else if (step == 1)
            add_hist(hist, im->px + y * im->stride, im->w);
        else {
            const u8 *s = im->px + y * im->stride;
            for (x = 0; x < im->w; x += 2, s += 6)
                add_hist(hist, s, 1);
        }
        if ((y & 31) == 0 && prog_step(pg, y, im->h * 2)) {
            free(hist);
            return 0;
        }
    }
    return hist;
}

/* ------------------------------------------------- UI pens in a picture */

static int luma(const u8 *c) { return (77 * c[0] + 150 * c[1] + 29 * c[2]) >> 8; }

/* the UI pens (d->ui_pix: pens 0, 1, then the 3D greys 8 and 9) that a
   picture's palette may take                                          */
static int ui_free(const DISPINFO *d, int j, int ncol)
{
    return j < d->ui_n && d->ui_pix[j] < ncol && !d->reserved[d->ui_pix[j]];
}

int ui_needs_bw(const DISPINFO *d, const u8 *cols, int got)
{
    int k, lo = 255, hi = 0;
    int ncol = d->planes >= 8 ? 256 : 1 << d->planes;
    if (d->truecolor || !ui_free(d, 0, ncol) || !ui_free(d, 1, ncol))
        return 0;
    for (k = 0; k < got; k++) {
        int l = luma(cols + k * 3);
        if (l < lo) lo = l;
        if (l > hi) hi = l;
    }
    return hi - lo < UI_CONTRAST;
}

void ui_arrange(const DISPINFO *d, int ncol, const u8 *cols, int got, int *slot)
{
    static const u8 grey[2][3] = { { 176, 176, 176 }, { 96, 96, 96 } };
    u8 taken[256];
    int j, k, i;
    memset(taken, 0, sizeof taken);
    for (k = 0; k < got; k++)
        slot[k] = -1;
    for (j = 0; j < 4; j++) {
        int best = -1;
        long bd = 0x7fffffffL;
        if (!ui_free(d, j, ncol))
            continue;
        for (k = 0; k < got; k++) {
            const u8 *c = cols + k * 3;
            long dd;
            if (slot[k] >= 0)
                continue;
            if (j == 0)
                dd = 255 - luma(c);            /* pen 0: the lightest */
            else if (j == 1)
                dd = luma(c);                  /* pen 1: the darkest  */
            else {
                long dr = c[0] - grey[j - 2][0], dg = c[1] - grey[j - 2][1],
                     db = c[2] - grey[j - 2][2];
                dd = dr * dr + dg * dg + db * db;
            }
            if (dd < bd) {
                bd = dd;
                best = k;
            }
        }
        if (best < 0)
            break;
        slot[best] = d->ui_pix[j];
        taken[d->ui_pix[j]] = 1;
    }
    for (k = 0, i = 0; k < got; k++) {
        if (slot[k] >= 0)
            continue;
        while (i < ncol && (d->reserved[i] || taken[i]))
            i++;
        if (i >= ncol)
            break;
        slot[k] = i++;
    }
}

DITHER *dith_new(const IMAGE *im, long tw, long th, const DISPINFO *d,
                 int method, int serp, int smooth, u8 *pal_out, PROG *pg)
{
    DITHER *dt = calloc(1, sizeof(DITHER));
    int ncol, i, nfree;
    long x;

    if (!dt)
        return 0;
    dt->im = im; dt->d = d;
    dt->tw = tw; dt->th = th;
    dt->method = method; dt->serp = serp; dt->smooth = smooth;
    dt->lasty = -1;

    dt->row  = malloc(tw * 3 + 6);
    dt->srow = malloc(im->w * 3 + 6);
    dt->xmap = malloc((tw + 1) * sizeof(long));
    if (!dt->row || !dt->srow || !dt->xmap)
        goto fail;
    for (x = 0; x <= tw; x++)
        dt->xmap[x] = (long)((double)x * im->w / tw);
    if (smooth && (tw < im->w || th < im->h)) {
        dt->acc = malloc(tw * 3 * sizeof(u32));
        dt->cnt = malloc(tw * sizeof(u16));
        if (!dt->acc || !dt->cnt)
            goto fail;
    } else
        dt->smooth = 0;

    if (d->truecolor) {
        disp_make_tc_luts(d, dt->lut);
        return dt;
    }

    ncol = d->planes >= 8 ? 256 : 1 << d->planes;
    dt->cache = malloc(32768L);
    dt->cvalid = calloc(4096, 1);
    dt->err0 = calloc((tw + 4) * 3, sizeof(short));
    dt->err1 = calloc((tw + 4) * 3, sizeof(short));
    dt->err2 = calloc((tw + 4) * 3, sizeof(short));
    if (!dt->cache || !dt->cvalid || !dt->err0 || !dt->err1 || !dt->err2)
        goto fail;

    /* start with the reserved (or all, in system mode) colours */
    memcpy(pal_out, d->sysrgb, 256 * 3);
    dt->npal = 0;
    nfree = 0;
    for (i = 0; i < ncol; i++) {
        if (d->palmode == PAL_SYSTEM || d->fixed_palette || d->reserved[i]) {
            memcpy(dt->prgb + dt->npal * 3, d->sysrgb + i * 3, 3);
            dt->ppix[dt->npal++] = i;
        } else
            nfree++;
    }

    if (nfree) {
        u8 newc[256 * 3];
        int got = 0, k, slot[256];

        if (im->type == IT_INDEXED && im->ncolors <= nfree) {
            /* exact colours fit */
            memcpy(newc, im->pal, im->ncolors * 3);
            got = im->ncolors;
            dt->exact = 1;
        } else {
            u32 *hist = make_hist(im, pg);
            if (!hist)
                goto fail;
            got = median_cut(hist, nfree, newc, 0);
            free(hist);
            if (got < 0)
                goto fail;
        }
        /* the picture may take the UI pens: if it has no light and dark
           colour for them, make room for white and black               */
        if (ui_needs_bw(d, newc, got)) {
            if (got > nfree - 2) {
                u32 *hist;
                dt->exact = 0;
                got = 0;
                if (nfree > 2) {
                    if (!(hist = make_hist(im, pg)))
                        goto fail;
                    got = median_cut(hist, nfree - 2, newc, 0);
                    free(hist);
                    if (got < 0)
                        goto fail;
                }
            }
            memset(newc + got * 3, 255, 3);
            memset(newc + got * 3 + 3, 0, 3);
            got += 2;
        }
        /* give the new colours free pixel values, the UI pens first */
        ui_arrange(d, ncol, newc, got, slot);
        for (k = 0; k < got; k++) {
            i = slot[k];
            if (i < 0)
                continue;
            memcpy(pal_out + i * 3, newc + k * 3, 3);
            if (dt->exact)
                dt->exmap[k] = dt->npal;
            memcpy(dt->prgb + dt->npal * 3, newc + k * 3, 3);
            dt->ppix[dt->npal++] = i;
        }
    }
    if (dt->exact && (tw != im->w || th != im->h) && dt->smooth)
        dt->exact = 0;           /* box filter creates new colours */
    memset(dt->cache, 0, 32768L);
    return dt;

fail:
    dith_free(dt);
    return 0;
}

void dith_free(DITHER *dt)
{
    if (!dt)
        return;
    free(dt->row); free(dt->srow); free(dt->xmap);
    free(dt->acc); free(dt->cnt);
    free(dt->cache); free(dt->cvalid);
    free(dt->err0); free(dt->err1); free(dt->err2);
    free(dt);
}

/* ------------------------------------------------------- scaled row */

static void scaled_row(DITHER *dt, long y, u8 *out)
{
    const IMAGE *im = dt->im;
    long x, tw = dt->tw;

    if (!dt->smooth) {
        long sy = (long)((double)y * im->h / dt->th);
        if (sy >= im->h) sy = im->h - 1;
        if (tw == im->w) {
            img_get_rgb_row(im, sy, out);
            return;
        }
        img_get_rgb_row(im, sy, dt->srow);
        for (x = 0; x < tw; x++) {
            const u8 *s = dt->srow + dt->xmap[x] * 3;
            *out++ = s[0]; *out++ = s[1]; *out++ = s[2];
        }
        return;
    }
    /* box filter: average every source pixel that falls in the cell */
    {
        long sy0 = (long)((double)y * im->h / dt->th);
        long sy1 = (long)((double)(y + 1) * im->h / dt->th);
        long sy;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > im->h) sy1 = im->h;
        memset(dt->acc, 0, tw * 3 * sizeof(u32));
        memset(dt->cnt, 0, tw * sizeof(u16));
        for (sy = sy0; sy < sy1; sy++) {
            img_get_rgb_row(im, sy, dt->srow);
            for (x = 0; x < tw; x++) {
                long sx = dt->xmap[x], ex = dt->xmap[x + 1];
                const u8 *s;
                u32 *a = dt->acc + x * 3;
                if (ex <= sx) ex = sx + 1;
                if (ex > im->w) ex = im->w;
                for (s = dt->srow + sx * 3; sx < ex; sx++, s += 3) {
                    a[0] += s[0]; a[1] += s[1]; a[2] += s[2];
                    dt->cnt[x]++;
                }
            }
        }
        for (x = 0; x < tw; x++) {
            unsigned c = dt->cnt[x] ? dt->cnt[x] : 1;
            *out++ = dt->acc[x * 3] / c;
            *out++ = dt->acc[x * 3 + 1] / c;
            *out++ = dt->acc[x * 3 + 2] / c;
        }
    }
}

/* ---------------------------------------------------------- dithering */

static const u8 bayer8[64] = {
     0,32, 8,40, 2,34,10,42, 48,16,56,24,50,18,58,26,
    12,44, 4,36,14,46, 6,38, 60,28,52,20,62,30,54,22,
     3,35,11,43, 1,33, 9,41, 51,19,59,27,49,17,57,25,
    15,47, 7,39,13,45, 5,37, 63,31,55,23,61,29,53,21
};

static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

void dith_row(DITHER *dt, long y, u8 *out)
{
    long tw = dt->tw, x;
    u8 *rgb = dt->row;

    /* exact palette, no scaling: copy indices straight through */
    if (dt->exact && dt->im->type == IT_INDEXED &&
        (tw == dt->im->w || !dt->smooth)) {
        const IMAGE *im = dt->im;
        long sy = tw == im->w && dt->th == im->h ? y :
                  (long)((double)y * im->h / dt->th);
        const u8 *s = im->px + sy * im->stride;
        for (x = 0; x < tw; x++) {
            int idx = tw == im->w ? s[x] : s[dt->xmap[x]];
            *out++ = dt->ppix[dt->exmap[idx]];
        }
        return;
    }

    scaled_row(dt, y, rgb);

    if (dt->d->truecolor) {
        int bpp = dt->d->bytespp, intel = dt->d->intel;
        for (x = 0; x < tw; x++, rgb += 3) {
            int r = rgb[0], g = rgb[1], b = rgb[2];
            u32 p;
            if (dt->method == DITH_ORDERED && dt->d->nbits[1] < 8) {
                int t = bayer8[((y & 7) << 3) | (x & 7)] >> 3;   /* 0..7 */
                r = clamp8(r + t); g = clamp8(g + (t >> 1)); b = clamp8(b + t);
            }
            p = dt->lut[0][r] | dt->lut[1][g] | dt->lut[2][b];
            if (bpp == 2) {
                if (intel) { *out++ = p; *out++ = p >> 8; }
                else       { *out++ = p >> 8; *out++ = p; }
            } else if (bpp == 3) {
                *out++ = p >> 16; *out++ = p >> 8; *out++ = p;
            } else {
                if (intel) { *out++ = p; *out++ = p >> 8; *out++ = p >> 16; *out++ = p >> 24; }
                else { *out++ = p >> 24; *out++ = p >> 16; *out++ = p >> 8; *out++ = p; }
            }
        }
        return;
    }

    switch (dt->method) {
    case DITH_NONE:
        for (x = 0; x < tw; x++, rgb += 3)
            out[x] = dt->ppix[nearest(dt, rgb[0], rgb[1], rgb[2])];
        return;

    case DITH_ORDERED: {
        /* spread proportional to the palette's average spacing */
        int spread = dt->npal <= 2 ? 255 : dt->npal <= 4 ? 128 :
                     dt->npal <= 16 ? 64 : 32;
        for (x = 0; x < tw; x++, rgb += 3) {
            int t = ((int)bayer8[((y & 7) << 3) | (x & 7)] * 2 - 63) * spread / 128;
            out[x] = dt->ppix[nearest(dt, clamp8(rgb[0] + t), clamp8(rgb[1] + t),
                                      clamp8(rgb[2] + t))];
        }
        return;
    }

    case DITH_ATKINSON: {
        /* errors for this row (err0), next (err1) and the one after (err2) */
        short *e0 = dt->err0 + 6, *e1 = dt->err1 + 6, *e2 = dt->err2 + 6, *t;
        for (x = 0; x < tw; x++, rgb += 3) {
            int c, i;
            int v[3];
            for (c = 0; c < 3; c++)
                v[c] = clamp8(rgb[c] + e0[x * 3 + c]);
            i = nearest(dt, v[0], v[1], v[2]);
            out[x] = dt->ppix[i];
            for (c = 0; c < 3; c++) {
                int e = (v[c] - dt->prgb[i * 3 + c]) >> 3;
                e0[(x + 1) * 3 + c] += e;
                e0[(x + 2) * 3 + c] += e;
                e1[(x - 1) * 3 + c] += e;
                e1[x * 3 + c] += e;
                e1[(x + 1) * 3 + c] += e;
                e2[x * 3 + c] += e;
            }
        }
        /* rotate rows */
        t = dt->err0; dt->err0 = dt->err1; dt->err1 = dt->err2; dt->err2 = t;
        memset(dt->err2, 0, (tw + 4) * 3 * sizeof(short));
        return;
    }

    default: { /* Floyd-Steinberg, serpentine */
        short *cur = dt->err0 + 3, *nxt = dt->err1 + 3, *t;
        int rev = dt->serp && (y & 1);
        long i0 = rev ? tw - 1 : 0, dx = rev ? -1 : 1, n;
        memset(dt->err1, 0, (tw + 4) * 3 * sizeof(short));
        for (n = 0, x = i0; n < tw; n++, x += dx) {
            const u8 *s = dt->row + x * 3;
            short *ec = cur + x * 3;
            int r = clamp8(s[0] + (ec[0] >> 4));
            int g = clamp8(s[1] + (ec[1] >> 4));
            int b = clamp8(s[2] + (ec[2] >> 4));
            int i = nearest(dt, r, g, b), c;
            const u8 *p = dt->prgb + i * 3;
            int e[3];
            out[x] = dt->ppix[i];
            e[0] = r - p[0]; e[1] = g - p[1]; e[2] = b - p[2];
            /* errors kept in 1/16 units: 7 3 5 1 */
            for (c = 0; c < 3; c++) {
                int ev = e[c];
                cur[(x + dx) * 3 + c] += ev * 7;
                nxt[(x - dx) * 3 + c] += ev * 3;
                nxt[x * 3 + c]        += ev * 5;
                nxt[(x + dx) * 3 + c] += ev;
            }
        }
        t = dt->err0; dt->err0 = dt->err1; dt->err1 = t;
        return;
    }
    }
}

/* ------------------------------------------------- planes conversion */

void chunky_to_planes(const u8 *src, long w, int planes, u16 *dst,
                      long planesize_words, long wdwidth)
{
    long wx;
    int p;
    (void)wdwidth;
    for (wx = 0; wx * 16 < w; wx++) {
        u16 pw[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        int n = w - wx * 16 >= 16 ? 16 : (int)(w - wx * 16), i;
        const u8 *s = src + wx * 16;
        for (i = 0; i < n; i++) {
            unsigned v = s[i];
            u16 bit = 0x8000 >> i;
            for (p = 0; v && p < planes; p++, v >>= 1)
                if (v & 1)
                    pw[p] |= bit;
        }
        for (p = 0; p < planes; p++)
            dst[p * planesize_words + wx] = pw[p];
    }
}

u16 *std_row(u16 *buf, long y, long h, long wdw, int planes, long *psize)
{
    long band = y / STD_BAND, rows = h - band * STD_BAND;
    if (rows > STD_BAND)
        rows = STD_BAND;
    *psize = wdw * rows;
    return buf + band * STD_BAND * wdw * planes + (y % STD_BAND) * wdw;
}

/* ------------------------------------------------------ whole render */

int render_display(const IMAGE *im, long tw, long th, const DISPINFO *d,
                   const PREFS *pf, u8 *pal_out, u8 **buf, long *wdwidth,
                   PROG *pg)
{
    DITHER *dt;
    long wdw = (tw + 15) / 16, y, size;
    u8 *row, *b;
    int method = pf->dither;

    dt = dith_new(im, tw, th, d, method, pf->serpentine, pf->smooth,
                  pal_out, pg);
    if (!dt)
        return pg && pg->cancelled ? E_CANCEL : E_NOMEM;

    if (d->truecolor)
        size = wdw * 16 * d->bytespp * th;
    else if (d->chunky)
        size = wdw * 16 * th;               /* one byte per pixel */
    else
        size = wdw * 2 * d->planes * th;
    b = res_alloc(RB_DISP, size);
    row = malloc(wdw * 16 * (d->truecolor ? d->bytespp : 1));
    if (!b || !row) {
        free(row);
        dith_free(dt);
        if (b) res_free(RB_DISP);
        return E_NOMEM;
    }
    memset(b, 0, size);
    for (y = 0; y < th; y++) {
        if (d->truecolor)
            dith_row(dt, y, b + y * wdw * 16 * d->bytespp);
        else if (d->chunky)
            dith_row(dt, y, b + y * wdw * 16);
        else {
            long psize;
            u16 *dst = std_row((u16 *)b, y, th, wdw, d->planes, &psize);
            dith_row(dt, y, row);
            chunky_to_planes(row, tw, d->planes, dst, psize, wdw);
        }
        if ((y & 7) == 0 && prog_step(pg, th + y, th * 2)) {
            free(row);
            dith_free(dt);
            res_free(RB_DISP);
            return E_CANCEL;
        }
    }
    free(row);
    dith_free(dt);
    *buf = b;
    *wdwidth = wdw;
    return E_OK;
}
