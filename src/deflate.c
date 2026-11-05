/*
 * deflate.c - streaming deflate compressor (RFC 1951) for ZIP, GZ and
 * the PDF writer. LZ77 over a 32K window with hash chains and lazy
 * matching; each block is written with dynamic Huffman codes, or fixed
 * codes when those come out smaller.
 */
#include <stdlib.h>
#include <string.h>
#include "arcio.h"
#include "deflate.h"

#define RING     65536L
#define RMASK    (RING - 1)
#define WINDOW   32768L
#define HBITS    14
#define HSIZE    (1 << HBITS)
#define MINMATCH 3
#define MAXMATCH 258
#define NIL      (-1L)
#define TOKENS   16384                 /* LZ77 tokens per block           */

struct DEFL {
    BITW  w;
    u8   *ring;
    long *head, *prev;
    long  inpos, cur;
    int   chain;
    /* the block being collected */
    u16  *lit;                         /* literal 0..255 or length 3..258 + 256 */
    u16  *dist;                        /* 0 for literals                  */
    int   ntok;
    /* a match found at cur - 1, not yet written (lazy matching) */
    int   pend_len;
    long  pend_dist;
    int   pend_lit;
    int   have_pend;
};

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

static int len_code(int len)
{
    int i;
    for (i = 28; i > 0 && len_base[i] > len; i--)
        ;
    return i;
}

static int dist_code(long d)
{
    int i;
    for (i = 29; i > 0 && dist_base[i] > d; i--)
        ;
    return i;
}

/* Huffman codes go out most significant bit first in an LSB-first stream */
static void put_huff(BITW *w, unsigned code, int len)
{
    unsigned r = 0;
    int i;
    for (i = 0; i < len; i++)
        r |= ((code >> i) & 1) << (len - 1 - i);
    bitw_put(w, r, len);
}

static void fixed_lens(u8 *ll, u8 *dl)
{
    int i;
    for (i = 0; i < 288; i++)
        ll[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    for (i = 0; i < 30; i++)
        dl[i] = 5;
}

static long block_cost(DEFL *d, const u8 *ll, const u8 *dl)
{
    long bits = 0;
    int i;
    for (i = 0; i < d->ntok; i++) {
        if (d->dist[i]) {
            int lc = len_code(d->lit[i] - 256), dc = dist_code(d->dist[i]);
            bits += ll[257 + lc] + len_extra[lc] + dl[dc] + dist_extra[dc];
        } else
            bits += ll[d->lit[i]];
    }
    return bits + ll[256];
}

static void write_tokens(DEFL *d, const u8 *ll, const u8 *dl)
{
    u16 lc_codes[288], d_codes[30];
    int i;
    huff_codes(ll, 288, lc_codes);
    huff_codes(dl, 30, d_codes);
    for (i = 0; i < d->ntok; i++) {
        if (d->dist[i]) {
            int len = d->lit[i] - 256, lc = len_code(len), dc = dist_code(d->dist[i]);
            put_huff(&d->w, lc_codes[257 + lc], ll[257 + lc]);
            if (len_extra[lc])
                bitw_put(&d->w, (unsigned)(len - len_base[lc]), len_extra[lc]);
            put_huff(&d->w, d_codes[dc], dl[dc]);
            if (dist_extra[dc])
                bitw_put(&d->w, (unsigned)(d->dist[i] - dist_base[dc]), dist_extra[dc]);
        } else
            put_huff(&d->w, lc_codes[d->lit[i]], ll[d->lit[i]]);
    }
    put_huff(&d->w, lc_codes[256], ll[256]);
}

/* write the collected tokens as one block */
static void flush_block(DEFL *d, int final)
{
    u32 lf[288], df[30], cf[19];
    u8 ll[288], dl[30], cl[19], fl[288], fd[30];
    u16 cl_codes[19];
    u8 seq[320], ext[320];              /* RLE of the code lengths */
    int i, nseq = 0, hlit, hdist, hclen;
    long dyn, fix, hdr;
    memset(lf, 0, sizeof lf);
    memset(df, 0, sizeof df);
    for (i = 0; i < d->ntok; i++) {
        if (d->dist[i]) {
            lf[257 + len_code(d->lit[i] - 256)]++;
            df[dist_code(d->dist[i])]++;
        } else
            lf[d->lit[i]]++;
    }
    lf[256] = 1;
    huff_lengths(lf, 288, 15, ll);
    huff_lengths(df, 30, 15, dl);
    /* at least one distance code, as some decoders want */
    {
        int any = 0;
        for (i = 0; i < 30; i++)
            any |= dl[i];
        if (!any)
            dl[0] = 1;
    }
    for (hlit = 286; hlit > 257 && !ll[hlit - 1]; hlit--)
        ;
    for (hdist = 30; hdist > 1 && !dl[hdist - 1]; hdist--)
        ;
    /* run-length code the two length lists together */
    {
        u8 all[320];
        int n = hlit + hdist, k = 0;
        memcpy(all, ll, hlit);
        memcpy(all + hlit, dl, hdist);
        while (k < n) {
            int v = all[k], run = 1;
            while (k + run < n && all[k + run] == v)
                run++;
            if (v == 0 && run >= 3) {
                int r = run > 138 ? 138 : run;
                seq[nseq] = r >= 11 ? 18 : 17;
                ext[nseq++] = (u8)(r >= 11 ? r - 11 : r - 3);
                k += r;
            } else if (v != 0 && run >= 4) {
                int r = run - 1 > 6 ? 6 : run - 1;
                seq[nseq] = (u8)v;
                ext[nseq++] = 0;
                seq[nseq] = 16;
                ext[nseq++] = (u8)(r - 3);
                k += r + 1;
            } else {
                seq[nseq] = (u8)v;
                ext[nseq++] = 0;
                k++;
            }
        }
    }
    memset(cf, 0, sizeof cf);
    for (i = 0; i < nseq; i++)
        cf[seq[i]]++;
    huff_lengths(cf, 19, 7, cl);
    for (hclen = 19; hclen > 4 && !cl[cl_order[hclen - 1]]; hclen--)
        ;
    hdr = 14 + 3L * hclen;
    for (i = 0; i < nseq; i++)
        hdr += cl[seq[i]] + (seq[i] == 16 ? 2 : seq[i] == 17 ? 3 : seq[i] == 18 ? 7 : 0);
    dyn = hdr + block_cost(d, ll, dl);
    fixed_lens(fl, fd);
    fix = block_cost(d, fl, fd);

    bitw_put(&d->w, final ? 1 : 0, 1);
    if (fix <= dyn) {
        bitw_put(&d->w, 1, 2);
        write_tokens(d, fl, fd);
    } else {
        bitw_put(&d->w, 2, 2);
        bitw_put(&d->w, (unsigned)(hlit - 257), 5);
        bitw_put(&d->w, (unsigned)(hdist - 1), 5);
        bitw_put(&d->w, (unsigned)(hclen - 4), 4);
        for (i = 0; i < hclen; i++)
            bitw_put(&d->w, cl[cl_order[i]], 3);
        huff_codes(cl, 19, cl_codes);
        for (i = 0; i < nseq; i++) {
            put_huff(&d->w, cl_codes[seq[i]], cl[seq[i]]);
            if (seq[i] == 16) bitw_put(&d->w, ext[i], 2);
            else if (seq[i] == 17) bitw_put(&d->w, ext[i], 3);
            else if (seq[i] == 18) bitw_put(&d->w, ext[i], 7);
        }
        write_tokens(d, ll, dl);
    }
    d->ntok = 0;
}

static void add_token(DEFL *d, int lit_or_len, long dist)
{
    d->lit[d->ntok] = (u16)(dist ? lit_or_len + 256 : lit_or_len);
    d->dist[d->ntok] = (u16)dist;
    if (++d->ntok == TOKENS)
        flush_block(d, 0);
}

/* ------------------------------------------------------------- LZ77 */

static unsigned hash3(DEFL *d, long pos)
{
    const u8 *r = d->ring;
    u32 v = ((u32)r[pos & RMASK] << 16) | ((u32)r[(pos + 1) & RMASK] << 8) | r[(pos + 2) & RMASK];
    return (unsigned)(((v * 2654435761UL) & 0xFFFFFFFFUL) >> (32 - HBITS)) & (HSIZE - 1);
}

static void insert(DEFL *d, long pos)
{
    unsigned h = hash3(d, pos);
    d->prev[pos & (WINDOW - 1)] = d->head[h];
    d->head[h] = pos;
}

static int longest(DEFL *d, long pos, long avail, long *dist)
{
    const u8 *r = d->ring;
    long cand = d->head[hash3(d, pos)], best = 0, maxlen = avail < MAXMATCH ? avail : MAXMATCH;
    int chain = d->chain;
    while (cand != NIL && pos - cand < WINDOW && chain--) {
        if (r[(cand + best) & RMASK] == r[(pos + best) & RMASK]) {
            long n = 0;
            while (n < maxlen && r[(cand + n) & RMASK] == r[(pos + n) & RMASK])
                n++;
            if (n > best) {
                best = n;
                *dist = pos - cand;
                if (n == maxlen)
                    break;
            }
        }
        cand = d->prev[cand & (WINDOW - 1)];
    }
    return (int)best;
}

static void run(DEFL *d, int final)
{
    long limit = final ? d->inpos : d->inpos - MAXMATCH;
    while (d->cur < limit) {
        long pos = d->cur, avail = d->inpos - pos, dist = 0;
        int len = avail >= MINMATCH ? longest(d, pos, avail, &dist) : 0;
        if (len < MINMATCH || (len == MINMATCH && dist > 4096))
            len = 0;
        if (d->have_pend) {
            /* lazy: keep the previous match unless this one is longer */
            if (len > d->pend_len) {
                add_token(d, d->pend_lit, 0);       /* previous byte as a literal */
                d->pend_len = len;
                d->pend_dist = dist;
                d->pend_lit = d->ring[pos & RMASK];
                if (avail >= MINMATCH) insert(d, pos);
                d->cur++;
                continue;
            }
            {
                long k, start = pos - 1;
                add_token(d, d->pend_len, d->pend_dist);
                for (k = 1; k < d->pend_len; k++)
                    if (d->inpos - (start + k) >= MINMATCH)
                        insert(d, start + k);
                d->cur = start + d->pend_len;
                d->have_pend = 0;
                continue;
            }
        }
        if (len >= MINMATCH) {
            d->have_pend = 1;
            d->pend_len = len;
            d->pend_dist = dist;
            d->pend_lit = d->ring[pos & RMASK];
            insert(d, pos);
            d->cur++;
        } else {
            add_token(d, d->ring[pos & RMASK], 0);
            if (avail >= MINMATCH) insert(d, pos);
            d->cur++;
        }
    }
    if (final && d->have_pend) {
        add_token(d, d->pend_len, d->pend_dist);
        d->have_pend = 0;
    }
}

DEFL *defl_new(BOUT *out, int level)
{
    DEFL *d = calloc(1, sizeof(DEFL));
    long i;
    if (!d)
        return 0;
    d->ring = malloc(RING);
    d->head = malloc(HSIZE * sizeof(long));
    d->prev = malloc(WINDOW * sizeof(long));
    d->lit = malloc(TOKENS * sizeof(u16));
    d->dist = malloc(TOKENS * sizeof(u16));
    if (!d->ring || !d->head || !d->prev || !d->lit || !d->dist) {
        defl_free(d);
        return 0;
    }
    for (i = 0; i < HSIZE; i++) d->head[i] = NIL;
    for (i = 0; i < WINDOW; i++) d->prev[i] = NIL;
    d->chain = level <= 1 ? 8 : level >= 3 ? 128 : 32;
    bitw_init(&d->w, out, 0);
    return d;
}

void defl_put(DEFL *d, const u8 *p, long n)
{
    while (n > 0) {
        long room = RING - WINDOW - (d->inpos - d->cur), k;
        if (room > n) room = n;
        if (room <= 0) {
            run(d, 0);
            continue;
        }
        for (k = 0; k < room; k++)
            d->ring[(d->inpos + k) & RMASK] = p[k];
        d->inpos += room;
        p += room;
        n -= room;
        run(d, 0);
    }
}

void defl_end(DEFL *d)
{
    run(d, 1);
    flush_block(d, 1);
    bitw_flush(&d->w);
}

void defl_free(DEFL *d)
{
    if (!d)
        return;
    free(d->ring);
    free(d->head);
    free(d->prev);
    free(d->lit);
    free(d->dist);
    free(d);
}

int deflate_level = 2;                  /* for deflate_stream: 1 .. 3     */

/* raw deflate of a whole stream */
int deflate_stream(BIN *in, BOUT *out, long usize, PROG *pg)
{
    u8 buf[4096];
    long done = 0, got;
    DEFL *d = defl_new(out, deflate_level);
    if (!d)
        return E_NOMEM;
    while ((got = bin_read(in, buf, sizeof buf)) > 0) {
        defl_put(d, buf, got);
        done += got;
        if (codec_progress(pg, done, usize)) {
            defl_free(d);
            return E_CANCEL;
        }
        if (out->err)
            break;
    }
    defl_end(d);
    defl_free(d);
    return out->err;
}
