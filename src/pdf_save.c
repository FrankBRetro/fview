/*
 * pdf_save.c - write a picture as a one-page PDF (FConv).
 *
 * The page holds one image: palette pictures keep their palette
 * (/Indexed, 1, 2, 4 or 8 bits per pixel), true colour is /DeviceRGB.
 * The image data is Flate (zlib) compressed by FArchive's deflate
 * compressor (deflate.c), so every PDF reader (FView included) can show
 * it and no library is needed.
 *
 * The page is the picture at 72 dpi (one pixel = one point), stretched
 * to keep non-square pixels (Amiga interlace, ST medium) in proportion.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fview.h"
#include "deflate.h"

/* ------------------------------------------------------------- deflate */

struct PDFW {
    FILE *f;
    long  nout;                        /* bytes written to the file       */
    int   err;
    /* image */
    long  w, h, rowbytes, rows;
    long  obj[8];                      /* file offsets of objects 1..7    */
    long  stream_start;
    /* compression: the shared deflate compressor writes through zout */
    DEFL *defl;
    BOUT  zout;
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

static int zsink(void *ctx, const u8 *d, long n)
{
    PDFW *p = ctx;
    out_bytes(p, d, n);
    p->zbytes += n;
    return p->err;
}

static void deflate_put(PDFW *p, const u8 *d, long n)
{
    long i;
    for (i = 0; i < n; i++) {
        p->ad_a = (p->ad_a + d[i]) % 65521UL;
        p->ad_b = (p->ad_b + p->ad_a) % 65521UL;
    }
    defl_put(p->defl, d, n);
}

static void deflate_end(PDFW *p)
{
    defl_end(p->defl);
    bout_flush(&p->zout);
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
    long pw, ph;
    *err = E_NOMEM;
    if (!p)
        return 0;
    bout_func(&p->zout, zsink, p);
    p->defl = defl_new(&p->zout, 2);
    if (!p->defl)
        goto fail;
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
    defl_free(p->defl);
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
    defl_free(p->defl);
    free(p);
    return abort ? E_CANCEL : e;
}
