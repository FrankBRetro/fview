/*
 * dec_gem.c - GEM metafiles (.GEM), as written by GEM Draw, Easy-Draw,
 * Artline, Calamus, META.SYS and the like.
 *
 * A metafile is a recording of VDI calls. They are replayed into the
 * page display list used for PDF (doc.h), so the window gets zoom,
 * scrolling and GDOS printing for free. Polylines, markers, fills,
 * rectangles, rounded boxes, circles, ellipses, arcs and pie slices,
 * graphic text and justified text are drawn with their colours, line
 * widths and fill styles. Bit image references (v_bit_image, used by
 * Easy-Draw and others for placed pictures) load the named GEM IMG file,
 * looked for next to the metafile and then as written; a missing file
 * shows as a box.
 *
 * Files are normally in Intel byte order; Motorola order is accepted too.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rd.h"
#include "doc.h"

void dbg(const char *fmt, ...);      /* step log (log build) */

typedef struct {
    const u8 *f;
    long n, pos;
    int intel;
} MR;

static int mw(MR *m)
{
    int v;
    if (m->pos + 1 >= m->n) {
        m->pos = m->n;
        return -1;
    }
    v = m->intel ? m->f[m->pos] | (m->f[m->pos + 1] << 8)
                 : (m->f[m->pos] << 8) | m->f[m->pos + 1];
    m->pos += 2;
    return (short)v;
}

/* standard VDI colours */
static const u8 vdicol[16][3] = {
    {255,255,255},{0,0,0},{255,0,0},{0,255,0},{0,0,255},{0,255,255},
    {255,255,0},{255,0,255},{192,192,192},{128,128,128},{128,0,0},
    {0,128,0},{0,0,128},{0,128,128},{128,128,0},{128,0,128}
};


typedef struct {
    /* user space -> points */
    double ox, oy, sx, sy;
    int yup;
    double pw, ph;               /* page size in points */
    /* attributes */
    int lcol, lwidth, ltype;
    int fcol, finter, fstyle, fperim;
    int tcol, theight, tpoint, teffects, thalign, tvalign;
    int mcol, mtype, mheight;
    BUF *out;
    long nitems;
} GST;

#define MAXPT 512

static double ux(GST *g, double x) { return (x - g->ox) * g->sx; }
static double uy(GST *g, double y) { return g->yup ? (y - g->oy) * g->sy : g->ph - (y - g->oy) * g->sy; }

static void colour(int c, u8 *o)
{
    if (c < 0 || c > 15) c = 1;
    memcpy(o, vdicol[c], 3);
}

/* points are in user space, pt[2*i], pt[2*i+1] */
/* for PI_FILL, a wpt below 0 carries a VDI fill pattern: -(interior<<8|style) */
static void emit_poly(GST *g, int kind, const double *pt, int n, const u8 *rgb, double wpt)
{
    int i;
    if (n < 2)
        return;
    if (n > 4000) n = 4000;
    buf_u8(g->out, kind);
    buf_put(g->out, rgb, 3);
    if (wpt < 0)
        buf_u16(g->out, 0x8000 | ((unsigned)-wpt & 0x7FFF));
    else
        buf_u16(g->out, (unsigned)(wpt * 16 > 32767 ? 32767 : wpt * 16));
    buf_u16(g->out, n);
    for (i = 0; i < n; i++) {
        buf_u32(g->out, (u32)(i32)(ux(g, pt[i * 2]) * 16));
        buf_u32(g->out, (u32)(i32)(uy(g, pt[i * 2 + 1]) * 16));
    }
    g->nitems++;
}

static double line_pt(GST *g)
{
    double w = g->lwidth * (g->sx < 0 ? -g->sx : g->sx);
    return w < 0.5 ? 0.5 : w;
}

static void stroke(GST *g, const double *pt, int n)
{
    u8 rgb[3];
    colour(g->lcol, rgb);
    emit_poly(g, PI_LINE, pt, n, rgb, line_pt(g));
}

/* filled area with the fill attributes, plus perimeter if switched on */
static void fill(GST *g, double *pt, int n, int perim)
{
    u8 rgb[3];
    colour(g->fcol, rgb);
    if (g->finter == 1)
        emit_poly(g, PI_FILL, pt, n, rgb, 0);
    else if (g->finter == 2 || g->finter == 3) {
        /* the real VDI pattern / hatch, drawn in the fill colour */
        int st = g->fstyle < 1 ? 1 : g->fstyle > 24 ? 24 : g->fstyle;
        emit_poly(g, PI_FILL, pt, n, rgb, -(double)(g->finter << 8 | st));
    } else if (g->finter == 4)                   /* user pattern: medium */
        emit_poly(g, PI_FILL, pt, n, rgb, -(double)(2 << 8 | 4));
    if (perim && n >= 2) {
        /* outline in the fill colour, closed */
        if (n < MAXPT) {
            pt[n * 2] = pt[0];
            pt[n * 2 + 1] = pt[1];
            n++;
        }
        emit_poly(g, PI_LINE, pt, n, rgb, 0.5);
    }
}

/* ellipse / arc outline points; angles in tenths of a degree */
static int arc_pts(double *pt, double cx, double cy, double rx, double ry,
                   int a0, int a1, int pie)
{
    int n = 0, i, steps;
    double span;
    /* sine table approximation good enough for drawing (no maths library) */
    while (a1 < a0) a1 += 3600;
    span = a1 - a0;
    steps = (int)(span / 3600.0 * 48) + 2;
    if (steps > MAXPT - 4) steps = MAXPT - 4;
    if (pie) {
        pt[n * 2] = cx; pt[n * 2 + 1] = cy; n++;
    }
    for (i = 0; i <= steps; i++) {
        double a = (a0 + span * i / steps) * 3.14159265358979 / 1800.0, s, c, a2;
        /* reduce to -pi..pi and use a short series */
        while (a > 3.14159265358979) a -= 6.28318530717959;
        while (a < -3.14159265358979) a += 6.28318530717959;
        a2 = a * a;
        s = a * (1 - a2 / 6 * (1 - a2 / 20 * (1 - a2 / 42 * (1 - a2 / 72 * (1 - a2 / 110)))));
        c = 1 - a2 / 2 * (1 - a2 / 12 * (1 - a2 / 30 * (1 - a2 / 56 * (1 - a2 / 90 * (1 - a2 / 132)))));
        pt[n * 2] = cx + rx * c;
        pt[n * 2 + 1] = cy + ry * s;
        n++;
    }
    return n;
}

static void text(GST *g, double x, double y, const int *chars, int nc, double width)
{
    char s[256];
    int i, k = 0;
    double size;
    u8 rgb[3];
    for (i = 0; i < nc && k < 255; i++) {
        int c = chars[i] & 0xFF;
        s[k++] = (char)(c < 32 ? ' ' : c);
    }
    while (k && s[k - 1] == ' ') k--;
    if (!k)
        return;
    /* font size in points: from vst_point, or vst_height in user units
       (character height, about 70% of the em)                          */
    if (g->tpoint > 0)
        size = g->tpoint;
    else
        size = g->theight * (g->sy < 0 ? -g->sy : g->sy) / 0.7;
    if (size < 1) size = 1;
    if (size > 500) size = 500;
    /* alignment: move the reference point to the left end, baseline */
    {
        double w = width > 0 ? width * g->sx : k * size * 0.55;
        double px = ux(g, x), py = uy(g, y);
        if (g->thalign == 1) px -= w / 2;
        else if (g->thalign == 2) px -= w;
        if (g->tvalign == 5) py -= size * 0.75;       /* top */
        else if (g->tvalign == 2 || g->tvalign == 4) py -= size * 0.25;
        else if (g->tvalign == 1) py -= size * 0.35;  /* half */
        else if (g->tvalign == 3) py += size * 0.2;   /* descent */
        colour(g->tcol, rgb);
        buf_u8(g->out, PI_TEXT);
        buf_put(g->out, rgb, 3);
        buf_u32(g->out, (u32)(i32)(px * 16));
        buf_u32(g->out, (u32)(i32)(py * 16));
        buf_u32(g->out, (u32)(i32)(w * 16));
        buf_u16(g->out, (unsigned)(size * 16));
        buf_u8(g->out, g->teffects & (TA_BOLD | TA_ITALIC | TA_UNDER));
        buf_u8(g->out, width > 0 ? 0 : 1);            /* 1: natural width */
        buf_u16(g->out, k);
        buf_put(g->out, s, k);
        g->nitems++;
    }
}

/* ---------------------------------------------------- v_bit_image */

#define BI_BUDGET (1536L * 1024)     /* largest embedded raster */

static int try_img(const char *fn, IMAGE *im, const PREFS *pf)
{
    long l;
    u8 hd[4];
    FILE *fp = fopen(fn, "rb");
    if (!fp)
        return 0;
    l = (long)fread(hd, 1, 4, fp);
    fclose(fp);
    if (l < 4)
        return 0;
    img_init(im);
    if (dec_img(fn, im, pf, 0, 0) != E_OK) {
        res_free(RB_ORIG);
        return 0;
    }
    return 1;
}

/* find and decode the IMG named in the record: by its base name in the
   metafile's folder (as written, upper, lower case), then the name as
   written, which may carry Easy-Draw's original drive and path        */
static int load_bitimage(const char *gempath, const char *name, IMAGE *im,
                         const PREFS *pf)
{
    char fn[300], base[64];
    const char *b = name, *q;
    int k, n;
    if (!*name)
        return 0;
    for (q = name; *q; q++)
        if (*q == '\\' || *q == '/' || *q == ':')
            b = q + 1;
    strncpy(base, b, 63);
    base[63] = 0;
    n = 0;
    for (q = gempath; *q; q++)
        if (*q == '\\' || *q == '/' || *q == ':')
            n = (int)(q - gempath) + 1;
    if (n > 230)
        return 0;
    for (k = 0; k < 3; k++) {
        char *c;
        memcpy(fn, gempath, n);
        strcpy(fn + n, base);
        for (c = fn + n; *c; c++)
            if (k == 1 && *c >= 'a' && *c <= 'z') *c -= 32;
            else if (k == 2 && *c >= 'A' && *c <= 'Z') *c += 32;
        if (try_img(fn, im, pf))
            return 1;
    }
    return try_img(name, im, pf);           /* full path as written */
}

/* v_bit_image: ptsin = two corners, intin = aspect, x scale, y scale,
   h align, v align, file name. The picture is fitted into the box,
   keeping its pixel shape when aspect is 1, and aligned as asked.    */
static void bit_image(GST *g, const char *gempath, const PREFS *pf,
                      const double *pt, const int *in, int ni)
{
    IMAGE im;
    char name[200];
    int k, n = 0, fmt = PIF_NONE, have;
    double x0 = pt[0] < pt[2] ? pt[0] : pt[2], x1 = pt[0] < pt[2] ? pt[2] : pt[0];
    double y0 = pt[1] < pt[3] ? pt[1] : pt[3], y1 = pt[1] < pt[3] ? pt[3] : pt[1];
    double bx = ux(g, x0), by = uy(g, y0), bx2 = ux(g, x1), by2 = uy(g, y1);
    double px, py, pw, ph;
    long iw = 0, ih = 0, dlen = 0, step = 1;
    u8 *data = 0, ink[3] = { 0, 0, 0 };

    for (k = 5; k < ni && n < 199; k++)
        if (in[k] & 0xFF)
            name[n++] = (char)(in[k] & 0xFF);
    name[n] = 0;
    have = load_bitimage(gempath, name, &im, pf);

    /* box in page space (y up) */
    if (bx2 < bx) { double t = bx; bx = bx2; bx2 = t; }
    if (by2 < by) { double t = by; by = by2; by2 = t; }
    px = bx; py = by; pw = bx2 - bx; ph = by2 - by;

    if (have) {
        int mono = im.type == IT_INDEXED && im.ncolors <= 2, grey = 1;
        long x, y;
        /* keep the picture within the budget by skipping pixels */
        for (;;) {
            iw = (im.w + step - 1) / step;
            ih = (im.h + step - 1) / step;
            dlen = mono ? (iw + 7) / 8 * ih : iw * ih * 3;
            if (dlen <= BI_BUDGET || step > 64)
                break;
            step++;
        }
        if (!mono)
            for (y = 0; y < im.h && grey; y++) {
                const u8 *r = im.px + y * im.stride;
                for (x = 0; x < im.w; x++) {
                    const u8 *c = im.type == IT_RGB ? r + x * 3 : im.pal + r[x] * 3;
                    if (c[0] != c[1] || c[1] != c[2]) { grey = 0; break; }
                }
            }
        if (!mono && grey)
            dlen = iw * ih;
        data = malloc(dlen ? dlen : 1);
        if (data) {
            u8 *o = data;
            memset(data, 0, dlen);
            for (y = 0; y < ih; y++) {
                const u8 *r = im.px + y * step * im.stride;
                for (x = 0; x < iw; x++) {
                    long sx = x * step;
                    const u8 *c = im.type == IT_RGB ? r + sx * 3 : im.pal + r[sx] * 3;
                    if (mono) {
                        /* ink = the darker of the two colours */
                        int v = c[0] * 3 + c[1] * 6 + c[2];
                        if (v < 1280)
                            o[x >> 3] |= 0x80 >> (x & 7);
                    } else if (grey)
                        o[x] = c[0];
                    else {
                        o[x * 3] = c[0]; o[x * 3 + 1] = c[1]; o[x * 3 + 2] = c[2];
                    }
                }
                o += mono ? (iw + 7) / 8 : grey ? iw : iw * 3;
            }
            fmt = mono ? PIF_MONO : grey ? PIF_GREY : PIF_RGB;
        }
        /* fit, keeping the pixel shape (IMG pixel size in microns) */
        if (ni >= 1 && in[0] == 1 && pw > 0 && ph > 0) {
            double aw = im.w * (double)(im.aspx > 0 ? im.aspx : 1);
            double ah = im.h * (double)(im.aspy > 0 ? im.aspy : 1);
            double fw = pw, fh = pw * ah / aw;
            int ha = ni >= 4 ? in[3] : 0, va = ni >= 5 ? in[4] : 0;
            if (fh > ph) { fh = ph; fw = ph * aw / ah; }
            px = bx + (ha == 1 ? (pw - fw) / 2 : ha == 2 ? pw - fw : 0);
            /* page space is y up: "top" alignment is the high end */
            py = by + (va == 1 ? (ph - fh) / 2 : va == 2 ? 0 : ph - fh);
            pw = fw; ph = fh;
        }
        res_free(RB_ORIG);
    }
    buf_u8(g->out, PI_IMAGE);
    buf_put(g->out, ink, 3);
    buf_u32(g->out, (u32)(i32)(px * 16));
    buf_u32(g->out, (u32)(i32)(py * 16));
    buf_u32(g->out, (u32)(i32)(pw * 16));
    buf_u32(g->out, (u32)(i32)(ph * 16));
    buf_u8(g->out, data ? fmt : PIF_NONE);
    buf_u8(g->out, 0);
    buf_u16(g->out, data ? (unsigned)iw : 0);
    buf_u16(g->out, data ? (unsigned)ih : 0);
    buf_u32(g->out, data ? (u32)dlen : 0);
    if (data) {
        buf_put(g->out, data, dlen);
        free(data);
    }
    g->nitems++;
}

int dec_gem(const char *path, const PREFS *pf, PROG *pg, u8 **blob, long *len)
{
    MR m;
    u8 *f;
    long n;
    int e, hlen, ndc, i;
    int wx0, wy0, wx1, wy1, pagew, pageh;
    long bx0 = 32767, by0 = 32767, bx1 = -32768, by1 = -32768;
    double pt[(MAXPT + 2) * 2];
    int in[260];
    GST g;
    BUF out;
    long start;

    f = rd_slurp(path, &n, pg, &e);
    if (!f)
        return e;
    if (n < 32 || f[0] != 0xFF || f[1] != 0xFF) {
        free(f);
        return E_FORMAT;
    }
    m.f = f; m.n = n; m.pos = 2;
    m.intel = !(f[2] == 0 && f[3] >= 8);
    hlen = mw(&m);
    if (hlen < 8 || hlen > 64) {
        free(f);
        return E_FORMAT;
    }
    mw(&m);                                  /* version */
    ndc = mw(&m) == 0;                       /* 0 NDC (y up), 2 raster */
    wx0 = mw(&m); wy0 = mw(&m); wx1 = mw(&m); wy1 = mw(&m);   /* extents */
    pagew = mw(&m); pageh = mw(&m);          /* 1/10 mm */

    /* first pass: find the drawing's extent and any coordinate window */
    {
        int cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0, havewin = 0;
        if (hlen >= 14) {
            cx0 = mw(&m); cy0 = mw(&m); cx1 = mw(&m); cy1 = mw(&m);
            havewin = cx1 != cx0 && cy1 != cy0;
        }
        m.pos = 2L * hlen;               /* the header length counts the -1 */
        for (;;) {
            int op = mw(&m), np, ni, sub, k, iv[5] = { 0, 0, 0, 0, 0 }, ev[4] = { 0, 0, 0, 0 };
            if (op == -1 || m.pos >= n)
                break;
            np = mw(&m); ni = mw(&m); sub = mw(&m);
            if (np < 0 || ni < 0 || np > 10000 || ni > 10000)
                break;
            for (k = 0; k < np; k++) {
                int x = mw(&m), y = mw(&m);
                if (op == 5) {               /* escapes: not drawing coords */
                    if (k < 2) { ev[k * 2] = x; ev[k * 2 + 1] = y; }
                    continue;
                }
                if (x < bx0) bx0 = x;
                if (x > bx1) bx1 = x;
                if (y < by0) by0 = y;
                if (y > by1) by1 = y;
            }
            for (k = 0; k < ni; k++) {
                int v = mw(&m);
                if (k < 5) iv[k] = v;
            }
            if (op == 5 && sub == 99 && ni >= 5 && iv[0] == 1) {         /* vm_coords */
                cx0 = iv[1]; cy0 = iv[2]; cx1 = iv[3]; cy1 = iv[4];
                havewin = cx1 != cx0 && cy1 != cy0;
            } else if (op == 5 && sub == 99 && ni >= 3 && iv[0] == 0) {  /* vm_pagesize */
                pagew = iv[1];
                pageh = iv[2];
            } else if (op == 5 && sub == 98 && np >= 2 &&                /* v_meta_extents */
                       ev[2] > ev[0] && ev[3] > ev[1] && !(wx1 > wx0 && wy1 > wy0)) {
                wx0 = ev[0]; wy0 = ev[1]; wx1 = ev[2]; wy1 = ev[3];
            }
        }
        memset(&g, 0, sizeof g);
        if (havewin) {
            g.ox = cx0 < cx1 ? cx0 : cx1;
            g.oy = cy0 < cy1 ? cy0 : cy1;
            g.yup = cy1 > cy0;
            wx0 = cx0; wx1 = cx1; wy0 = cy0; wy1 = cy1;
        } else if (wx1 > wx0 && wy1 > wy0) {
            g.ox = wx0; g.oy = wy0;
            g.yup = ndc;
        } else if (bx1 > bx0 && by1 > by0) {
            wx0 = (int)bx0; wy0 = (int)by0; wx1 = (int)bx1; wy1 = (int)by1;
            g.ox = wx0; g.oy = wy0;
            g.yup = ndc;
        } else {
            free(f);
            return E_FORMAT;
        }
        {
            double uw = wx1 > wx0 ? wx1 - wx0 : wx0 - wx1;
            double uh = wy1 > wy0 ? wy1 - wy0 : wy0 - wy1;
            if (uw < 1) uw = 1;
            if (uh < 1) uh = 1;
            if (pagew > 0 && pageh > 0) {
                g.pw = pagew * 72.0 / 254.0;
                g.ph = pageh * 72.0 / 254.0;
            } else {                         /* keep proportions, 8in wide */
                g.pw = 576;
                g.ph = 576 * uh / uw;
            }
            g.sx = g.pw / uw;
            g.sy = g.ph / uh;
        }
    }

    dbg("gem: %ld bytes, hlen %d, intel %d, ndc %d, window %d,%d %d,%d, page %dx%d,"
        " yup %d, origin %ld,%ld, scale %ld/%ld per 1000 units",
        n, hlen, m.intel, ndc, wx0, wy0, wx1, wy1, pagew, pageh, g.yup,
        (long)g.ox, (long)g.oy, (long)(g.sx * 1000), (long)(g.sy * 1000));

    /* defaults */
    g.lcol = 1; g.lwidth = 1; g.ltype = 1;
    g.fcol = 1; g.finter = 0; g.fstyle = 1; g.fperim = 1;
    g.tcol = 1; g.theight = 0; g.tpoint = 10;
    g.mcol = 1; g.mtype = 3; g.mheight = 0;

    buf_init(&out);
    g.out = &out;
    buf_put(&out, "FVPD", 4);
    buf_u32(&out, 1);
    start = out.len;
    buf_u32(&out, 0);
    buf_u32(&out, 0); buf_u32(&out, 0);
    buf_u32(&out, (u32)(i32)(g.pw * 16));
    buf_u32(&out, (u32)(i32)(g.ph * 16));
    buf_u32(&out, 0);                        /* item count */

    m.pos = 2L * hlen;               /* the header length counts the -1 */
    for (i = 0; ; i++) {
        int op = mw(&m), np, ni, sub, k, nk;
        if (op == -1 || m.pos >= n)
            break;
        np = mw(&m); ni = mw(&m); sub = mw(&m);
        if (i < 4 || np < 0 || ni < 0 || np > 10000 || ni > 10000)
            dbg("gem: record %d at %ld: op %d np %d ni %d sub %d", i, m.pos - 8, op, np, ni, sub);
        if (np < 0 || ni < 0 || np > 10000 || ni > 10000)
            break;
        nk = np < MAXPT ? np : MAXPT;
        for (k = 0; k < np; k++) {
            int x = mw(&m), y = mw(&m);
            if (k < nk) { pt[k * 2] = x; pt[k * 2 + 1] = y; }
        }
        for (k = 0; k < ni; k++) {
            int v = mw(&m);
            if (k < 260) in[k] = v;
        }
        if (ni > 260) ni = 260;
        if ((i & 63) == 0 && prog_step(pg, m.pos, n * 2L) ) {
            buf_free(&out);
            free(f);
            return E_CANCEL;
        }
        switch (op) {
        case 6:                              /* v_pline */
            stroke(&g, pt, nk);
            break;
        case 7: {                            /* v_pmarker: small crosses */
            double s = (g.mheight > 0 ? g.mheight : 6 / g.sy) / 2, q[4];
            u8 rgb[3];
            colour(g.mcol, rgb);
            for (k = 0; k < nk; k++) {
                q[0] = pt[k * 2] - s; q[1] = pt[k * 2 + 1];
                q[2] = pt[k * 2] + s; q[3] = pt[k * 2 + 1];
                emit_poly(&g, PI_LINE, q, 2, rgb, 0.5);
                q[0] = pt[k * 2]; q[1] = pt[k * 2 + 1] - s;
                q[2] = pt[k * 2]; q[3] = pt[k * 2 + 1] + s;
                emit_poly(&g, PI_LINE, q, 2, rgb, 0.5);
            }
            break;
        }
        case 8:                              /* v_gtext */
            if (nk >= 1)
                text(&g, pt[0], pt[1], in, ni, 0);
            break;
        case 9:                              /* v_fillarea */
            fill(&g, pt, nk, g.fperim);
            break;
        case 114:                            /* vr_recfl */
        case 11:                             /* GDP */
            if (op == 114 || sub == 1 || sub == 8 || sub == 9) {
                /* bar / rounded box: two corners */
                double r[12];
                if (nk < 2) break;
                r[0] = pt[0]; r[1] = pt[1]; r[2] = pt[2]; r[3] = pt[1];
                r[4] = pt[2]; r[5] = pt[3]; r[6] = pt[0]; r[7] = pt[3];
                if (op == 114)
                    fill(&g, r, 4, 0);
                else if (sub == 8) {         /* v_rbox: outline */
                    r[8] = pt[0]; r[9] = pt[1];
                    stroke(&g, r, 5);
                } else
                    fill(&g, r, 4, op == 11 && sub == 1 ? g.fperim : sub == 9 ? g.fperim : 0);
            } else if (sub >= 2 && sub <= 7 && nk >= 1) {
                double rx, ry;
                int a0 = 0, a1 = 3600, cnt, pie = sub == 3 || sub == 7, filled;
                if (sub == 2 || sub == 3) {          /* arc / pie: radius in pt 3 */
                    rx = nk >= 4 ? pt[6] : 10;
                    ry = rx * g.sx / g.sy;
                    if (ni >= 2) { a0 = in[0]; a1 = in[1]; }
                } else if (sub == 4) {               /* circle: radius in pt 2 */
                    rx = nk >= 3 ? pt[4] : 10;
                    ry = rx * g.sx / g.sy;
                } else {                             /* ellipses: radii in pt 1 */
                    rx = nk >= 2 ? pt[2] : 10;
                    ry = nk >= 2 ? pt[3] : 10;
                    if ((sub == 6 || sub == 7) && ni >= 2) { a0 = in[0]; a1 = in[1]; }
                }
                if (ry < 0) ry = -ry;
                /* angles count anticlockwise with y up */
                {
                    double cx = pt[0], cy = pt[1], rys = g.yup ? ry : -ry;
                    cnt = arc_pts(pt, cx, cy, rx, rys, a0, a1, pie);
                }
                filled = sub == 3 || sub == 4 || sub == 5 || sub == 7;
                if (filled)
                    fill(&g, pt, cnt, g.fperim);
                else
                    stroke(&g, pt, cnt);
            } else if (sub == 10 && nk >= 2 && ni >= 2) {    /* v_justified */
                text(&g, pt[0], pt[1], in + 2, ni - 2, pt[2]);
            }
            break;
        case 5:                              /* escapes */
            if (sub == 23 && nk >= 2)        /* v_bit_image */
                bit_image(&g, path, pf, pt, in, ni);
            break;
        case 12: if (nk >= 1) { g.theight = (int)(pt[1] > 0 ? pt[1] : -pt[1]); g.tpoint = 0; } break;
        case 107: if (ni >= 1) { g.tpoint = in[0]; } break;
        case 22: if (ni >= 1) g.tcol = in[0]; break;
        case 106: if (ni >= 1) g.teffects = in[0]; break;
        case 39: if (ni >= 2) { g.thalign = in[0]; g.tvalign = in[1]; } break;
        case 15: if (ni >= 1) g.ltype = in[0]; break;
        case 16: if (nk >= 1) g.lwidth = (int)(pt[0] > 0 ? pt[0] : 1); break;
        case 17: if (ni >= 1) g.lcol = in[0]; break;
        case 18: if (ni >= 1) g.mtype = in[0]; break;
        case 19: if (nk >= 1) g.mheight = (int)(pt[1] > 0 ? pt[1] : -pt[1]); break;
        case 20: if (ni >= 1) g.mcol = in[0]; break;
        case 23: if (ni >= 1) g.finter = in[0]; break;
        case 24: if (ni >= 1) g.fstyle = in[0]; break;
        case 25: if (ni >= 1) g.fcol = in[0]; break;
        case 104: if (ni >= 1) g.fperim = in[0] != 0; break;
        default: break;                      /* clipping, modes, ... */
        }
    }
    free(f);
    dbg("gem: %d records, %ld items, %ld bytes, buffer error %d", i, g.nitems, out.len, out.err);
    buf_set_u32(&out, start, out.len - start - 4);
    buf_set_u32(&out, start + 4 + 16, g.nitems);
    return buf_finish(&out, RB_DOC, blob, len);
}
