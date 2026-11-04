/*
 * print.c - GDOS printing.
 *
 * Opens the printer workstation (device 21, as set up in ASSIGN.SYS) and
 *   - pictures: dithers the original image at printer resolution, either
 *     fitted to the page or at its real size, and sends it in bands with
 *     vrt_cpyfm();
 *   - text, Markdown, RTF: lays the document out for the page with the
 *     printer's system font, with an optional header line;
 *   - PDF: draws the current page or all pages scaled to the paper.
 * All of it runs with a progress bar and Cancel in the document window.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gemui.h"

#define PRN_DEVICE 21
#define BAND 64

typedef struct {
    int h;
    int pw, ph;          /* page size in pixels      */
    int um_w, um_h;      /* pixel size in micrometres */
    int ncolors;
    int nfonts;          /* system fonts of the driver */
    FONTSET fs;
} PRN;

static int prn_open(PRN *p)
{
    short in[11], out[57], h = 0;
    int i;
    for (i = 0; i < 10; i++) in[i] = 1;
    in[0] = PRN_DEVICE;
    in[10] = 2;                          /* raster coordinates */
    memset(out, 0, sizeof out);
    v_opnwk(in, &h, out);
    if (h <= 0)
        return 0;
    p->h = h;
    p->pw = out[0] + 1;
    p->ph = out[1] + 1;
    p->um_w = out[3] > 0 ? out[3] : 85;
    p->um_h = out[4] > 0 ? out[4] : 85;
    p->ncolors = out[13] > 0 ? out[13] : 2;
    p->nfonts = out[10] > 0 ? out[10] : 1;
    fonts_load(&p->fs, h, p->nfonts);      /* the printer's GDOS fonts */
    return 1;
}

static void prn_page_done(PRN *p)
{
    v_updwk(p->h);
    v_clrwk(p->h);
}

static void prn_header(PRN *p, VWIN *w, long page, int *top)
{
    short d, cw, ch, cellw, cellh;
    char t[120];
    int hgt = 2540 / p->um_h;            /* about 10 pt */
    if (!prefs.prt_header) {
        *top = 0;
        return;
    }
    vst_height(p->h, hgt, &cw, &ch, &cellw, &cellh);
    vst_alignment(p->h, 0, 5, &d, &d);
    vst_effects(p->h, TA_BOLD);
    vst_color(p->h, 1);
    vswr_mode(p->h, MD_TRANS);
    sprintf(t, "%.80s   page %ld", w->path, page);
    v_gtext(p->h, 0, 0, t);
    vst_effects(p->h, 0);
    {
        short pxy[4];
        pxy[0] = 0; pxy[2] = p->pw - 1;
        pxy[1] = pxy[3] = cellh + cellh / 4;
        vsl_width(p->h, 1);
        vsl_color(p->h, 1);
        v_pline(p->h, 2, pxy);
    }
    *top = cellh * 2;
}

/* ------------------------------------------------------------- pictures */

static int print_image(PRN *p, VWIN *w, PROG *pg)
{
    IMAGE *im = &w->img.im;
    DISPINFO pd;
    DITHER *dt;
    u8 pal[768], *row, *band;
    long tw, th, wdw, y, x;
    int top, e = E_OK;
    long dw0, dh0;
    double iw_um, ih_um;
    img_display_size(im, &dinfo, &dw0, &dh0);   /* as it looks on screen */
    iw_um = (double)dw0 * (pix_w_um > 0 ? pix_w_um : 278);
    ih_um = (double)dh0 * (pix_h_um > 0 ? pix_h_um : 278);

    prn_header(p, w, 1, &top);
    if (prefs.prt_scale == PRS_FIT) {
        double fx = (double)p->pw * p->um_w / iw_um;
        double fy = (double)(p->ph - top) * p->um_h / ih_um;
        double f = fx < fy ? fx : fy;
        tw = (long)(iw_um * f / p->um_w);
        th = (long)(ih_um * f / p->um_h);
    } else {
        tw = (long)(iw_um / p->um_w);
        th = (long)(ih_um / p->um_h);
        if (tw > p->pw) tw = p->pw;          /* clipped to the page */
        if (th > p->ph - top) th = p->ph - top;
    }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;

    memset(&pd, 0, sizeof pd);
    pd.planes = 1;
    pd.palmode = PAL_SYSTEM;
    pd.fixed_palette = 1;
    pd.sysrgb[0] = pd.sysrgb[1] = pd.sysrgb[2] = 255;   /* 0 = paper */
    pd.reserved[0] = pd.reserved[1] = 1;
    dt = dith_new(im, tw, th, &pd, prefs.prt_dither, 1, prefs.smooth, pal, 0);
    wdw = (tw + 15) / 16;
    row = malloc(tw + 16);
    band = malloc(wdw * 2 * BAND);
    if (!dt || !row || !band) {
        dith_free(dt);
        free(row);
        free(band);
        return E_NOMEM;
    }
    for (y = 0; y < th && !e; y += BAND) {
        int rows = (int)(th - y > BAND ? BAND : th - y), r;
        MFDB src, dev, scr;
        short pxy[8], col[2];
        memset(band, 0, wdw * 2 * rows);
        for (r = 0; r < rows; r++) {
            u16 *dst = (u16 *)band + r * wdw;
            dith_row(dt, y + r, row);
            for (x = 0; x < tw; x++)
                if (row[x])
                    dst[x >> 4] |= (u16)(0x8000U >> (x & 15));
        }
        memset(&src, 0, sizeof src);
        src.fd_addr = band;
        src.fd_w = (short)(wdw * 16);
        src.fd_h = rows;
        src.fd_wdwidth = (short)wdw;
        src.fd_stand = 1;
        src.fd_nplanes = 1;
        dev = src;
        dev.fd_stand = 0;
        vr_trnfm(p->h, &src, &dev);      /* same buffer: 1 plane is identical */
        memset(&scr, 0, sizeof scr);
        pxy[0] = 0; pxy[1] = 0;
        pxy[2] = (short)(tw - 1); pxy[3] = rows - 1;
        pxy[4] = (short)((p->pw - tw) / 2); pxy[5] = (short)(top + y);
        pxy[6] = pxy[4] + pxy[2]; pxy[7] = pxy[5] + rows - 1;
        col[0] = 1; col[1] = 0;
        vrt_cpyfm(p->h, MD_TRANS, pxy, &dev, &scr, col);
        if (prog_step(pg, y + rows, th))
            e = E_CANCEL;
    }
    dith_free(dt);
    free(row);
    free(band);
    if (!e)
        prn_page_done(p);
    return e;
}

/* ---------------------------------------------------------------- text */

static int print_text(PRN *p, VWIN *w, PROG *pg)
{
    TXTDEV td;
    TLINE *lines = 0;
    long n, i, pagestart = 0, page = 1;
    int top, avail, margin = 1270 / p->um_w;      /* 5 mm */
    int printed = 0;

    txt_measure(&td, p->h, 2540 / p->um_h, p->ncolors, p->um_w);
    n = txt_layout(w->blob, w->img.doclen, &td, p->pw - 2 * margin, &lines, 0);
    if (n < 0)
        return E_NOMEM;
    prn_header(p, w, page, &top);
    avail = p->ph - top;
    for (i = 0; i < n; i++) {
        TLINE *l = &lines[i];
        if (l->y + l->h - pagestart > avail) {
            prn_page_done(p);
            pagestart = l->y;
            page++;
            prn_header(p, w, page, &top);
            avail = p->ph - top;
            printed = 0;
        }
        txt_draw_line(&td, w->blob, l, margin, top + (int)(l->y - pagestart),
                      p->pw - 2 * margin);
        printed = 1;
        if ((i & 15) == 0 && prog_step(pg, i, n)) {
            free(lines);
            return E_CANCEL;
        }
    }
    if (printed || n == 0)
        prn_page_done(p);
    free(lines);
    return E_OK;
}

/* ----------------------------------------------------------------- pdf */

static int print_pdf(PRN *p, VWIN *w, long from, long to, PROG *pg)
{
    long pg_i;
    for (pg_i = from; pg_i <= to; pg_i++) {
        long pw, ph, x0, y0, sx, sy, sx2;
        int top;
        GRECT clip;
        short c[4];
        pdf_page_size(w->blob, w->pageoff[pg_i], &pw, &ph, &x0, &y0);
        prn_header(p, w, pg_i + 1, &top);
        if (pw < 16) pw = 16;
        if (ph < 16) ph = 16;
        /* fit the page: pixels per point * 1000 */
        sx = (long)p->pw * 16000L / pw;
        sy = sx * p->um_w / p->um_h;
        if (ph * sy / 16000L > p->ph - top) {
            sy = (long)(p->ph - top) * 16000L / ph;
            sx2 = sy * p->um_h / p->um_w;
            sx = sx2;
        }
        clip.g_x = 0; clip.g_y = top;
        clip.g_w = p->pw; clip.g_h = p->ph - top;
        c[0] = 0; c[1] = top; c[2] = p->pw - 1; c[3] = p->ph - 1;
        vs_clip(p->h, 1, c);
        pdf_draw_page(p->h, w->blob, w->pageoff[pg_i], 0, top, sx, sy,
                      p->ncolors, p->ncolors <= 2, &clip);
        vs_clip(p->h, 0, c);
        prn_page_done(p);
        if (prog_step(pg, pg_i - from + 1, to - from + 1))
            return E_CANCEL;
    }
    return E_OK;
}

/* ---------------------------------------------------------------- entry */

void print_window(VWIN *w)
{
    PRN p;
    PROG pg;
    int e = E_OK;
    long from = 0, to = 0;
    char msg[160];

    if (!w || w->kind == K_MAIN || win_busy(w) || w->state == ST_ERROR)
        return;
    if (!vq_gdos()) {
        alertf("[1][GDOS is not installed.|Printing needs GDOS, AMCGDOS,|"
               "NVDI or SpeedoGDOS with a|printer driver.][ OK ]");
        return;
    }
    if (w->kind == K_PDF) {
        int b;
        sprintf(msg, "[2][Print %.24s|on the GDOS printer?][All pages|Page %ld|Cancel]",
                win_base_name(w->path), w->page + 1);
        b = form_alert(1, msg);
        if (b == 3)
            return;
        from = b == 1 ? 0 : w->page;
        to = b == 1 ? w->npages - 1 : w->page;
    } else {
        sprintf(msg, "[2][Print %.24s|on the GDOS printer?][Print|Cancel]",
                win_base_name(w->path));
        if (form_alert(1, msg) != 1)
            return;
    }
    if (!prn_open(&p)) {
        alertf("[1][The printer driver (device %d)|could not be opened. Check|ASSIGN.SYS.][ OK ]",
               PRN_DEVICE);
        return;
    }
    if (!win_begin_op(w, ST_PRINTING, "Printing", &pg)) {
        fonts_unload(&p.fs);
        v_clswk(p.h);
        return;
    }
    if (w->kind == K_IMAGE)
        e = print_image(&p, w, &pg);
    else if (w->kind == K_PDF)
        e = print_pdf(&p, w, from, to, &pg);
    else
        e = print_text(&p, w, &pg);
    fonts_unload(&p.fs);
    v_clswk(p.h);
    win_end_op(w);
    if (e && e != E_CANCEL)
        alertf("[1][fview: printing failed.|%.30s][ OK ]", fv_errstr(e));
}
