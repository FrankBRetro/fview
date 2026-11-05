/* arcio.c - byte streams, CRCs, bit I/O and canonical Huffman codes for
 * FArchive's codecs (see arcio.h).                                    */
#include <stdlib.h>
#include <string.h>
#include "arcio.h"

/* ------------------------------------------------------------ streams */

void bin_file(BIN *b, FILE *f, long limit)
{
    memset(b, 0, sizeof *b);
    b->f = f;
    b->left = limit;
}

void bin_func(BIN *b, long (*fn)(void *, u8 *, long), void *ctx)
{
    memset(b, 0, sizeof *b);
    b->fn = fn;
    b->ctx = ctx;
    b->left = -1;
}

static int bin_fill(BIN *b)
{
    long want = sizeof b->buf, got;
    if (b->eof)
        return 0;
    if (b->left >= 0 && want > b->left)
        want = b->left;
    if (want <= 0) {
        b->eof = 1;
        return 0;
    }
    got = b->f ? (long)fread(b->buf, 1, want, b->f) : b->fn(b->ctx, b->buf, want);
    if (got <= 0) {
        b->eof = 1;
        return 0;
    }
    if (b->left >= 0)
        b->left -= got;
    b->pos = 0;
    b->len = (int)got;
    return 1;
}

int bin_byte(BIN *b)
{
    if (b->pos >= b->len && !bin_fill(b))
        return -1;
    b->total++;
    return b->buf[b->pos++];
}

long bin_read(BIN *b, void *d, long n)
{
    long got = 0;
    u8 *p = d;
    while (got < n) {
        long k;
        if (b->pos >= b->len && !bin_fill(b))
            break;
        k = b->len - b->pos;
        if (k > n - got) k = n - got;
        memcpy(p + got, b->buf + b->pos, k);
        b->pos += (int)k;
        got += k;
    }
    b->total += got;
    return got;
}

long bin_skip(BIN *b, long n)
{
    u8 tmp[512];
    long got = 0;
    while (got < n) {
        long k = n - got > (long)sizeof tmp ? (long)sizeof tmp : n - got, r = bin_read(b, tmp, k);
        got += r;
        if (r < k)
            break;
    }
    return got;
}

void bout_file(BOUT *o, FILE *f)
{
    memset(o, 0, sizeof *o);
    o->f = f;
    o->crc32 = 0xFFFFFFFFUL;
}

void bout_func(BOUT *o, int (*fn)(void *, const u8 *, long), void *ctx)
{
    memset(o, 0, sizeof *o);
    o->fn = fn;
    o->ctx = ctx;
    o->crc32 = 0xFFFFFFFFUL;
}

static void bout_drain(BOUT *o)
{
    if (!o->pos)
        return;
    o->crc32 = crc32_update(o->crc32, o->buf, o->pos);
    o->crc16 = crc16_update(o->crc16, o->buf, o->pos);
    if (!o->err) {
        if (o->f) {
            if ((long)fwrite(o->buf, 1, o->pos, o->f) != o->pos)
                o->err = E_WRITE;
        } else if (o->fn) {
            int e = o->fn(o->ctx, o->buf, o->pos);
            if (e)
                o->err = e;
        }
    }
    o->pos = 0;
}

void bout_put(BOUT *o, const u8 *d, long n)
{
    o->count += n;
    while (n > 0) {
        long k = (long)sizeof o->buf - o->pos;
        if (k > n) k = n;
        memcpy(o->buf + o->pos, d, k);
        o->pos += (int)k;
        d += k;
        n -= k;
        if (o->pos == (int)sizeof o->buf)
            bout_drain(o);
    }
}

void bout_byte(BOUT *o, int c)
{
    o->buf[o->pos++] = (u8)c;
    o->count++;
    if (o->pos == (int)sizeof o->buf)
        bout_drain(o);
}

int bout_flush(BOUT *o)
{
    bout_drain(o);
    return o->err;
}

u32 bout_crc32(const BOUT *o)
{
    return (o->crc32 ^ 0xFFFFFFFFUL) & 0xFFFFFFFFUL;
}

/* ---------------------------------------------------------------- CRCs */

static u32 crc32_tab[256];
static u16 crc16_tab[256];
static int crc_ready;

static void crc_init(void)
{
    int i, k;
    for (i = 0; i < 256; i++) {
        u32 c = (u32)i;
        unsigned d = (unsigned)i;
        for (k = 0; k < 8; k++) {
            c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
            d = (d & 1) ? 0xA001 ^ (d >> 1) : d >> 1;
        }
        crc32_tab[i] = c & 0xFFFFFFFFUL;
        crc16_tab[i] = (u16)d;
    }
    crc_ready = 1;
}

u32 crc32_update(u32 crc, const u8 *d, long n)
{
    if (!crc_ready)
        crc_init();
    while (n-- > 0)
        crc = crc32_tab[(crc ^ *d++) & 0xFF] ^ ((crc >> 8) & 0x00FFFFFFUL);
    return crc;
}

unsigned crc16_update(unsigned crc, const u8 *d, long n)
{
    if (!crc_ready)
        crc_init();
    while (n-- > 0)
        crc = crc16_tab[(crc ^ *d++) & 0xFF] ^ (crc >> 8);
    return crc & 0xFFFF;
}

/* ----------------------------------------------------------- bit input */

void bits_init(BITS *b, BIN *in, int msb)
{
    memset(b, 0, sizeof *b);
    b->in = in;
    b->msb = msb;
}

static void bits_fill(BITS *b, int n)
{
    while (b->cnt < n) {
        int c = bin_byte(b->in);
        if (c < 0) {
            c = 0;
            b->over += 8;
        }
        if (b->msb == 2) {                   /* LZX: big-endian words, lsb first */
            int d = bin_byte(b->in);
            if (d < 0) {
                d = 0;
                b->over += 8;
            }
            b->buf |= (u32)(c << 8 | d) << b->cnt;
            b->cnt += 16;
            continue;
        }
        if (b->msb)
            b->buf = (b->buf << 8) | (u32)c;
        else
            b->buf |= (u32)c << b->cnt;
        b->cnt += 8;
    }
}

unsigned bits_peek(BITS *b, int n)
{
    if (n == 0)
        return 0;
    bits_fill(b, n);
    if (b->msb == 1)
        return (unsigned)((b->buf >> (b->cnt - n)) & ((1UL << n) - 1));
    return (unsigned)(b->buf & ((1UL << n) - 1));
}

void bits_drop(BITS *b, int n)
{
    bits_fill(b, n);
    b->cnt -= n;
    if (b->msb == 1)
        b->buf &= b->cnt ? ((1UL << b->cnt) - 1) : 0;
    else
        b->buf = n >= 32 ? 0 : b->buf >> n;
}

unsigned bits_get(BITS *b, int n)
{
    unsigned v = bits_peek(b, n);
    bits_drop(b, n);
    return v;
}

void bits_align(BITS *b)
{
    bits_drop(b, b->cnt & 7);
}

/* ---------------------------------------------------------- bit output */

void bitw_init(BITW *w, BOUT *out, int msb)
{
    memset(w, 0, sizeof *w);
    w->out = out;
    w->msb = msb;
}

void bitw_put(BITW *w, unsigned v, int n)
{
    v &= (1U << n) - 1;
    if (w->msb == 2) {                       /* LZX: big-endian words, lsb first */
        w->buf |= (u32)v << w->cnt;
        w->cnt += n;
        while (w->cnt >= 16) {
            bout_byte(w->out, (int)((w->buf >> 8) & 0xFF));
            bout_byte(w->out, (int)(w->buf & 0xFF));
            w->buf >>= 16;
            w->cnt -= 16;
        }
    } else if (w->msb) {
        w->buf = (w->buf << n) | v;
        w->cnt += n;
        while (w->cnt >= 8) {
            w->cnt -= 8;
            bout_byte(w->out, (int)((w->buf >> w->cnt) & 0xFF));
        }
        w->buf &= (1UL << w->cnt) - 1;
    } else {
        w->buf |= (u32)v << w->cnt;
        w->cnt += n;
        while (w->cnt >= 8) {
            bout_byte(w->out, (int)(w->buf & 0xFF));
            w->buf >>= 8;
            w->cnt -= 8;
        }
    }
}

void bitw_flush(BITW *w)
{
    if (w->cnt)
        bitw_put(w, 0, (w->msb == 2 ? 16 : 8) - w->cnt);
}

/* ------------------------------------------------- canonical Huffman */

void huff_codes(const u8 *lens, int n, u16 *codes)
{
    u16 count[18], next[18];
    int i, code = 0;
    memset(count, 0, sizeof count);
    for (i = 0; i < n; i++)
        count[lens[i]]++;
    count[0] = 0;
    for (i = 1; i <= 17; i++) {
        code = (code + count[i - 1]) << 1;
        next[i] = (u16)code;
    }
    for (i = 0; i < n; i++)
        codes[i] = lens[i] ? next[lens[i]]++ : 0;
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

int huff_build(HUFF *h, const u8 *lens, int n, int msb)
{
    u16 offs[18], codes[1024];
    int i, left = 1, len;
    memset(h->count, 0, sizeof h->count);
    memset(h->fast, 0, sizeof h->fast);
    h->n = n;
    h->ok = 0;
    if (n > 1024)
        return 0;
    for (i = 0; i < n; i++)
        if (lens[i] > 16)
            return 0;
        else
            h->count[lens[i]]++;
    h->count[0] = 0;
    for (len = 1; len <= 16; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return 0;                    /* over-subscribed */
    }
    offs[1] = 0;
    for (len = 1; len < 16; len++)
        offs[len + 1] = (u16)(offs[len] + h->count[len]);
    for (i = 0; i < n; i++)
        if (lens[i])
            h->sym[offs[lens[i]]++] = (u16)i;
    huff_codes(lens, n, codes);
    for (i = 0; i < n; i++) {
        int l = lens[i], k;
        if (!l || l > HUFF_FAST)
            continue;
        if (msb) {
            unsigned base = (unsigned)codes[i] << (HUFF_FAST - l);
            for (k = 0; k < (1 << (HUFF_FAST - l)); k++)
                h->fast[base + k] = (u16)((l << 11) | i);
        } else {
            unsigned r = rev(codes[i], l);
            for (k = 0; k < (1 << (HUFF_FAST - l)); k++)
                h->fast[r | (k << l)] = (u16)((l << 11) | i);
        }
    }
    h->ok = 1;
    return 1;
}

int huff_decode(HUFF *h, BITS *b)
{
    unsigned e = h->fast[bits_peek(b, HUFF_FAST)];
    int code = 0, first = 0, index = 0, len;
    if (e) {
        bits_drop(b, (int)(e >> 11));
        return (int)(e & 0x7FF);
    }
    /* longer (or unused) code: one bit at a time */
    for (len = 1; len <= 16; len++) {
        int count = h->count[len];
        code |= (int)bits_get(b, 1);
        if (code - first < count)
            return h->sym[index + code - first];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

/* Huffman code lengths with a length limit (the JPEG way: build the
   tree, count lengths, move overlong leaves up, hand the lengths out by
   frequency)                                                         */
void huff_lengths(const u32 *freq, int n, int maxlen, u8 *lens)
{
    int *order, *node, nl = 0, i, j;
    u32 *w;
    int *parent, nn, a, b, q1, q2, e1, e2;
    int bl[64];
    memset(lens, 0, n);
    order = malloc(n * sizeof(int));
    w = malloc(2 * n * sizeof(u32));
    parent = malloc(2 * n * sizeof(int));
    node = malloc(2 * n * sizeof(int));
    if (!order || !w || !parent || !node)
        goto done;
    for (i = 0; i < n; i++)
        if (freq[i])
            order[nl++] = i;
    if (nl == 0)
        goto done;
    if (nl == 1) {
        lens[order[0]] = 1;
        goto done;
    }
    /* sort leaves by frequency (insertion sort is fine for n <= 1024) */
    for (i = 1; i < nl; i++) {
        int s = order[i];
        for (j = i - 1; j >= 0 && freq[order[j]] > freq[s]; j--)
            order[j + 1] = order[j];
        order[j + 1] = s;
    }
    /* two-queue Huffman: leaves 0..nl-1, internal nodes nl.. */
    for (i = 0; i < nl; i++)
        w[i] = freq[order[i]];
    nn = nl;
    q1 = 0; e1 = nl;            /* leaves queue  */
    q2 = nl; e2 = nl;           /* internal queue */
    while ((e1 - q1) + (e2 - q2) > 1) {
        int pick[2], k;
        for (k = 0; k < 2; k++) {
            if (q1 < e1 && (q2 >= e2 || w[q1] <= w[q2]))
                pick[k] = q1++;
            else
                pick[k] = q2++;
        }
        a = pick[0];
        b = pick[1];
        w[nn] = w[a] + w[b];
        parent[a] = nn;
        parent[b] = nn;
        nn++;
        e2 = nn;
    }
    /* depths */
    memset(bl, 0, sizeof bl);
    parent[nn - 1] = -1;
    node[nn - 1] = 0;
    for (i = nn - 2; i >= 0; i--)
        node[i] = node[parent[i]] + 1;
    for (i = 0; i < nl; i++)
        bl[node[i] > 63 ? 63 : node[i]]++;
    /* limit */
    for (i = 63; i > maxlen; i--)
        while (bl[i] > 0) {
            j = i - 2;
            while (j > 0 && bl[j] == 0)
                j--;
            bl[i] -= 2;
            bl[i - 1] += 1;
            bl[j + 1] += 2;
            bl[j] -= 1;
        }
    /* shortest codes to the most frequent symbols */
    j = nl - 1;
    for (i = 1; i <= maxlen; i++) {
        int k;
        for (k = 0; k < bl[i]; k++)
            lens[order[j--]] = (u8)i;
    }
done:
    free(order);
    free(w);
    free(parent);
    free(node);
}

/* -------------------------------------------------------------- codecs */

int codec_progress(PROG *pg, long done, long total)
{
    if (prog_step(pg, done, total > 0 ? total : done + 1))
        return E_CANCEL;
    return E_OK;
}
