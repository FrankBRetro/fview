/* img_save.c - GEM (X)IMG writer.
 * Palette images: 1/2/4/8 planes, XIMG RGB palette (VDI pen order by
 * default, the convention used by screen grabbers and most viewers).
 * True colour: 24 planes, chunky RGB scanlines.
 * Compression: solid runs, pattern runs, bit strings and vertical
 * replication of identical scanlines. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fview.h"

typedef struct {
    FILE *f;
    int   planes;         /* data planes (1 for true colour chunky)     */
    long  bpl;            /* bytes per plane line                        */
    u8   *prev, *cur;     /* full scanline (all planes)                  */
    int   rep;            /* number of pending identical lines           */
    int   err;
} IW;

static void wr16(FILE *f, unsigned v) { fputc(v >> 8, f); fputc(v & 255, f); }

static void enc_plane(IW *w, const u8 *s, long n)
{
    long i = 0;
    FILE *f = w->f;
    while (i < n) {
        /* solid run of 00 or FF */
        if (s[i] == 0 || s[i] == 0xFF) {
            long j = i;
            while (j < n && s[j] == s[i] && j - i < 127) j++;
            fputc((s[i] ? 0x80 : 0) | (int)(j - i), f);
            i = j;
            continue;
        }
        /* pattern run (patlen 2) repeating at least 3 times */
        if (i + 5 < n && s[i] == s[i + 2] && s[i + 1] == s[i + 3] &&
            s[i] == s[i + 4] && s[i + 1] == s[i + 5]) {
            long j = i + 2;
            int cnt = 1;
            while (j + 1 < n && s[j] == s[i] && s[j + 1] == s[i + 1] && cnt < 255) {
                j += 2;
                cnt++;
            }
            fputc(0, f); fputc(cnt, f); fputc(s[i], f); fputc(s[i + 1], f);
            i = j;
            continue;
        }
        /* bit string until something compressible starts */
        {
            long j = i;
            while (j < n && j - i < 255 && s[j] != 0 && s[j] != 0xFF)
                j++;
            if (j == i) j++;
            fputc(0x80, f);
            fputc((int)(j - i), f);
            fwrite(s + i, 1, j - i, f);
            i = j;
        }
    }
}

static void flush_line(IW *w)
{
    int p;
    if (!w->rep)
        return;
    if (w->rep > 1) {
        fputc(0, w->f); fputc(0, w->f); fputc(0xFF, w->f); fputc(w->rep, w->f);
    }
    for (p = 0; p < w->planes; p++)
        enc_plane(w, w->prev + p * w->bpl, w->bpl);
    w->rep = 0;
}

/* w->cur holds the new line */
static void put_line(IW *w)
{
    long n = w->bpl * w->planes;
    if (w->rep && w->rep < 255 && !memcmp(w->prev, w->cur, n)) {
        w->rep++;
        return;
    }
    flush_line(w);
    memcpy(w->prev, w->cur, n);
    w->rep = 1;
}

/* pixel size written to the header; img_save() sets the picture's shape */
static unsigned iw_pixw = 279, iw_pixh = 279;

static int iw_open(IW *w, const char *path, int planes, long width, long height,
                   const u8 *pal_by_pix, int vdiorder)
{
    int ncol = planes <= 8 ? 1 << planes : 0, i;
    memset(w, 0, sizeof *w);
    w->f = fopen(path, "wb");
    if (!w->f)
        return E_OPEN;
    w->planes = planes <= 8 ? planes : 1;
    w->bpl = planes <= 8 ? (width + 7) / 8 : width * 3;
    w->prev = malloc(w->bpl * w->planes);
    w->cur = malloc(w->bpl * w->planes);
    if (!w->prev || !w->cur) {
        fclose(w->f);
        free(w->prev); free(w->cur);
        return E_NOMEM;
    }
    wr16(w->f, 1);                                  /* version        */
    wr16(w->f, ncol ? 11 + ncol * 3 : 8);           /* header words   */
    wr16(w->f, planes);
    wr16(w->f, 2);                                  /* pattern length */
    wr16(w->f, iw_pixw); wr16(w->f, iw_pixh);       /* pixel size um  */
    wr16(w->f, (unsigned)width);
    wr16(w->f, (unsigned)height);
    if (ncol) {
        fwrite("XIMG", 1, 4, w->f);
        wr16(w->f, 0);                              /* RGB */
        for (i = 0; i < ncol; i++) {
            int pix = vdiorder ? vdi2pix(i, planes) : i, c;
            for (c = 0; c < 3; c++)
                wr16(w->f, (pal_by_pix[pix * 3 + c] * 1000 + 127) / 255);
        }
    }
    return E_OK;
}

static int iw_close(IW *w)
{
    int e;
    flush_line(w);
    e = ferror(w->f) ? E_WRITE : E_OK;
    if (fclose(w->f))
        e = E_WRITE;
    free(w->prev);
    free(w->cur);
    return e;
}

/* --------------------------------------------- original colour depth */

void img_set_pixel_shape(int aspx, int aspy)
{
    iw_pixw = iw_pixh = 279;
    if (aspx > 0 && aspy > 0) {
        /* the smaller side is 279 um */
        if (aspx <= aspy)
            iw_pixh = (unsigned)(279L * aspy / aspx);
        else
            iw_pixw = (unsigned)(279L * aspx / aspy);
    }
}

int img_save(const char *path, const IMAGE *im, const PREFS *pf, PROG *pg)
{
    IW w;
    int e, planes;
    long y, x;
    u8 pal[256 * 3];

    if (im->type == IT_RGB)
        planes = 24;
    else {
        int need = im->ncolors <= 2 ? 1 : im->ncolors <= 4 ? 2 :
                   im->ncolors <= 16 ? 4 : 8;
        planes = im->orig_bits <= 1 ? 1 : im->orig_bits <= 2 ? 2 :
                 im->orig_bits <= 4 ? 4 : 8;
        if (planes < need)
            planes = need;
        memset(pal, 0, sizeof pal);
        memcpy(pal, im->pal, im->ncolors * 3);
        /* mono IMGs conventionally have pixel 1 = black: when index 0 is
           the darker colour the data is inverted below so plain (non
           XIMG) readers show it the right way round                    */
    }
    if (im->aspx > 0 && im->aspy > 0) {
        /* keep the pixel shape: scale so the smaller side is 279 um */
        if (im->aspx <= im->aspy) {
            iw_pixw = 279;
            iw_pixh = (unsigned)(279L * im->aspy / im->aspx);
        } else {
            iw_pixh = 279;
            iw_pixw = (unsigned)(279L * im->aspx / im->aspy);
        }
    }
    e = iw_open(&w, path, planes, im->w, im->h, pal, pf->img_vdiorder);
    iw_pixw = iw_pixh = 279;
    if (e)
        return e;
    for (y = 0; y < im->h; y++) {
        const u8 *s = im->px + y * im->stride;
        if (planes == 24)
            memcpy(w.cur, s, im->w * 3);
        else {
            int p, inv = planes == 1 && pal[0] < pal[3];
            memset(w.cur, 0, w.bpl * planes);
            for (x = 0; x < im->w; x++) {
                unsigned v = s[x];
                if (inv) v ^= 1;
                for (p = 0; v && p < planes; p++, v >>= 1)
                    if (v & 1)
                        w.cur[p * w.bpl + (x >> 3)] |= 0x80 >> (x & 7);
            }
        }
        put_line(&w);
        if ((y & 15) == 0 && prog_step(pg, y, im->h)) {
            iw_close(&w);
            remove(path);
            return E_CANCEL;
        }
    }
    if (planes == 1 && pal[0] < pal[3]) {
        /* rewrite the palette to match the inverted data */
        u8 t[3];
        memcpy(t, pal, 3); memcpy(pal, pal + 3, 3); memcpy(pal + 3, t, 3);
        fseek(w.f, 22, SEEK_SET);
        {
            int i, c;
            for (i = 0; i < 2; i++)
                for (c = 0; c < 3; c++)
                    wr16(w.f, (pal[i * 3 + c] * 1000 + 127) / 255);
        }
        fseek(w.f, 0, SEEK_END);
    }
    return iw_close(&w);
}

/* -------------------------------------------- dithered (display) data */

int img_save_planes(const char *path, const u16 *std, long width, long height,
                    long wdwidth, int planes, const u8 *pal_by_pix,
                    const PREFS *pf, PROG *pg)
{
    IW w;
    int e, p;
    long y, x, psize;
    e = iw_open(&w, path, planes, width, height, pal_by_pix, pf->img_vdiorder);
    iw_pixw = iw_pixh = 279;
    if (e)
        return e;
    for (y = 0; y < height; y++) {
        const u16 *row = std_row((u16 *)std, y, height, wdwidth, planes, &psize);
        for (p = 0; p < planes; p++) {
            const u16 *src = row + p * psize;
            u8 *d = w.cur + p * w.bpl;
            for (x = 0; x < w.bpl; x++) {
                u16 v = src[x >> 1];
                d[x] = (x & 1) ? (u8)v : (u8)(v >> 8);
            }
        }
        put_line(&w);
        if ((y & 15) == 0 && prog_step(pg, y, height)) {
            iw_close(&w);
            remove(path);
            return E_CANCEL;
        }
    }
    return iw_close(&w);
}

int img_save_rgbrows(const char *path, long width, long height,
                     void (*getrow)(void *ctx, long y, u8 *rgb), void *ctx,
                     PROG *pg)
{
    IW w;
    int e;
    long y;
    e = iw_open(&w, path, 24, width, height, 0, 0);
    iw_pixw = iw_pixh = 279;
    if (e)
        return e;
    for (y = 0; y < height; y++) {
        getrow(ctx, y, w.cur);
        put_line(&w);
        if ((y & 15) == 0 && prog_step(pg, y, height)) {
            iw_close(&w);
            remove(path);
            return E_CANCEL;
        }
    }
    return iw_close(&w);
}
