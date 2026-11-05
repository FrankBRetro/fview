/*
 * lzw.c - the ZOO and ARC codecs.
 *
 *   ZOO method 1   LZW, 9..13 bit codes, LSB first, CLEAR 256, EOF 257
 *   ARC 3          RLE ("non-repeat packing", DLE 0x90)
 *   ARC 4          RLE + Huffman squeezing (tree stored in the data)
 *   ARC 5/6/7      old static 12 bit LZW (hash-placed codes) +/- RLE
 *   ARC 8          RLE + LZW 9..12 bits, compress-style code groups
 *   ARC 9          LZW 9..13 bits ("squashed")
 *
 * Encoders: ZOO method 1 and ARC 8 (ARC 9 shares the code).
 */
#include <stdlib.h>
#include <string.h>
#include "arcio.h"

#define PROG_EVERY 0x3FFF

static int out_progress(BOUT *o, long *next, long usize, PROG *pg)
{
    if (o->count >= *next) {
        *next = o->count + PROG_EVERY;
        return codec_progress(pg, o->count, usize);
    }
    return E_OK;
}

static int in_progress(BIN *in, long *next, long usize, PROG *pg)
{
    if (in->total >= *next) {
        *next = in->total + PROG_EVERY;
        return codec_progress(pg, in->total, usize);
    }
    return E_OK;
}

static int finish(BOUT *out, long usize, int e)
{
    if (!e && out->err)
        e = out->err;
    if (!e && usize >= 0 && out->count != usize)
        e = E_FORMAT;
    return e;
}

/* ================================================================ ZOO */

#define Z_CLEAR    256
#define Z_EOF      257
#define Z_FIRST    258
#define Z_MAXBITS  13
#define Z_MAXMAX   8192

int zoolzw_unpack(BIN *in, BOUT *out, long usize, PROG *pg)
{
    BITS b;
    u16 *head = malloc((Z_MAXMAX + 16) * sizeof(u16));
    u8  *tail = malloc(Z_MAXMAX + 16);
    u8  *stack = malloc(Z_MAXMAX + 16);
    unsigned free_code = Z_FIRST, max_code = 512, old_code = 0, code, in_code;
    int nbits = 9, fin = 0, e = E_OK, have_old = 0;
    long next = 0;
    long base = out->count;
    if (!head || !tail || !stack) {
        e = E_NOMEM;
        goto out;
    }
    bits_init(&b, in, 0);
    for (;;) {
        int sp = 0;
        code = bits_get(&b, nbits);
        if (b.over > 16) {
            e = E_FORMAT;
            break;
        }
        if (code == Z_EOF)
            break;
        if (code == Z_CLEAR) {
            nbits = 9;
            max_code = 512;
            free_code = Z_FIRST;
            code = bits_get(&b, nbits);
            if (code == Z_EOF)
                break;
            if (code > 255 || b.over > 16) {
                e = E_FORMAT;
                break;
            }
            old_code = code;
            fin = (int)code;
            have_old = 1;
            bout_byte(out, (int)code);
            continue;
        }
        if (!have_old) {                         /* stream without CLEAR */
            if (code > 255) { e = E_FORMAT; break; }
            old_code = code;
            fin = (int)code;
            have_old = 1;
            bout_byte(out, (int)code);
            continue;
        }
        in_code = code;
        if (code >= free_code) {                 /* KwKwK */
            if (code > free_code) { e = E_FORMAT; break; }
            stack[sp++] = (u8)fin;
            code = old_code;
        }
        while (code > 255) {
            if (sp >= Z_MAXMAX) { e = E_FORMAT; break; }
            stack[sp++] = tail[code];
            code = head[code];
        }
        if (e)
            break;
        fin = (int)code;
        bout_byte(out, fin);
        while (sp)
            bout_byte(out, stack[--sp]);
        if (free_code <= Z_MAXMAX) {
            head[free_code] = (u16)old_code;
            tail[free_code] = (u8)fin;
            free_code++;
            if (free_code >= max_code && nbits < Z_MAXBITS) {
                nbits++;
                max_code <<= 1;
            }
        }
        old_code = in_code;
        if ((e = out_progress(out, &next, usize, pg)) != E_OK)
            break;
        if (usize >= 0 && out->count - base > usize) {
            e = E_FORMAT;
            break;
        }
    }
out:
    free(head);
    free(tail);
    free(stack);
    return finish(out, usize < 0 ? -1 : usize + base, e);
}

/* dictionary for the encoders: (prefix << 8 | char) -> code */
#define DHASH 16384
typedef struct {
    long key[DHASH];
    u16 val[DHASH];
} DICT;

static void dict_clear(DICT *d)
{
    memset(d->key, 0xFF, sizeof d->key);
}

static int dict_find(DICT *d, long key, int *slot)
{
    u32 h = ((u32)key * 2654435761UL) >> 18;     /* 14 bits */
    for (;;) {
        h &= DHASH - 1;
        if (d->key[h] == key) {
            *slot = (int)h;
            return d->val[h];
        }
        if (d->key[h] < 0) {
            *slot = (int)h;
            return -1;
        }
        h++;
    }
}

int zoolzw_pack(BIN *in, BOUT *out, long usize, PROG *pg)
{
    DICT *d = malloc(sizeof(DICT));
    BITW w;
    unsigned free_code = Z_FIRST, max_code = 512;
    int nbits = 9, c, ent, e = E_OK;
    long next = 0;
    if (!d)
        return E_NOMEM;
    dict_clear(d);
    bitw_init(&w, out, 0);
    bitw_put(&w, Z_CLEAR, nbits);
    ent = bin_byte(in);
    if (ent >= 0) {
        while ((c = bin_byte(in)) >= 0) {
            long key = ((long)ent << 8) | c;
            int slot, v = dict_find(d, key, &slot);
            if (v >= 0) {
                ent = v;
                continue;
            }
            d->key[slot] = key;
            d->val[slot] = (u16)free_code++;
            bitw_put(&w, (unsigned)ent, nbits);
            ent = c;
            if (free_code > max_code) {
                if (nbits >= Z_MAXBITS) {
                    bitw_put(&w, Z_CLEAR, nbits);
                    dict_clear(d);
                    nbits = 9;
                    max_code = 512;
                    free_code = Z_FIRST;
                } else {
                    nbits++;
                    max_code <<= 1;
                }
            }
            if ((e = in_progress(in, &next, usize, pg)) != E_OK)
                break;
        }
        if (!e)
            bitw_put(&w, (unsigned)ent, nbits);
    }
    if (!e) {
        bitw_put(&w, Z_EOF, nbits);
        bitw_flush(&w);
    }
    free(d);
    return e ? e : out->err;
}

/* ================================================================ ARC */

#define DLE 0x90

/* RLE90 decoding, as an output filter */
typedef struct {
    BOUT *out;
    int   rep;                   /* the last byte was DLE              */
    int   last;
} UNRLE;

static void unrle_put(UNRLE *r, int c)
{
    if (r->rep) {
        r->rep = 0;
        if (c == 0)
            bout_byte(r->out, DLE);
        else
            while (--c > 0)
                bout_byte(r->out, r->last);
    } else if (c == DLE)
        r->rep = 1;
    else
        bout_byte(r->out, r->last = c);
}

/* emit one byte of decoded data, through RLE or straight */
typedef struct {
    UNRLE  r;
    int    rle;
} SINK;

#define SINK_PUT(s, c) do { if ((s)->rle) unrle_put(&(s)->r, (c)); \
                            else bout_byte((s)->r.out, (c)); } while (0)

/* ---------------------------------------------------- ARC 4: squeeze */

static int unsqueeze(BIN *in, SINK *s, BOUT *out, long usize, PROG *pg)
{
    short (*node)[2];
    int n, i, k, bit = 8, cur = 0, e = E_OK;
    long next = 0;
    int lo = bin_byte(in), hi = bin_byte(in);
    if (lo < 0 || hi < 0)
        return usize > 0 ? E_FORMAT : E_OK;
    n = lo | hi << 8;
    if (n > 257)
        return E_FORMAT;
    node = malloc(258 * sizeof *node);
    if (!node)
        return E_NOMEM;
    node[0][0] = node[0][1] = -(256 + 1);
    for (i = 0; i < n; i++)
        for (k = 0; k < 2; k++) {
            int a = bin_byte(in), b = bin_byte(in), v;
            if (a < 0 || b < 0) { e = E_FORMAT; goto out; }
            v = (short)(a | b << 8);
            if (v >= n || v < -257) { e = E_FORMAT; goto out; }
            node[i][k] = (short)v;
        }
    for (;;) {
        i = 0;
        while (i >= 0) {
            if (bit > 7) {
                cur = bin_byte(in);
                if (cur < 0)
                    goto out;            /* data ended without SPEOF */
                bit = 0;
            } else
                cur >>= 1;
            bit++;
            i = node[i][cur & 1];
        }
        i = -(i + 1);
        if (i == 256)
            break;
        SINK_PUT(s, i);
        if ((e = out_progress(out, &next, usize, pg)) != E_OK)
            break;
    }
out:
    free(node);
    return e;
}

/* ------------------------------------------- ARC 5/6/7: old crunching */

#define OT_SIZE 4096
#define NO_PRED 0xFFFF
typedef struct {
    u8  used;
    u16 next, pred;
    u8  foll;
} OENT;

typedef struct {
    OENT t[OT_SIZE];
    int  newhash;
} OTAB;

static unsigned ohash(OTAB *o, unsigned pred, unsigned foll)
{
    if (o->newhash)
        return (((pred + foll) & 0xFFFF) * 15073UL) & 0xFFF;
    {
        u32 l = ((pred + foll) | 0x0800) & 0xFFFF;
        l *= l;
        return (unsigned)(l >> 6) & 0x0FFF;
    }
}

static void oupd(OTAB *o, unsigned pred, unsigned foll)
{
    unsigned l = ohash(o, pred, foll);
    if (o->t[l].used) {
        unsigned nx;
        while (o->t[l].next)
            l = o->t[l].next;
        nx = (l + 101) & 0x0FFF;
        while (o->t[nx].used)
            nx = (nx + 1) & 0x0FFF;
        o->t[l].next = (u16)nx;
        l = nx;
    }
    o->t[l].used = 1;
    o->t[l].next = 0;
    o->t[l].pred = (u16)pred;
    o->t[l].foll = (u8)foll;
}

static int uncrunch_old(BIN *in, SINK *s, BOUT *out, int newhash, long usize, PROG *pg)
{
    OTAB *o = calloc(1, sizeof(OTAB));
    u8 *stack = malloc(OT_SIZE);
    int i, sp, held = -1, half = 0, count = OT_SIZE - 256, e = E_OK;
    unsigned oldcode, newcode, code, fin;
    long next = 0;
    if (!o || !stack) {
        e = E_NOMEM;
        goto out;
    }
    o->newhash = newhash;
    for (i = 0; i < 256; i++)
        oupd(o, NO_PRED, (unsigned)i);
#define GOCODE(x) do { int a_, b_; \
        if (!half) { a_ = bin_byte(in); b_ = bin_byte(in); \
            if (a_ < 0 || b_ < 0) goto out; \
            x = (unsigned)(a_ << 4 | b_ >> 4); held = b_; half = 1; } \
        else { b_ = bin_byte(in); if (b_ < 0) goto out; \
            x = (unsigned)((held & 15) << 8 | b_); half = 0; } } while (0)
    GOCODE(oldcode);
    fin = o->t[oldcode].foll;
    SINK_PUT(s, (int)fin);
    for (;;) {
        GOCODE(newcode);
        sp = 0;
        code = newcode;
        if (!o->t[code].used) {
            code = oldcode;
            stack[sp++] = (u8)fin;
        }
        while (o->t[code].pred != NO_PRED) {
            if (sp >= OT_SIZE - 1 || !o->t[code].used) { e = E_FORMAT; goto out; }
            stack[sp++] = o->t[code].foll;
            code = o->t[code].pred;
        }
        stack[sp++] = (u8)(fin = o->t[code].foll);
        if (count) {
            oupd(o, oldcode, fin);
            count--;
        }
        oldcode = newcode;
        while (sp)
            SINK_PUT(s, stack[--sp]);
        if ((e = out_progress(out, &next, usize, pg)) != E_OK)
            break;
    }
#undef GOCODE
out:
    free(o);
    free(stack);
    return e;
}

/* ---------------------------------------- ARC 8/9: dynamic LZW (decode) */

#define INIT_BITS 9
#define A_CLEAR   256
#define A_FIRST   257
#define MAXCODE(n) ((1 << (n)) - 1)

typedef struct {
    BIN *in;
    int  bits, n_bits, maxcode, max_maxcode, free_ent, clear_flg;
    int  offset, size;
    u8   buf[16];
} AGET;

static int aget(AGET *g)
{
    int code, r_off, bits;
    u8 *bp = g->buf;
    if (g->clear_flg > 0 || g->offset >= g->size || g->free_ent > g->maxcode) {
        if (g->free_ent > g->maxcode) {
            g->n_bits++;
            g->maxcode = g->n_bits == g->bits ? g->max_maxcode : MAXCODE(g->n_bits);
        }
        if (g->clear_flg > 0) {
            g->maxcode = MAXCODE(g->n_bits = INIT_BITS);
            g->clear_flg = 0;
        }
        for (g->size = 0; g->size < g->n_bits; g->size++) {
            int c = bin_byte(g->in);
            if (c < 0)
                break;
            g->buf[g->size] = (u8)c;
        }
        if (g->size <= 0)
            return -1;
        g->offset = 0;
        g->size = (g->size << 3) - (g->n_bits - 1);
        if (g->size <= 0)
            return -1;
    }
    r_off = g->offset;
    bits = g->n_bits;
    bp += r_off >> 3;
    r_off &= 7;
    code = *bp++ >> r_off;
    bits -= 8 - r_off;
    r_off = 8 - r_off;
    if (bits >= 8) {
        code |= *bp++ << r_off;
        r_off += 8;
        bits -= 8;
    }
    if (bits > 0)
        code |= (*bp & ((1 << bits) - 1)) << r_off;
    g->offset += g->n_bits;
    return code & MAXCODE(g->bits);
}

static int decomp(BIN *in, SINK *s, BOUT *out, int squash, long usize, PROG *pg)
{
    AGET g;
    u16 *prefix = malloc(8192 * sizeof(u16));
    u8 *suffix = malloc(8192), *stack = malloc(8192);
    int code, oldcode, incode, fin, sp, e = E_OK;
    long next = 0;
    if (!prefix || !suffix || !stack) {
        e = E_NOMEM;
        goto out;
    }
    memset(&g, 0, sizeof g);
    g.in = in;
    if (squash)
        g.bits = 13;
    else {
        int c = bin_byte(in);
        if (c < 0)
            goto out;                    /* empty */
        if (c != 12) {
            e = E_UNSUP;
            goto out;
        }
        g.bits = 12;
    }
    g.max_maxcode = 1 << g.bits;
    g.n_bits = INIT_BITS;
    g.maxcode = MAXCODE(INIT_BITS);
    for (code = 0; code < 256; code++) {
        prefix[code] = 0;
        suffix[code] = (u8)code;
    }
    g.free_ent = A_FIRST;
    fin = oldcode = aget(&g);
    if (oldcode < 0)
        goto out;
    if (oldcode > 255) {
        e = E_FORMAT;
        goto out;
    }
    SINK_PUT(s, fin);
    while ((code = aget(&g)) > -1) {
        if (code == A_CLEAR) {
            g.clear_flg = 1;
            g.free_ent = A_FIRST - 1;
            if ((code = aget(&g)) == -1)
                break;
        }
        incode = code;
        sp = 0;
        if (code >= g.free_ent) {
            if (code > g.free_ent) {
                e = E_FORMAT;
                break;
            }
            stack[sp++] = (u8)fin;
            code = oldcode;
        }
        while (code >= 256) {
            if (sp >= 8191) { e = E_FORMAT; goto out; }
            stack[sp++] = suffix[code];
            code = prefix[code];
        }
        stack[sp++] = (u8)(fin = suffix[code]);
        while (sp)
            SINK_PUT(s, stack[--sp]);
        if ((code = g.free_ent) < g.max_maxcode) {
            prefix[code] = (u16)oldcode;
            suffix[code] = (u8)fin;
            g.free_ent = code + 1;
        }
        oldcode = incode;
        if ((e = out_progress(out, &next, usize, pg)) != E_OK)
            break;
    }
out:
    free(prefix);
    free(suffix);
    free(stack);
    return e;
}

int arc_unpack(int method, BIN *in, BOUT *out, long usize, PROG *pg)
{
    SINK s;
    int e;
    long base = out->count;
    long next = 0;
    memset(&s, 0, sizeof s);
    s.r.out = out;
    switch (method) {
    case CM_ARCPACK: {
        int c;
        s.rle = 1;
        e = E_OK;
        while ((c = bin_byte(in)) >= 0) {
            unrle_put(&s.r, c);
            if ((e = out_progress(out, &next, usize, pg)) != E_OK)
                break;
        }
        break;
    }
    case CM_ARCSQUEEZE:
        s.rle = 1;
        e = unsqueeze(in, &s, out, usize, pg);
        break;
    case CM_ARCOLD5: case CM_ARCOLD6: case CM_ARCOLD7:
        s.rle = method != CM_ARCOLD5;
        e = uncrunch_old(in, &s, out, method == CM_ARCOLD7, usize, pg);
        break;
    case CM_ARCCRUNCH:
        s.rle = 1;
        e = decomp(in, &s, out, 0, usize, pg);
        break;
    case CM_ARCSQUASH:
        e = decomp(in, &s, out, 1, usize, pg);
        break;
    default:
        return E_UNSUP;
    }
    return finish(out, usize < 0 ? -1 : usize + base, e);
}

/* ---------------------------------------- ARC 8/9: dynamic LZW (encode) */

/* RLE90 encoding, as an input filter */
typedef struct {
    BIN *in;
    int  last, cnt, eof;
    u8   q[4];
    int  qn, qp;
} RLE;

static void rle_emit(RLE *r)
{
    if (r->cnt == 2)
        r->q[r->qn++] = (u8)r->last;
    else if (r->cnt > 2) {
        r->q[r->qn++] = DLE;
        r->q[r->qn++] = (u8)r->cnt;
    }
}

static int rle_get(RLE *r)
{
    while (r->qp >= r->qn) {
        int c;
        r->qp = r->qn = 0;
        if (r->eof)
            return -1;
        c = bin_byte(r->in);
        if (c < 0) {
            r->eof = 1;
            if (r->cnt > 1)
                rle_emit(r);
            continue;
        }
        if (c == r->last && r->cnt < 255)
            r->cnt++;
        else {
            rle_emit(r);
            r->q[r->qn++] = (u8)c;
            r->last = c;
            r->cnt = 1;
        }
        if (c == DLE) {
            r->q[r->qn++] = 0;
            r->last = -1;
        }
    }
    return r->q[r->qp++];
}

typedef struct {
    BOUT *out;
    int   bits, n_bits, maxcode, max_maxcode, free_ent, clear_flg;
    int   offset;
    long  bytes_out;
    u8    buf[16];
} APUT;

static void aflush(APUT *p, int n)
{
    bout_put(p->out, p->buf, n);
    p->bytes_out += n;
    memset(p->buf, 0, sizeof p->buf);
    p->offset = 0;
}

static void aput(APUT *p, int code)
{
    int r_off = p->offset & 7, bits = p->n_bits;
    u8 *bp = p->buf + (p->offset >> 3);
    *bp = (u8)((*bp & ((1 << r_off) - 1)) | (code << r_off));
    bp++;
    bits -= 8 - r_off;
    code >>= 8 - r_off;
    if (bits >= 8) {
        *bp++ = (u8)code;
        code >>= 8;
        bits -= 8;
    }
    if (bits > 0)
        *bp = (u8)code;
    p->offset += p->n_bits;
    if (p->offset == p->n_bits << 3)
        aflush(p, p->n_bits);
    if (p->free_ent > p->maxcode || p->clear_flg > 0) {
        if (p->offset > 0)
            aflush(p, p->n_bits);
        if (p->clear_flg) {
            p->maxcode = MAXCODE(p->n_bits = INIT_BITS);
            p->clear_flg = 0;
        } else {
            p->n_bits++;
            p->maxcode = p->n_bits == p->bits ? p->max_maxcode : MAXCODE(p->n_bits);
        }
    }
}

static int arc_lzw_pack(BIN *in, BOUT *out, int squash, long usize, PROG *pg)
{
    int hsize = squash ? 10007 : 5003, gap = squash ? 10000 : 2048;
    long *htab = malloc(hsize * sizeof(long));
    u16 *codetab = malloc(hsize * sizeof(u16));
    APUT p;
    RLE r;
    int hshift = 0, ent, c, e = E_OK;
    long fc, in_count = 1, checkpoint = gap, ratio = 0, next = 0;
    if (!htab || !codetab) {
        free(htab);
        free(codetab);
        return E_NOMEM;
    }
    memset(&p, 0, sizeof p);
    memset(&r, 0, sizeof r);
    r.in = in;
    r.last = -1;
    r.cnt = 1;
    p.out = out;
    p.bits = squash ? 13 : 12;
    p.max_maxcode = 1 << p.bits;
    p.n_bits = INIT_BITS;
    p.maxcode = MAXCODE(INIT_BITS);
    p.free_ent = A_FIRST;
    if (!squash) {
        bout_byte(out, 12);
        p.bytes_out = 1;
    }
    memset(htab, 0xFF, hsize * sizeof(long));
    for (fc = hsize; fc < 65536L; fc *= 2)
        hshift++;
    hshift = 8 - hshift;
#define NEXTC() (squash ? bin_byte(in) : rle_get(&r))
    ent = NEXTC();
    if (ent < 0)
        goto done;
    while ((c = NEXTC()) >= 0) {
        long fcode = ((long)c << p.bits) + ent;
        int i = (c << hshift) ^ ent, disp;
        in_count++;
        if (htab[i] == fcode) {
            ent = codetab[i];
            continue;
        }
        if (htab[i] >= 0) {
            disp = i == 0 ? 1 : hsize - i;
            for (;;) {
                if ((i -= disp) < 0)
                    i += hsize;
                if (htab[i] == fcode || htab[i] < 0)
                    break;
            }
            if (htab[i] == fcode) {
                ent = codetab[i];
                continue;
            }
        }
        aput(&p, ent);
        ent = c;
        if (p.free_ent < p.max_maxcode) {
            codetab[i] = (u16)p.free_ent++;
            htab[i] = fcode;
        } else if (in_count >= checkpoint) {
            long rat;
            checkpoint = in_count + gap;
            if (in_count > 0x007FFFFFL) {
                rat = p.bytes_out >> 8;
                rat = rat ? in_count / rat : 0x7FFFFFFFL;
            } else
                rat = (in_count << 8) / (p.bytes_out ? p.bytes_out : 1);
            if (rat > ratio)
                ratio = rat;
            else {
                ratio = 0;
                memset(htab, 0xFF, hsize * sizeof(long));
                p.free_ent = A_FIRST;
                p.clear_flg = 1;
                aput(&p, A_CLEAR);
            }
        }
        if ((e = in_progress(in, &next, usize, pg)) != E_OK)
            break;
    }
#undef NEXTC
    if (!e) {
        aput(&p, ent);
        if (p.offset)
            aflush(&p, (p.offset + 7) / 8);
    }
done:
    free(htab);
    free(codetab);
    return e ? e : out->err;
}

int arc_crunch(BIN *in, BOUT *out, long usize, PROG *pg)
{
    return arc_lzw_pack(in, out, 0, usize, pg);
}

int arc_squash(BIN *in, BOUT *out, long usize, PROG *pg)
{
    return arc_lzw_pack(in, out, 1, usize, pg);
}
