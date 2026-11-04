/*
 * util.c - preferences defaults, progress, file type detection, buffers,
 *          image helpers, charset conversion and VDI pen tables.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "fview.h"
#include "doc.h"

char fv_errdetail[80];

void prefs_default(PREFS *p)
{
    memset(p, 0, sizeof(*p));
    p->start_iconified = 1;
    p->use_workers   = 1;
    p->dither        = DITH_FS;
    p->serpentine    = 1;
    p->zoom          = ZOOM_REAL;
    p->smooth        = 1;
    p->palmode       = PAL_OPTIMISE;
    p->keep_sys      = 1;
    p->bg            = BG_WHITE;
    p->gif_trans     = 1;
    p->png_alpha     = 1;
    p->iff_mask      = 1;
    p->iff_ham       = 1;
    p->img_syspal    = 1;
    p->img_vdiorder  = 1;
    p->jpg_grey_mono = 1;
    p->tabw          = 8;
    p->wrap          = 0;
    p->charset       = CS_ATARI;
    p->md_markup     = 0;
    p->rtf_pict      = 1;
    p->pdf_zoom      = PDFZ_FITW;
    p->pdf_gfx       = 1;
    p->pdf_imgbox    = 1;
    p->prt_scale     = PRS_FIT;
    p->prt_dither    = DITH_FS;
    p->prt_header    = 1;
}

int prog_step(PROG *p, long done, long total)
{
    if (!p)
        return 0;
    if (p->cancelled)
        return 1;
    if (p->fn && p->fn(p, done, total))
        p->cancelled = 1;
    return p->cancelled;
}

const char *fv_errstr(int e)
{
    switch (e) {
    case E_OK:     return "OK";
    case E_OPEN:   return "Cannot open file";
    case E_FORMAT: return "File is damaged or not in the expected format";
    case E_NOMEM:  return "Not enough memory";
    case E_CANCEL: return "Cancelled";
    case E_UNSUP:  return "This variant of the format is not supported";
    case E_WRITE:  return "Error writing file";
    }
    return "Unknown error";
}

/* ------------------------------------------------------------ detection */

static int ext_is(const char *path, const char *ext)
{
    const char *d = strrchr(path, '.');
    const char *s1 = strrchr(path, '\\'), *s2 = strrchr(path, '/');
    if (!d || (s1 && s1 > d) || (s2 && s2 > d))
        return 0;
    d++;
    while (*d && *ext) {
        if (tolower((unsigned char)*d) != *ext)
            return 0;
        d++, ext++;
    }
    return *d == 0 && *ext == 0;
}

int ftype_detect(const char *path)
{
    u8 h[16];
    long n;
    FILE *f = fopen(path, "rb");
    if (!f)
        return FT_UNKNOWN;
    memset(h, 0, sizeof h);
    n = (long)fread(h, 1, sizeof h, f);
    fclose(f);

    /* magic numbers first */
    if (n >= 6 && !memcmp(h, "GIF8", 4))                  return FT_GIF;
    if (n >= 8 && !memcmp(h, "\x89PNG\r\n\x1a\n", 8))     return FT_PNG;
    if (n >= 3 && h[0] == 0xFF && h[1] == 0xD8 && h[2] == 0xFF) return FT_JPEG;
    if (n >= 12 && !memcmp(h, "FORM", 4) &&
        (!memcmp(h + 8, "ILBM", 4) || !memcmp(h + 8, "PBM ", 4)))
                                                          return FT_IFF;
    if (n >= 16 && h[0] == 'B' && h[1] == 'M') {
        /* BMP: the info header size must be one of the known ones */
        unsigned long hs = h[14] | (h[15] << 8);
        if (hs == 12 || hs == 16 || hs == 40 || hs == 52 || hs == 56 || hs == 64 ||
            hs == 108 || hs == 124)
            return FT_BMP;
    }
    if (n >= 5 && !memcmp(h, "%PDF-", 5))                 return FT_PDF;
    if (n >= 5 && !memcmp(h, "{\\rtf", 5))                return FT_RTF;
    if (n >= 2 && ((h[0] == 0xFF && h[1] == 0xFE) || (h[0] == 0xFE && h[1] == 0xFF)))
                                                          return FT_UTEXT;
    if (n >= 3 && h[0] == 0xEF && h[1] == 0xBB && h[2] == 0xBF) {
        if (ext_is(path, "md") || ext_is(path, "mkd") || ext_is(path, "markdown"))
            return FT_MARKDOWN;
        return FT_UTEXT;
    }
    /* GEM metafile: -1, then the header length (Intel or Motorola order) */
    if (n >= 4 && h[0] == 0xFF && h[1] == 0xFF &&
        ((h[2] >= 8 && h[2] < 64 && h[3] == 0) || (h[2] == 0 && h[3] >= 8 && h[3] < 64)))
        return FT_GEM;
    if (n >= 4 && h[0] == 'S' && h[1] == 'P' && h[2] == 0 && h[3] == 0)
        return ext_is(path, "sps") ? FT_SPS : FT_SPC;
    if (ext_is(path, "pi1") || ext_is(path, "pi2") || ext_is(path, "pi3"))
        return h[0] == 0x80 ? FT_DEGASC : FT_DEGAS;
    if (ext_is(path, "pc1") || ext_is(path, "pc2") || ext_is(path, "pc3"))
        return FT_DEGASC;
    if (ext_is(path, "neo"))  return FT_NEO;
    if (ext_is(path, "spu"))  return FT_SPU;
    if (ext_is(path, "spc"))  return FT_SPC;
    if (ext_is(path, "sps"))  return FT_SPS;
    if (ext_is(path, "pcs"))  return FT_PCS;
    if (ext_is(path, "pbx"))  return FT_PBX;
    /* GEM IMG has no magic, check header sanity plus extension */
    if (ext_is(path, "img") && n >= 16) {
        unsigned hl = (h[2] << 8) | h[3], pl = (h[4] << 8) | h[5];
        if (hl >= 8 && hl < 2048 && pl >= 1 && pl <= 32)
            return FT_IMG;
    }
    if (ext_is(path, "gif"))  return FT_GIF;
    if (ext_is(path, "png"))  return FT_PNG;
    if (ext_is(path, "jpg") || ext_is(path, "jpeg") || ext_is(path, "jpe"))
                              return FT_JPEG;
    if (ext_is(path, "iff") || ext_is(path, "ilb") || ext_is(path, "lbm"))
                              return FT_IFF;
    if (ext_is(path, "bmp") || ext_is(path, "dib"))
                              return FT_BMP;
    if (ext_is(path, "md") || ext_is(path, "mkd") || ext_is(path, "markdown"))
                              return FT_MARKDOWN;
    if (ext_is(path, "rtf"))  return FT_RTF;
    if (ext_is(path, "pdf"))  return FT_PDF;
    if (ext_is(path, "img"))  return FT_IMG;
    /* anything else is shown as text; UTF-8 is detected by the decoder */
    return FT_TEXT;
}

const char *ftype_name(int t)
{
    static const char *nm[] = { "Unknown", "GIF", "PNG", "IFF", "GEM IMG",
        "JPEG", "BMP", "Degas", "Degas Elite", "NEOchrome", "Spectrum 512",
        "Spectrum 512", "Spectrum 512", "Photochrome", "QuantumPaint",
        "Text", "Unicode text", "Markdown", "RTF", "PDF", "GEM metafile" };
    if (t < 0 || t > FT_GEM)
        t = 0;
    return nm[t];
}

/* -------------------------------------------------------------- buffers */

void buf_init(BUF *b) { b->p = 0; b->len = b->cap = 0; b->err = 0; }
void buf_free(BUF *b) { free(b->p); buf_init(b); }

static int buf_grow(BUF *b, long need)
{
    long nc;
    u8 *np;
    if (b->err)
        return 0;
    if (b->len + need <= b->cap)
        return 1;
    nc = b->cap ? b->cap : 4096;
    while (nc < b->len + need)
        nc += nc / 2 + 4096;
    np = realloc(b->p, nc);
    if (!np) {
        b->err = 1;
        return 0;
    }
    b->p = np;
    b->cap = nc;
    return 1;
}

void buf_put(BUF *b, const void *d, long n)
{
    if (n <= 0 || !buf_grow(b, n))
        return;
    memcpy(b->p + b->len, d, n);
    b->len += n;
}
void buf_u8(BUF *b, int v) { u8 c = (u8)v; buf_put(b, &c, 1); }
void buf_u16(BUF *b, unsigned v) { u8 c[2]; c[0] = v >> 8; c[1] = v; buf_put(b, c, 2); }
void buf_u32(BUF *b, u32 v)
{
    u8 c[4];
    c[0] = v >> 24; c[1] = v >> 16; c[2] = v >> 8; c[3] = v;
    buf_put(b, c, 4);
}
void buf_set_u32(BUF *b, long at, u32 v)
{
    if (b->err || at + 4 > b->len)
        return;
    b->p[at] = v >> 24; b->p[at + 1] = v >> 16; b->p[at + 2] = v >> 8; b->p[at + 3] = v;
}

int buf_finish(BUF *b, int slot, u8 **out, long *len)
{
    u8 *r;
    if (b->err) {
        buf_free(b);
        return E_NOMEM;
    }
    r = res_alloc(slot, b->len ? b->len : 1);
    if (!r) {
        buf_free(b);
        return E_NOMEM;
    }
    memcpy(r, b->p, b->len);
    *out = r;
    *len = b->len;
    buf_free(b);
    return E_OK;
}

/* --------------------------------------------------------------- images */

void img_init(IMAGE *im)
{
    memset(im, 0, sizeof(*im));
    im->trans = -1;
}

int img_alloc_px(IMAGE *im, int slot)
{
    long bpp = im->type == IT_RGB ? 3 : 1;
    im->stride = im->w * bpp;
    if (im->w <= 0 || im->h <= 0 || im->w > 32000 || im->h > 32000)
        return E_FORMAT;
    im->px = res_alloc(slot, im->stride * im->h);
    if (!im->px)
        return E_NOMEM;
    return E_OK;
}

void img_get_rgb_row(const IMAGE *im, long y, u8 *out)
{
    const u8 *s = im->px + y * im->stride;
    long x;
    if (im->type == IT_RGB) {
        memcpy(out, s, im->w * 3);
        return;
    }
    for (x = 0; x < im->w; x++) {
        const u8 *c = im->pal + s[x] * 3;
        *out++ = c[0]; *out++ = c[1]; *out++ = c[2];
    }
}

/* Convert an RGB image in place to indexed when it uses <= 256 colours.
 * Lossless; makes GIF/palette PNG originals small and exactly savable.  */
void img_reduce(IMAGE *im, int slot)
{
    enum { HS = 1024 };
    u32 *keys;
    short *vals;
    long n = im->w * im->h, i;
    int ncol = 0;
    u8 *p;

    (void)slot;
    if (im->type != IT_RGB)
        return;
    keys = malloc(HS * sizeof(u32));
    vals = malloc(HS * sizeof(short));
    if (!keys || !vals) {
        free(keys); free(vals);
        return;
    }
    for (i = 0; i < HS; i++)
        vals[i] = -1;
    /* pass 1: collect up to 256 colours */
    for (p = im->px, i = 0; i < n; i++, p += 3) {
        u32 k = ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
        unsigned h = (unsigned)((k * 2654435761UL) >> 22) & (HS - 1);
        while (vals[h] >= 0 && keys[h] != k)
            h = (h + 1) & (HS - 1);
        if (vals[h] < 0) {
            if (ncol == 256) {
                free(keys); free(vals);
                return;
            }
            keys[h] = k;
            vals[h] = ncol;
            im->pal[ncol * 3] = p[0];
            im->pal[ncol * 3 + 1] = p[1];
            im->pal[ncol * 3 + 2] = p[2];
            ncol++;
        }
    }
    /* pass 2: rewrite in place (index row is shorter than rgb row) */
    for (p = im->px, i = 0; i < n; i++, p += 3) {
        u32 k = ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
        unsigned h = (unsigned)((k * 2654435761UL) >> 22) & (HS - 1);
        while (keys[h] != k)
            h = (h + 1) & (HS - 1);
        im->px[i] = (u8)vals[h];
    }
    free(keys);
    free(vals);
    im->type = IT_INDEXED;
    im->ncolors = ncol;
    im->stride = im->w;
    if (im->orig_bits > 8 || im->orig_bits == 0)
        im->orig_bits = ncol <= 2 ? 1 : ncol <= 4 ? 2 : ncol <= 16 ? 4 : 8;
}

/* drop unused palette entries of an indexed image */
void img_compact(IMAGE *im)
{
    u8 used[256], map[256], np[256 * 3];
    long i, n = im->w * im->h;
    int k = 0, j;
    if (im->type != IT_INDEXED)
        return;
    memset(used, 0, sizeof used);
    for (i = 0; i < n; i++)
        used[im->px[i]] = 1;
    for (j = 0; j < 256; j++)
        if (used[j]) {
            map[j] = k;
            memcpy(np + k * 3, im->pal + j * 3, 3);
            k++;
        }
    for (i = 0; i < n; i++)
        im->px[i] = map[im->px[i]];
    memcpy(im->pal, np, k * 3);
    im->ncolors = k;
    if (im->trans >= 0)
        im->trans = used[im->trans] ? map[im->trans] : -1;
    im->orig_bits = k <= 2 ? 1 : k <= 4 ? 2 : k <= 16 ? 4 : 8;
}

void img_display_size(const IMAGE *im, const DISPINFO *d, long *w, long *h)
{
    /* ratio of the picture's pixel shape to the screen's pixel shape */
    long fx, fy;
    int pw = d && d->pix_w > 0 ? d->pix_w : 1, ph = d && d->pix_h > 0 ? d->pix_h : 1;
    *w = im->w;
    *h = im->h;
    if (im->aspx <= 0 || im->aspy <= 0)
        return;                         /* square pixels, as before */
    fx = (long)im->aspx * ph;
    fy = (long)im->aspy * pw;
    /* stretch up to 2x; beyond that stretch 2x and shrink the other way
       (e.g. Amiga lores interlace on ST medium's tall pixels)          */
    if (fx * 100 > fy * 103) {          /* wider pixels */
        if (fx > fy * 2) {
            *w = im->w * 2;
            *h = (im->h * fy * 2 + fx / 2) / fx;
        } else
            *w = (im->w * fx + fy / 2) / fy;
    } else if (fy * 100 > fx * 103) {   /* taller pixels */
        if (fy > fx * 2) {
            *h = im->h * 2;
            *w = (im->w * fx * 2 + fy / 2) / fy;
        } else
            *h = (im->h * fy + fx / 2) / fx;
    }
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
    if (*w > 8000) *w = 8000;
    if (*h > 8000) *h = 8000;
}

/* fit sw x sh into bw x bh keeping aspect, never zero */
void img_fit(long sw, long sh, long bw, long bh, long *tw, long *th)
{
    if (bw < 1) bw = 1;
    if (bh < 1) bh = 1;
    if (sw * bh > sh * bw) {          /* width limited */
        *tw = bw;
        *th = (sh * bw + sw / 2) / sw;
    } else {
        *th = bh;
        *tw = (sw * bh + sh / 2) / sh;
    }
    if (*tw < 1) *tw = 1;
    if (*th < 1) *th = 1;
}

/* --------------------------------------------- VDI pen <-> pixel values */

static const u8 v2p4[16] = { 0, 15, 1, 2, 4, 6, 3, 5, 7, 8, 9, 10, 12, 14, 11, 13 };
static const u8 v2p2[4]  = { 0, 3, 1, 2 };

int vdi2pix(int pen, int planes)
{
    switch (planes) {
    case 1: return pen ? 1 : 0;
    case 2: return v2p2[pen & 3];
    case 4: return v2p4[pen & 15];
    case 8:
        if (pen < 16) return pen == 1 ? 255 : v2p4[pen];
        if (pen == 255) return 15;
        return pen;
    }
    return pen;
}

int pix2vdi(int pix, int planes)
{
    int n = planes >= 8 ? 256 : 1 << planes, i;
    for (i = 0; i < n; i++)
        if (vdi2pix(i, planes) == pix)
            return i;
    return pix;
}

/* ------------------------------------------------- true colour packing */

void disp_make_tc_luts(const DISPINFO *d, u32 lut[3][256])
{
    int c, v, i;
    for (c = 0; c < 3; c++)
        for (v = 0; v < 256; v++) {
            u32 pix = 0;
            int nb = d->nbits[c];
            for (i = 0; i < nb; i++)        /* i = 0 is the channel MSB */
                if (v & (0x80 >> i))
                    pix |= 1UL << d->bitpos[c][i];
            lut[c][v] = pix;
        }
}

/* ------------------------------------------------------------- charset */

/* Atari ST character set, 0x80..0xFF, as Unicode */
static const unsigned short atari_hi[128] = {
    0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,
    0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
    0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,
    0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x00DF,0x0192,
    0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,
    0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,
    0x00E3,0x00F5,0x00D8,0x00F8,0x0153,0x0152,0x00C0,0x00C3,
    0x00D5,0x00A8,0x00B4,0x2020,0x00B6,0x00A9,0x00AE,0x2122,
    0x0133,0x0132,0x05D0,0x05D1,0x05D2,0x05D3,0x05D4,0x05D5,
    0x05D6,0x05D7,0x05D8,0x05D9,0x05DB,0x05DC,0x05DE,0x05E0,
    0x05E1,0x05E2,0x05E4,0x05E6,0x05E7,0x05E8,0x05E9,0x05EA,
    0x05DF,0x05DA,0x05DD,0x05E3,0x05E5,0x00A7,0x2227,0x221E,
    0x03B1,0x03B2,0x0393,0x03C0,0x03A3,0x03C3,0x00B5,0x03C4,
    0x03A6,0x0398,0x03A9,0x03B4,0x222E,0x03D5,0x2208,0x2229,
    0x2261,0x00B1,0x2265,0x2264,0x2320,0x2321,0x00F7,0x2248,
    0x00B0,0x2022,0x00B7,0x221A,0x207F,0x00B2,0x00B3,0x00AF
};

/* Windows-1252 0x80..0x9F */
static const unsigned short cp1252_80[32] = {
    0x20AC,0,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,
    0x02C6,0x2030,0x0160,0x2039,0x0152,0,0x017D,0,
    0,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,
    0x02DC,0x2122,0x0161,0x203A,0x0153,0,0x017E,0x0178
};

int uni_to_atari(long cp)
{
    int i;
    if (cp < 0x80)
        return (int)cp;
    for (i = 0; i < 128; i++)
        if (atari_hi[i] == cp)
            return 0x80 + i;
    if (cp == 0x00A0) return ' ';
    if (cp == 0x03B2) return 0xE1;
    if (cp == 0x00DF) return 0x9E;
    if (cp == 0x00B5) return 0xE6;
    return -1;
}

/* write a printable fallback for code points the ST font lacks */
void atari_translit(long cp, char *out)
{
    const char *s = 0;
    char *o = out;
    switch (cp) {
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: s = "'"; break;
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: s = "\""; break;
    case 0x2013: case 0x2010: case 0x2011: case 0x2212: s = "-"; break;
    case 0x2014: case 0x2015: s = "--"; break;
    case 0x2026: s = "..."; break;
    case 0x20AC: s = "EUR"; break;
    case 0x2039: s = "<"; break;
    case 0x203A: s = ">"; break;
    case 0x2030: s = "%."; break;
    case 0x2021: s = "+"; break;
    case 0x02C6: s = "^"; break;
    case 0x02DC: s = "~"; break;
    case 0x00D7: s = "x"; break;
    case 0x2192: s = "->"; break;
    case 0x2190: s = "<-"; break;
    case 0x2122: s = "TM"; break;
    case 0x00A6: s = "|"; break;
    case 0x00AD: s = "-"; break;
    case 0x00B9: s = "1"; break;
    case 0x00BE: s = "3/4"; break;
    case 0x200B: case 0xFEFF: case 0x200C: case 0x200D: s = ""; break;
    case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x202F: s = " "; break;
    case 0x25CF: case 0x25E6: case 0x25AA: case 0x2023: s = "\xF9"; break;
    case 0x2713: case 0x2714: s = "\xFB"; break;
    }
    if (!s) {
        /* strip accents from Latin Extended-A letters */
        static const char lat1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
        static const char latA[] =
            "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
        if (cp >= 0xC0 && cp <= 0xFF) {
            o[0] = lat1[cp - 0xC0]; o[1] = 0;
            return;
        }
        if (cp >= 0x100 && cp < 0x100 + (long)sizeof(latA) - 1) {
            o[0] = latA[cp - 0x100]; o[1] = 0;
            return;
        }
        s = "?";
    }
    strcpy(out, s);
}

int byte_to_atari(int b, int charset)
{
    long cp;
    int r;
    b &= 0xFF;
    if (b < 0x80 || charset == CS_ATARI)
        return b;
    if (charset == CS_CP1252 && b < 0xA0) {
        cp = cp1252_80[b - 0x80];
        if (!cp)
            return '?';
    } else
        cp = b;                        /* Latin-1 == Unicode below 0x100 */
    r = uni_to_atari(cp);
    if (r < 0) {
        char t[8];
        atari_translit(cp, t);
        r = t[0] ? (u8)t[0] : ' ';
    }
    return r;
}
