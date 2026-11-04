/* dec_stb.c - PNG and JPEG via stb_image (public domain, nothings/stb),
 * fed through read callbacks so loading reports progress and can be
 * cancelled. Also exposes the zlib inflater for the PDF reader. */
#include <stdlib.h>
#include <string.h>
#include "rd.h"

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_ASSERT(x) ((void)0)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static int cb_read(void *user, char *data, int size)
{
    RD *r = user;
    if (r->pg && r->pg->cancelled)
        return 0;
    return (int)rd_read(r, data, size);
}
static void cb_skip(void *user, int n)
{
    RD *r = user;
    if (n > 0)
        rd_skip(r, n);
    else if (n < 0) {
        /* stb only rewinds within data it has just read */
        long p = ftell(r->f) - (r->len - r->pos) + n;
        fseek(r->f, p, SEEK_SET);
        r->pos = r->len = 0;
        r->done = p;
        r->eof = 0;
    }
}
static int cb_eof(void *user)
{
    RD *r = user;
    return rd_peek(r, 0) < 0 || (r->pg && r->pg->cancelled);
}

/* PNG header: bit depth and colour type for orig_bits */
static int png_bits(const char *path, int *ctype)
{
    u8 h[29];
    FILE *f = fopen(path, "rb");
    int n;
    if (!f)
        return 8;
    n = (int)fread(h, 1, sizeof h, f);
    fclose(f);
    if (n < 29 || memcmp(h + 12, "IHDR", 4))
        return 8;
    *ctype = h[25];
    switch (h[25]) {
    case 0: return h[24];            /* grey */
    case 3: return h[24];            /* palette */
    case 2: return h[24] * 3;
    case 4: return h[24] * 2;
    case 6: return h[24] * 4;
    }
    return 8;
}

int dec_stb(const char *path, IMAGE *im, const PREFS *pf, const DISPINFO *d, PROG *pg)
{
    RD r;
    stbi_io_callbacks cb;
    int e, w, h, comp, want, isjpg, ctype = 2, bits;
    u8 *data;
    long x, y;
    int bv = bg_value(pf);

    cb.read = cb_read;
    cb.skip = cb_skip;
    cb.eof = cb_eof;

    e = rd_open(&r, path, pg);
    if (e)
        return e;
    if (!stbi_info_from_callbacks(&cb, &r, &w, &h, &comp)) {
        rd_close(&r);
        strncpy(fv_errdetail, stbi_failure_reason(), sizeof fv_errdetail - 1);
        return E_FORMAT;
    }
    rd_close(&r);

    isjpg = ftype_detect(path) == FT_JPEG;
    bits = isjpg ? (comp == 1 ? 8 : 24) : png_bits(path, &ctype);

    want = comp;
    if (isjpg && comp == 3 && pf->jpg_grey_mono && d && d->planes == 1)
        want = 1;                       /* stb converts during decode */
    if (comp == 2 || comp == 4)
        want = pf->png_alpha ? 4 : 3;

    e = rd_open(&r, path, pg);
    if (e)
        return e;
    data = stbi_load_from_callbacks(&cb, &r, &w, &h, &comp, want);
    rd_close(&r);
    if (!data) {
        if (pg && pg->cancelled)
            return E_CANCEL;
        strncpy(fv_errdetail, stbi_failure_reason(), sizeof fv_errdetail - 1);
        return strstr(fv_errdetail, "outofmem") ? E_NOMEM : E_FORMAT;
    }

    im->w = w; im->h = h;
    im->type = want == 1 ? IT_INDEXED : IT_RGB;
    e = img_alloc_px(im, RB_ORIG);
    if (e) {
        stbi_image_free(data);
        return e;
    }
    if (want == 1) {
        int i;
        for (i = 0; i < 256; i++)
            im->pal[i * 3] = im->pal[i * 3 + 1] = im->pal[i * 3 + 2] = (u8)i;
        im->ncolors = 256;
        memcpy(im->px, data, (long)w * h);
    } else {
        for (y = 0; y < h; y++) {
            const u8 *s = data + (long)y * w * want;
            u8 *o = im->px + y * im->stride;
            if (want == 3)
                memcpy(o, s, (long)w * 3);
            else
                for (x = 0; x < w; x++, s += 4, o += 3) {
                    int a = s[3], ia = 255 - a;
                    o[0] = (s[0] * a + bv * ia + 127) / 255;
                    o[1] = (s[1] * a + bv * ia + 127) / 255;
                    o[2] = (s[2] * a + bv * ia + 127) / 255;
                }
            if ((y & 31) == 0 && prog_step(pg, y, h)) {
                stbi_image_free(data);
                return E_CANCEL;
            }
        }
    }
    stbi_image_free(data);
    im->orig_bits = bits > 24 ? 24 : bits;
    if (isjpg)
        sprintf(im->fmtinfo, "JPEG, %s", comp == 1 ? "greyscale" : "colour");
    else
        sprintf(im->fmtinfo, "PNG, %d bit%s", bits,
                ctype == 3 ? " palette" : (comp == 2 || comp == 4) ? " + alpha" : "");
    if (im->type == IT_RGB)
        img_reduce(im, RB_ORIG);
    else if (!isjpg)
        img_compact(im);        /* low bit depth grey: only levels used */
    return E_OK;
}

/* JPEG data held in memory (PDF DCTDecode images); malloc'd pixels */
u8 *fv_jpeg_mem(const u8 *src, long len, int *w, int *h, int *comp)
{
    int c = 0;
    u8 *p = stbi_load_from_memory(src, (int)len, w, h, &c, 0);
    if (p && c == 2) {                 /* grey + alpha: drop alpha */
        long i, n = (long)*w * *h;
        for (i = 0; i < n; i++) p[i] = p[i * 2];
        c = 1;
    }
    *comp = c;
    return p;
}

/* zlib for the PDF reader */
u8 *fv_inflate(const u8 *src, long len, long *outlen)
{
    int ol = 0;
    char *r = stbi_zlib_decode_malloc((const char *)src, (int)len, &ol);
    *outlen = ol;
    return (u8 *)r;
}
void fv_inflate_free(u8 *p) { STBI_FREE(p); }
