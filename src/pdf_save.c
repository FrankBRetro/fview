/*
 * pdf_save.c - write a picture as a one-page PDF (FConv).
 *
 * The page holds one image: palette pictures keep their palette
 * (/Indexed, 1, 2, 4 or 8 bits per pixel), true colour is /DeviceRGB.
 * The image data is Flate (zlib) compressed by a small compressor of our
 * own - LZ77 with hash chains, fixed Huffman codes - so every PDF reader
 * (FView included) can show it and no library is needed.
 *
 * The page is the picture at 72 dpi (one pixel = one point), stretched
 * to keep non-square pixels (Amiga interlace, ST medium) in proportion.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fview.h"

/* ------------------------------------------------------------- deflate */

#define RING     65536L                /* input ring, power of two        */
#define RMASK    (RING - 1)
#define WINDOW   32768L                /* deflate's look-back             */
#define HBITS    13
#define HSIZE    (1 << HBITS)
#define MAXCHAIN 12
#define MINMATCH 3
#define MAXMATCH 258
#define NIL      (-1L)

struct PDFW {
    FILE *f;
    long  nout;                        /* bytes written to the file       */
    int   err;
    /* image */
    long  w, h, rowbytes, rows;
    long  obj[8];                      /* file offsets of objects 1..7    */
    long  stream_start;
    /* deflate state */
    u8   *ring;
    long *head, *prev;                 /* hash heads, chains (by pos & 32K) */
    long  inpos;                       /* bytes received                  */
    long  cur;                         /* next byte to encode             */
    u32   bitbuf;
    int   bitcnt;
    u32   ad_a, ad_b;                  /* adler-32                        */
    long  zbytes;                      /* compressed stream length        */
};

static void out_bytes(PDFW *p, const void *d, long n)
{
    if (p->err)
        return;
    if ((long)fwrite(d, 1, n, p->f) != n)
        p->err = E_WRITE;
    p->nout += n;
}

static void out_str(PDFW *p, const char *s)
{
    out_bytes(p, s, (long)strlen(s));
}

static void zbyte(PDFW *p, int b)
{
    u8 c = (u8)b;
    out_bytes(p, &c, 1);
    p->zbytes++;
}

/* deflate packs bits from the least significant end */
static void put_bits(PDFW *p, u32 v, int n)
{
    p->bitbuf |= v << p->bitcnt;
    p->bitcnt += n;
    while (p->bitcnt >= 8) {
        zbyte(p, (int)(p->bitbuf & 0xFF));
        p->bitbuf >>= 8;
        p->bitcnt -= 8;
    }
}

/* Huffman codes go most significant bit first */
static void put_code(PDFW *p, u32 code, int n)
{
    u32 r = 0;
    int i;
    for (i = 0; i < n; i++)
        r |= ((code >> i) & 1) << (n - 1 - i);
    put_bits(p, r, n);
}

/* fixed literal/length code for symbol 0..287 */
static void put_sym(PDFW *p, int s)
{
    if (s < 144)      put_code(p, 0x30 + s, 8);
    else if (s < 256) put_code(p, 0x190 + (s - 144), 9);
    else if (s < 280) put_code(p, s - 256, 7);
    else              put_code(p, 0xC0 + (s - 280), 8);
}

static const unsigned short len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const unsigned char len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const unsigned short dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const unsigned char dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static void put_match(PDFW *p, int len, long dist)
{
    int i;
    for (i = 28; i > 0 && len_base[i] > len; i--)
        ;
    put_sym(p, 257 + i);
    if (len_extra[i])
        put_bits(p, (u32)(len - len_base[i]), len_extra[i]);
    for (i = 29; i > 0 && dist_base[i] > dist; i--)
        ;
    put_code(p, (u32)i, 5);
    if (dist_extra[i])
        put_bits(p, (u32)(dist - dist_base[i]), dist_extra[i]);
}

static unsigned hash3(PDFW *p, long pos)
{
    const u8 *r = p->ring;
    u32 v = ((u32)r[pos & RMASK] << 16) | ((u32)r[(pos + 1) & RMASK] << 8) |
            r[(pos + 2) & RMASK];
    return (unsigned)((v * 2654435761UL) >> (32 - HBITS)) & (HSIZE - 1);
}

static void insert(PDFW *p, long pos)
{
    unsigned h = hash3(p, pos);
    p->prev[pos & (WINDOW - 1)] = p->head[h];
    p->head[h] = pos;
}

/* encode what has arrived, keeping MAXMATCH bytes of look-ahead unless
   this is the end of the data                                         */
static void deflate_run(PDFW *p, int final)
{
    const u8 *r = p->ring;
    long limit = final ? p->inpos : p->inpos - MAXMATCH;
    while (p->cur < limit && !p->err) {
        long pos = p->cur, avail = p->inpos - pos, best = 0, bdist = 0;
        if (avail >= MINMATCH) {
            long cand = p->head[hash3(p, pos)];
            int chain = MAXCHAIN;
            long maxlen = avail < MAXMATCH ? avail : MAXMATCH;
            while (cand != NIL && pos - cand <= WINDOW - 1 && chain--) {
                long n = 0;
                if (r[(cand + best) & RMASK] == r[(pos + best) & RMASK])
                    while (n < maxlen && r[(cand + n) & RMASK] == r[(pos + n) & RMASK])
                        n++;
                if (n > best) {
                    best = n;
                    bdist = pos - cand;
                    if (n == maxlen)
                        break;
                }
                cand = p->prev[cand & (WINDOW - 1)];
            }
        }
        if (best >= MINMATCH) {
            long k;
            put_match(p, (int)best, bdist);
            for (k = 0; k < best; k++)
                if (p->inpos - (pos + k) >= MINMATCH)
                    insert(p, pos + k);
            p->cur += best;
        } else {
            put_sym(p, r[pos & RMASK]);
            if (avail >= MINMATCH)
                insert(p, pos);
            p->cur++;
        }
    }
}

static void deflate_put(PDFW *p, const u8 *d, long n)
{
    long i;
    for (i = 0; i < n; i++) {
        p->ad_a = (p->ad_a + d[i]) % 65521UL;
        p->ad_b = (p->ad_b + p->ad_a) % 65521UL;
    }
    while (n > 0 && !p->err) {
        /* never overwrite bytes still in the window or not yet encoded */
        long room = RING - WINDOW - (p->inpos - p->cur), k;
        if (room > n) room = n;
        if (room <= 0) {
            deflate_run(p, 0);
            continue;
        }
        for (k = 0; k < room; k++)
            p->ring[(p->inpos + k) & RMASK] = d[k];
        p->inpos += room;
        d += room;
        n -= room;
        deflate_run(p, 0);
    }
}

static void deflate_end(PDFW *p)
{
    deflate_run(p, 1);
    put_sym(p, 256);                         /* end of block */
    if (p->bitcnt)
        put_bits(p, 0, 8 - p->bitcnt);       /* flush to a byte */
    zbyte(p, (int)(p->ad_b >> 8)); zbyte(p, (int)(p->ad_b & 0xFF));
    zbyte(p, (int)(p->ad_a >> 8)); zbyte(p, (int)(p->ad_a & 0xFF));
}

/* ----------------------------------------------------------------- PDF */

static void begin_obj(PDFW *p, int n)
{
    char b[24];
    p->obj[n] = p->nout;
    sprintf(b, "%d 0 obj\n", n);
    out_str(p, b);
}

/* w x h pixels, ncomp 1 (palette: pal/npal) or 3 (RGB), bpc bits per
   component, pixel shape aspx:aspy                                   */
PDFW *pdfw_open(const char *path, long w, long h, int ncomp, int bpc,
                const u8 *pal, int npal, int aspx, int aspy, int *err)
{
    PDFW *p = calloc(1, sizeof(PDFW));
    char b[200];
    long pw, ph, i;
    *err = E_NOMEM;
    if (!p)
        return 0;
    p->ring = malloc(RING);
    p->head = malloc(HSIZE * sizeof(long));
    p->prev = malloc(WINDOW * sizeof(long));
    if (!p->ring || !p->head || !p->prev)
        goto fail;
    for (i = 0; i < HSIZE; i++)
        p->head[i] = NIL;
    for (i = 0; i < WINDOW; i++)
        p->prev[i] = NIL;
    p->f = fopen(path, "wb");
    if (!p->f) {
        *err = E_WRITE;
        goto fail;
    }
    p->w = w;
    p->h = h;
    p->rowbytes = (w * ncomp * bpc + 7) / 8;
    p->ad_a = 1;
    /* page size in points: square pixels are 1 pt, others stretched */
    pw = w;
    ph = h;
    if (aspx > 0 && aspy > 0 && aspx != aspy) {
        if (aspx > aspy) pw = w * aspx / aspy;
        else ph = h * aspy / aspx;
    }
    if (pw < 1) pw = 1;
    if (ph < 1) ph = 1;

    out_str(p, "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n");
    begin_obj(p, 1);
    out_str(p, "<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    begin_obj(p, 2);
    out_str(p, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n");
    begin_obj(p, 3);
    sprintf(b, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %ld %ld]\n"
            "   /Resources << /XObject << /Im0 5 0 R >> >> /Contents 4 0 R >>\nendobj\n",
            pw, ph);
    out_str(p, b);
    begin_obj(p, 4);
    {
        char c[80];
        sprintf(c, "q %ld 0 0 %ld 0 0 cm /Im0 Do Q\n", pw, ph);
        sprintf(b, "<< /Length %ld >>\nstream\n", (long)strlen(c));
        out_str(p, b);
        out_str(p, c);
        out_str(p, "endstream\nendobj\n");
    }
    begin_obj(p, 5);
    sprintf(b, "<< /Type /XObject /Subtype /Image /Width %ld /Height %ld\n"
            "   /BitsPerComponent %d /ColorSpace ", w, h, bpc);
    out_str(p, b);
    if (ncomp == 3)
        out_str(p, "/DeviceRGB");
    else {
        /* [/Indexed /DeviceRGB hival <palette>] */
        int hi = (1 << bpc) - 1, k;
        sprintf(b, "[/Indexed /DeviceRGB %d <", hi);
        out_str(p, b);
        for (k = 0; k <= hi; k++) {
            const u8 *c = k < npal ? pal + k * 3 : (const u8 *)"\0\0\0";
            sprintf(b, "%02X%02X%02X%s", c[0], c[1], c[2], (k & 7) == 7 ? "\n" : "");
            out_str(p, b);
        }
        out_str(p, ">]");
    }
    out_str(p, "\n   /Filter /FlateDecode /Length 6 0 R >>\nstream\n");
    p->stream_start = p->nout;
    zbyte(p, 0x78);                          /* zlib header: deflate, 32K */
    zbyte(p, 0x01);
    put_bits(p, 1, 1);                       /* BFINAL */
    put_bits(p, 1, 2);                       /* fixed Huffman codes */
    if (p->err) {
        *err = p->err;
        fclose(p->f);
        remove(path);
        p->f = 0;
        goto fail;
    }
    *err = E_OK;
    return p;
fail:
    if (p->f) fclose(p->f);
    free(p->ring);
    free(p->head);
    free(p->prev);
    free(p);
    return 0;
}

long pdfw_rowbytes(PDFW *p) { return p->rowbytes; }

int pdfw_row(PDFW *p, const u8 *row)
{
    deflate_put(p, row, p->rowbytes);
    p->rows++;
    return p->err;
}

/* finish the file; with abort set, just close and delete it */
int pdfw_close(PDFW *p, const char *path, int abort)
{
    char b[200];
    int e, i;
    long xref;
    if (!abort) {
        deflate_end(p);
        out_str(p, "\nendstream\nendobj\n");
        begin_obj(p, 6);
        sprintf(b, "%ld\nendobj\n", p->zbytes);
        out_str(p, b);
        xref = p->nout;
        out_str(p, "xref\n0 7\n0000000000 65535 f \n");
        for (i = 1; i <= 6; i++) {
            sprintf(b, "%010ld 00000 n \n", p->obj[i]);
            out_str(p, b);
        }
        sprintf(b, "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n", xref);
        out_str(p, b);
    }
    e = p->err;
    if (fclose(p->f) != 0 && !e)
        e = E_WRITE;
    if (abort || e)
        remove(path);
    free(p->ring);
    free(p->head);
    free(p->prev);
    free(p);
    return abort ? E_CANCEL : e;
}
