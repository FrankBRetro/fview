/*
 * lzx.c - Amiga LZX (1.21) compression stream: decoder and compressor.
 *
 * The stream is a run of blocks. Bits come in big-endian 16 bit words,
 * least significant bit first. Each block starts with
 *    3 bits    type: 1 reuse the trees, 2 verbatim, 3 aligned offsets
 *    (type 3)  8 x 3 bits aligned-offset code lengths
 *    24 bits   bytes the block decodes to (high byte first)
 *    (2, 3)    main tree (768 symbols) as two pretree-coded sections
 * Main symbols: 0..255 literals, 256 + (length slot << 5) + position slot.
 * Position slot 0 repeats the last distance. Window 64K.
 */
#include <stdlib.h>
#include <string.h>
#include "arcio.h"

#define NMAIN    768
#define NLIT     256
#define NALIGN   8
#define NPRE     20
#define WSIZE    0x10000L
#define MINMATCH 3
#define MAXMATCH 258
#define MAXDIST  0xFEFCL

static const u8 tab_bits[32] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14 };
static const u16 tab_base[32] = {
    0, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192,
    256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192, 12288,
    16384, 24576, 32768, 49152 };

/* pretree run parameters: [section][0 = sym17 base, 1 = sym18 base, 2 = sym18 bits] */
static const u8 sect_par[2][3] = { { 4, 20, 5 }, { 3, 19, 6 } };

/* =========================================================== decoder */

static int read_section(BITS *b, HUFF *pre, u8 *len, int n, int sect)
{
    u8 pl[NPRE];
    int i = 0, k;
    for (k = 0; k < NPRE; k++)
        pl[k] = (u8)bits_get(b, 4);
    if (!huff_build(pre, pl, NPRE, 0))
        return E_FORMAT;
    while (i < n) {
        int sym = huff_decode(pre, b), run, v;
        if (sym < 0 || b->over > 32)
            return E_FORMAT;
        if (sym <= 16) {
            len[i] = (u8)((len[i] + 17 - sym) % 17);
            i++;
            continue;
        }
        if (sym == 17) {
            run = sect_par[sect][0] + (int)bits_get(b, 4);
            v = -1;
        } else if (sym == 18) {
            run = sect_par[sect][1] + (int)bits_get(b, sect_par[sect][2]);
            v = -1;
        } else {
            int d;
            run = sect_par[sect][0] + (int)bits_get(b, 1);
            d = huff_decode(pre, b);
            if (d < 0 || d > 16)
                return E_FORMAT;
            v = (len[i] + 17 - d) % 17;
        }
        while (run-- > 0 && i < n)
            len[i++] = (u8)(v < 0 ? 0 : v);
    }
    return E_OK;
}

int lzx_unpack(BIN *in, BOUT *out, long usize, PROG *pg)
{
    BITS b;
    u8 *win = malloc(WSIZE);
    HUFF *mt = malloc(sizeof(HUFF)), *at = malloc(sizeof(HUFF)), *pt = malloc(sizeof(HUFF));
    u8 mlen[NMAIN], alen[NALIGN];
    long done = 0, left = 0, next = 0;
    unsigned wpos = 0;
    u32 last = 1;
    int type = 0, primed = 0, e = E_OK;
    if (!win || !mt || !at || !pt) {
        e = E_NOMEM;
        goto out;
    }
    memset(mlen, 0, sizeof mlen);
    memset(alen, 0, sizeof alen);
    bits_init(&b, in, 2);
    while (usize < 0 || done < usize) {
        int sym;
        if (left <= 0) {
            int k;
            type = (int)bits_get(&b, 3);
            if (b.over) {
                if (usize < 0)
                    break;               /* the stream just ended */
                e = E_FORMAT;
                break;
            }
            if (type < 1 || type > 3 || (type == 1 && !primed)) {
                e = E_FORMAT;
                break;
            }
            if (type == 3) {
                for (k = 0; k < NALIGN; k++)
                    alen[k] = (u8)bits_get(&b, 3);
                if (!huff_build(at, alen, NALIGN, 0)) {
                    e = E_FORMAT;
                    break;
                }
            }
            left = (long)bits_get(&b, 8) << 16;
            left |= (long)bits_get(&b, 8) << 8;
            left |= (long)bits_get(&b, 8);
            if (type != 1) {
                if ((e = read_section(&b, pt, mlen, NLIT, 0)) != E_OK ||
                    (e = read_section(&b, pt, mlen + NLIT, NMAIN - NLIT, 1)) != E_OK)
                    break;
                if (!huff_build(mt, mlen, NMAIN, 0)) {
                    e = E_FORMAT;
                    break;
                }
            }
            primed = 1;
            if (left == 0)
                continue;
        }
        sym = huff_decode(mt, &b);
        if (sym < 0 || b.over > 32) {
            e = E_FORMAT;
            break;
        }
        if (sym < NLIT) {
            win[wpos] = (u8)sym;
            wpos = (wpos + 1) & (WSIZE - 1);
            bout_byte(out, sym);
            left--;
            done++;
        } else {
            int ps = (sym - NLIT) & 31, ls = ((sym - NLIT) >> 5) & 15, pb = tab_bits[ps], len;
            u32 d = tab_base[ps];
            if (pb >= 3 && type == 3) {
                int a;
                d += (u32)bits_get(&b, pb - 3) << 3;
                a = huff_decode(at, &b);
                if (a < 0) {
                    e = E_FORMAT;
                    break;
                }
                d += (u32)a;
            } else if (pb)
                d += bits_get(&b, pb);
            if (d == 0)
                d = last;
            last = d;
            len = tab_base[ls] + MINMATCH;
            if (tab_bits[ls])
                len += (int)bits_get(&b, tab_bits[ls]);
            left -= len;
            done += len;
            while (len--) {
                int c = win[(wpos - d) & (WSIZE - 1)];
                win[wpos] = (u8)c;
                wpos = (wpos + 1) & (WSIZE - 1);
                bout_byte(out, c);
            }
        }
        if (done >= next) {
            next = done + 0x3FFF;
            if ((e = codec_progress(pg, done, usize)) != E_OK)
                break;
        }
    }
    if (!e && usize >= 0 && done != usize)
        e = E_FORMAT;
out:
    free(win);
    free(mt);
    free(at);
    free(pt);
    if (!e && out->err)
        e = out->err;
    return e;
}

/* ========================================================== compressor */

#define TOKMAX 0x4000
#define HBITS  13
#define HSIZE  (1 << HBITS)
#define BUFSZ  (2 * WSIZE + MAXMATCH + 8)
#define CHAIN  48

typedef struct {
    BIN   *in;
    BITW   w;
    PROG  *pg;
    long   usize, next;
    /* window: buf[0..avail) holds stream bytes base .. base + avail */
    u8    *buf;
    long   base, avail, pos;          /* pos: absolute position of the scan */
    int    eof;
    long  *head;                        /* absolute positions, -1 none     */
    u16   *prev;                        /* distance to the previous one     */
    /* tokens of the current block */
    u16   *tlen, *tdist;                /* tlen 0: literal tdist           */
    int    ntok;
    long   bbytes;
    u32    lastoff;                     /* at the start of this block       */
    u32    curoff;                      /* while matching                   */
    u8     mlen[NMAIN];                 /* the previous block's main tree   */
} LZXE;

static int pos_slot(u32 d)
{
    int s = 0;
    while (s < 31 && tab_base[s + 1] <= d)
        s++;
    return s;
}

static int len_slot(int l)
{
    int s = 0;
    l -= MINMATCH;
    while (s < 15 && tab_base[s + 1] <= (u32)l)
        s++;
    return s;
}

static unsigned rev(unsigned c, int n)
{
    unsigned r = 0;
    while (n--) {
        r = (r << 1) | (c & 1);
        c >>= 1;
    }
    return r;
}

/* Huffman lengths for a tree every LZX decoder accepts: complete, two
   codes at least */
static void tree_lengths(u32 *freq, int n, int maxlen, u8 *len)
{
    int i, used = 0;
    for (i = 0; i < n; i++)
        if (freq[i])
            used++;
    for (i = 0; used < 2 && i < n; i++)
        if (!freq[i]) {
            freq[i] = 1;
            used++;
        }
    huff_lengths(freq, n, maxlen, len);
}

/* token -> main symbol, footer, aligned symbol; updates *last */
typedef struct { int sym, fbits, aligned; u32 foot; } TSYM;

static void tok_sym(int l, u32 d, u32 *last, TSYM *t)
{
    if (!l) {
        t->sym = (int)d;
        t->fbits = 0;
        t->aligned = -1;
        return;
    }
    {
        int ps, ls = len_slot(l);
        if (d == *last) {
            ps = 0;
            t->fbits = 0;
            t->foot = 0;
        } else {
            ps = pos_slot(d);
            t->fbits = tab_bits[ps];
            t->foot = d - tab_base[ps];
        }
        *last = d;
        t->aligned = t->fbits >= 3 ? (int)(t->foot & 7) : -1;
        t->sym = NLIT + (ls << 5) + ps;
        t->foot |= (u32)(l - MINMATCH - tab_base[ls]) << 16;   /* length footer */
    }
}

static void put_code(LZXE *z, const u16 *codes, const u8 *lens, int s)
{
    bitw_put(&z->w, rev(codes[s], lens[s]), lens[s]);
}

static void write_section(LZXE *z, const u8 *prev, const u8 *cur, int n, int sect)
{
    /* two passes: count the pretree symbols, then write them */
    u32 freq[NPRE];
    u8 pl[NPRE];
    u16 pc[NPRE];
    int pass, i, k;
    int b17 = sect_par[sect][0], b18 = sect_par[sect][1], x18 = sect_par[sect][2];
    int max17 = b17 + 15, max18 = b18 + (1 << x18) - 1, max19 = b17 + 1;
    memset(freq, 0, sizeof freq);
    for (pass = 0; pass < 2; pass++) {
        if (pass) {
            tree_lengths(freq, NPRE, 15, pl);
            huff_codes(pl, NPRE, pc);
            for (k = 0; k < NPRE; k++)
                bitw_put(&z->w, pl[k], 4);
        }
        i = 0;
        while (i < n) {
            int c = cur[i], run = 1;
            while (i + run < n && cur[i + run] == c && run < max18)
                run++;
            if (run >= b17) {
                if (c == 0) {
                    if (run <= max17) {
                        if (pass) { put_code(z, pc, pl, 17); bitw_put(&z->w, run - b17, 4); }
                        else freq[17]++;
                    } else {
                        if (pass) { put_code(z, pc, pl, 18); bitw_put(&z->w, run - b18, x18); }
                        else freq[18]++;
                    }
                    i += run;
                    continue;
                }
                {
                    int r = run < max19 ? run : max19;
                    int d = (prev[i] + 17 - c) % 17;
                    if (pass) {
                        put_code(z, pc, pl, 19);
                        bitw_put(&z->w, r - b17, 1);
                        put_code(z, pc, pl, d);
                    } else {
                        freq[19]++;
                        freq[d]++;
                    }
                    i += r;
                    continue;
                }
            }
            {
                int d = (prev[i] + 17 - c) % 17;
                if (pass) put_code(z, pc, pl, d);
                else freq[d]++;
                i++;
            }
        }
    }
}

static void flush_block(LZXE *z)
{
    u32 mf[NMAIN], af[NALIGN], last;
    u8 ml[NMAIN], al[NALIGN];
    u16 mc[NMAIN], ac[NALIGN];
    long cost_v = 0, cost_a = 24;
    int i, type, naligned = 0;
    TSYM t;
    if (!z->ntok)
        return;
    memset(mf, 0, sizeof mf);
    memset(af, 0, sizeof af);
    last = z->lastoff;
    for (i = 0; i < z->ntok; i++) {
        tok_sym(z->tlen[i], z->tdist[i], &last, &t);
        mf[t.sym]++;
        if (t.aligned >= 0) {
            af[t.aligned]++;
            naligned++;
        }
    }
    tree_lengths(mf, NMAIN, 16, ml);
    huff_codes(ml, NMAIN, mc);
    type = 2;
    if (naligned > 64) {
        tree_lengths(af, NALIGN, 7, al);
        for (i = 0; i < NALIGN; i++) {
            cost_v += (long)af[i] * 3;
            cost_a += (long)af[i] * al[i];
        }
        if (cost_a < cost_v)
            type = 3;
        huff_codes(al, NALIGN, ac);
    }
    bitw_put(&z->w, (unsigned)type, 3);
    if (type == 3)
        for (i = 0; i < NALIGN; i++)
            bitw_put(&z->w, al[i], 3);
    bitw_put(&z->w, (unsigned)(z->bbytes >> 16) & 0xFF, 8);
    bitw_put(&z->w, (unsigned)(z->bbytes >> 8) & 0xFF, 8);
    bitw_put(&z->w, (unsigned)z->bbytes & 0xFF, 8);
    write_section(z, z->mlen, ml, NLIT, 0);
    write_section(z, z->mlen + NLIT, ml + NLIT, NMAIN - NLIT, 1);
    memcpy(z->mlen, ml, NMAIN);
    last = z->lastoff;
    for (i = 0; i < z->ntok; i++) {
        tok_sym(z->tlen[i], z->tdist[i], &last, &t);
        put_code(z, mc, ml, t.sym);
        if (t.sym >= NLIT) {
            u32 pf = t.foot & 0xFFFF;
            int ls = ((t.sym - NLIT) >> 5) & 15;
            if (type == 3 && t.fbits >= 3) {
                bitw_put(&z->w, (unsigned)(pf >> 3), t.fbits - 3);
                put_code(z, ac, al, (int)(pf & 7));
            } else if (t.fbits)
                bitw_put(&z->w, (unsigned)pf, t.fbits);
            if (tab_bits[ls])
                bitw_put(&z->w, (unsigned)(t.foot >> 16), tab_bits[ls]);
        }
    }
    z->lastoff = last;
    z->ntok = 0;
    z->bbytes = 0;
}

static void add_tok(LZXE *z, int l, u32 d)
{
    z->tlen[z->ntok] = (u16)l;
    z->tdist[z->ntok] = (u16)d;
    z->ntok++;
    z->bbytes += l ? l : 1;
    if (l)
        z->curoff = d;
    if (z->ntok >= TOKMAX)
        flush_block(z);
}

/* keep at least MAXMATCH bytes ahead of pos in the buffer */
static void fill(LZXE *z)
{
    if (z->eof || z->pos + MAXMATCH < z->base + z->avail)
        return;
    {
        long drop = z->pos - z->base - WSIZE;   /* keep a full window behind */
        if (drop > 0) {
            memmove(z->buf, z->buf + drop, z->avail - drop);
            z->base += drop;
            z->avail -= drop;
        }
    }
    while (!z->eof && z->avail < BUFSZ - 8) {
        long got = bin_read(z->in, z->buf + z->avail, BUFSZ - 8 - z->avail);
        if (got <= 0)
            z->eof = 1;
        else
            z->avail += got;
        if (z->pos + MAXMATCH < z->base + z->avail)
            break;
    }
}

#define HASH(p) ((((u32)(p)[0] << 10) ^ ((u32)(p)[1] << 5) ^ (p)[2]) & (HSIZE - 1))

static void insert(LZXE *z, long ap)
{
    const u8 *p = z->buf + (ap - z->base);
    u32 h;
    long o, dd;
    if (ap + 3 > z->base + z->avail)
        return;
    h = HASH(p);
    o = z->head[h];
    dd = o < 0 ? 0 : ap - o;
    z->prev[ap & (WSIZE - 1)] = (u16)(dd > MAXDIST ? 0 : dd);
    z->head[h] = ap;
}

static int match_len(const u8 *a, const u8 *b, int max)
{
    int n = 0;
    while (n < max && a[n] == b[n])
        n++;
    return n;
}

/* longest match at ap (not yet inserted) */
static int find(LZXE *z, long ap, u32 *dist)
{
    const u8 *p = z->buf + (ap - z->base);
    long lim = z->base + z->avail - ap, o;
    int best = 0, chain = CHAIN, max = lim < MAXMATCH ? (int)lim : MAXMATCH;
    u32 h;
    if (max < MINMATCH)
        return 0;
    /* the repeat distance is the cheapest of all */
    if (z->curoff && (long)z->curoff <= ap - z->base) {
        int l = match_len(p, p - z->curoff, max);
        if (l >= MINMATCH) {
            best = l;
            *dist = z->curoff;
            if (l == max)
                return l;
        }
    }
    h = HASH(p);
    o = z->head[h];
    while (o >= 0 && chain-- > 0) {
        long d = ap - o;
        u16 pd;
        if (d > MAXDIST || o < z->base)
            break;
        if (p[best] == z->buf[o - z->base + best]) {
            int l = match_len(p, z->buf + (o - z->base), max);
            int need = best >= MINMATCH && *dist == z->curoff ? best + 2 : best + 1;
            if (l >= need && l >= MINMATCH) {
                best = l;
                *dist = (u32)d;
                if (l == max)
                    break;
            }
        }
        pd = z->prev[o & (WSIZE - 1)];
        if (!pd)
            break;
        o -= pd;
    }
    /* a 3 byte match far away costs more than its literals */
    if (best == MINMATCH && *dist > 4096 && *dist != z->curoff)
        best = 0;
    return best;
}

int lzx_pack(BIN *in, BOUT *out, long usize, PROG *pg)
{
    LZXE *z = calloc(1, sizeof(LZXE));
    int e = E_OK, i;
    if (!z)
        return E_NOMEM;
    z->buf = malloc(BUFSZ);
    z->head = malloc(HSIZE * sizeof(long));
    z->prev = malloc(WSIZE * sizeof(u16));
    z->tlen = malloc(TOKMAX * sizeof(u16));
    z->tdist = malloc(TOKMAX * sizeof(u16));
    if (!z->buf || !z->head || !z->prev || !z->tlen || !z->tdist) {
        e = E_NOMEM;
        goto out;
    }
    for (i = 0; i < HSIZE; i++)
        z->head[i] = -1;
    z->in = in;
    z->pg = pg;
    z->usize = usize;
    z->lastoff = z->curoff = 1;
    bitw_init(&z->w, out, 2);
    for (;;) {
        int l;
        u32 d = 0;
        fill(z);
        if (z->pos >= z->base + z->avail)
            break;
        l = find(z, z->pos, &d);
        insert(z, z->pos);
        if (l >= MINMATCH && l < 32 && z->pos + 1 < z->base + z->avail) {
            /* lazy: a longer match one byte on? */
            u32 d2 = 0;
            int l2 = find(z, z->pos + 1, &d2);
            if (l2 > l + 1 || (l2 > l && d2 == z->curoff)) {
                add_tok(z, 0, z->buf[z->pos - z->base]);
                z->pos++;
                insert(z, z->pos);
                l = l2;
                d = d2;
            }
        }
        if (l >= MINMATCH) {
            long k;
            add_tok(z, l, d);
            for (k = 1; k < l; k++)
                insert(z, z->pos + k);
            z->pos += l;
        } else {
            add_tok(z, 0, z->buf[z->pos - z->base]);
            z->pos++;
        }
        if (z->pos >= z->next) {
            z->next = z->pos + 0x3FFF;
            if ((e = codec_progress(pg, z->pos, usize)) != E_OK)
                goto out;
        }
    }
    flush_block(z);
    bitw_flush(&z->w);
out:
    if (z) {
        free(z->buf);
        free(z->head);
        free(z->prev);
        free(z->tlen);
        free(z->tdist);
        free(z);
    }
    return e ? e : out->err;
}
