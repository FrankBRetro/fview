/*
 * lzh.c - LHA family codecs.
 *
 * Decoding: -lh1- (LZHUF: 4K window, adaptive Huffman, Yoshizaki and
 * Okumura's algorithm), -lh4- .. -lh7- (static Huffman blocks, windows of
 * 4K to 64K), LArc -lzs- and -lz5-. ZOO's method 2 is -lh5- as well.
 * Encoding: -lh5- (8K window).
 * All of these read bits most significant first.
 */
#include <stdlib.h>
#include <string.h>
#include "arcio.h"

/* ------------------------------------------------------ output window */

typedef struct {
    u8   *win;
    long  size, mask, pos;     /* pos counts every byte written      */
    BOUT *out;
    long  usize;
    PROG *pg;
} LWIN;

static int lw_init(LWIN *w, long size, int fill, BOUT *out, long usize, PROG *pg)
{
    w->win = malloc(size);
    if (!w->win)
        return E_NOMEM;
    memset(w->win, fill, size);
    w->size = size;
    w->mask = size - 1;
    w->pos = 0;
    w->out = out;
    w->usize = usize;
    w->pg = pg;
    return E_OK;
}

static int lw_full(LWIN *w)
{
    return w->usize >= 0 && w->pos >= w->usize;
}

static void lw_put(LWIN *w, int c)
{
    if (lw_full(w))
        return;
    w->win[w->pos & w->mask] = (u8)c;
    w->pos++;
    bout_byte(w->out, c);
}

/* copy len bytes from 'back' bytes behind the end (back 1 = last byte) */
static void lw_copy(LWIN *w, long back, int len)
{
    while (len-- > 0)
        lw_put(w, w->win[(w->pos - back) & w->mask]);
}

/* copy from an absolute window index (LArc) */
static void lw_copy_abs(LWIN *w, long idx, int len)
{
    while (len-- > 0) {
        int c = w->win[idx & w->mask];
        idx++;
        lw_put(w, c);
    }
}

static int lw_prog(LWIN *w)
{
    if ((w->pos & 0x3FFF) < 300)
        return codec_progress(w->pg, w->pos, w->usize);
    return E_OK;
}

/* -------------------------------------------------- -lh1- (LZHUF) */

#define H1_NCHAR (256 + 58)
#define H1_T     (H1_NCHAR * 2 - 1)
#define H1_R     (H1_T - 1)
#define H1_MAXF  0x8000

typedef struct {
    u16 freq[H1_T + 1];
    u16 prnt[H1_T + H1_NCHAR];
    u16 son[H1_T];
} H1;

static void h1_start(H1 *h)
{
    int i, j;
    for (i = 0; i < H1_NCHAR; i++) {
        h->freq[i] = 1;
        h->son[i] = (u16)(i + H1_T);
        h->prnt[i + H1_T] = (u16)i;
    }
    i = 0;
    j = H1_NCHAR;
    while (j <= H1_R) {
        h->freq[j] = (u16)(h->freq[i] + h->freq[i + 1]);
        h->son[j] = (u16)i;
        h->prnt[i] = h->prnt[i + 1] = (u16)j;
        i += 2;
        j++;
    }
    h->freq[H1_T] = 0xFFFF;
    h->prnt[H1_R] = 0;
}

static void h1_reconst(H1 *h)
{
    int i, j, k, l;
    unsigned f;
    for (i = j = 0; i < H1_T; i++)
        if (h->son[i] >= H1_T) {
            h->freq[j] = (u16)((h->freq[i] + 1) / 2);
            h->son[j] = h->son[i];
            j++;
        }
    for (i = 0, j = H1_NCHAR; j < H1_T; i += 2, j++) {
        k = i + 1;
        f = h->freq[j] = (u16)(h->freq[i] + h->freq[k]);
        for (k = j - 1; f < h->freq[k]; k--)
            ;
        k++;
        l = j - k;
        memmove(&h->freq[k + 1], &h->freq[k], l * sizeof(u16));
        h->freq[k] = (u16)f;
        memmove(&h->son[k + 1], &h->son[k], l * sizeof(u16));
        h->son[k] = (u16)i;
    }
    for (i = 0; i < H1_T; i++) {
        k = h->son[i];
        h->prnt[k] = (u16)i;
        if (k < H1_T)
            h->prnt[k + 1] = (u16)i;
    }
}

static void h1_update(H1 *h, int c)
{
    int i, j, k, l;
    if (h->freq[H1_R] == H1_MAXF)
        h1_reconst(h);
    c = h->prnt[c + H1_T];
    do {
        k = ++h->freq[c];
        l = c + 1;
        if ((unsigned)k > h->freq[l]) {
            while ((unsigned)k > h->freq[++l])
                ;
            l--;
            h->freq[c] = h->freq[l];
            h->freq[l] = (u16)k;
            i = h->son[c];
            h->prnt[i] = (u16)l;
            if (i < H1_T) h->prnt[i + 1] = (u16)l;
            j = h->son[l];
            h->son[l] = (u16)i;
            h->prnt[j] = (u16)c;
            if (j < H1_T) h->prnt[j + 1] = (u16)c;
            h->son[c] = (u16)j;
            c = l;
        }
    } while ((c = h->prnt[c]) != 0);
}

static void h1_dcode(unsigned n, unsigned *code, unsigned *len)
{
    if (n < 32)       { *code = 0; *len = 3; }
    else if (n < 80)  { *code = (n - 16) >> 4; *len = 4; }
    else if (n < 144) { *code = (n - 48) >> 3; *len = 5; }
    else if (n < 192) { *code = (n - 96) >> 2; *len = 6; }
    else if (n < 240) { *code = (n - 144) >> 1; *len = 7; }
    else              { *code = n - 192; *len = 8; }
}

static int unpack_lh1(BITS *b, LWIN *w)
{
    H1 *h = malloc(sizeof(H1));
    int e = E_OK;
    if (!h)
        return E_NOMEM;
    h1_start(h);
    while (!lw_full(w)) {
        unsigned c = h->son[H1_R];
        while (c < H1_T)
            c = h->son[c + bits_get(b, 1)];
        c -= H1_T;
        h1_update(h, (int)c);
        if (c < 256)
            lw_put(w, (int)c);
        else {
            unsigned i = bits_get(b, 8), code, len, j;
            h1_dcode(i, &code, &len);
            j = len - 2;
            i = ((i << j) | bits_get(b, (int)j)) & 0x3F;
            lw_copy(w, (long)((code << 6) | i) + 1, (int)(c - 253));
        }
        if (b->over > 64) { e = E_FORMAT; break; }
        if ((e = lw_prog(w)) != E_OK) break;
    }
    free(h);
    return e;
}

/* --------------------------------------- -lh4- .. -lh7- (static) */

#define NC   510                 /* 256 literals + lengths 3..256     */
#define NT   19
#define TBIT 5
#define CBIT 9

typedef struct {
    BITS *b;
    HUFF  c, p, t;
    int   csingle, psingle, tsingle;   /* one code only: no bits used   */
    int   np, pbit;
} LHS;

/* the t (code length code) or p (position) table */
static int read_pt_len(LHS *s, int nn, int nbit, int ispecial, HUFF *h, int *single)
{
    u8 len[NT > 20 ? NT : 20];
    int n = (int)bits_get(s->b, nbit), i = 0;
    memset(len, 0, sizeof len);
    if (n == 0) {
        *single = (int)bits_get(s->b, nbit);
        return E_OK;
    }
    *single = -1;
    if (n > nn)
        return E_FORMAT;
    while (i < n) {
        int c = (int)bits_get(s->b, 3);
        if (c == 7) {
            while (bits_get(s->b, 1)) {
                c++;
                if (c > 16)
                    return E_FORMAT;
            }
        }
        len[i++] = (u8)c;
        if (i == ispecial) {
            int z = (int)bits_get(s->b, 2);
            while (z-- > 0 && i < nn)
                len[i++] = 0;
        }
    }
    return huff_build(h, len, nn, 1) ? E_OK : E_FORMAT;
}

static int read_c_len(LHS *s)
{
    u8 len[NC];
    int n = (int)bits_get(s->b, CBIT), i = 0;
    memset(len, 0, sizeof len);
    if (n == 0) {
        s->csingle = (int)bits_get(s->b, CBIT);
        return E_OK;
    }
    s->csingle = -1;
    if (n > NC)
        return E_FORMAT;
    while (i < n) {
        int c = s->tsingle >= 0 ? s->tsingle : huff_decode(&s->t, s->b);
        if (c < 0)
            return E_FORMAT;
        if (c <= 2) {
            int z = c == 0 ? 1 : c == 1 ? (int)bits_get(s->b, 4) + 3 : (int)bits_get(s->b, CBIT) + 20;
            while (z-- > 0 && i < NC)
                len[i++] = 0;
        } else
            len[i++] = (u8)(c - 2);
    }
    return huff_build(&s->c, len, NC, 1) ? E_OK : E_FORMAT;
}

static int unpack_static(BITS *b, LWIN *w, int dicbit)
{
    LHS *s = malloc(sizeof(LHS));
    long blocksize = 0;
    int e = E_OK;
    if (!s)
        return E_NOMEM;
    memset(s, 0, sizeof *s);
    s->b = b;
    s->np = dicbit + 1;
    s->pbit = dicbit <= 13 ? 4 : 5;
    while (!lw_full(w)) {
        int c;
        if (blocksize == 0) {
            blocksize = (long)bits_get(b, 16);
            if (b->over || blocksize == 0) { e = E_FORMAT; break; }
            if ((e = read_pt_len(s, NT, TBIT, 3, &s->t, &s->tsingle)) != E_OK) break;
            if ((e = read_c_len(s)) != E_OK) break;
            if ((e = read_pt_len(s, s->np, s->pbit, -1, &s->p, &s->psingle)) != E_OK) break;
        }
        blocksize--;
        c = s->csingle >= 0 ? s->csingle : huff_decode(&s->c, b);
        if (c < 0 || c >= NC) { e = E_FORMAT; break; }
        if (c < 256)
            lw_put(w, c);
        else {
            int j = s->psingle >= 0 ? s->psingle : huff_decode(&s->p, b);
            long d;
            if (j < 0) { e = E_FORMAT; break; }
            d = j == 0 ? 0 : (1L << (j - 1)) + (long)bits_get(b, j - 1);
            lw_copy(w, d + 1, c - 256 + 3);
        }
        if (b->over > 64) { e = E_FORMAT; break; }
        if ((e = lw_prog(w)) != E_OK) break;
    }
    free(s);
    return e;
}

/* ------------------------------------------------------ LArc lzs, lz5 */

static int unpack_lzs(BITS *b, LWIN *w)
{
    while (!lw_full(w)) {
        if (bits_get(b, 1))
            lw_put(w, (int)bits_get(b, 8));
        else {
            long matchpos = (long)bits_get(b, 11);
            int len = (int)bits_get(b, 4) + 2;
            lw_copy_abs(w, (w->pos - ((w->pos - matchpos - 18) & 0x7FF) - 1), len);
        }
        if (b->over > 64)
            return E_FORMAT;
        if (lw_prog(w))
            return E_CANCEL;
    }
    return E_OK;
}

static void lz5_fill(u8 *t)
{
    int i;
    memset(t, 0, 18);                  /* the ring's last 18 bytes, wrapped */
    for (i = 0; i < 256; i++)
        memset(&t[i * 13 + 18], i, 13);
    for (i = 0; i < 256; i++)
        t[256 * 13 + 18 + i] = (u8)i;
    for (i = 0; i < 256; i++)
        t[256 * 13 + 256 + 18 + i] = (u8)(255 - i);
    memset(&t[256 * 13 + 512 + 18], 0, 128);
    memset(&t[256 * 13 + 512 + 128 + 18], ' ', 128 - 18);
}

static int unpack_lz5(BIN *in, LWIN *w)
{
    int flags = 0, nflag = 0;
    while (!lw_full(w)) {
        if (nflag == 0) {
            flags = bin_byte(in);
            if (flags < 0)
                return E_FORMAT;
            nflag = 8;
        }
        nflag--;
        if (flags & 1) {
            int c = bin_byte(in);
            if (c < 0) return E_FORMAT;
            lw_put(w, c);
        } else {
            int b1 = bin_byte(in), b2 = bin_byte(in);
            long matchpos;
            if (b1 < 0 || b2 < 0) return E_FORMAT;
            matchpos = b1 | ((b2 & 0xF0) << 4);
            lw_copy_abs(w, (w->pos - ((w->pos - matchpos - 19) & 0xFFF) - 1), (b2 & 0x0F) + 3);
        }
        flags >>= 1;
        if (lw_prog(w))
            return E_CANCEL;
    }
    return E_OK;
}

int lzh_unpack(int method, BIN *in, BOUT *out, long usize, PROG *pg)
{
    LWIN w;
    BITS b;
    int e, dicbit = method == CM_LH4 ? 12 : method == CM_LH5 ? 13 : method == CM_LH6 ? 15 :
                    method == CM_LH7 ? 16 : method == CM_LZS ? 11 : 12;
    e = lw_init(&w, 1L << dicbit, ' ', out, usize, pg);
    if (e)
        return e;
    bits_init(&b, in, 1);
    if (method == CM_LZ5) {
        lz5_fill(w.win);
        e = unpack_lz5(in, &w);
    } else if (method == CM_LZS)
        e = unpack_lzs(&b, &w);
    else if (method == CM_LH1)
        e = unpack_lh1(&b, &w);
    else
        e = unpack_static(&b, &w, dicbit);
    free(w.win);
    if (!e && usize >= 0 && w.pos < usize)
        e = E_FORMAT;
    return e ? e : out->err;
}

/* ----------------------------------------------------- -lh5- encoder */

#define EDIC     8192L             /* -lh5- window                    */
#define EMASK    (EDIC - 1)
#define ERING    32768L            /* input ring: window + look-ahead */
#define ERMASK   (ERING - 1)
#define EMAXM    256
#define EHBITS   13
#define EHSIZE   (1 << EHBITS)
#define ECHAIN   48
#define EBLOCK   16384             /* tokens per block (< 65536)     */
#define NP5      14
#define PBIT5    4

typedef struct {
    BITW  w;
    u8   *ring;
    long *head, *prev;
    long  inpos, cur;
    u16  *tc;                      /* code: literal or 256 + len - 3   */
    u16  *tp;                      /* position (distance - 1)          */
    int   n;
} LENC;

static int bitlen(unsigned v)
{
    int n = 0;
    while (v) { n++; v >>= 1; }
    return n;
}

/* write a t or p code length table, as read_pt_len reads it */
static void write_pt_len(LENC *e, const u8 *len, int nn, int nbit, int ispecial)
{
    int n = nn, i = 0;
    while (n > 0 && len[n - 1] == 0)
        n--;
    bitw_put(&e->w, (unsigned)n, nbit);
    while (i < n) {
        int k = len[i++];
        if (k <= 6)
            bitw_put(&e->w, (unsigned)k, 3);
        else {
            bitw_put(&e->w, 7, 3);
            while (k-- > 7)
                bitw_put(&e->w, 1, 1);
            bitw_put(&e->w, 0, 1);
        }
        if (i == ispecial) {
            int z = 0;
            while (i < 6 && i < n && len[i] == 0) { i++; z++; }
            bitw_put(&e->w, (unsigned)z, 2);
        }
    }
}

/* one tree's code lengths; returns the single symbol when only one is used */
static int make_lens(const u32 *freq, int n, u8 *len)
{
    int i, used = 0, last = 0;
    for (i = 0; i < n; i++)
        if (freq[i]) { used++; last = i; }
    memset(len, 0, n);
    if (used <= 1)
        return last;
    huff_lengths(freq, n, 16, len);
    return -1;
}

static void put_code(LENC *e, const u16 *codes, const u8 *len, int sym)
{
    if (len[sym])
        bitw_put(&e->w, codes[sym], len[sym]);
}

static void flush_lh5(LENC *e)
{
    u32 cf[NC], pf[NP5], tf[NT];
    u8 cl[NC], pl[NP5], tl[NT];
    u16 cc[NC], pc[NP5], tc[NT];
    int i, csing, psing, tsing, n;
    if (!e->n)
        return;
    memset(cf, 0, sizeof cf);
    memset(pf, 0, sizeof pf);
    for (i = 0; i < e->n; i++) {
        cf[e->tc[i]]++;
        if (e->tc[i] >= 256)
            pf[bitlen(e->tp[i])]++;
    }
    csing = make_lens(cf, NC, cl);
    psing = make_lens(pf, NP5, pl);
    bitw_put(&e->w, (unsigned)e->n, 16);
    if (csing >= 0) {
        /* one code: t table and c table both "single" */
        bitw_put(&e->w, 0, TBIT);
        bitw_put(&e->w, 0, TBIT);
        bitw_put(&e->w, 0, CBIT);
        bitw_put(&e->w, (unsigned)csing, CBIT);
    } else {
        /* code lengths of the c table, run-length coded through the t table */
        for (n = NC; n > 0 && cl[n - 1] == 0; n--)
            ;
        memset(tf, 0, sizeof tf);
        for (i = 0; i < n; ) {
            int k = cl[i++];
            if (k == 0) {
                int count = 1;
                while (i < n && cl[i] == 0) { i++; count++; }
                if (count <= 2) tf[0] += (u32)count;
                else if (count <= 18) tf[1]++;
                else if (count == 19) { tf[0]++; tf[1]++; }
                else tf[2]++;
            } else
                tf[k + 2]++;
        }
        tsing = make_lens(tf, NT, tl);
        if (tsing >= 0) {
            bitw_put(&e->w, 0, TBIT);
            bitw_put(&e->w, (unsigned)tsing, TBIT);
        } else
            write_pt_len(e, tl, NT, TBIT, 3);
        huff_codes(tl, NT, tc);
        bitw_put(&e->w, (unsigned)n, CBIT);
        for (i = 0; i < n; ) {
            int k = cl[i++];
            if (k == 0) {
                int count = 1;
                while (i < n && cl[i] == 0) { i++; count++; }
                if (count <= 2) {
                    while (count--) put_code(e, tc, tl, 0);
                } else if (count <= 18) {
                    put_code(e, tc, tl, 1);
                    bitw_put(&e->w, (unsigned)(count - 3), 4);
                } else if (count == 19) {
                    put_code(e, tc, tl, 0);
                    put_code(e, tc, tl, 1);
                    bitw_put(&e->w, 15, 4);
                } else {
                    put_code(e, tc, tl, 2);
                    bitw_put(&e->w, (unsigned)(count - 20), CBIT);
                }
            } else
                put_code(e, tc, tl, k + 2);
        }
    }
    if (psing >= 0) {
        bitw_put(&e->w, 0, PBIT5);
        bitw_put(&e->w, (unsigned)psing, PBIT5);
    } else
        write_pt_len(e, pl, NP5, PBIT5, -1);
    huff_codes(cl, NC, cc);
    huff_codes(pl, NP5, pc);
    for (i = 0; i < e->n; i++) {
        int c = e->tc[i];
        put_code(e, cc, cl, c);
        if (c >= 256) {
            unsigned p = e->tp[i];
            int nb = bitlen(p);
            put_code(e, pc, pl, nb);
            if (nb > 1)
                bitw_put(&e->w, p & ((1U << (nb - 1)) - 1), nb - 1);
        }
    }
    e->n = 0;
}

static void lh5_token(LENC *e, int c, unsigned p)
{
    e->tc[e->n] = (u16)c;
    e->tp[e->n] = (u16)p;
    if (++e->n == EBLOCK)
        flush_lh5(e);
}

static unsigned ehash(LENC *e, long pos)
{
    const u8 *r = e->ring;
    u32 v = ((u32)r[pos & ERMASK] << 16) | ((u32)r[(pos + 1) & ERMASK] << 8) | r[(pos + 2) & ERMASK];
    return (unsigned)(((v * 2654435761UL) & 0xFFFFFFFFUL) >> (32 - EHBITS)) & (EHSIZE - 1);
}

static void einsert(LENC *e, long pos)
{
    unsigned h = ehash(e, pos);
    e->prev[pos & EMASK] = e->head[h];
    e->head[h] = pos;
}

static void lh5_run(LENC *e, int final)
{
    const u8 *r = e->ring;
    long limit = final ? e->inpos : e->inpos - EMAXM;
    while (e->cur < limit) {
        long pos = e->cur, avail = e->inpos - pos, best = 0, bdist = 0;
        if (avail >= 3) {
            long cand = e->head[ehash(e, pos)], maxlen = avail < EMAXM ? avail : EMAXM;
            int chain = ECHAIN;
            while (cand >= 0 && pos - cand < EDIC && chain--) {
                if (r[(cand + best) & ERMASK] == r[(pos + best) & ERMASK]) {
                    long k = 0;
                    while (k < maxlen && r[(cand + k) & ERMASK] == r[(pos + k) & ERMASK])
                        k++;
                    if (k > best) {
                        best = k;
                        bdist = pos - cand;
                        if (k == maxlen)
                            break;
                    }
                }
                cand = e->prev[cand & EMASK];
            }
        }
        if (best >= 3) {
            long k;
            lh5_token(e, 256 + (int)best - 3, (unsigned)(bdist - 1));
            for (k = 0; k < best; k++)
                if (e->inpos - (pos + k) >= 3)
                    einsert(e, pos + k);
            e->cur += best;
        } else {
            lh5_token(e, r[pos & ERMASK], 0);
            if (avail >= 3)
                einsert(e, pos);
            e->cur++;
        }
    }
}

int lzh_pack(BIN *in, BOUT *out, long usize, PROG *pg)
{
    LENC *e = calloc(1, sizeof(LENC));
    u8 buf[2048];
    long got, i, done = 0;
    int err = E_OK;
    if (!e)
        return E_NOMEM;
    e->ring = malloc(ERING);
    e->head = malloc(EHSIZE * sizeof(long));
    e->prev = malloc(EDIC * sizeof(long));
    e->tc = malloc(EBLOCK * sizeof(u16));
    e->tp = malloc(EBLOCK * sizeof(u16));
    if (!e->ring || !e->head || !e->prev || !e->tc || !e->tp) {
        err = E_NOMEM;
        goto out;
    }
    for (i = 0; i < EHSIZE; i++) e->head[i] = -1;
    for (i = 0; i < EDIC; i++) e->prev[i] = -1;
    bitw_init(&e->w, out, 1);
    while ((got = bin_read(in, buf, sizeof buf)) > 0) {
        long k = 0;
        while (k < got) {
            long room = ERING - EDIC - (e->inpos - e->cur), n = got - k, j;
            if (room > n) room = n;
            if (room <= 0) { lh5_run(e, 0); continue; }
            for (j = 0; j < room; j++)
                e->ring[(e->inpos + j) & ERMASK] = buf[k + j];
            e->inpos += room;
            k += room;
            lh5_run(e, 0);
        }
        done += got;
        if (codec_progress(pg, done, usize)) { err = E_CANCEL; goto out; }
    }
    lh5_run(e, 1);
    flush_lh5(e);
    bitw_flush(&e->w);
    err = out->err;
out:
    free(e->ring);
    free(e->head);
    free(e->prev);
    free(e->tc);
    free(e->tp);
    free(e);
    return err;
}
