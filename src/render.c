/*
 * render.c - text and PDF drawing shared by the screen and the GDOS
 * printer: layout of text blobs into lines, drawing of lines with inline
 * attributes, and drawing of PDF display lists through any VDI handle.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gemui.h"

extern short syspal[256][3];

int pen_for_rgb(const u8 *rgb, int ncolors)
{
    int i, best = 1, n = ncolors > 16 ? 16 : ncolors;
    long bd = 0x7FFFFFFFL;
    if (n <= 2) {
        int l = rgb[0] * 3 + rgb[1] * 6 + rgb[2];
        return l > 1900 ? 0 : 1;
    }
    for (i = 0; i < n; i++) {
        long d, dr, dg, db;
        dr = rgb[0] - syspal[i][0] * 255 / 1000;
        dg = rgb[1] - syspal[i][1] * 255 / 1000;
        db = rgb[2] - syspal[i][2] * 255 / 1000;
        d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (d < bd) bd = d, best = i;
    }
    return best;
}

/* -------------------------------------------------------- text layout */

void txt_measure(TXTDEV *td, int handle, int base_height, int ncolors, int um_w)
{
    int s;
    short cw, ch, cellw, cellh;
    memset(td, 0, sizeof *td);
    td->handle = handle;
    td->fs = fonts_for(handle);
    td->um_w = um_w > 0 ? um_w : 278;
    td->cur_font = -1;
    vst_font(handle, 1);
    td->ncolors = ncolors;
    td->mono = ncolors <= 2;
    for (s = 0; s <= PS_BLANK; s++) {
        int h = base_height;
        if (s == PS_H1) h = base_height * 2;
        else if (s == PS_H2) h = base_height * 3 / 2;
        vst_height(handle, h, &cw, &ch, &cellw, &cellh);
        td->height[s] = h;
        td->cw[s] = cellw > 0 ? cellw : 8;
        td->ch[s] = cellh > 0 ? cellh : 16;
        td->base[s] = 0;
    }
    vst_height(handle, base_height, &cw, &ch, &cellw, &cellh);
}

/* count visible characters in [s, s+n) (inline attribute marks excluded) */
long txt_vis(const u8 *s, long n)
{
    long i, v = 0;
    for (i = 0; i < n; i++) {
        if (s[i] == TA_MARK) { i++; continue; }
        v++;
    }
    return v;
}

/* --------------------------------------------------------- rich text */

typedef struct {
    int  nfonts, ncols;
    const u8 *fname[64];
    int  flen[64], ffam[64];
    const u8 *cols;
    long twips;
    long body;
} RICH;

int txt_is_rich(const u8 *blob)
{
    return (rd_u32(blob + 8) & DOCF_RICH) != 0;
}

static void rich_header(const u8 *b, RICH *r)
{
    long o = 12;
    int i, n;
    memset(r, 0, sizeof *r);
    n = (int)rd_u16(b + o);
    o += 2;
    for (i = 0; i < n; i++) {
        int l = b[o + 1];
        if (i < 64) {
            r->ffam[i] = b[o];
            r->fname[i] = b + o + 2;
            r->flen[i] = l;
        }
        o += 2 + l;
    }
    r->nfonts = n > 64 ? 64 : n;
    r->ncols = (int)rd_u16(b + o);
    o += 2;
    r->cols = b + o;
    o += r->ncols * 3L;
    r->twips = (long)rd_u32(b + o);
    r->body = o + 4;
}

static long twips_px(long tw, int um) { return tw * 25400L / (1440L * (um > 0 ? um : 278)); }

/* put a character state on the VDI (only what changed) */
static void rich_apply(TXTDEV *td, const RICH *r, int font, int size, int attr, int color)
{
    int h = td->handle, st, fx;
    if (font < 0 || font >= r->nfonts) font = 0;
    st = ((attr & TA_BOLD) ? 1 : 0) | ((attr & TA_ITALIC) ? 2 : 0);
    if (!td->pick[font][st]) {
        char nm[48];
        int l = r->flen[font] > 46 ? 46 : r->flen[font];
        memcpy(nm, r->fname[font], l);
        nm[l] = 0;
        td->pick[font][st] = (short)font_pick(td->fs, nm, r->ffam[font], attr, &fx);
        td->pfx[font][st] = (short)fx;
    }
    if (font != td->cur_font || size != td->cur_size ||
        (attr & (TA_BOLD | TA_ITALIC)) != (td->cur_attr & (TA_BOLD | TA_ITALIC))) {
        short minade, maxade, dist[5], maxw, eff[3];
        vst_font(h, td->pick[font][st]);
        font_size(td->fs, h, size);
        vst_effects(h, 0);
        vqt_fontinfo(h, &minade, &maxade, dist, &maxw, eff);
        td->asc = dist[4] > 0 ? dist[4] : 8;
        td->desc = dist[0] > 0 ? dist[0] : 2;
        td->cur_font = font;
        td->cur_size = size;
        td->cur_attr = -1;
    }
    if (attr != td->cur_attr) {
        vst_effects(h, (attr & (TA_UNDER | TA_LIGHT)) | td->pfx[font][st]);
        td->cur_attr = attr;
    }
    if (color != td->cur_color) {
        u8 rgb[3] = { 0, 0, 0 };
        if (color >= 0 && color < r->ncols)
            memcpy(rgb, r->cols + color * 3, 3);
        vst_color(h, td->mono ? pen_for_rgb(rgb, 2) : pen_for_rgb(rgb, td->ncolors));
        td->cur_color = color;
    }
}

static int rich_width(TXTDEV *td, const u8 *s, int n)
{
    char buf[124];
    short ext[8];
    int w = 0;
    while (n > 0) {
        int k = n > 120 ? 120 : n;
        memcpy(buf, s, k);
        buf[k] = 0;
        vqt_extent(td->handle, buf, ext);
        w += ext[2] - ext[0];
        s += k;
        n -= k;
    }
    return w;
}

typedef struct { int font, size, attr, color; } RSTATE;

/* apply the escape at t[i]; returns its length */
static int rich_escape(const u8 *t, long i, long n, RSTATE *s)
{
    if (t[i] == TA_MARK && i + 1 < n) { s->attr = t[i + 1]; return 2; }
    if (t[i] == FM_FONT && i + 2 < n) { s->font = t[i + 1]; s->size = t[i + 2]; return 3; }
    if (t[i] == FM_COLOR && i + 1 < n) { s->color = t[i + 1]; return 2; }
    return 1;
}
#define IS_ESC(c) ((c) == TA_MARK || (c) == FM_FONT || (c) == FM_COLOR)

static long rich_layout(const u8 *blob, TXTDEV *td, int width_px, TLINE **out,
                        PROG *pg)
{
    RICH r;
    long np = rd_u32(blob + 4), o, p, n = 0, cap = np + 64, y = 0, avail;
    TLINE *L = malloc(cap * sizeof(TLINE));
    RSTATE st = { 0, 24, 0, 0 };
    if (!L)
        return -1;
    rich_header(blob, &r);
    o = r.body;
    if (td->doc != blob) {
        memset(td->pick, 0, sizeof td->pick);
        td->doc = blob;
    }
    td->cur_font = -1;
    td->cur_color = -1;
    avail = width_px - 2 * MARGIN;
    if (r.twips > 0) {
        long pw = twips_px(r.twips, td->um_w);
        if (pw < avail) avail = pw;
    }
    if (avail < 64) avail = 64;

    for (p = 0; p < np; p++) {
        unsigned plen = rd_u16(blob + o);
        int style = blob[o + 2], indent = blob[o + 3], ps = style & 15;
        const u8 *t = blob + o + 4;
        long indpx = twips_px(indent * 360L, td->um_w), av = avail - indpx;
        long i = 0, lstart = 0, lw = 0, psw = 0, nsp = 0;
        int psn = 0, have = 0, first = 1, asc, desc;
        RSTATE ls;
        if (av < 32) av = 32;
        rich_apply(td, &r, st.font, st.size, st.attr, st.color);
        /* the paragraph's own opening escapes set its first line's state */
        while (i < plen && IS_ESC(t[i]))
            i += rich_escape(t, i, plen, &st);
        rich_apply(td, &r, st.font, st.size, st.attr, st.color);
        ls = st;
        asc = td->asc;
        desc = td->desc;
        lstart = i;

#define EMIT(endpos, islast) do { \
            TLINE *ln; \
            if (n == cap) { \
                TLINE *nl = realloc(L, (cap = cap * 3 / 2 + 64) * sizeof(TLINE)); \
                if (!nl) { free(L); return -1; } \
                L = nl; \
            } \
            ln = &L[n++]; \
            memset(ln, 0, sizeof *ln); \
            ln->para = o; ln->start = (u16)lstart; ln->len = (u16)((endpos) - lstart); \
            ln->style = (u8)style; ln->indent = (u8)indent; ln->attr = (u8)ls.attr; \
            ln->first = (u8)first; ln->rich = 1; ln->last = (u8)(islast); \
            ln->font = (u8)ls.font; ln->size = (u8)ls.size; ln->color = (u8)ls.color; \
            ln->base = (short)asc; ln->w = (short)lw; ln->nsp = (short)nsp; \
            ln->avail = (short)av; ln->y = y; \
            ln->h = (short)(asc + desc + 1 + (asc + desc) / 8); \
            if (ps == PS_RULE) ln->h = (short)(asc + desc); \
            y += ln->h; first = 0; \
        } while (0)

        while (i < plen) {
            if (IS_ESC(t[i])) {
                i += rich_escape(t, i, plen, &st);
                rich_apply(td, &r, st.font, st.size, st.attr, st.color);
                continue;
            }
            if (t[i] == ' ') {
                long j = i;
                while (j < plen && t[j] == ' ') j++;
                if (have) {
                    psw += rich_width(td, t + i, (int)(j - i));
                    psn++;
                } else if (first) {
                    /* leading spaces of a paragraph are indentation */
                    lw += rich_width(td, t + i, (int)(j - i));
                    have = 1;
                    psn = 0;
                } else
                    lstart = j;           /* continuation: drop them */
                i = j;
                continue;
            }
            {
                /* a word: up to the next space, across style changes */
                RSTATE ws = st, save = st;
                long j = i, segs = i, ww = 0;
                int wasc = td->asc, wdesc = td->desc;
                while (j < plen && t[j] != ' ') {
                    if (IS_ESC(t[j])) {
                        ww += rich_width(td, t + segs, (int)(j - segs));
                        j += rich_escape(t, j, plen, &ws);
                        rich_apply(td, &r, ws.font, ws.size, ws.attr, ws.color);
                        if (td->asc > wasc) wasc = td->asc;
                        if (td->desc > wdesc) wdesc = td->desc;
                        segs = j;
                        continue;
                    }
                    j++;
                }
                ww += rich_width(td, t + segs, (int)(j - segs));
                if (have && lw + psw + ww > av) {
                    /* break before this word */
                    EMIT(i - 0, 0);
                    lstart = i;
                    ls = save;
                    lw = psw = nsp = 0;
                    psn = 0;
                    have = 0;
                    rich_apply(td, &r, save.font, save.size, save.attr, save.color);
                    asc = td->asc;
                    desc = td->desc;
                }
                if (!have && ww > av) {
                    /* a word wider than the line: cut it by characters */
                    long k = i, cw = 0;
                    rich_apply(td, &r, save.font, save.size, save.attr, save.color);
                    st = save;
                    while (k < j) {
                        int c1;
                        if (IS_ESC(t[k])) {
                            k += rich_escape(t, k, plen, &st);
                            rich_apply(td, &r, st.font, st.size, st.attr, st.color);
                            continue;
                        }
                        c1 = rich_width(td, t + k, 1);
                        if (cw + c1 > av && k > lstart) {
                            lw = cw;
                            EMIT(k, 0);
                            lstart = k;
                            ls = st;
                            cw = 0;
                            asc = td->asc;
                            desc = td->desc;
                        }
                        cw += c1;
                        k++;
                    }
                    lw = cw;
                    have = 1;
                    i = j;
                    continue;
                }
                if (have && psn)
                    nsp++;
                lw += psw + ww;
                psw = 0;
                psn = 0;
                have = 1;
                if (wasc > asc) asc = wasc;
                if (wdesc > desc) desc = wdesc;
                st = ws;
                i = j;
            }
        }
        EMIT(plen, 1);
#undef EMIT
        o += 4 + plen;
        if ((p & 63) == 0 && prog_step(pg, p, np)) {
            free(L);
            return -1;
        }
    }
    *out = L;
    return n;
}

static void rich_draw_line(TXTDEV *td, const u8 *blob, const TLINE *l, int x, int y)
{
    RICH r;
    const u8 *t = blob + l->para + 4;
    long i = l->start, end = l->start + l->len;
    int h = td->handle, align = (l->style >> 4) & 3, extra = 0, gap = 0;
    long indpx;
    RSTATE st;
    short d;
    rich_header(blob, &r);
    if (td->doc != blob) {
        memset(td->pick, 0, sizeof td->pick);
        td->doc = blob;
        td->cur_font = -1;
    }
    indpx = twips_px(l->indent * 360L, td->um_w);
    if ((l->style & 15) == PS_RULE) {
        short pxy[4];
        pxy[0] = x; pxy[2] = (short)(x + l->avail);
        pxy[1] = pxy[3] = (short)(y + l->h / 2);
        vsl_color(h, 1); vsl_width(h, 1); vsl_type(h, 1);
        v_pline(h, 2, pxy);
        return;
    }
    x += (int)indpx;
    if (align == AL_RIGHT)
        x += l->avail - l->w;
    else if (align == AL_CENTER)
        x += (l->avail - l->w) / 2;
    else if (align == AL_JUST && !l->last && l->nsp > 0)
        extra = l->avail - l->w;
    if (extra < 0) extra = 0;
    st.font = l->font; st.size = l->size; st.attr = l->attr; st.color = l->color;
    rich_apply(td, &r, st.font, st.size, st.attr, st.color);
    vst_alignment(h, 0, 0, &d, &d);
    vswr_mode(h, MD_TRANS);
    y += l->base;
    while (i < end) {
        if (IS_ESC(t[i])) {
            i += rich_escape(t, i, end, &st);
            rich_apply(td, &r, st.font, st.size, st.attr, st.color);
            continue;
        }
        if (t[i] == ' ') {
            long j = i;
            while (j < end && t[j] == ' ') j++;
            x += rich_width(td, t + i, (int)(j - i));
            if (extra && i > l->start) {
                /* spread the slack evenly over the gaps */
                x += (int)((long)extra * (gap + 1) / l->nsp - (long)extra * gap / l->nsp);
                gap++;
            }
            i = j;
            continue;
        }
        {
            long j = i;
            char buf[124];
            int k;
            while (j < end && t[j] != ' ' && !IS_ESC(t[j]) && j - i < 120) j++;
            k = (int)(j - i);
            memcpy(buf, t + i, k);
            buf[k] = 0;
            v_gtext(h, x, y, buf);
            x += rich_width(td, t + i, k);
            i = j;
        }
    }
}

long txt_layout(const u8 *blob, long len, TXTDEV *td, int width_px,
                TLINE **out, PROG *pg)
{
    if (txt_is_rich(blob))
        return rich_layout(blob, td, width_px, out, pg);
    {
    long np = rd_u32(blob + 4), flags = rd_u32(blob + 8), o = 12, p;
    long cap = np + 64, n = 0, y = 0;
    TLINE *L = malloc(cap * sizeof(TLINE));
    int nowrap = (flags & DOCF_NOWRAP) != 0;
    if (!L)
        return -1;
    for (p = 0; p < np && o + 4 <= len; p++) {
        unsigned plen = rd_u16(blob + o);
        int style = blob[o + 2], indent = blob[o + 3];
        const u8 *t = blob + o + 4;
        int cw = td->cw[style], ch = td->ch[style];
        int ind = indent * 2 * td->cw[PS_NORMAL];
        long cols, pos = 0;
        int attr = 0, first = 1;
        if (style == PS_BULLET) ind += 2 * cw;
        if (style == PS_QUOTE) ind += 2 * cw;
        cols = (width_px - ind - 2 * MARGIN) / cw;
        if (cols < 8) cols = 8;
        if (nowrap || style == PS_PRE) cols = 0x7FFFFFFL;

        do {
            long i, vis = 0, brk = -1, brkattr = 0, end;
            int a = attr;
            /* skip leading blanks on continuation lines */
            if (!first)
                while (pos < plen && t[pos] == ' ') pos++;
            for (i = pos; i < plen; i++) {
                if (t[i] == TA_MARK) {
                    a = i + 1 < plen ? t[i + 1] : 0;
                    i++;
                    continue;
                }
                if (vis >= cols)
                    break;
                if (t[i] == ' ') {
                    brk = i;
                    brkattr = a;
                }
                vis++;
            }
            if (i >= plen)
                end = plen;
            else if (brk > pos)
                end = brk, a = (int)brkattr;
            else
                end = i;
            if (n == cap) {
                TLINE *nl = realloc(L, (cap = cap * 3 / 2 + 64) * sizeof(TLINE));
                if (!nl) {
                    free(L);
                    return -1;
                }
                L = nl;
            }
            memset(&L[n], 0, sizeof L[n]);
            L[n].para = o;
            L[n].start = (u16)pos;
            L[n].len = (u16)(end - pos);
            L[n].style = style;
            L[n].indent = (u8)(ind / (td->cw[PS_NORMAL] ? td->cw[PS_NORMAL] : 8));
            L[n].attr = attr;
            L[n].first = first;
            L[n].y = y;
            L[n].h = style == PS_BLANK ? ch / 2 : style == PS_RULE ? ch : ch;
            if (style == PS_H1 || style == PS_H2)
                L[n].h += ch / 4;
            y += L[n].h;
            n++;
            /* attribute at the end of this line carries on */
            {
                long k;
                for (k = pos; k < end; k++)
                    if (t[k] == TA_MARK && k + 1 < end) { attr = t[k + 1]; k++; }
            }
            pos = end;
            first = 0;
        } while (pos < plen);
        o += 4 + plen;
        if ((p & 63) == 0 && prog_step(pg, p, np)) {
            free(L);
            return -1;
        }
    }
    *out = L;
    return n;
}
}

void txt_draw_line(TXTDEV *td, const u8 *blob, const TLINE *l, int x, int y,
                   int width)
{
    if (l->rich) {
        rich_draw_line(td, blob, l, x, y);
        return;
    }
    if (td->cur_font != -2) {           /* -2: system font is set */
        vst_font(td->handle, 1);
        td->cur_font = -2;
    }
    const u8 *t = blob + l->para + 4 + l->start;
    int h = td->handle, style = l->style, a = l->attr, effects, seg = 0;
    int cw = td->cw[style], cx;
    char buf[130];
    short d;
    unsigned i;

    vst_height(h, td->height[style], &d, &d, &d, &d);
    vst_alignment(h, 0, 5, &d, &d);
    vst_color(h, 1);
    vswr_mode(h, MD_TRANS);
    effects = style == PS_H1 || style == PS_H2 ? TA_BOLD | TA_UNDER :
              style == PS_H3 ? TA_BOLD : style == PS_QUOTE ? TA_ITALIC : 0;

    if (style == PS_RULE) {
        short pxy[4];
        pxy[0] = x; pxy[2] = x + width - 2 * MARGIN;
        pxy[1] = pxy[3] = y + l->h / 2;
        vsl_color(h, 1);
        vsl_width(h, 1);
        vsl_type(h, 1);
        v_pline(h, 2, pxy);
        return;
    }
    cx = x + l->indent * td->cw[PS_NORMAL];
    if (style == PS_QUOTE) {
        short pxy[4];
        pxy[0] = pxy[2] = cx - 2 * cw + cw / 2;
        pxy[1] = y; pxy[3] = y + l->h - 1;
        vsl_color(h, 1); vsl_width(h, 1); vsl_type(h, 1);
        v_pline(h, 2, pxy);
    }
    if (style == PS_BULLET && l->first) {
        char b[2];
        b[0] = (char)0xF9; b[1] = 0;
        vst_effects(h, TA_BOLD);
        v_gtext(h, cx - 2 * cw, y, b);
    }
    vst_effects(h, effects | a);
    for (i = 0; i <= l->len; i++) {
        int flush = i == l->len || t[i] == TA_MARK || seg == (int)sizeof buf - 2;
        if (flush && seg) {
            buf[seg] = 0;
            v_gtext(h, cx, y, buf);
            cx += seg * cw;
            seg = 0;
        }
        if (i == l->len)
            break;
        if (t[i] == TA_MARK) {
            if (i + 1 < l->len) {
                a = t[i + 1];
                vst_effects(h, effects | a);
            }
            i++;
            continue;
        }
        buf[seg++] = t[i] ? (char)t[i] : ' ';
    }
    vst_effects(h, 0);
}

/* ---------------------------------------------------------------- pdf */

long pdf_index(const u8 *blob, long len, long **offs)
{
    long n = rd_u32(blob + 4), o = 8, i;
    long *p = malloc((n + 1) * sizeof(long));
    if (!p)
        return 0;
    for (i = 0; i < n && o + 4 <= len; i++) {
        p[i] = o;
        o += 4 + rd_u32(blob + o);
    }
    *offs = p;
    return i;
}

void pdf_page_size(const u8 *blob, long off, long *w16, long *h16, long *x0, long *y0)
{
    const u8 *p = blob + off + 4;
    *x0 = rd_i32(p);
    *y0 = rd_i32(p + 4);
    *w16 = rd_i32(p + 8) - *x0;
    *h16 = rd_i32(p + 12) - *y0;
}

static void fill_style(int h, const u8 *rgb, int ncolors, int mono)
{
    if (mono) {
        int l = (rgb[0] * 3 + rgb[1] * 6 + rgb[2]) / 10;
        if (l >= 235) {
            vsf_interior(h, FIS_SOLID);
            vsf_color(h, 0);
        } else if (l < 20) {
            vsf_interior(h, FIS_SOLID);
            vsf_color(h, 1);
        } else {
            vsf_interior(h, FIS_PATTERN);
            vsf_style(h, 1 + (235 - l) * 7 / 236);
            vsf_color(h, 1);
        }
    } else {
        vsf_interior(h, FIS_SOLID);
        vsf_color(h, pen_for_rgb(rgb, ncolors));
    }
}

/* ---------------------------------------------------- PDF page images */

/* Images are scaled and dithered for the output once and kept here, so
 * scrolling is just a blit. Screen images are cached; printer images are
 * built, sent and freed straight away.                                  */
typedef struct {
    const u8 *item;            /* PI_IMAGE item in the blob              */
    int  handle, dw, dh;
    int  mono;                 /* 1-plane, drawn with vrt_cpyfm          */
    MFDB m;
    long stamp, bytes;
} ICACHE;

#define NICACHE 8
#define ICACHE_BYTES (1536L * 1024)
static ICACHE icache[NICACHE];
static long istamp;

static void ic_free(ICACHE *c)
{
    free(c->m.fd_addr);
    memset(c, 0, sizeof *c);
}

void render_cache_flush(void)
{
    int i;
    for (i = 0; i < NICACHE; i++)
        if (icache[i].item)
            ic_free(&icache[i]);
}

/* ---- 1-bit page images (scanned pages, fax-style PDFs) ----
 * The source is reduced to an ink coverage value 0..255 per output pixel
 * (box filter over the source bits, a row at a time). Colour screens
 * show the coverage with the system palette's neutral greys, which makes
 * small text far easier to read than dithering; where there are no greys
 * (mono screens, printers) a clean threshold keeps strokes solid.     */
/* progress while page images are built (set by pdf_build_page) */
static PROG *render_pg;
static long render_base, render_cur, render_total;
int render_nobuild;            /* draw only what is cached, build nothing */
int render_boxes;              /* ... and show boxes for what is missing  */
static long render_missing;    /* source pixels of the missing images     */

/* report progress for row y of dh in the image being built */
static int rprog(long y, long dh)
{
    if (!render_pg || render_total <= 0)
        return 0;
    return prog_step(render_pg, render_base + (dh > 0 ? render_cur * y / dh : 0), render_total);
}

typedef struct {
    const u8 *src;
    int  iw, ih, dw, dh, enlarge;
    long sbpr;
    short *xmap;           /* source x -> output x                     */
    u16 *tot, *cnt;        /* source columns per output x; ink counts  */
} COV;

static void cov_free(COV *c)
{
    free(c->xmap); free(c->tot); free(c->cnt);
    c->xmap = 0; c->tot = c->cnt = 0;
}

static int cov_init(COV *c, const u8 *src, int iw, int ih, int dw, int dh)
{
    long x;
    memset(c, 0, sizeof *c);
    c->src = src; c->iw = iw; c->ih = ih; c->dw = dw; c->dh = dh;
    c->sbpr = (iw + 7) / 8;
    c->enlarge = dw >= iw && dh >= ih;
    if (c->enlarge)
        return 1;
    c->xmap = malloc(iw * sizeof(short));
    c->tot = calloc(dw, sizeof(u16));
    c->cnt = malloc(dw * sizeof(u16));
    if (!c->xmap || !c->tot || !c->cnt) {
        cov_free(c);
        return 0;
    }
    for (x = 0; x < iw; x++) {
        c->xmap[x] = (short)(x * dw / iw);
        c->tot[c->xmap[x]]++;
    }
    return 1;
}

/* coverage of output row y: out[x] = 0 (paper) .. 255 (all ink) */
static void cov_row(COV *c, long y, u8 *out)
{
    long x;
    if (c->enlarge) {
        const u8 *r = c->src + (long)(y * c->ih / c->dh) * c->sbpr;
        for (x = 0; x < c->dw; x++) {
            long sx = x * c->iw / c->dw;
            out[x] = (r[sx >> 3] & (0x80 >> (sx & 7))) ? 255 : 0;
        }
        return;
    }
    {
        long sy0 = y * c->ih / c->dh, sy1 = (y + 1) * c->ih / c->dh, sy, rows;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > c->ih) sy1 = c->ih;
        rows = sy1 - sy0;
        memset(c->cnt, 0, c->dw * sizeof(u16));
        for (sy = sy0; sy < sy1; sy++) {
            const u8 *r = c->src + sy * c->sbpr;
            long b;
            for (b = 0; b < c->sbpr; b++) {
                int v = r[b], k;
                if (!v)
                    continue;              /* white: most of a page */
                for (k = 0; k < 8; k++)
                    if (v & (0x80 >> k)) {
                        long sx = b * 8 + k;
                        if (sx < c->iw)
                            c->cnt[c->xmap[sx]]++;
                    }
            }
        }
        for (x = 0; x < c->dw; x++) {
            long t = (long)c->tot[x] * rows;
            out[x] = t ? (u8)((long)c->cnt[x] * 255 / t) : 0;
        }
    }
}

/* grey level (0 black .. 255 white) -> pixel value of the nearest neutral
   colour in the system palette. Returns the number of neutral colours
   found; fewer than 3 (just black and white) means no anti-aliasing.  */
static int grey_lut(u8 *lut)
{
    int ncol, i, n = 0, g;
    int lum[256], pix[256];
    if (dinfo.truecolor)
        return 0;
    ncol = dinfo.planes >= 8 ? 256 : 1 << dinfo.planes;
    for (i = 0; i < ncol; i++) {
        const u8 *c = dinfo.sysrgb + i * 3;
        int mx = c[0], mn = c[0];
        if (c[1] > mx) mx = c[1];
        if (c[2] > mx) mx = c[2];
        if (c[1] < mn) mn = c[1];
        if (c[2] < mn) mn = c[2];
        if (mx - mn <= 24) {
            lum[n] = (c[0] * 3 + c[1] * 6 + c[2]) / 10;
            pix[n] = i;
            n++;
        }
    }
    if (n < 3)
        return n;
    for (g = 0; g < 256; g++) {
        int best = 0, bd = 9999;
        for (i = 0; i < n; i++) {
            int dd = lum[i] - g;
            if (dd < 0) dd = -dd;
            if (dd < bd) bd = dd, best = i;
        }
        lut[g] = (u8)pix[best];
    }
    return n;
}

/* 1-bit, 1-plane standard raster (1 = ink): threshold the coverage */
static u16 *mono_raster(const u8 *src, int iw, int ih, int dw, int dh, int flags)
{
    long wdw = (dw + 15) / 16, x, y;
    u16 *out = calloc(wdw * dh, 2);
    u8 *cov = malloc(dw + 1);
    COV c;
    int fx = flags & PIFL_FLIPX, fy = flags & PIFL_FLIPY;
    if (!out || !cov || !cov_init(&c, src, iw, ih, dw, dh)) {
        free(out); free(cov);
        return 0;
    }
    for (y = 0; y < dh; y++) {
        u16 *o = out + (fy ? dh - 1 - y : y) * wdw;
        cov_row(&c, y, cov);
        for (x = 0; x < dw; x++)
            if (cov[x] > 80) {            /* about a third: strokes stay solid */
                long dx = fx ? dw - 1 - x : x;
                o[dx >> 4] |= (u16)(0x8000U >> (dx & 15));
            }
        if ((y & 15) == 15 && rprog(y, dh)) {
            cov_free(&c);
            free(cov);
            free(out);
            return 0;
        }
    }
    cov_free(&c);
    free(cov);
    return out;
}

/* anti-aliased device raster in the screen's own format */
static int grey_raster(ICACHE *ic, int h, const u8 *src, int iw, int ih, int dw, int dh,
                       int flags, const u8 *lut)
{
    /* converted to device format 16 rows at a time, so only the finished
       raster and one small band are in memory                          */
    long wdw = (dw + 15) / 16, rowbytes = wdw * 2 * dinfo.planes, oy, x;
    u8 *band = malloc(rowbytes * 16), *dev = malloc(rowbytes * dh);
    u8 *cov = malloc(dw + 1), *row = malloc(wdw * 16);
    COV c;
    int fx = flags & PIFL_FLIPX, fy = flags & PIFL_FLIPY;
    if (!band || !dev || !cov || !row || !cov_init(&c, src, iw, ih, dw, dh)) {
        free(band); free(dev); free(cov); free(row);
        return 0;
    }
    ic->m.fd_nplanes = (short)dinfo.planes;
    for (oy = 0; oy < dh; oy += 16) {
        int rows = (int)(dh - oy > 16 ? 16 : dh - oy), k;
        MFDB s, d;
        memset(band, 0, rowbytes * rows);
        for (k = 0; k < rows; k++) {
            long y = fy ? dh - 1 - (oy + k) : oy + k;
            cov_row(&c, y, cov);
            for (x = 0; x < dw; x++)
                row[fx ? dw - 1 - x : x] = lut[255 - cov[x]];
            chunky_to_planes(row, dw, dinfo.planes, (u16 *)band + k * wdw, wdw * rows, wdw);
        }
        s = ic->m;
        s.fd_addr = band;
        s.fd_h = rows;
        s.fd_stand = 1;
        d = s;
        d.fd_addr = dev + oy * rowbytes;
        d.fd_stand = 0;
        vr_trnfm(h, &s, &d);
        if (rprog(oy + rows, dh)) {
            cov_free(&c);
            free(band); free(dev); free(cov); free(row);
            return 0;
        }
    }
    cov_free(&c);
    free(cov);
    free(row);
    free(band);
    ic->m.fd_addr = dev;
    ic->mono = 0;
    ic->bytes = rowbytes * dh;
    return 1;
}

/* build the output raster for an image item */
static int ic_build(ICACHE *c, int h, const u8 *q, int dw, int dh, int mono)
{
    int fmt = q[20], flags = q[21], iw = (int)rd_u16(q + 22), ih = (int)rd_u16(q + 24);
    const u8 *data = q + 30;
    long wdw = (dw + 15) / 16;
    memset(&c->m, 0, sizeof c->m);
    c->m.fd_w = (short)(wdw * 16);
    c->m.fd_h = (short)dh;
    c->m.fd_wdwidth = (short)wdw;

    if (fmt == PIF_MONO) {
        MFDB s;
        u16 *bits;
        u8 lut[256];
        /* shrinking on the colour screen, not a stencil mask: grey levels */
        if (!mono && h == vdih && !(flags & PIFL_MASK) && (dw < iw || dh < ih) &&
            grey_lut(lut) >= 3)
            return grey_raster(c, h, data, iw, ih, dw, dh, flags, lut);
        bits = mono_raster(data, iw, ih, dw, dh, flags);
        if (!bits)
            return 0;
        c->m.fd_nplanes = 1;
        s = c->m;
        s.fd_addr = bits;
        s.fd_stand = 1;
        c->m.fd_addr = bits;          /* 1 plane: transform in place */
        vr_trnfm(h, &s, &c->m);
        c->mono = 1;
        c->bytes = wdw * 2 * dh;
        return 1;
    }

    /* grey / colour: dither like a picture, onto the system palette (or
       black and white for mono output) */
    {
        IMAGE im;
        DISPINFO d;
        DITHER *dt;
        u8 pal[768], *row;
        int planes, i;
        long y, rowbytes, psize;
        u8 *dev, *std = 0;

        img_init(&im);
        im.w = iw; im.h = ih;
        im.px = (u8 *)data;
        if (fmt == PIF_GREY) {
            im.type = IT_INDEXED;
            im.stride = iw;
            im.ncolors = 256;
            for (i = 0; i < 256; i++)
                im.pal[i * 3] = im.pal[i * 3 + 1] = im.pal[i * 3 + 2] = (u8)i;
        } else {
            im.type = IT_RGB;
            im.stride = (long)iw * 3;
        }
        if (mono) {
            memset(&d, 0, sizeof d);
            d.planes = 1;
            d.palmode = PAL_SYSTEM;
            d.fixed_palette = 1;
            d.sysrgb[0] = d.sysrgb[1] = d.sysrgb[2] = 255;
            d.reserved[0] = d.reserved[1] = 1;
        } else {
            d = dinfo;
            d.palmode = PAL_SYSTEM;           /* PDF windows keep GEM colours */
            memset(d.reserved, 1, sizeof d.reserved);
        }
        planes = d.planes;
        dt = dith_new(&im, dw, dh, &d, prefs.dither == DITH_NONE ? DITH_NONE : DITH_FS,
                      1, 1, pal, 0);
        if (!dt)
            return 0;
        if (d.truecolor) {
            rowbytes = wdw * 16 * d.bytespp;
            dev = malloc(rowbytes * dh);
            row = malloc(wdw * 16 * d.bytespp);
        } else {
            rowbytes = wdw * 2 * planes;
            dev = malloc(rowbytes * dh);
            std = malloc(rowbytes * dh);
            row = malloc(wdw * 16);
        }
        if (!dev || !row || (!d.truecolor && !std)) {
            free(dev); free(std); free(row);
            dith_free(dt);
            return 0;
        }
        if (std)
            memset(std, 0, rowbytes * dh);
        for (y = 0; y < dh; y++) {
            long oy = (flags & PIFL_FLIPY) ? dh - 1 - y : y;
            if ((y & 15) == 0 && rprog(y, dh)) {
                dith_free(dt);
                free(dev); free(std); free(row);
                return 0;
            }
            dith_row(dt, y, row);
            if (flags & PIFL_FLIPX) {            /* mirror the row */
                int bpp = d.truecolor ? d.bytespp : 1, k;
                long a, b2;
                for (a = 0, b2 = dw - 1; a < b2; a++, b2--)
                    for (k = 0; k < bpp; k++) {
                        u8 t = row[a * bpp + k];
                        row[a * bpp + k] = row[b2 * bpp + k];
                        row[b2 * bpp + k] = t;
                    }
            }
            if (d.truecolor)
                memcpy(dev + oy * rowbytes, row, dw * d.bytespp);
            else {
                psize = wdw * dh;
                chunky_to_planes(row, dw, planes, (u16 *)std + oy * wdw, psize, wdw);
            }
        }
        dith_free(dt);
        free(row);
        c->m.fd_nplanes = (short)(d.truecolor ? scr_planes : planes);
        if (std) {
            MFDB s = c->m;
            s.fd_addr = std;
            s.fd_stand = 1;
            c->m.fd_addr = dev;
            vr_trnfm(h, &s, &c->m);
            free(std);
        } else
            c->m.fd_addr = dev;
        c->mono = planes == 1;
        c->bytes = rowbytes * dh;
        return 1;
    }
}

/* find or build the raster for an image item at dw x dh */
static ICACHE *ic_get(int h, const u8 *q, int dw, int dh, int mono, ICACHE *tmp)
{
    int i, lru = 0;
    long total = 0;
    int cacheable = h == vdih;
    for (i = 0; i < NICACHE; i++)
        if (icache[i].item == q && icache[i].handle == h &&
            icache[i].dw == dw && icache[i].dh == dh) {
            icache[i].stamp = ++istamp;
            return &icache[i];
        }
    if (!cacheable) {
        memset(tmp, 0, sizeof *tmp);
        return ic_build(tmp, h, q, dw, dh, mono) ? tmp : 0;
    }
    if (render_nobuild) {
        render_missing += (long)rd_u16(q + 22) * rd_u16(q + 24);
        return 0;
    }
    /* make room: drop the least recently used entries until there is a
       free slot and the new raster fits the budget                     */
    {
        long need = (long)((dw + 15) & ~15) * dh / 8 * (mono ? 1 : (scr_planes > 8 ? 32 : scr_planes));
        for (;;) {
            int oldest = -1, free_slot = -1;
            total = 0;
            for (i = 0; i < NICACHE; i++) {
                if (!icache[i].item) {
                    if (free_slot < 0) free_slot = i;
                    continue;
                }
                total += icache[i].bytes;
                if (oldest < 0 || icache[i].stamp < icache[oldest].stamp)
                    oldest = i;
            }
            if (free_slot >= 0 && (total + need <= ICACHE_BYTES || oldest < 0)) {
                lru = free_slot;
                break;
            }
            if (oldest < 0)
                return 0;
            ic_free(&icache[oldest]);
        }
    }
    {
        ICACHE *c = &icache[lru];
        memset(c, 0, sizeof *c);
        if (!ic_build(c, h, q, dw, dh, mono))
            return 0;
        c->item = q;
        c->handle = h;
        c->dw = dw;
        c->dh = dh;
        c->stamp = ++istamp;
        return c;
    }
}

static void draw_placeholder(int h, short *pxy, int mono, int ncolors)
{
    short p[10];
    p[0] = pxy[0]; p[1] = pxy[1]; p[2] = pxy[2]; p[3] = pxy[1];
    p[4] = pxy[2]; p[5] = pxy[3]; p[6] = pxy[0]; p[7] = pxy[3];
    p[8] = pxy[0]; p[9] = pxy[1];
    vsl_width(h, 1);
    vsl_type(h, mono ? 3 : 1);
    vsl_color(h, mono ? 1 : (ncolors >= 16 ? 9 : 1));
    v_pline(h, 5, p);
    p[2] = pxy[2]; p[3] = pxy[3];
    v_pline(h, 2, p);
    p[0] = pxy[0]; p[1] = pxy[3]; p[2] = pxy[2]; p[3] = pxy[1];
    v_pline(h, 2, p);
    vsl_type(h, 1);
}

/* draw (or with prepare set, only get ready) one PI_IMAGE item */
static void draw_image(int h, const u8 *q, short X0, short Y0, short X1, short Y1,
                       int ncolors, int mono, const GRECT *clip, int prepare)
{
    int fmt = q[20], flags = q[21];
    int dw = X1 - X0, dh = Y1 - Y0;
    GRECT r, cl = *clip;
    ICACHE *c, tmp;
    short pxy[8];
    if (dw <= 0 || dh <= 0 || dw > 8000 || dh > 8000)
        return;
    r.g_x = X0; r.g_y = Y0; r.g_w = dw; r.g_h = dh;
    if (!rc_intersect(&r, &cl))
        return;
    if (fmt == PIF_NONE) {
        if (!prepare) {
            pxy[0] = X0; pxy[1] = Y0; pxy[2] = X1 - 1; pxy[3] = Y1 - 1;
            draw_placeholder(h, pxy, mono, ncolors);
        }
        return;
    }
    render_cur = (long)rd_u16(q + 22) * rd_u16(q + 24);
    c = ic_get(h, q, dw, dh, mono, &tmp);
    if (c && prepare)
        render_base += render_cur;          /* (cached: counts as done) */
    if (prepare)
        return;
    if (!c && render_nobuild && !render_boxes)
        return;                             /* being prepared: leave blank */
    if (!c) {
        pxy[0] = X0; pxy[1] = Y0; pxy[2] = X1 - 1; pxy[3] = Y1 - 1;
        draw_placeholder(h, pxy, mono, ncolors);
        return;
    }
    {
        MFDB scr;
        memset(&scr, 0, sizeof scr);
        pxy[0] = cl.g_x - X0; pxy[1] = cl.g_y - Y0;
        pxy[2] = pxy[0] + cl.g_w - 1; pxy[3] = pxy[1] + cl.g_h - 1;
        pxy[4] = cl.g_x; pxy[5] = cl.g_y;
        pxy[6] = cl.g_x + cl.g_w - 1; pxy[7] = cl.g_y + cl.g_h - 1;
        if (c->mono) {
            short col[2];
            col[0] = (flags & PIFL_MASK) ? (short)pen_for_rgb(q + 1, mono ? 2 : ncolors) : 1;
            col[1] = 0;
            vrt_cpyfm(h, (flags & PIFL_MASK) ? MD_TRANS : MD_REPLACE, pxy, &c->m, &scr, col);
        } else
            vro_cpyfm(h, S_ONLY, pxy, &c->m, &scr);
    }
    if (c == &tmp)
        free(tmp.m.fd_addr);
}

/* build the image rasters for a page before drawing, outside the AES
   screen lock, so the desktop isn't held while they are made          */
void pdf_prepare_page(int h, const u8 *blob, long off, int ox, int oy,
                      long sx, long sy, int ncolors, int mono, const GRECT *clip)
{
    const u8 *p = blob + off;
    long size = rd_u32(p), x0 = rd_i32(p + 4), y1 = rd_i32(p + 16);
    long nitems = rd_u32(p + 20), i;
    const u8 *q = p + 24, *end = p + 4 + size;
    for (i = 0; i < nitems && q < end; i++) {
        int kind = q[0];
        if (kind == PI_TEXT)
            q += 22 + rd_u16(q + 20);
        else if (kind == PI_LINE || kind == PI_FILL)
            q += 8 + rd_u16(q + 6) * 8L;
        else if (kind == PI_IMAGE) {
            long x = rd_i32(q + 4), y = rd_i32(q + 8), wd = rd_i32(q + 12), ht = rd_i32(q + 16);
            draw_image(h, q, (short)(ox + (x - x0) * sx / 16000L),
                       (short)(oy + (y1 - (y + ht)) * sy / 16000L),
                       (short)(ox + (x + wd - x0) * sx / 16000L),
                       (short)(oy + (y1 - y) * sy / 16000L), ncolors, mono, clip, 1);
            q += 30 + rd_u32(q + 26);
        } else
            break;
    }
}

/* source pixels of this page's images that are not cached yet at this
   scale (0 = the page can be drawn straight away)                      */
long pdf_page_missing(int h, const u8 *blob, long off, long sx, long sy,
                      int ncolors, int mono)
{
    GRECT all = { -16000, -16000, 32000, 32000 };
    int keep = render_nobuild;
    render_nobuild = 1;
    render_missing = 0;
    pdf_prepare_page(h, blob, off, 0, 0, sx, sy, ncolors, mono, &all);
    render_nobuild = keep;
    return render_missing;
}

/* scale and dither every image on the page now, reporting through pg.
   Returns 0 if cancelled or out of memory.                             */
int pdf_build_page(int h, const u8 *blob, long off, long sx, long sy,
                   int ncolors, int mono, PROG *pg)
{
    GRECT all = { -16000, -16000, 32000, 32000 };
    long left, before;
    before = render_total = pdf_page_missing(h, blob, off, sx, sy, ncolors, mono);
    render_base = 0;
    render_pg = pg;
    pdf_prepare_page(h, blob, off, 0, 0, sx, sy, ncolors, mono, &all);
    render_pg = 0;
    render_total = 0;
    if (pg && pg->cancelled)
        return 0;
    /* a page whose images don't all fit the cache still counts as done:
       the rest are built as they are drawn                            */
    left = pdf_page_missing(h, blob, off, sx, sy, ncolors, mono);
    return before == 0 || left < before;
}

void pdf_draw_page(int h, const u8 *blob, long off, int ox, int oy,
                   long sx, long sy, int ncolors, int mono, const GRECT *clip)
{
    const u8 *p = blob + off;
    long size = rd_u32(p), x0 = rd_i32(p + 4), y1 = rd_i32(p + 16);
    long nitems = rd_u32(p + 20), i;
    const u8 *q = p + 24, *end = p + 4 + size;
    short pxy[258], d, pick[32], pickfx[32];
    int curh = -1, curfont = -1;
    FONTSET *fs = fonts_for(h);

#define DX(v) ((short)(ox + ((v) - x0) * sx / 16000L))
#define DY(v) ((short)(oy + (y1 - (v)) * sy / 16000L))
    memset(pick, 0, sizeof pick);
    vswr_mode(h, MD_TRANS);
    vst_alignment(h, 0, 0, &d, &d);      /* left, baseline */
    vsf_perimeter(h, 0);
    for (i = 0; i < nitems && q < end; i++) {
        int kind = q[0];
        const u8 *rgb = q + 1;
        if (kind == PI_TEXT) {
            long x = rd_i32(q + 4), y = rd_i32(q + 8), wd = rd_i32(q + 12);
            long sz = rd_u16(q + 16), hp;
            int attr = q[18], n = rd_u16(q + 20), natural = q[19] & 1;
            const u8 *s = q + 22;
            short X = DX(x), Y = DY(y), W = (short)(wd * sx / 16000L);
            q += 22 + n;
            hp = sz * sy / 16000L;
            if (Y < clip->g_y - 2 || Y - hp > clip->g_y + clip->g_h + 2)
                continue;
            if (X > clip->g_x + clip->g_w || X + W < clip->g_x)
                continue;
            {
                int col = mono ? pen_for_rgb(rgb, 2) : pen_for_rgb(rgb, ncolors);
                if (hp < 6) {
                    /* too small to read: show where the text is */
                    pxy[0] = X; pxy[1] = pxy[3] = (short)(Y - hp / 3);
                    pxy[2] = X + (W > 1 ? W : n * hp / 2);
                    vsl_color(h, col); vsl_width(h, 1);
                    vsl_type(h, mono ? 1 : 1);
                    v_pline(h, 2, pxy);
                    continue;
                }
                {
                    /* a GDOS font of the same family when there is one */
                    int fam = (attr >> 4) & 3, sty = attr & (TA_BOLD | TA_ITALIC), fx = sty;
                    int key = fam * 8 + sty, id = 1;
                    if (fs && fs->n > 1) {
                        if (!pick[key]) {
                            pick[key] = (short)font_pick(fs, 0, fam ? fam : FF_SERIF, sty, &fx);
                            pickfx[key] = (short)fx;
                        }
                        id = pick[key];
                        fx = pickfx[key];
                    }
                    if (id != curfont) {
                        vst_font(h, id);
                        curfont = id;
                        curh = -1;
                    }
                    if (hp * 7 / 10 != curh) {
                        curh = (int)(hp * 7 / 10);
                        vst_height(h, curh, &d, &d, &d, &d);
                    }
                    vst_color(h, col);
                    vst_effects(h, fx);
                }
                {
                    char buf[124];
                    int k = 0, start = 0;
                    while (start < n) {
                        int chunk = n - start > 120 ? 120 : n - start;
                        short cx = (short)(X + (long)W * start / n);
                        short cw = (short)((long)W * chunk / n);
                        for (k = 0; k < chunk; k++)
                            buf[k] = s[start + k] ? (char)s[start + k] : ' ';
                        buf[k] = 0;
                        if (cw > 4 && chunk > 1 && !natural)
                            v_justified(h, cx, Y, buf, cw, 0, 1);
                        else
                            v_gtext(h, cx, Y, buf);
                        start += chunk;
                    }
                }
            }
        } else if (kind == PI_LINE || kind == PI_FILL) {
            int wl = rd_u16(q + 4), np = rd_u16(q + 6), k, step = 1, m = 0;
            const u8 *pts = q + 8;
            q += 8 + np * 8;
            if (np > 128)
                step = (np + 127) / 128;
            for (k = 0; k < np && m < 128; k += step) {
                pxy[m * 2] = DX(rd_i32(pts + k * 8));
                pxy[m * 2 + 1] = DY(rd_i32(pts + k * 8 + 4));
                m++;
            }
            if (step > 1 && m < 129) {        /* keep the last point */
                pxy[m * 2] = DX(rd_i32(pts + (np - 1) * 8));
                pxy[m * 2 + 1] = DY(rd_i32(pts + (np - 1) * 8 + 4));
                m++;
            }
            if (kind == PI_FILL) {
                if (m >= 3) {
                    if (wl & 0x8000) {          /* VDI pattern (GEM metafile) */
                        vsf_interior(h, (wl >> 8) & 7);
                        vsf_style(h, wl & 0xFF);
                        vsf_color(h, pen_for_rgb(rgb, mono ? 2 : ncolors));
                    } else
                        fill_style(h, rgb, ncolors, mono);
                    v_fillarea(h, m, pxy);
                }
            } else {
                int lw = (int)(wl * sx / 16000L);
                if (lw < 1) lw = 1;
                if (!(lw & 1)) lw++;
                if (lw > 15) lw = 15;
                vsl_width(h, lw);
                vsl_type(h, 1);
                vsl_color(h, mono ? pen_for_rgb(rgb, 2) : pen_for_rgb(rgb, ncolors));
                if (mono && (rgb[0] * 3 + rgb[1] * 6 + rgb[2]) / 10 > 200)
                    vsl_type(h, 3);           /* light lines: dotted */
                v_pline(h, m, pxy);
            }
        } else if (kind == PI_IMAGE) {
            long x = rd_i32(q + 4), y = rd_i32(q + 8), wd = rd_i32(q + 12), ht = rd_i32(q + 16);
            draw_image(h, q, DX(x), DY(y + ht), DX(x + wd), DY(y), ncolors, mono, clip, 0);
            q += 30 + rd_u32(q + 26);
        } else {
            dbg("page draw: unknown item kind %d at item %ld", kind, i);
            break;
        }
    }
    vsl_type(h, 1);
    vst_effects(h, 0);
    vst_font(h, 1);
#undef DX
#undef DY
}

