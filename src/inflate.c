/* inflate.c - streaming inflate (RFC 1951) for ZIP and GZ entries. */
#include <stdlib.h>
#include <string.h>
#include "arcio.h"

static const u16 len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const u8 len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const u16 dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const u8 dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static const u8 cl_order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

#define WSIZE 32768L

typedef struct {
    BITS  b;
    BOUT *out;
    u8   *win;
    long  wpos;                  /* bytes written (window position)     */
    long  usize;
    PROG *pg;
    HUFF *lit, *dist;
} INF;

static void put(INF *s, int c)
{
    s->win[s->wpos & (WSIZE - 1)] = (u8)c;
    s->wpos++;
    bout_byte(s->out, c);
}

static int codes(INF *s)
{
    for (;;) {
        int sym = huff_decode(s->lit, &s->b);
        if (sym < 0 || s->b.over > 64)
            return E_FORMAT;
        if (sym < 256) {
            put(s, sym);
        } else if (sym == 256) {
            return E_OK;
        } else {
            int lc = sym - 257, dc, len, k;
            long d;
            if (lc >= 29)
                return E_FORMAT;
            len = len_base[lc] + (int)bits_get(&s->b, len_extra[lc]);
            dc = huff_decode(s->dist, &s->b);
            if (dc < 0 || dc >= 30)
                return E_FORMAT;
            d = dist_base[dc] + (long)bits_get(&s->b, dist_extra[dc]);
            if (d > s->wpos)
                return E_FORMAT;
            for (k = 0; k < len; k++)
                put(s, s->win[(s->wpos - d) & (WSIZE - 1)]);
        }
        if ((s->wpos & 0x3FFF) < 258) {
            if (s->out->err)
                return s->out->err;
            if (codec_progress(s->pg, s->wpos, s->usize))
                return E_CANCEL;
        }
    }
}

int inflate_stream(BIN *in, BOUT *out, long usize, PROG *pg)
{
    INF s;
    int final = 0, e = E_OK;
    memset(&s, 0, sizeof s);
    bits_init(&s.b, in, 0);
    s.out = out;
    s.usize = usize;
    s.pg = pg;
    s.win = malloc(WSIZE);
    s.lit = malloc(sizeof(HUFF));
    s.dist = malloc(sizeof(HUFF));
    if (!s.win || !s.lit || !s.dist) {
        e = E_NOMEM;
        goto out;
    }
    while (!final && !e) {
        int type;
        final = (int)bits_get(&s.b, 1);
        type = (int)bits_get(&s.b, 2);
        if (s.b.over > 8) {
            e = E_FORMAT;
            break;
        }
        if (type == 0) {                         /* stored */
            unsigned len, nlen;
            bits_align(&s.b);
            len = bits_get(&s.b, 16);
            nlen = bits_get(&s.b, 16);
            if ((len ^ 0xFFFF) != nlen) {
                e = E_FORMAT;
                break;
            }
            while (len--)
                put(&s, (int)bits_get(&s.b, 8));
            if (s.b.over)
                e = E_FORMAT;
        } else if (type == 1) {                  /* fixed codes */
            u8 l[288];
            int i;
            for (i = 0; i < 288; i++)
                l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
            huff_build(s.lit, l, 288, 0);
            for (i = 0; i < 30; i++)
                l[i] = 5;
            huff_build(s.dist, l, 30, 0);
            e = codes(&s);
        } else if (type == 2) {                  /* dynamic codes */
            u8 l[320], cl[19];
            HUFF *ch = s.dist;                   /* borrowed for the code lengths */
            int hlit = (int)bits_get(&s.b, 5) + 257, hdist = (int)bits_get(&s.b, 5) + 1;
            int hclen = (int)bits_get(&s.b, 4) + 4, i, n = 0;
            memset(cl, 0, sizeof cl);
            for (i = 0; i < hclen; i++)
                cl[cl_order[i]] = (u8)bits_get(&s.b, 3);
            if (hlit > 286 || hdist > 30 || !huff_build(ch, cl, 19, 0)) {
                e = E_FORMAT;
                break;
            }
            while (n < hlit + hdist) {
                int sym = huff_decode(ch, &s.b), rep = 0, v = 0;
                if (sym < 0) { e = E_FORMAT; break; }
                if (sym < 16) { l[n++] = (u8)sym; continue; }
                if (sym == 16) {
                    if (!n) { e = E_FORMAT; break; }
                    v = l[n - 1];
                    rep = 3 + (int)bits_get(&s.b, 2);
                } else if (sym == 17)
                    rep = 3 + (int)bits_get(&s.b, 3);
                else
                    rep = 11 + (int)bits_get(&s.b, 7);
                if (n + rep > hlit + hdist) { e = E_FORMAT; break; }
                while (rep--)
                    l[n++] = (u8)v;
            }
            if (e)
                break;
            if (!huff_build(s.lit, l, hlit, 0) || !huff_build(s.dist, l + hlit, hdist, 0)) {
                e = E_FORMAT;
                break;
            }
            e = codes(&s);
        } else
            e = E_FORMAT;
        if (!e && s.b.over > 32)
            e = E_FORMAT;
    }
    if (!e && usize >= 0 && s.wpos != usize)
        e = E_FORMAT;
    /* give back the bytes the bit reader took but did not use */
    if (!e) {
        int extra = s.b.cnt / 8;
        if (extra && in->pos >= extra) {
            in->pos -= extra;
            in->total -= extra;
        }
    }
out:
    free(s.win);
    free(s.lit);
    free(s.dist);
    if (!e && out->err)
        e = out->err;
    return e;
}
