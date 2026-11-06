/*
 * palette.c - picture tools that change a window's own copy of a picture,
 * or look at it: palette files, colour reduction, remapping, resizing,
 * counting colours, and the palette several pictures can share.
 *
 * GEM-free: runs in fview.prg (cooperative jobs), in fvwork.ttp and in
 * the host tests. Long loops report through PROG so they can be cancelled.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fview.h"
#include "palette.h"

/* -------------------------------------------------------- palette files */

static int is_space(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* three numbers 0..255 from a text line; 0 if it has none */
static int three(const char *s, u8 *rgb)
{
    int k;
    for (k = 0; k < 3; k++) {
        long v;
        char *e;
        while (is_space((u8)*s))
            s++;
        v = strtol(s, &e, 10);
        if (e == s || v < 0 || v > 255)
            return 0;
        rgb[k] = (u8)v;
        s = e;
    }
    return 1;
}

static int ext_is(const char *path, const char *ext)
{
    const char *d = strrchr(path, '.');
    if (!d)
        return 0;
    d++;
    while (*d && *ext) {
        int a = *d++, b = *ext++;
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b)
            return 0;
    }
    return !*d && !*ext;
}

/* 16 Atari hardware colour words (0RGB, STE bit order) */
static void st_words(const u8 *w, u8 *pal)
{
    int i, k;
    for (i = 0; i < 16; i++) {
        unsigned v = w[i * 2] << 8 | w[i * 2 + 1];
        for (k = 0; k < 3; k++) {
            /* STE: 4 bits with the low bit on top; ST: 3 bits */
            unsigned c = (v >> (8 - k * 4)) & 15;
            c = ((c & 7) << 1) | (c >> 3);
            pal[i * 3 + k] = (u8)(c * 17);
        }
    }
}

int pal_load_file(const char *path, u8 *pal, int *n)
{
    FILE *f = fopen(path, "rb");
    u8 buf[1024];
    long len;
    *n = 0;
    if (!f)
        return E_OPEN;
    len = (long)fread(buf, 1, sizeof buf, f);
    /* the palette of an Atari picture: Degas (PI1-3, PC1-3), NEOchrome */
    if (len >= 36 && (ext_is(path, "PI1") || ext_is(path, "PI2") || ext_is(path, "PI3") ||
                      ext_is(path, "PC1") || ext_is(path, "PC2") || ext_is(path, "PC3") ||
                      ext_is(path, "NEO"))) {
        int res = (buf[ext_is(path, "NEO") ? 2 : 0] << 8 | buf[ext_is(path, "NEO") ? 3 : 1]) & 3;
        fclose(f);
        st_words(buf + (ext_is(path, "NEO") ? 4 : 2), pal);
        *n = res == 0 ? 16 : res == 1 ? 4 : 2;
        return E_OK;
    }
    if (len >= 8 && (!memcmp(buf, "JASC-PAL", 8) || !memcmp(buf, "GIMP Pal", 8))) {
        /* text: JASC-PAL (Paint Shop Pro) or GIMP .gpl */
        char line[160];
        int jasc = buf[0] == 'J', lno = 0, want = 256;
        fseek(f, 0, SEEK_SET);
        while (fgets(line, sizeof line, f) && *n < 256) {
            lno++;
            if (jasc && lno == 3)
                want = atoi(line);
            if (jasc && lno <= 3)
                continue;
            if (!jasc && (line[0] == '#' || !strncmp(line, "Name:", 5) ||
                          !strncmp(line, "Columns:", 8) || lno == 1))
                continue;
            if (three(line, pal + *n * 3))
                (*n)++;
            if (jasc && *n >= want)
                break;
        }
        fclose(f);
        return *n ? E_OK : E_FORMAT;
    }
    fclose(f);
    if (len == 772 || len == 768) {     /* Adobe .ACT or raw 256 x RGB */
        *n = 256;
        if (len == 772) {
            int c = buf[768] << 8 | buf[769];
            if (c > 0 && c <= 256)
                *n = c;
        }
        memcpy(pal, buf, *n * 3);
        return E_OK;
    }
    if (len == 32 || len == 34) {       /* Atari hardware: 16 words 0RGB */
        st_words(buf + len - 32, pal);
        *n = 16;
        return E_OK;
    }
    if (len > 0 && len < 768 && len % 3 == 0 && !ext_is(path, "TXT")) {
        *n = (int)(len / 3);            /* raw RGB triples */
        memcpy(pal, buf, len);
        return E_OK;
    }
    return E_FORMAT;
}

int pal_save_file(const char *path, const u8 *pal, int n)
{
    FILE *f;
    int i, e;
    if (n < 1 || n > 256)
        return E_FORMAT;
    f = fopen(path, ext_is(path, "ACT") ? "wb" : "w");
    if (!f)
        return E_WRITE;
    if (ext_is(path, "ACT")) {
        u8 tail[4];
        u8 full[768];
        memset(full, 0, sizeof full);
        memcpy(full, pal, n * 3);
        fwrite(full, 1, 768, f);
        tail[0] = (u8)(n >> 8); tail[1] = (u8)n;
        tail[2] = 0xFF; tail[3] = 0xFF;  /* no transparent colour */
        fwrite(tail, 1, 4, f);
    } else if (ext_is(path, "GPL")) {
        fprintf(f, "GIMP Palette\nName: FView\nColumns: 16\n#\n");
        for (i = 0; i < n; i++)
            fprintf(f, "%3d %3d %3d\tIndex %d\n", pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2], i);
    } else {
        fprintf(f, "JASC-PAL\n0100\n%d\n", n);
        for (i = 0; i < n; i++)
            fprintf(f, "%d %d %d\n", pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]);
    }
    e = ferror(f);
    if (fclose(f) != 0 || e) {
        remove(path);
        return E_WRITE;
    }
    return E_OK;
}

/* ------------------------------------------------- histogram, median cut */

#define CELL(r, g, b) ((((unsigned)(r) >> 3) << 10) | (((unsigned)(g) >> 3) << 5) | ((unsigned)(b) >> 3))

int quant_hist_add(u32 *hist, const IMAGE *im, PROG *pg, long base, long total)
{
    long y, x, step = im->w * im->h > 300000L ? 2 : 1;
    for (y = 0; y < im->h; y += step) {
        const u8 *s = im->px + y * im->stride;
        if (im->type == IT_INDEXED)
            for (x = 0; x < im->w; x += step) {
                const u8 *c = im->pal + s[x] * 3;
                hist[CELL(c[0], c[1], c[2])]++;
            }
        else
            for (x = 0; x < im->w; x += step, s += 3 * step)
                hist[CELL(s[0], s[1], s[2])]++;
        if ((y & 15) == 0 && prog_step(pg, base + y, total))
            return E_CANCEL;
    }
    return E_OK;
}

/* median cut on the list of filled cells (fast on a 68000: the work
   depends on how many colours there are, not on the size of the cube) */
typedef struct { u16 cell; u32 n; } CELLN;
typedef struct { int a, b; u8 lo[3], hi[3]; u32 n; } QBOX;

static int sort_axis;

static int comp(unsigned cell, int axis)
{
    return axis == 0 ? (cell >> 10) & 31 : axis == 1 ? (cell >> 5) & 31 : cell & 31;
}

static int cmp_cell(const void *x, const void *y)
{
    return comp(((const CELLN *)x)->cell, sort_axis) - comp(((const CELLN *)y)->cell, sort_axis);
}

static void qbox_fit(QBOX *b, const CELLN *c)
{
    int i, k;
    b->n = 0;
    for (k = 0; k < 3; k++) {
        b->lo[k] = 31;
        b->hi[k] = 0;
    }
    for (i = b->a; i < b->b; i++) {
        for (k = 0; k < 3; k++) {
            int v = comp(c[i].cell, k);
            if (v < b->lo[k]) b->lo[k] = (u8)v;
            if (v > b->hi[k]) b->hi[k] = (u8)v;
        }
        b->n += c[i].n;
    }
}

int quant_from_hist(const u32 *hist, int want, u8 *out, PROG *pg)
{
    CELLN *c;
    QBOX *bx;
    long nc = 0, i;
    int nb = 1, k;
    for (i = 0; i < 32768L; i++)
        if (hist[i])
            nc++;
    if (!nc || want < 1)
        return 0;
    c = malloc(nc * sizeof(CELLN));
    bx = malloc((want + 1) * sizeof(QBOX));
    if (!c || !bx) {
        free(c);
        free(bx);
        return -1;
    }
    for (i = 0, nc = 0; i < 32768L; i++)
        if (hist[i]) {
            c[nc].cell = (u16)i;
            c[nc++].n = hist[i];
        }
    bx[0].a = 0;
    bx[0].b = (int)nc;
    qbox_fit(&bx[0], c);
    while (nb < want) {
        long best = -1;
        int bi = -1, axis, mid;
        u32 half, run = 0;
        for (k = 0; k < nb; k++) {
            int dr = (bx[k].hi[0] - bx[k].lo[0]) * 3, dg = (bx[k].hi[1] - bx[k].lo[1]) * 4,
                db = (bx[k].hi[2] - bx[k].lo[2]) * 2;
            int len = dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
            long score;
            if (bx[k].b - bx[k].a < 2 || len == 0)
                continue;
            score = (long)(bx[k].n > 0x7FFFFL ? 0x7FFFFL : bx[k].n) * len;
            if (score > best)
                best = score, bi = k;
        }
        if (bi < 0)
            break;
        {
            QBOX *b = &bx[bi];
            int dr = (b->hi[0] - b->lo[0]) * 3, dg = (b->hi[1] - b->lo[1]) * 4,
                db = (b->hi[2] - b->lo[2]) * 2;
            axis = (dg >= dr && dg >= db) ? 1 : (dr >= db ? 0 : 2);
            sort_axis = axis;
            qsort(c + b->a, b->b - b->a, sizeof(CELLN), cmp_cell);
            half = b->n / 2;
            for (mid = b->a; mid < b->b - 1; mid++) {
                run += c[mid].n;
                if (run >= half)
                    break;
            }
            mid++;                           /* first cell of the upper half */
            /* do not cut between two cells of the same value on the axis */
            while (mid < b->b && mid > b->a + 1 &&
                   comp(c[mid].cell, axis) == comp(c[mid - 1].cell, axis))
                mid++;
            if (mid >= b->b)
                for (mid = b->b - 1; mid > b->a + 1 &&
                     comp(c[mid].cell, axis) == comp(c[mid - 1].cell, axis); mid--)
                    ;
            bx[nb].a = mid;
            bx[nb].b = b->b;
            b->b = mid;
            qbox_fit(b, c);
            qbox_fit(&bx[nb], c);
            nb++;
        }
        if (prog_step(pg, nb, want)) {
            free(c);
            free(bx);
            return -1;
        }
    }
    for (k = 0; k < nb; k++) {
        double s[3] = { 0, 0, 0 };
        double n = 0;
        int j;
        for (i = bx[k].a; i < bx[k].b; i++) {
            for (j = 0; j < 3; j++)
                s[j] += (double)c[i].n * comp(c[i].cell, j);
            n += c[i].n;
        }
        for (j = 0; j < 3; j++) {
            int v = n > 0 ? (int)(s[j] / n * 255.0 / 31.0 + 0.5) : 0;
            out[k * 3 + j] = (u8)(v > 255 ? 255 : v);
        }
    }
    free(c);
    free(bx);
    return nb;
}

int quant_palette(const IMAGE *im, int want, u8 *pal, PROG *pg)
{
    u32 *hist;
    int n;
    if (im->type == IT_INDEXED && im->ncolors <= want) {
        memcpy(pal, im->pal, im->ncolors * 3);
        return im->ncolors;
    }
    hist = calloc(32768L, sizeof(u32));
    if (!hist)
        return -1;
    if (quant_hist_add(hist, im, pg, 0, im->h * 2)) {
        free(hist);
        return -1;
    }
    n = quant_from_hist(hist, want, pal, 0);
    free(hist);
    return n;
}

/* --------------------------------------------------- reduce and remap */

int img_remap(const IMAGE *src, IMAGE *dst, const u8 *pal, int n,
              const PREFS *pf, PROG *pg)
{
    DISPINFO d;
    DITHER *dt;
    u8 scratch[256 * 3];
    long y;
    int i;
    if (n < 1 || n > 256)
        return E_FORMAT;
    /* a made-up 256 colour screen whose palette is the target: the
       ditherer then picks among exactly these colours                 */
    memset(&d, 0, sizeof d);
    d.planes = 8;
    d.palmode = PAL_SYSTEM;
    d.fixed_palette = 1;
    for (i = 0; i < 256; i++)
        memcpy(d.sysrgb + i * 3, pal + (i < n ? i : 0) * 3, 3);
    memset(d.reserved, 1, sizeof d.reserved);
    *dst = *src;
    dst->type = IT_INDEXED;
    dst->ncolors = n;
    dst->trans = -1;
    memcpy(dst->pal, pal, n * 3);
    memset(dst->pal + n * 3, 0, (256 - n) * 3);
    dst->orig_bits = n <= 2 ? 1 : n <= 4 ? 2 : n <= 16 ? 4 : 8;
    if (img_alloc_px(dst, RB_ORIG))
        return E_NOMEM;
    dt = dith_new(src, src->w, src->h, &d, pf->dither, pf->serpentine, 0, scratch, 0);
    if (!dt) {
        res_free(RB_ORIG);
        return E_NOMEM;
    }
    for (y = 0; y < src->h; y++) {
        dith_row(dt, y, dst->px + y * dst->stride);
        if ((y & 7) == 0 && prog_step(pg, y, src->h)) {
            dith_free(dt);
            res_free(RB_ORIG);
            return E_CANCEL;
        }
    }
    dith_free(dt);
    /* the ditherer may never pick some colours, and duplicates map to the
       first one: indices stay below n either way                      */
    for (y = 0; y < src->h; y++) {
        u8 *p = dst->px + y * dst->stride;
        long x;
        for (x = 0; x < dst->w; x++)
            if (p[x] >= n)
                p[x] = 0;
    }
    sprintf(dst->fmtinfo, "%.30s, %d colours", src->fmtinfo, n);
    return E_OK;
}

int img_reduce_colours(const IMAGE *src, IMAGE *dst, int n, const PREFS *pf, PROG *pg)
{
    u8 pal[256 * 3];
    int got;
    PROG sub;
    if (n < 2) n = 2;
    if (n > 256) n = 256;
    if (pg) {
        sub = *pg;
        sub.halves = 1;
        sub.phase = 0;
        pg = &sub;
    }
    got = quant_palette(src, n, pal, pg);
    if (got <= 0)
        return got < 0 ? (pg && pg->cancelled ? E_CANCEL : E_NOMEM) : E_FORMAT;
    if (pg)
        pg->phase = 1;
    return img_remap(src, dst, pal, got, pf, pg);
}

/* ---------------------------------------------------------------- resize */

int img_resize(const IMAGE *src, IMAGE *dst, long nw, long nh, int smooth, PROG *pg)
{
    long y, x, *xs = 0;
    int bpp = src->type == IT_RGB ? 3 : 1, e = E_OK;
    if (nw < 1 || nh < 1 || nw > 16000 || nh > 16000)
        return E_FORMAT;
    *dst = *src;
    dst->w = nw;
    dst->h = nh;
    if (img_alloc_px(dst, RB_ORIG))
        return E_NOMEM;
    xs = malloc((nw + 1) * sizeof(long));
    if (!xs)
        goto nomem;
    for (x = 0; x <= nw; x++)
        xs[x] = (long)((double)x * src->w / nw);
    if (src->type == IT_INDEXED || !smooth) {
        /* nearest: a paletted picture keeps its exact colours */
        for (y = 0; y < nh; y++) {
            long sy = (long)((double)y * src->h / nh);
            const u8 *s = src->px + (sy < src->h ? sy : src->h - 1) * src->stride;
            u8 *d = dst->px + y * dst->stride;
            if (bpp == 1)
                for (x = 0; x < nw; x++)
                    d[x] = s[xs[x] < src->w ? xs[x] : src->w - 1];
            else
                for (x = 0; x < nw; x++) {
                    const u8 *p = s + (xs[x] < src->w ? xs[x] : src->w - 1) * 3;
                    *d++ = p[0]; *d++ = p[1]; *d++ = p[2];
                }
            if ((y & 15) == 0 && prog_step(pg, y, nh)) {
                e = E_CANCEL;
                break;
            }
        }
    } else {
        /* true colour: average the source pixels under each new pixel
           when shrinking, blend neighbours (bilinear) when enlarging     */
        u32 *acc = calloc(nw * 3, sizeof(u32));
        u32 *cnt = calloc(nw, sizeof(u32));
        if (!acc || !cnt) {
            free(acc);
            free(cnt);
            goto nomem;
        }
        for (y = 0; y < nh && !e; y++) {
            u8 *d = dst->px + y * dst->stride;
            if (nh <= src->h && nw <= src->w) {
                long sy0 = (long)((double)y * src->h / nh), sy1 = (long)((double)(y + 1) * src->h / nh), sy;
                if (sy1 <= sy0) sy1 = sy0 + 1;
                memset(acc, 0, nw * 3 * sizeof(u32));
                memset(cnt, 0, nw * sizeof(u32));
                for (sy = sy0; sy < sy1 && sy < src->h; sy++) {
                    const u8 *s = src->px + sy * src->stride;
                    for (x = 0; x < nw; x++) {
                        long sx = xs[x], ex = xs[x + 1];
                        if (ex <= sx) ex = sx + 1;
                        if (ex > src->w) ex = src->w;
                        for (; sx < ex; sx++) {
                            acc[x * 3] += s[sx * 3];
                            acc[x * 3 + 1] += s[sx * 3 + 1];
                            acc[x * 3 + 2] += s[sx * 3 + 2];
                            cnt[x]++;
                        }
                    }
                }
                for (x = 0; x < nw; x++) {
                    u32 c = cnt[x] ? cnt[x] : 1;
                    *d++ = (u8)(acc[x * 3] / c);
                    *d++ = (u8)(acc[x * 3 + 1] / c);
                    *d++ = (u8)(acc[x * 3 + 2] / c);
                }
            } else {
                /* fixed point 8.8 source position of each pixel centre */
                long fy = (long)(((double)y + 0.5) * src->h / nh * 256.0) - 128;
                long y0 = fy < 0 ? 0 : fy >> 8, wy = fy < 0 ? 0 : fy & 255;
                long y1 = y0 + 1 < src->h ? y0 + 1 : y0;
                const u8 *r0 = src->px + y0 * src->stride, *r1 = src->px + y1 * src->stride;
                for (x = 0; x < nw; x++) {
                    long fx = (long)(((double)x + 0.5) * src->w / nw * 256.0) - 128;
                    long x0 = fx < 0 ? 0 : fx >> 8, wx = fx < 0 ? 0 : fx & 255;
                    long x1 = x0 + 1 < src->w ? x0 + 1 : x0;
                    int k;
                    for (k = 0; k < 3; k++) {
                        long a = r0[x0 * 3 + k] * (256 - wx) + r0[x1 * 3 + k] * wx;
                        long b = r1[x0 * 3 + k] * (256 - wx) + r1[x1 * 3 + k] * wx;
                        *d++ = (u8)((a * (256 - wy) + b * wy) >> 16);
                    }
                }
            }
            if ((y & 7) == 0 && prog_step(pg, y, nh))
                e = E_CANCEL;
        }
        free(acc);
        free(cnt);
    }
    free(xs);
    if (e)
        res_free(RB_ORIG);
    else
        sprintf(dst->fmtinfo, "%.30s, resized", src->fmtinfo);
    return e;
nomem:
    free(xs);
    res_free(RB_ORIG);
    return E_NOMEM;
}

/* ------------------------------------------------------------- counting */

/* exact number of different colours (24 bit) */
long img_count_real(const IMAGE *im, int *used_entries, PROG *pg)
{
    long y, x, total = 0;
    *used_entries = 0;
    if (im->type == IT_INDEXED) {
        u8 used[256];
        int i, k;
        memset(used, 0, sizeof used);
        for (y = 0; y < im->h; y++) {
            const u8 *s = im->px + y * im->stride;
            for (x = 0; x < im->w; x++)
                used[s[x]] = 1;
            if ((y & 31) == 0 && prog_step(pg, y, im->h))
                return -1;
        }
        for (i = 0; i < 256; i++) {
            int dup = 0;
            if (!used[i])
                continue;
            (*used_entries)++;
            for (k = 0; k < i && !dup; k++)
                if (used[k] && !memcmp(im->pal + k * 3, im->pal + i * 3, 3))
                    dup = 1;
            if (!dup)
                total++;
        }
        return total;
    }
    {
        /* one bit per colour, for a range of red values at a time: as many
           passes over the picture as the memory needs (2 MB: one pass)   */
        long span;
        u8 *bits = 0;
        int r0, passes;
        for (span = 256; span >= 4; span /= 2)
            if ((bits = malloc(span * 8192L)) != 0)
                break;
        if (!bits)
            return -2;
        passes = (int)(256 / span);
        for (r0 = 0; r0 < 256; r0 += (int)span) {
            long i, pass = r0 / span;
            memset(bits, 0, span * 8192L);
            for (y = 0; y < im->h; y++) {
                const u8 *s = im->px + y * im->stride;
                for (x = 0; x < im->w; x++, s += 3) {
                    long r = s[0] - r0, idx;
                    if (r < 0 || r >= span)
                        continue;
                    idx = (r << 16) | (s[1] << 8) | s[2];
                    bits[idx >> 3] |= (u8)(1 << (idx & 7));
                }
                if ((y & 15) == 0 && prog_step(pg, pass * im->h + y, (long)passes * im->h)) {
                    free(bits);
                    return -1;
                }
            }
            for (i = 0; i < span * 8192L; i++) {
                unsigned v = bits[i];
                while (v) {
                    total += v & 1;
                    v >>= 1;
                }
            }
        }
        free(bits);
        return total;
    }
}

/* Colours the eye sees once the picture is dithered for this screen:
   neighbouring dots blend, so each 2x2 group of shown pixels is averaged
   (to 5 bits a channel) and every blend seen at least 8 times counts.
   On true colour screens nothing is dithered: the shown colours count.  */
long img_count_seen(const IMAGE *im, long tw, long th, const DISPINFO *d,
                    const PREFS *pf, PROG *pg)
{
    DITHER *dt;
    u8 pal[256 * 3], *r0, *r1, *c;
    long y, x, total = 0, i;
    if (tw < 1) tw = im->w;
    if (th < 1) th = im->h;
    c = calloc(32768L, 1);
    r0 = malloc(tw * 3 + 8);
    r1 = malloc(tw * 3 + 8);
    if (!c || !r0 || !r1) {
        free(c); free(r0); free(r1);
        return -2;
    }
    if (d->truecolor) {
        /* the picture as shown, scaled, at the screen's precision */
        for (y = 0; y < th; y++) {
            long sy = (long)((double)y * im->h / th);
            img_get_rgb_row(im, sy < im->h ? sy : im->h - 1, r0);
            for (x = 0; x < tw; x++) {
                long sx = (long)((double)x * im->w / tw);
                const u8 *p = r0 + (sx < im->w ? sx : im->w - 1) * 3;
                unsigned k = CELL(p[0], p[1], p[2]);
                if (c[k] < 255) c[k]++;
            }
            if ((y & 15) == 0 && prog_step(pg, y, th))
                goto cancel;
        }
    } else {
        u8 *px = malloc(tw + 16);
        if (!px) {
            free(c); free(r0); free(r1);
            return -2;
        }
        dt = dith_new(im, tw, th, d, pf->dither, pf->serpentine, pf->smooth, pal, 0);
        if (!dt) {
            free(px);
            free(c); free(r0); free(r1);
            return -2;
        }
        for (y = 0; y < th; y++) {
            dith_row(dt, y, px);
            for (x = 0; x < tw; x++)
                memcpy(r1 + x * 3, pal + px[x] * 3, 3);
            if (y > 0)
                for (x = 0; x + 1 < tw; x++) {
                    int k, v[3];
                    for (k = 0; k < 3; k++)
                        v[k] = (r0[x * 3 + k] + r0[x * 3 + 3 + k] + r1[x * 3 + k] +
                                r1[x * 3 + 3 + k] + 2) >> 2;
                    {
                        unsigned key = CELL(v[0], v[1], v[2]);
                        if (c[key] < 255) c[key]++;
                    }
                }
            memcpy(r0, r1, tw * 3);
            if ((y & 7) == 0 && prog_step(pg, y, th)) {
                dith_free(dt);
                free(px);
                goto cancel;
            }
        }
        dith_free(dt);
        free(px);
    }
    for (i = 0; i < 32768L; i++)
        if (c[i] >= 8 || (c[i] && tw * th < 4096))
            total++;
    free(c); free(r0); free(r1);
    return total;
cancel:
    free(c); free(r0); free(r1);
    return -1;
}

/* ------------------------------------------------------- shared palette */

/* one palette for several pictures, by pixel value: reserved pixel values
   keep the screen's colours, the free ones get the pictures' colours   */
int shared_palette(const IMAGE *const *ims, int n, const DISPINFO *d, u8 *pal_by_pix,
                   PROG *pg)
{
    u32 *hist = calloc(32768L, sizeof(u32));
    u8 cols[256 * 3];
    int ncol = d->planes >= 8 ? 256 : 1 << d->planes, nfree = 0, i, k, got;
    int slot[256];
    long total = 0, base = 0;
    if (!hist)
        return E_NOMEM;
    for (i = 0; i < n; i++)
        total += ims[i]->h;
    for (i = 0; i < n; i++) {
        if (quant_hist_add(hist, ims[i], pg, base, total + 1)) {
            free(hist);
            return E_CANCEL;
        }
        base += ims[i]->h;
    }
    for (i = 0; i < ncol; i++)
        if (!d->reserved[i])
            nfree++;
    memcpy(pal_by_pix, d->sysrgb, 256 * 3);
    got = nfree ? quant_from_hist(hist, nfree, cols, 0) : 0;
    /* UI pens free: the lightest and darkest go there (white and black
       added if the pictures have no contrast), see ui_arrange         */
    if (got >= 0 && ui_needs_bw(d, cols, got)) {
        if (got > nfree - 2)
            got = nfree > 2 ? quant_from_hist(hist, nfree - 2, cols, 0) : 0;
        if (got >= 0) {
            memset(cols + got * 3, 255, 3);
            memset(cols + got * 3 + 3, 0, 3);
            got += 2;
        }
    }
    free(hist);
    if (got < 0)
        return E_NOMEM;
    ui_arrange(d, ncol, cols, got, slot);
    for (k = 0; k < got; k++)
        if (slot[k] >= 0)
            memcpy(pal_by_pix + slot[k] * 3, cols + k * 3, 3);
    return E_OK;
}
