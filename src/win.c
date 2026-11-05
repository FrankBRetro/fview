/*
 * win.c - fview document windows.
 *
 * Every file gets its own GEM window. While a file loads (or is
 * re-rendered, saved or printed) the window shows a progress panel with a
 * Cancel button; the window can be moved, sized and closed meanwhile.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <osbind.h>
#include "gemui.h"

VWIN wins[MAXWIN];

#define WKIND_DOC (NAME | CLOSER | FULLER | MOVER | INFO | SIZER | UPARROW | \
                   DNARROW | VSLIDE | LFARROW | RTARROW | HSLIDE)

static TXTDEV scrtd;
static int scrtd_ok;

/* ---------------------------------------------------------------- utils */

static const char *base_name(const char *p)
{
    const char *a = strrchr(p, '\\'), *b = strrchr(p, '/');
    if (b > a) a = b;
    return a ? a + 1 : p;
}

const char *win_base_name(const char *p) { return base_name(p); }

VWIN *win_new(void)
{
    int i;
    for (i = 0; i < MAXWIN; i++)
        if (!wins[i].used) {
            memset(&wins[i], 0, sizeof(VWIN));
            wins[i].used = 1;
            wins[i].zoom = prefs.zoom;
            return &wins[i];
        }
    return 0;
}

VWIN *win_find(int handle)
{
    int i;
    if (handle <= 0)
        return 0;
    for (i = 0; i < MAXWIN; i++)
        if (wins[i].used && wins[i].handle == handle)
            return &wins[i];
    return 0;
}

VWIN *win_top(void)
{
    short top = 0, d;
    wind_get(0, WF_TOP, &top, &d, &d, &d);
    return win_find(top);
}

int win_busy(VWIN *w)
{
    return w && (w->state == ST_LOADING || w->state == ST_RENDER ||
                 w->state == ST_SAVING || w->state == ST_PRINTING);
}

static void get_work(VWIN *w)
{
    wind_get_grect(w->handle, WF_WORKXYWH, &w->work);
}

static int kind_flags(VWIN *w)
{
    int k = w->kind == K_MAIN ? (NAME | CLOSER | MOVER) : WKIND_DOC;
    if (aes_iconify)
        k |= SMALLER;
    return k;
}

/* --------------------------------------------------------- open / close */

int win_open(VWIN *w, int kind)
{
    static int cascade;
    GRECT r, wk;
    int k;
    w->kind = kind;
    k = kind_flags(w);
    w->handle = wind_create_grect(k, &desk);
    if (w->handle <= 0) {
        w->used = 0;
        alertf("[1][fview: no more windows are|available.][ OK ]");
        return 0;
    }
    if (kind == K_MAIN) {
        wk.g_w = 34 * cell_w;
        wk.g_h = 10 * cell_h;
    } else {
        wk.g_w = desk.g_w * 2 / 3;
        wk.g_h = desk.g_h * 2 / 3;
    }
    wk.g_x = wk.g_y = 0;
    wind_calc_grect(WC_BORDER, k, &wk, &r);
    r.g_x = desk.g_x + 8 + (cascade % 8) * 16;
    r.g_y = desk.g_y + 4 + (cascade % 8) * 12;
    if (kind != K_MAIN)
        cascade++;
    if (r.g_x + r.g_w > desk.g_x + desk.g_w) r.g_w = desk.g_x + desk.g_w - r.g_x;
    if (r.g_y + r.g_h > desk.g_y + desk.g_h) r.g_h = desk.g_y + desk.g_h - r.g_y;
    w->full = kind == K_MAIN ? r : desk;
    wind_set_str(w->handle, WF_NAME, w->title[0] ? w->title : APPNAME);
    if (k & INFO)
        wind_set_str(w->handle, WF_INFO, w->info);
    wind_open_grect(w->handle, &r);
    get_work(w);
    win_set_sliders(w);
    win_post_redraw(w);
    return 1;
}

void win_free_content(VWIN *w)
{
    int i;
    if (w->kind == K_PDF)
        render_cache_flush();          /* cached rasters point into the blob */
    for (i = 0; i < RB_MAX; i++)
        job_release(&w->img, i);
    if (w->dispmem)
        free(w->dispmem);
    w->dispmem = 0;
    memset(&w->disp, 0, sizeof w->disp);
    free(w->lines);
    w->lines = 0;
    w->nlines = 0;
    free(w->pageoff);
    w->pageoff = 0;
    w->npages = 0;
    w->blob = 0;
    w->haspal = 0;
}

static void win_destroy(VWIN *w)
{
    progdlg_close(w);
    if (w->handle > 0) {
        wind_close(w->handle);
        wind_delete(w->handle);
        w->handle = 0;
    }
    win_free_content(w);
    if (w->job) {
        free(w->job);
        w->job = 0;
    }
    w->used = 0;
    pal_invalidate();
    menu_update();
}

void win_close(VWIN *w)
{
    if (w->kind == K_MAIN)
        return;
    if (w->job && !w->job->done) {
        /* stop the job first; the window disappears now, the slot is
           freed when the job reports back */
        w->closing = 1;
        progdlg_close(w);
        job_cancel(w->job);
        if (w->handle > 0) {
            wind_close(w->handle);
            wind_delete(w->handle);
            w->handle = 0;
        }
        pal_invalidate();
        return;
    }
    if (w->state == ST_SAVING || w->state == ST_PRINTING ||
        (w->state == ST_RENDER && !w->job)) {   /* PDF page being built */
        w->closing = 1;
        w->job = 0;
        if (w->handle > 0) {
            wind_close(w->handle);
            wind_delete(w->handle);
            w->handle = 0;
        }
        return;
    }
    win_destroy(w);
}

/* ---------------------------------------------------- progress dialog */

/* Each busy window gets a small non-modal dialog window of its own with
 * the bar, a status line and Cancel (3D look where the AES has it). If no
 * window is available the same panel is drawn inside the document window. */

#define PD_PAD 12

void win_dlg_redraw(VWIN *w, GRECT *area);

static void layout_tree(VWIN *w)
{
    OBJECT *t = w->prog;
    int pw = 34 * cell_w, bh = cell_h > 10 ? 12 : 8;
    t[1].ob_x = PD_PAD;
    t[1].ob_y = PD_PAD;
    t[1].ob_width = pw;
    t[2].ob_x = PD_PAD;
    t[2].ob_y = t[1].ob_y + cell_h + 6;
    t[2].ob_width = pw;
    t[2].ob_height = bh;
    t[3].ob_x = t[3].ob_y = 0;
    t[3].ob_height = bh;
    t[3].ob_width = w->ptotal > 0 ? (int)((long)pw * (w->pdone > w->ptotal ? w->ptotal : w->pdone) / w->ptotal) : 0;
    t[4].ob_x = PD_PAD;
    t[4].ob_y = t[2].ob_y + bh + 6;
    t[4].ob_width = pw - t[5].ob_width - 8;
    t[5].ob_x = PD_PAD + pw - t[5].ob_width;
    t[5].ob_y = t[4].ob_y + cell_h / 2;
    t[0].ob_width = pw + 2 * PD_PAD;
    t[0].ob_height = t[5].ob_y + t[5].ob_height + PD_PAD;
}

/* place the tree's root where it is drawn */
static void layout_progress(VWIN *w)
{
    OBJECT *t = w->prog;
    layout_tree(w);
    if (w->pdlg > 0) {
        GRECT wk;
        wind_get_grect(w->pdlg, WF_WORKXYWH, &wk);
        t[0].ob_x = wk.g_x;
        t[0].ob_y = wk.g_y;
    } else {
        t[0].ob_x = w->work.g_x + (w->work.g_w - t[0].ob_width) / 2;
        t[0].ob_y = w->work.g_y + (w->work.g_h - t[0].ob_height) / 2;
    }
}

static void progdlg_open(VWIN *w)
{
    GRECT wk, r, doc;
    int k = NAME | MOVER;
    if (w->pdlg > 0)
        return;
    layout_tree(w);
    wk.g_x = wk.g_y = 0;
    wk.g_w = w->prog[0].ob_width;
    wk.g_h = w->prog[0].ob_height;
    wind_calc_grect(WC_BORDER, k, &wk, &r);
    /* centred over the document window, kept on the desktop */
    if (w->handle > 0)
        wind_get_grect(w->handle, WF_CURRXYWH, &doc);
    else
        doc = desk;
    r.g_x = doc.g_x + (doc.g_w - r.g_w) / 2;
    r.g_y = doc.g_y + (doc.g_h - r.g_h) / 2;
    if (r.g_x + r.g_w > desk.g_x + desk.g_w) r.g_x = desk.g_x + desk.g_w - r.g_w;
    if (r.g_y + r.g_h > desk.g_y + desk.g_h) r.g_y = desk.g_y + desk.g_h - r.g_h;
    if (r.g_x < desk.g_x) r.g_x = desk.g_x;
    if (r.g_y < desk.g_y) r.g_y = desk.g_y;
    w->pdlg = wind_create_grect(k, &r);
    dbg("progress dialog for window %d: handle %d at %d,%d %dx%d", w->handle,
        w->pdlg, r.g_x, r.g_y, r.g_w, r.g_h);
    if (w->pdlg <= 0) {
        w->pdlg = 0;                 /* no window: panel in the document */
        return;
    }
    sprintf(w->ptitle, " %.40s ", w->ptext);
    wind_set_str(w->pdlg, WF_NAME, w->ptitle);
    wind_open_grect(w->pdlg, &r);
    /* draw it straight away - while a file loads the redraw message may
       wait behind the work - and ask the AES for one as well */
    {
        GRECT wk2;
        short msg[8];
        wind_get_grect(w->pdlg, WF_WORKXYWH, &wk2);
        win_dlg_redraw(w, &wk2);
        msg[0] = WM_REDRAW; msg[1] = ap_id; msg[2] = 0; msg[3] = w->pdlg;
        msg[4] = wk2.g_x; msg[5] = wk2.g_y; msg[6] = wk2.g_w; msg[7] = wk2.g_h;
        appl_write(ap_id, 16, msg);
    }
}

void progdlg_close(VWIN *w)
{
    if (w->pdlg > 0) {
        wind_close(w->pdlg);
        wind_delete(w->pdlg);
    }
    w->pdlg = 0;
}

VWIN *win_find_dlg(int handle)
{
    int i;
    if (handle <= 0)
        return 0;
    for (i = 0; i < MAXWIN; i++)
        if (wins[i].used && wins[i].pdlg == handle)
            return &wins[i];
    return 0;
}

static void begin_panel(VWIN *w, const char *verb)
{
    sprintf(w->ptext, "%s %.40s", verb, base_name(w->path));
    strcpy(w->pstat, "Starting...");
    build_progress(w->prog, w->ptext, w->pstat);
    w->pdone = 0;
    w->ptotal = 1;
    w->lastpct = -1;
    w->prog[5].ob_state = 0;
    progdlg_open(w);
}

int win_over_cancel(VWIN *w, int mx, int my)
{
    short x, y;
    if (!win_busy(w) || (w->pdlg <= 0 && (w->iconified || w->handle <= 0)))
        return 0;
    layout_progress(w);
    objc_offset(w->prog, 5, &x, &y);
    return mx >= x && mx < x + w->prog[5].ob_width &&
           my >= y && my < y + w->prog[5].ob_height;
}

/* draw the panel, clipped to one object, through the rectangle list */
static void draw_panel_objs(VWIN *w, int obj)
{
    GRECT r, o;
    short x, y;
    int h = w->pdlg > 0 ? w->pdlg : w->handle;
    if (h <= 0 || (w->pdlg <= 0 && w->iconified))
        return;
    layout_progress(w);
    objc_offset(w->prog, obj, &x, &y);
    o.g_x = x - 3; o.g_y = y - 3;
    o.g_w = w->prog[obj].ob_width + 6;
    o.g_h = w->prog[obj].ob_height + 6;
    wind_update(BEG_UPDATE);
    graf_mouse(M_OFF, 0);
    wind_get_grect(h, WF_FIRSTXYWH, &r);
    while (r.g_w && r.g_h) {
        if (rc_intersect(&o, &r) && rc_intersect(&desk, &r))
            objc_draw_grect(w->prog, 0, 8, &r);
        wind_get_grect(h, WF_NEXTXYWH, &r);
    }
    graf_mouse(M_ON, 0);
    wind_update(END_UPDATE);
}

/* WM_REDRAW for a progress dialog */
void win_dlg_redraw(VWIN *w, GRECT *area)
{
    GRECT r, a = *area;
    if (w->pdlg <= 0 || !rc_intersect(&desk, &a))
        return;
    layout_progress(w);
    wind_update(BEG_UPDATE);
    graf_mouse(M_OFF, 0);
    wind_get_grect(w->pdlg, WF_FIRSTXYWH, &r);
    while (r.g_w && r.g_h) {
        if (rc_intersect(&a, &r))
            objc_draw_grect(w->prog, 0, 8, &r);
        wind_get_grect(w->pdlg, WF_NEXTXYWH, &r);
    }
    graf_mouse(M_ON, 0);
    wind_update(END_UPDATE);
}

static void do_cancel(VWIN *w)
{
    w->prog[5].ob_state |= SELECTED;
    draw_panel_objs(w, 5);
    strcpy(w->pstat, "Cancelling...");
    draw_panel_objs(w, 4);
    if (w->job)
        job_cancel(w->job);
    else
        w->closing |= 2;              /* save / print cancel flag */
}

/* click in a progress dialog */
void win_dlg_click(VWIN *w, int mx, int my)
{
    if (win_over_cancel(w, mx, my) && !(w->prog[5].ob_state & SELECTED))
        do_cancel(w);
}

/* the job moved on from loading the file to converting it for the screen */
void win_stage(VWIN *w, int stage)
{
    if (stage != 1 || w->state != ST_LOADING)
        return;
    sprintf(w->ptext, "Dithering %.40s", base_name(w->path));
    strcpy(w->pstat, "0%");
    w->lastpct = -1;
    if (w->pdlg > 0) {
        sprintf(w->ptitle, " %.40s ", w->ptext);
        wind_set_str(w->pdlg, WF_NAME, w->ptitle);
    }
    draw_panel_objs(w, 1);
    draw_panel_objs(w, 4);
    dbg("window %d: dithering stage", w->handle);
}

/* FArchive's jobs send log lines; FView's and FConv's do not */
void win_note(VWIN *w, int kind, const char *text)
{
    (void)w;
    (void)kind;
    (void)text;
}

void win_progress(VWIN *w, long done, long total)
{
    int pct;
    if (total <= 0) total = 1;
    if (done > total) done = total;
    w->pdone = done;
    w->ptotal = total;
    pct = (int)(done * 100 / total);
    if (pct == w->lastpct)
        return;
    sprintf(w->pstat, "%d%%", pct);
    if (w->lastpct < 0) {
        /* first update: draw the whole panel, in case it hasn't been */
        w->lastpct = pct;
        if (w->pdlg > 0) {
            GRECT wk2;
            wind_get_grect(w->pdlg, WF_WORKXYWH, &wk2);
            win_dlg_redraw(w, &wk2);
        } else if (w->handle > 0 && !w->iconified)
            win_redraw(w, &w->work);
        return;
    }
    w->lastpct = pct;
    draw_panel_objs(w, 2);
    draw_panel_objs(w, 4);
}

/* ------------------------------------------------------------ palette */

static VWIN *pal_owner = (VWIN *)-1;
short syspal[256][3];
static int npens;
static int pen2pix[256];

void pal_init(void)
{
    int i;
    /* standard GEM pen colours, for drivers that don't report them */
    static const short stdpal[16][3] = {
        {1000,1000,1000},{0,0,0},{1000,0,0},{0,1000,0},{0,0,1000},{0,1000,1000},
        {1000,1000,0},{1000,0,1000},{752,752,752},{502,502,502},{502,0,0},
        {0,502,0},{0,0,502},{0,502,502},{502,502,0},{502,0,502}
    };
    int nq, same = 1;
    npens = dinfo.truecolor ? 0 : (scr_planes >= 8 ? 256 : 1 << scr_planes);
    for (i = 0; i < npens; i++)
        pen2pix[i] = vdi2pix(i, scr_planes);
    /* The page renderer matches colours against the first 16 pens, so
       they are needed on true colour screens too (no hardware palette,
       but the VDI pens still have colours).                           */
    nq = npens ? npens : 16;
    for (i = 0; i < nq; i++)
        vq_color(vdih, i, 0, syspal[i]);
    for (i = 1; i < 16 && i < nq; i++)
        if (memcmp(syspal[i], syspal[0], sizeof syspal[0]))
            same = 0;
    if (same && nq >= 2)
        for (i = 0; i < 16 && i < nq; i++)
            memcpy(syspal[i], stdpal[i], sizeof syspal[i]);
    dbg("pens: %d palette, pen 0 %d,%d,%d pen 1 %d,%d,%d%s", npens,
        syspal[0][0], syspal[0][1], syspal[0][2], syspal[1][0], syspal[1][1],
        syspal[1][2], same ? " (standard colours used)" : "");
}

void pal_set_pen2pix(int pen, int pix) { pen2pix[pen] = pix; }

static void pal_apply(VWIN *w)
{
    int i;
    for (i = 0; i < npens; i++) {
        int pix = pen2pix[i];
        short rgb[3];
        if (dinfo.reserved[pix])
            continue;
        if (w) {
            rgb[0] = (w->pal[pix * 3] * 1000 + 127) / 255;
            rgb[1] = (w->pal[pix * 3 + 1] * 1000 + 127) / 255;
            rgb[2] = (w->pal[pix * 3 + 2] * 1000 + 127) / 255;
            vs_color(vdih, i, rgb);
        } else
            vs_color(vdih, i, syspal[i]);
    }
}

void pal_check_top(void)
{
    VWIN *w = win_top(), *want = 0;
    if (npens == 0 || dinfo.fixed_palette)
        return;
    if (w && w->kind == K_IMAGE && w->haspal && !w->iconified && w->disp.fd_addr)
        want = w;
    if (want != pal_owner) {
        pal_apply(want);
        pal_owner = want;
    }
}

void pal_invalidate(void) { pal_owner = (VWIN *)-1; }

void pal_restore(void)
{
    if (npens && !dinfo.fixed_palette)
        pal_apply(0);
    pal_owner = 0;
}

/* page scale in pixels per point * 1000, horizontal and vertical */
static void pdf_scale(VWIN *w, long *sx, long *sy)
{
    long pw, ph, x0, y0;
    pdf_page_size(w->blob, w->pageoff[w->page], &pw, &ph, &x0, &y0);
    if (pw < 16) pw = 16;
    if (w->pdfzoom == 0 && w->work.g_w > 4 * MARGIN)
        *sx = (long)(w->work.g_w - 2 * MARGIN) * 16000L / pw;
    else
        *sx = 25400L * 10L * (w->pdfzoom ? w->pdfzoom : 100) / 72L /
              (pix_w_um > 0 ? pix_w_um : 278);
    *sy = *sx * (pix_w_um > 0 ? pix_w_um : 1) / (pix_h_um > 0 ? pix_h_um : 1);
}

/* ------------------------------------------------------------- layout */

static void doc_size(VWIN *w)
{
    w->docw = w->doch = 0;
    if (w->kind == K_IMAGE) {
        w->docw = w->disp.fd_addr ? w->img.dw : 0;
        w->doch = w->disp.fd_addr ? w->img.dh : 0;
    } else if (w->kind == K_TEXT && w->lines && w->nlines && w->lines[0].rich) {
        TLINE *l = &w->lines[w->nlines - 1];
        w->docw = l->avail + 2 * MARGIN;
        w->doch = l->y + l->h + 2 * MARGIN;
    } else if (w->kind == K_TEXT && w->lines && w->nlines) {
        long i, mx = 0;
        TLINE *l = &w->lines[w->nlines - 1];
        for (i = 0; i < w->nlines; i++) {
            long v = w->lines[i].indent * scrtd.cw[PS_NORMAL] +
                     txt_vis(w->blob + w->lines[i].para + 4 + w->lines[i].start, w->lines[i].len) *
                     scrtd.cw[w->lines[i].style];
            if (v > mx) mx = v;
        }
        w->docw = mx + 2 * MARGIN;
        w->doch = l->y + l->h + 2 * MARGIN;
    } else if (w->kind == K_PDF && w->npages) {
        long sx, sy, pw, ph, x0, y0;
        pdf_scale(w, &sx, &sy);
        pdf_page_size(w->blob, w->pageoff[w->page], &pw, &ph, &x0, &y0);
        w->docw = pw * sx / 16000L + 2 * MARGIN;
        w->doch = ph * sy / 16000L + 2 * MARGIN;
    }
}

void win_relayout(VWIN *w)
{
    if (w->kind == K_TEXT && w->blob) {
        TLINE *l = 0;
        long n;
        if (!scrtd_ok) {
            txt_measure(&scrtd, vdih, cell_h > 10 ? 13 : 6, 1 << (scr_planes > 8 ? 8 : scr_planes),
                        pix_w_um);
            scrtd_ok = 1;
        }
        n = txt_layout(w->blob, w->img.doclen, &scrtd, w->work.g_w, &l, 0);
        if (n >= 0) {
            free(w->lines);
            w->lines = l;
            w->nlines = n;
        }
        w->laywidth = w->work.g_w;
    }
    doc_size(w);
    win_scroll_to(w, w->xoff, w->yoff);
    win_set_sliders(w);
}

void win_update_info(VWIN *w)
{
    if (w->handle <= 0 || w->kind == K_MAIN)
        return;
    if (w->state == ST_ERROR)
        sprintf(w->info, " Error");
    else if (w->kind == K_IMAGE && w->disp.fd_addr)
    {
        long rw, rh;
        img_display_size(&w->img.im, &dinfo, &rw, &rh);
        sprintf(w->info, " %ldx%ld  %.30s  %s%s", w->img.im.w, w->img.im.h,
                w->img.im.fmtinfo,
                w->zoom == ZOOM_FIT ? "fit" : (rw != w->img.im.w || rh != w->img.im.h) ?
                "real size" : "1:1",
                (rw != w->img.im.w || rh != w->img.im.h) ? ", aspect corrected" : "");
    }
    else if (w->kind == K_IMAGE)
        sprintf(w->info, " %s", ftype_name(w->ftype));
    else if (w->kind == K_PDF && w->npages) {
        long sx, sy;
        pdf_scale(w, &sx, &sy);
        if (w->ftype == FT_GEM)
            sprintf(w->info, " GEM metafile   %s%ld%%   (+/-)", w->pdfzoom ? "" : "fit ",
                    sx * (pix_w_um > 0 ? pix_w_um : 278) * 72L / 254000L);
        else
        sprintf(w->info, " PDF  page %ld of %ld   %s%ld%%   (^B/^N, +/-)", w->page + 1,
                w->npages, w->pdfzoom ? "" : "fit ",
                sx * (pix_w_um > 0 ? pix_w_um : 278) * 72L / 254000L);
    }
    else if (w->kind == K_TEXT)
        sprintf(w->info, " %s  %ld lines", ftype_name(w->ftype), w->nlines);
    if (w->kind == K_IMAGE && w->disp.fd_addr && w->zoom == ZOOM_FIT) {
        char t[24];
        sprintf(t, " %ld%%", w->img.dw * 100 / (w->img.im.w ? w->img.im.w : 1));
        strcat(w->info, t);
    }
    wind_set_str(w->handle, WF_INFO, w->info);
}

/* ------------------------------------------------------------- drawing */

static void clear_rect(GRECT *r, int color)
{
    short pxy[4];
    vsf_interior(vdih, FIS_SOLID);
    vsf_color(vdih, color);
    vsf_perimeter(vdih, 0);
    vswr_mode(vdih, MD_REPLACE);
    pxy[0] = r->g_x; pxy[1] = r->g_y;
    pxy[2] = r->g_x + r->g_w - 1; pxy[3] = r->g_y + r->g_h - 1;
    vr_recfl(vdih, pxy);
}

static void grey_rect(GRECT *r)
{
    short pxy[4];
    vswr_mode(vdih, MD_REPLACE);
    vsf_perimeter(vdih, 0);
    if (scr_planes >= 4 && !dinfo.truecolor) {
        vsf_interior(vdih, FIS_SOLID);
        vsf_color(vdih, 8);              /* light grey */
    } else if (dinfo.truecolor) {
        vsf_interior(vdih, FIS_SOLID);
        vsf_color(vdih, 8);
    } else {
        vsf_interior(vdih, FIS_PATTERN);
        vsf_style(vdih, 2);
        vsf_color(vdih, 1);
    }
    pxy[0] = r->g_x; pxy[1] = r->g_y;
    pxy[2] = r->g_x + r->g_w - 1; pxy[3] = r->g_y + r->g_h - 1;
    vr_recfl(vdih, pxy);
}

/* ------------------------------------------------------- picture blit */

/* How pictures get onto the screen. 0: vro_cpyfm to the screen MFDB
 * (fd_addr 0); 1: vro_cpyfm to an explicit screen MFDB (Logbase); 2: our
 * own copy straight into ST-style interleaved screen memory. fview checks
 * the first copy by reading pixels back and steps down if it didn't land. */
static int blit_mode, blit_checked;

static void screen_mfdb(MFDB *m)
{
    memset(m, 0, sizeof *m);
    if (blit_mode >= 1) {
        m->fd_addr = Logbase();
        m->fd_w = (short)((scr_w + 15) & ~15);
        m->fd_h = (short)scr_h;
        m->fd_wdwidth = (short)((scr_w + 15) / 16);
        m->fd_nplanes = (short)scr_planes;
    }
}

/* pixel value at x,y in an interleaved device raster */
static int ilv_pixel(const MFDB *m, int x, int y)
{
    const u16 *g = (const u16 *)m->fd_addr +
                   ((long)y * m->fd_wdwidth + (x >> 4)) * m->fd_nplanes;
    int p, v = 0, bit = 15 - (x & 15);
    for (p = 0; p < m->fd_nplanes; p++)
        v |= ((g[p] >> bit) & 1) << p;
    return v;
}

/* mode 2: copy a rectangle into interleaved screen memory ourselves */
static void direct_copy(const MFDB *src, short *pxy)
{
    MFDB d;
    int planes = src->fd_nplanes, y, p;
    int sx = pxy[0], sy = pxy[1], dx = pxy[4], dy = pxy[5];
    int w = pxy[2] - pxy[0] + 1, h = pxy[3] - pxy[1] + 1;
    screen_mfdb(&d);
    d.fd_addr = Logbase();
    d.fd_wdwidth = (short)((scr_w + 15) / 16);
    for (y = 0; y < h; y++) {
        const u16 *srow = (const u16 *)src->fd_addr + (long)(sy + y) * src->fd_wdwidth * planes;
        u16 *drow = (u16 *)d.fd_addr + (long)(dy + y) * d.fd_wdwidth * planes;
        int x = 0;
        while (x < w) {
            int dpx = dx + x, dbit = dpx & 15, n = 16 - dbit;
            u16 mask;
            if (n > w - x) n = w - x;
            mask = (u16)(((1UL << n) - 1) << (16 - dbit - n));
            for (p = 0; p < planes; p++) {
                /* gather n source bits starting at sx + x */
                int spx = sx + x, sbit = spx & 15;
                const u16 *sg = srow + (spx >> 4) * planes + p;
                unsigned long bits = ((unsigned long)sg[0] << 16) |
                                     (((spx >> 4) + 1 < src->fd_wdwidth) ? sg[planes] : 0);
                u16 v = (u16)((bits << sbit) >> 16);      /* bits at the top */
                u16 *dg = drow + (dpx >> 4) * planes + p;
                v = (u16)(v >> dbit);
                *dg = (u16)((*dg & ~mask) | (v & mask));
            }
            x += n;
        }
    }
}

/* check a few copied pixels against the picture */
static int blit_landed(VWIN *w, short *pxy)
{
    int k, bad = 0;
    for (k = 0; k < 4; k++) {
        int fx = k & 1 ? 3 : 1, fy = k & 2 ? 3 : 1;
        int sx = pxy[0] + (pxy[2] - pxy[0]) * fx / 4;
        int sy = pxy[1] + (pxy[3] - pxy[1]) * fy / 4;
        int x = pxy[4] + (sx - pxy[0]), y = pxy[5] + (sy - pxy[1]);
        short pel = -1, idx = -1;
        int want = ilv_pixel(&w->disp, sx, sy);
        v_get_pixel(vdih, x, y, &pel, &idx);
        dbg("  check %d,%d: screen pel %d idx %d, picture %d", x, y, pel, idx, want);
        if (pel != want)
            bad++;
    }
    return bad < 2;                 /* allow one coincidental mismatch */
}

static void blit_image(VWIN *w, short *pxy)
{
    MFDB scr;
    if (blit_mode == 2) {
        direct_copy(&w->disp, pxy);
        return;
    }
    screen_mfdb(&scr);
    vro_cpyfm(vdih, S_ONLY, pxy, &w->disp, &scr);
    if (blit_checked || !screen_interleaved)
        return;
    blit_checked = 1;
    if (blit_landed(w, pxy)) {
        dbg("picture copy mode %d works", blit_mode);
        return;
    }
    dbg("picture copy mode %d did not reach the screen", blit_mode);
    blit_mode = 1;
    screen_mfdb(&scr);
    vro_cpyfm(vdih, S_ONLY, pxy, &w->disp, &scr);
    if (blit_landed(w, pxy)) {
        dbg("picture copy mode 1 (explicit screen MFDB %lx) works", (long)scr.fd_addr);
        return;
    }
    dbg("picture copy mode 1 failed too: copying into screen memory directly");
    blit_mode = 2;
    direct_copy(&w->disp, pxy);
    dbg("direct copy %s", blit_landed(w, pxy) ? "works" : "FAILED as well");
}

static void image_origin(VWIN *w, int *ox, int *oy)
{
    *ox = w->work.g_x - (int)w->xoff;
    *oy = w->work.g_y - (int)w->yoff;
    if (w->docw < w->work.g_w) *ox = w->work.g_x + (w->work.g_w - (int)w->docw) / 2;
    if (w->doch < w->work.g_h) *oy = w->work.g_y + (w->work.g_h - (int)w->doch) / 2;
}

static void draw_main(VWIN *w, GRECT *r)
{
    int i, y, n = 0;
    short d;
    char line[64];
    clear_rect(r, 0);
    vst_height(vdih, cell_h > 10 ? 13 : 6, &d, &d, &d, &d);
    vst_alignment(vdih, 0, 5, &d, &d);
    vst_color(vdih, 1);
    vswr_mode(vdih, MD_TRANS);
    vst_effects(vdih, TA_BOLD);
    y = w->work.g_y + 4;
    v_gtext(vdih, w->work.g_x + 8, y, "FView " FV_VERSION " - drop files here");
    vst_effects(vdih, 0);
    y += cell_h + 4;
    for (i = 0; i < MAXWIN; i++) {
        VWIN *v = &wins[i];
        if (!v->used || v->kind == K_MAIN || v->handle <= 0)
            continue;
        sprintf(line, "%c %-24.24s %s", win_busy(v) ? '*' : ' ', base_name(v->path),
                v->state == ST_ERROR ? "error" : win_busy(v) ? "busy" : "");
        v_gtext(vdih, w->work.g_x + 8, y, line);
        y += cell_h;
        n++;
    }
    if (!n)
        v_gtext(vdih, w->work.g_x + 8, y, "  No files open. ^O to open.");
}

static void draw_content(VWIN *w, GRECT *r)
{
    short pxy[8];
    int ox, oy;

    if (w->kind == K_MAIN) {
        draw_main(w, r);
        return;
    }
    if (win_busy(w)) {
        short d;
        clear_rect(r, 0);
        if (w->pdlg <= 0) {                 /* no dialog: panel in here */
            layout_progress(w);
            objc_draw_grect(w->prog, 0, 8, r);
            return;
        }
        vst_height(vdih, cell_h > 10 ? 13 : 6, &d, &d, &d, &d);
        vst_alignment(vdih, 0, 5, &d, &d);
        vst_color(vdih, 1);
        vswr_mode(vdih, MD_TRANS);
        v_gtext(vdih, w->work.g_x + 8, w->work.g_y + 8, w->ptext);
        return;
    }
    if (w->state == ST_ERROR) {
        short d;
        clear_rect(r, 0);
        vst_height(vdih, cell_h > 10 ? 13 : 6, &d, &d, &d, &d);
        vst_alignment(vdih, 0, 5, &d, &d);
        vst_color(vdih, 1);
        vswr_mode(vdih, MD_TRANS);
        v_gtext(vdih, w->work.g_x + 8, w->work.g_y + 8, w->errmsg);
        return;
    }
    image_origin(w, &ox, &oy);
    if (w->kind == K_IMAGE && w->disp.fd_addr) {
        GRECT img, c = *r;
        MFDB scr;
        img.g_x = ox; img.g_y = oy;
        img.g_w = (int)w->img.dw; img.g_h = (int)w->img.dh;
        /* background around a small picture */
        if (img.g_x > r->g_x || img.g_y > r->g_y ||
            img.g_x + img.g_w < r->g_x + r->g_w || img.g_y + img.g_h < r->g_y + r->g_h)
            grey_rect(r);
        if (rc_intersect(&img, &c)) {
            memset(&scr, 0, sizeof scr);
            pxy[0] = c.g_x - ox; pxy[1] = c.g_y - oy;
            pxy[2] = pxy[0] + c.g_w - 1; pxy[3] = pxy[1] + c.g_h - 1;
            pxy[4] = c.g_x; pxy[5] = c.g_y;
            pxy[6] = c.g_x + c.g_w - 1; pxy[7] = c.g_y + c.g_h - 1;
            dbg(" copy %d,%d-%d,%d to %d,%d mode %d", pxy[0], pxy[1], pxy[2], pxy[3],
                pxy[4], pxy[5], blit_mode);
            (void)scr;
            blit_image(w, pxy);
        }
    } else if (w->kind == K_TEXT && w->lines) {
        long lo = 0, hi = w->nlines - 1, first = 0, i;
        long top = r->g_y - oy - MARGIN;
        clear_rect(r, 0);
        while (lo <= hi) {                 /* first line intersecting r */
            long m = (lo + hi) / 2;
            if (w->lines[m].y + w->lines[m].h <= top) { first = m + 1; lo = m + 1; }
            else hi = m - 1;
        }
        for (i = first; i < w->nlines; i++) {
            TLINE *l = &w->lines[i];
            int y = oy + MARGIN + (int)l->y;
            if (y > r->g_y + r->g_h)
                break;
            txt_draw_line(&scrtd, w->blob, l, ox + MARGIN, y, (int)w->docw);
        }
    } else if (w->kind == K_PDF && w->npages) {
        long sx, sy, pw, ph, x0, y0;
        GRECT page, c = *r;
        pdf_scale(w, &sx, &sy);
        pdf_page_size(w->blob, w->pageoff[w->page], &pw, &ph, &x0, &y0);
        page.g_x = ox + MARGIN; page.g_y = oy + MARGIN;
        page.g_w = (int)(pw * sx / 16000L); page.g_h = (int)(ph * sy / 16000L);
        grey_rect(r);
        if (rc_intersect(&page, &c)) {
            short clip[4];
            clear_rect(&c, 0);
            clip[0] = c.g_x; clip[1] = c.g_y;
            clip[2] = c.g_x + c.g_w - 1; clip[3] = c.g_y + c.g_h - 1;
            vs_clip(vdih, 1, clip);
            pdf_draw_page(vdih, w->blob, w->pageoff[w->page], page.g_x, page.g_y,
                          sx, sy, npens ? npens : 256, scr_planes == 1, &c);
        }
    } else
        clear_rect(r, 0);
}

static void draw_iconified(VWIN *w, GRECT *r)
{
    OBJECT *t = build_icon(w->kind == K_MAIN ? APPNAME : w->title);
    clear_rect(r, 0);
    t[0].ob_x = w->work.g_x;
    t[0].ob_y = w->work.g_y;
    t[0].ob_width = w->work.g_w;
    t[0].ob_height = w->work.g_h;
    t[1].ob_x = (w->work.g_w - t[1].ob_width) / 2;
    t[1].ob_y = (w->work.g_h - t[1].ob_height) / 2;
    if (t[1].ob_y < 0) t[1].ob_y = 0;
    objc_draw_grect(t, 0, 2, r);
}

void win_redraw(VWIN *w, GRECT *area)
{
    GRECT r, a = *area;
    short clip[4];
    if (w->handle <= 0)
        return;
    get_work(w);
    if (!rc_intersect(&desk, &a))
        return;
    /* PDF pages: the page's images are scaled and dithered once per page
       and size. Small amounts are done here; anything that takes a while
       is left blank and done from the main loop with a progress dialog. */
    render_nobuild = 0;
    render_boxes = 0;
    if (w->kind == K_PDF && w->npages && !win_busy(w) && !w->iconified &&
        w->state != ST_ERROR) {
        long sx, sy, miss;
        pdf_scale(w, &sx, &sy);
        if (w->prepst == 0) {
            miss = pdf_page_missing(vdih, w->blob, w->pageoff[w->page], sx, sy,
                                    npens ? npens : 256, scr_planes == 1);
            if (miss > 150000L) {
                w->need_prep = 1;
                render_nobuild = 1;
            } else if (miss > 0)
                pdf_build_page(vdih, w->blob, w->pageoff[w->page], sx, sy,
                               npens ? npens : 256, scr_planes == 1, 0);
        } else if (w->prepst == 2) {
            render_nobuild = 1;                 /* cancelled: boxes only */
            render_boxes = 1;
        }
    }
    if (w->kind == K_IMAGE)
        dbg("redraw window %d area %d,%d %dx%d state %d", w->handle,
            a.g_x, a.g_y, a.g_w, a.g_h, w->state);
    wind_update(BEG_UPDATE);
    graf_mouse(M_OFF, 0);
    vst_font(vdih, 1);                   /* rich text may have left a GDOS font */
    scrtd.cur_font = -1;
    wind_get_grect(w->handle, WF_FIRSTXYWH, &r);
    while (r.g_w && r.g_h) {
        if (rc_intersect(&a, &r) && rc_intersect(&w->work, &r)) {
            clip[0] = r.g_x; clip[1] = r.g_y;
            clip[2] = r.g_x + r.g_w - 1; clip[3] = r.g_y + r.g_h - 1;
            vs_clip(vdih, 1, clip);
            if (w->iconified)
                draw_iconified(w, &r);
            else
                draw_content(w, &r);
        }
        wind_get_grect(w->handle, WF_NEXTXYWH, &r);
    }
    vs_clip(vdih, 0, clip);
    render_nobuild = 0;
    render_boxes = 0;
    vst_font(vdih, 1);
    vst_effects(vdih, 0);
    scrtd.cur_font = -1;
    graf_mouse(M_ON, 0);
    wind_update(END_UPDATE);
    if (w->kind == K_IMAGE)
        dbg("redraw window %d done", w->handle);
}

void win_redraw_all(VWIN *w)
{
    if (w->handle <= 0)
        return;
    get_work(w);
    win_redraw(w, &w->work);
}

/* ------------------------------------------------------------ scrolling */

void win_set_sliders(VWIN *w)
{
    long v;
    if (w->handle <= 0 || w->kind == K_MAIN || w->iconified)
        return;
    get_work(w);
    if (w->docw > w->work.g_w) {
        v = 1000L * w->work.g_w / w->docw;
        wind_set(w->handle, WF_HSLSIZE, (short)(v < 1 ? 1 : v), 0, 0, 0);
        v = 1000L * w->xoff / (w->docw - w->work.g_w);
        wind_set(w->handle, WF_HSLIDE, (short)v, 0, 0, 0);
    } else {
        wind_set(w->handle, WF_HSLSIZE, 1000, 0, 0, 0);
        wind_set(w->handle, WF_HSLIDE, 0, 0, 0, 0);
    }
    if (w->doch > w->work.g_h) {
        v = 1000L * w->work.g_h / w->doch;
        wind_set(w->handle, WF_VSLSIZE, (short)(v < 1 ? 1 : v), 0, 0, 0);
        v = 1000L * w->yoff / (w->doch - w->work.g_h);
        wind_set(w->handle, WF_VSLIDE, (short)v, 0, 0, 0);
    } else {
        wind_set(w->handle, WF_VSLSIZE, 1000, 0, 0, 0);
        wind_set(w->handle, WF_VSLIDE, 0, 0, 0, 0);
    }
}

void win_scroll_to(VWIN *w, long x, long y)
{
    long mx, my, dx, dy;
    GRECT first;
    get_work(w);
    mx = w->docw - w->work.g_w;
    my = w->doch - w->work.g_h;
    if (x > mx) x = mx;
    if (y > my) y = my;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    dx = x - w->xoff;
    dy = y - w->yoff;
    if (!dx && !dy)
        return;
    w->xoff = x;
    w->yoff = y;
    win_set_sliders(w);
    if (w->handle <= 0 || win_busy(w) || w->iconified)
        return;

    /* blit when the window is fully visible, otherwise redraw */
    wind_get_grect(w->handle, WF_FIRSTXYWH, &first);
    if (first.g_x == w->work.g_x && first.g_y == w->work.g_y &&
        first.g_w == w->work.g_w && first.g_h == w->work.g_h &&
        (dx == 0 || dy == 0) && labs(dx) < w->work.g_w && labs(dy) < w->work.g_h &&
        w->docw >= w->work.g_w && w->doch >= w->work.g_h) {
        MFDB scr;
        short pxy[8];
        GRECT ex = w->work;
        memset(&scr, 0, sizeof scr);
        pxy[0] = w->work.g_x + (dx > 0 ? (short)dx : 0);
        pxy[1] = w->work.g_y + (dy > 0 ? (short)dy : 0);
        pxy[2] = w->work.g_x + w->work.g_w - 1 - (dx < 0 ? (short)-dx : 0);
        pxy[3] = w->work.g_y + w->work.g_h - 1 - (dy < 0 ? (short)-dy : 0);
        pxy[4] = w->work.g_x + (dx < 0 ? (short)-dx : 0);
        pxy[5] = w->work.g_y + (dy < 0 ? (short)-dy : 0);
        pxy[6] = pxy[4] + (pxy[2] - pxy[0]);
        pxy[7] = pxy[5] + (pxy[3] - pxy[1]);
        wind_update(BEG_UPDATE);
        graf_mouse(M_OFF, 0);
        vro_cpyfm(vdih, S_ONLY, pxy, &scr, &scr);
        graf_mouse(M_ON, 0);
        wind_update(END_UPDATE);
        if (dy > 0) { ex.g_y = w->work.g_y + w->work.g_h - (int)dy; ex.g_h = (int)dy; }
        else if (dy < 0) ex.g_h = (int)-dy;
        else if (dx > 0) { ex.g_x = w->work.g_x + w->work.g_w - (int)dx; ex.g_w = (int)dx; }
        else ex.g_w = (int)-dx;
        win_redraw(w, &ex);
    } else
        win_redraw(w, &w->work);
}

void win_arrow(VWIN *w, int what)
{
    long lx = w->kind == K_TEXT ? cell_w : 16, ly = w->kind == K_TEXT ? cell_h : 16;
    get_work(w);
    switch (what) {
    case WA_UPPAGE: win_scroll_to(w, w->xoff, w->yoff - w->work.g_h + ly); break;
    case WA_DNPAGE: win_scroll_to(w, w->xoff, w->yoff + w->work.g_h - ly); break;
    case WA_UPLINE: win_scroll_to(w, w->xoff, w->yoff - ly); break;
    case WA_DNLINE: win_scroll_to(w, w->xoff, w->yoff + ly); break;
    case WA_LFPAGE: win_scroll_to(w, w->xoff - w->work.g_w + lx, w->yoff); break;
    case WA_RTPAGE: win_scroll_to(w, w->xoff + w->work.g_w - lx, w->yoff); break;
    case WA_LFLINE: win_scroll_to(w, w->xoff - lx, w->yoff); break;
    case WA_RTLINE: win_scroll_to(w, w->xoff + lx, w->yoff); break;
    }
}

void win_hslide(VWIN *w, int pos)
{
    get_work(w);
    win_scroll_to(w, (w->docw - w->work.g_w) * pos / 1000, w->yoff);
}

void win_vslide(VWIN *w, int pos)
{
    get_work(w);
    win_scroll_to(w, w->xoff, (w->doch - w->work.g_h) * pos / 1000);
}

/* ------------------------------------------------------- size / move */

static void start_render(VWIN *w);

void win_sized(VWIN *w, GRECT *r)
{
    GRECT n = *r;
    w->prepst = 0;                /* PDF: page images at the new size */
    int minw = 12 * cell_w, minh = 6 * cell_h;
    if (n.g_w < minw) n.g_w = minw;
    if (n.g_h < minh) n.g_h = minh;
    wind_set_grect(w->handle, WF_CURRXYWH, &n);
    get_work(w);
    if (w->kind == K_TEXT && w->laywidth != w->work.g_w && !win_busy(w))
        win_relayout(w);
    else if (w->kind == K_PDF && prefs.pdf_zoom == PDFZ_FITW && !win_busy(w))
        win_relayout(w);
    else {
        doc_size(w);
        win_scroll_to(w, w->xoff, w->yoff);
    }
    win_set_sliders(w);
    if (w->kind == K_IMAGE && w->zoom == ZOOM_FIT && w->disp.fd_addr)
        start_render(w);
    /* AES only redraws newly exposed areas; centred content moves */
    win_redraw_all(w);
}

void win_moved(VWIN *w, GRECT *r)
{
    wind_set_grect(w->handle, WF_CURRXYWH, r);
    get_work(w);
}

void win_fulled(VWIN *w)
{
    GRECT cur, prev;
    wind_get_grect(w->handle, WF_CURRXYWH, &cur);
    wind_get_grect(w->handle, WF_PREVXYWH, &prev);
    if (cur.g_x == desk.g_x && cur.g_y == desk.g_y && cur.g_w == desk.g_w &&
        cur.g_h == desk.g_h && prev.g_w > 0)
        win_sized(w, &prev);
    else
        win_sized(w, &desk);
}

/* Ask the AES to send us a redraw of the whole work area. Used after
 * iconify / uniconify: drawing straight away can happen before the AES
 * has finished with the window frame, and then the contents get lost.  */
void win_post_redraw(VWIN *w)
{
    short msg[8];
    if (w->handle <= 0)
        return;
    get_work(w);
    msg[0] = WM_REDRAW;
    msg[1] = ap_id;
    msg[2] = 0;
    msg[3] = w->handle;
    msg[4] = w->work.g_x;
    msg[5] = w->work.g_y;
    msg[6] = w->work.g_w;
    msg[7] = w->work.g_h;
    appl_write(ap_id, 16, msg);
}

void win_iconify(VWIN *w, GRECT *r)
{
    if (!aes_iconify)
        return;
    wind_set_grect(w->handle, WF_ICONIFY, r);
    w->iconified = 1;
    wind_set_str(w->handle, WF_NAME, w->kind == K_MAIN ? APPNAME : w->title);
    get_work(w);
    pal_invalidate();
    win_post_redraw(w);
}

/* For AESes without WF_ICONIFY: shrink the window to icon size and
 * draw the icon in it; a click opens it out again.                    */
void win_fake_iconify(VWIN *w, GRECT *r)
{
    wind_get_grect(w->handle, WF_CURRXYWH, &w->full);   /* to restore */
    wind_set_grect(w->handle, WF_CURRXYWH, r);
    w->iconified = 2;
    get_work(w);
    pal_invalidate();
    win_post_redraw(w);          /* the AES only moved stale pixels */
}

void win_fake_uniconify(VWIN *w)
{
    GRECT r = w->full;
    if (r.g_w <= 0 || r.g_h <= 0) {
        GRECT wk;
        wk.g_x = wk.g_y = 0;
        wk.g_w = 34 * cell_w;
        wk.g_h = 10 * cell_h;
        wind_calc_grect(WC_BORDER, kind_flags(w), &wk, &r);
        r.g_x = desk.g_x + 8;
        r.g_y = desk.g_y + desk.g_h - r.g_h - 8;
    }
    wind_set_grect(w->handle, WF_CURRXYWH, &r);
    w->iconified = 0;
    get_work(w);
    win_post_redraw(w);
}

void win_uniconify(VWIN *w, GRECT *r)
{
    if (!aes_iconify)
        return;
    wind_set_grect(w->handle, WF_UNICONIFY, r);
    w->iconified = 0;
    get_work(w);
    win_set_sliders(w);
    pal_invalidate();
    win_post_redraw(w);
}

/* ------------------------------------------------------------- loading */

static void make_job(VWIN *w, int op)
{
    JOB *j = calloc(1, sizeof(JOB));
    if (!j)
        return;
    j->q.op = op;
    strncpy(j->q.path, w->path, sizeof j->q.path - 1);
    j->q.ftype = w->ftype;
    j->q.pf = prefs;
    j->q.disp = dinfo;
    j->q.disp.palmode = prefs.palmode;
    j->win = w;
    w->job = j;
}

void win_load(VWIN *w, const char *path)
{
    strncpy(w->path, path, sizeof w->path - 1);
    w->ftype = ftype_detect(path);
    sprintf(w->title, " %.60s ", base_name(path));
    w->kind = FT_IS_GRAPHIC(w->ftype) ? K_IMAGE : FT_IS_PAGE(w->ftype) ? K_PDF : K_TEXT;
    w->pdfzoom = prefs.pdf_zoom == PDFZ_FITW ? 0 : 100;
    if (!win_open(w, w->kind))
        return;
    w->state = ST_LOADING;
    begin_panel(w, "Reading file");
    win_update_info(w);
    make_job(w, OP_LOAD);
    if (!w->job) {
        win_destroy(w);
        return;
    }
    if (w->kind == K_IMAGE && w->zoom == ZOOM_FIT) {
        w->job->q.tw = w->work.g_w;
        w->job->q.th = w->work.g_h;
    }
    job_start(w->job);
    menu_update();
}

static void start_render(VWIN *w)
{
    JOB *j;
    if (w->job) {                         /* restart a render in progress */
        if (w->job->q.op == OP_RENDER && !w->job->done) {
            w->want_rerender = 1;
            job_cancel(w->job);
        }
        return;
    }
    make_job(w, OP_RENDER);
    j = w->job;
    if (!j)
        return;
    get_work(w);
    j->q.src = w->img.im;
    strcpy(j->q.src_shm, w->img.shm[RB_ORIG]);
    img_display_size(&w->img.im, &dinfo, &j->q.tw, &j->q.th);
    if (w->zoom == ZOOM_FIT)
        img_fit(j->q.tw, j->q.th, w->work.g_w, w->work.g_h, &j->q.tw, &j->q.th);
    w->state = ST_RENDER;
    begin_panel(w, "Dithering");
    win_redraw_all(w);
    job_start(j);
}

void win_set_zoom(VWIN *w, int zoom)
{
    if (w->kind != K_IMAGE || !w->img.im.px)
        return;
    w->zoom = zoom;
    w->xoff = w->yoff = 0;
    start_render(w);
    menu_update();
}

/* standard -> device format, one band at a time */
static int to_device(VWIN *w, JOBRES *r)
{
    long wdw = r->wdwidth, h = r->dh, rowbytes, y;
    u8 *dev;
    if (dinfo.truecolor || dinfo.chunky) {
        /* the worker already produced device pixels: keep its block */
        w->dispmem = 0;
        memset(&w->disp, 0, sizeof w->disp);
        w->disp.fd_addr = r->ptr[RB_DISP];
        w->disp.fd_w = (short)(wdw * 16);
        w->disp.fd_h = (short)h;
        w->disp.fd_wdwidth = (short)wdw;
        w->disp.fd_stand = 0;
        w->disp.fd_nplanes = scr_planes;
        return 1;
    }
    rowbytes = wdw * 2 * scr_planes;
    dev = malloc(rowbytes * h);
    if (!dev)
        return 0;
    for (y = 0; y < h; y += STD_BAND) {
        MFDB s, d;
        long psize;
        int rows = (int)(h - y > STD_BAND ? STD_BAND : h - y);
        memset(&s, 0, sizeof s);
        s.fd_addr = std_row((u16 *)r->ptr[RB_DISP], y, h, wdw, scr_planes, &psize);
        s.fd_w = (short)(wdw * 16);
        s.fd_h = rows;
        s.fd_wdwidth = (short)wdw;
        s.fd_stand = 1;
        s.fd_nplanes = scr_planes;
        d = s;
        d.fd_addr = dev + y * rowbytes;
        d.fd_stand = 0;
        vr_trnfm(vdih, &s, &d);
    }
    job_release(r, RB_DISP);
    if (w->dispmem)
        free(w->dispmem);
    w->dispmem = dev;
    memset(&w->disp, 0, sizeof w->disp);
    w->disp.fd_addr = dev;
    w->disp.fd_w = (short)(wdw * 16);
    w->disp.fd_h = (short)h;
    w->disp.fd_wdwidth = (short)wdw;
    w->disp.fd_nplanes = scr_planes;
    return 1;
}

static void fit_window_to_image(VWIN *w)
{
    GRECT cur, wk, out;
    int k = kind_flags(w);
    wind_get_grect(w->handle, WF_CURRXYWH, &cur);
    wk.g_x = wk.g_y = 0;
    wk.g_w = (int)w->img.dw;
    wk.g_h = (int)w->img.dh;
    if (wk.g_w < 16 * cell_w) wk.g_w = 16 * cell_w;
    if (wk.g_h < 4 * cell_h) wk.g_h = 4 * cell_h;
    wind_calc_grect(WC_BORDER, k, &wk, &out);
    out.g_x = cur.g_x;
    out.g_y = cur.g_y;
    if (out.g_w > desk.g_w) out.g_w = desk.g_w;
    if (out.g_h > desk.g_h) out.g_h = desk.g_h;
    if (out.g_x + out.g_w > desk.g_x + desk.g_w) out.g_x = desk.g_x + desk.g_w - out.g_w;
    if (out.g_y + out.g_h > desk.g_y + desk.g_h) out.g_y = desk.g_y + desk.g_h - out.g_h;
    dbg("fit window %d to %d,%d %dx%d", w->handle, out.g_x, out.g_y, out.g_w, out.g_h);
    wind_set_grect(w->handle, WF_CURRXYWH, &out);
    get_work(w);
}

static void show_error(VWIN *w, JOBRES *r)
{
    w->state = ST_ERROR;
    sprintf(w->errmsg, "%.60s%s%.50s", fv_errstr(r->err),
            r->detail[0] ? ": " : "", r->detail);
}

void win_job_finished(VWIN *w, JOB *j)
{
    JOBRES *r = &j->res;
    int i, op = j->q.op;

    dbg("job finished for window %d: op %d err %d kind %d closing %d",
        w->handle, op, r->err, r->kind, w->closing);
    w->job = 0;
    progdlg_close(w);
    if (w->closing) {
        for (i = 0; i < RB_MAX; i++)
            job_release(r, i);
        free(j);
        win_destroy(w);
        return;
    }
    if (r->err) {
        for (i = 0; i < RB_MAX; i++)
            job_release(r, i);
        free(j);
        if (op == OP_RENDER) {
            w->state = ST_IDLE;
            if (w->want_rerender) {
                w->want_rerender = 0;
                start_render(w);
                return;
            }
            if (r->err != E_CANCEL)
                alertf("[1][fview: could not render|%.30s][ OK ]", base_name(w->path));
            /* the old rendering is still there */
            doc_size(w);
            win_set_sliders(w);
            win_redraw_all(w);
            return;
        }
        if (r->err == E_CANCEL) {         /* cancelled load: drop window */
            win_destroy(w);
            return;
        }
        show_error(w, r);
        win_update_info(w);
        win_redraw_all(w);
        menu_update();
        return;
    }

    if (r->kind == K_IMAGE) {
        if (op == OP_LOAD) {
            w->img = *r;                  /* takes ownership of blocks */
            w->img.im.px = r->ptr[RB_ORIG];
        } else {
            w->img.dw = r->dw;
            w->img.dh = r->dh;
            w->img.wdwidth = r->wdwidth;
            memcpy(w->img.pal, r->pal, sizeof r->pal);
            /* swap in the new display block */
            job_release(&w->img, RB_DISP);
            w->img.ptr[RB_DISP] = r->ptr[RB_DISP];
            w->img.size[RB_DISP] = r->size[RB_DISP];
            w->img.attached[RB_DISP] = r->attached[RB_DISP];
            strcpy(w->img.shm[RB_DISP], r->shm[RB_DISP]);
        }
        dbg("to_device %ldx%ld wdwidth %ld planes %d from %lx",
            w->img.dw, w->img.dh, w->img.wdwidth, scr_planes, (long)w->img.ptr[RB_DISP]);
        if (!to_device(w, &w->img)) {
            r->err = E_NOMEM;
            show_error(w, r);
            free(j);
            win_redraw_all(w);
            return;
        }
        memcpy(w->pal, w->img.pal, sizeof w->pal);
        w->haspal = !dinfo.truecolor && !dinfo.fixed_palette &&
                    prefs.palmode == PAL_OPTIMISE;
        dbg("display MFDB addr %lx w %d h %d wd %d planes %d", (long)w->disp.fd_addr,
            w->disp.fd_w, w->disp.fd_h, w->disp.fd_wdwidth, w->disp.fd_nplanes);
        w->state = ST_IDLE;
        if (op == OP_LOAD && w->zoom == ZOOM_REAL)
            fit_window_to_image(w);
        doc_size(w);
    } else {
        w->img = *r;
        w->blob = r->ptr[RB_DOC];
        w->state = ST_IDLE;
        if (r->kind == K_PDF) {
            w->npages = pdf_index(w->blob, r->doclen, &w->pageoff);
            w->page = 0;
            if (!w->npages) {
                r->err = E_FORMAT;
                show_error(w, r);
            }
        }
        win_relayout(w);
    }
    free(j);
    if (w->want_rerender) {
        w->want_rerender = 0;
        start_render(w);
        return;
    }
    win_update_info(w);
    win_set_sliders(w);
    pal_invalidate();
    pal_check_top();
    /* the window may just have been resized: let the AES finish with the
       frame and tell us what to draw, as well as drawing now           */
    win_redraw_all(w);
    win_post_redraw(w);
    dbg("window %d ready, work %d,%d %dx%d", w->handle, w->work.g_x, w->work.g_y,
        w->work.g_w, w->work.g_h);
    menu_update();
}

/* ---------------------------------------------------------------- pages */

void win_page(VWIN *w, long page)
{
    if (w->kind != K_PDF || !w->npages || win_busy(w))
        return;
    if (page < 0) page = 0;
    if (page >= w->npages) page = w->npages - 1;
    if (page == w->page)
        return;
    w->page = page;
    w->yoff = 0;
    w->prepst = 0;
    doc_size(w);
    win_set_sliders(w);
    win_update_info(w);
    win_redraw_all(w);
}

/* PDF zoom: fit width, or 50..400 % in steps; step -1 out, +1 in,
   0 fit width, 100 real size */
void win_pdf_zoom(VWIN *w, int step)
{
    static const int z[] = { 50, 75, 100, 125, 150, 200, 300, 400 };
    int n = (int)(sizeof z / sizeof z[0]), i, cur, nz;
    long sx, sy, cx, cy;
    if (w->kind != K_PDF || !w->npages || win_busy(w))
        return;
    /* current zoom in percent, also when fitting the width */
    pdf_scale(w, &sx, &sy);
    cur = (int)(sx * (pix_w_um > 0 ? pix_w_um : 278) * 72L / 254000L);
    if (step == 0)
        nz = 0;
    else if (step == 100)
        nz = 100;
    else if (step > 0) {
        for (i = 0; i < n && z[i] <= cur + 2; i++)
            ;
        nz = i < n ? z[i] : z[n - 1];
    } else {
        for (i = n - 1; i >= 0 && z[i] >= cur - 2; i--)
            ;
        nz = i >= 0 ? z[i] : z[0];
    }
    if (nz == w->pdfzoom)
        return;
    /* keep the centre of the view where it was */
    cx = w->docw ? (w->xoff + w->work.g_w / 2) * 1000L / w->docw : 0;
    cy = w->doch ? (w->yoff + w->work.g_h / 2) * 1000L / w->doch : 0;
    w->pdfzoom = nz;
    w->prepst = 0;
    doc_size(w);
    w->xoff = cx * w->docw / 1000L - w->work.g_w / 2;
    w->yoff = cy * w->doch / 1000L - w->work.g_h / 2;
    if (w->xoff > w->docw - w->work.g_w) w->xoff = w->docw - w->work.g_w;
    if (w->yoff > w->doch - w->work.g_h) w->yoff = w->doch - w->work.g_h;
    if (w->xoff < 0) w->xoff = 0;
    if (w->yoff < 0) w->yoff = 0;
    win_set_sliders(w);
    win_update_info(w);
    win_redraw_all(w);
}

/* --------------------------------------------------------------- clicks */

int win_click(VWIN *w, int mx, int my, int clicks)
{
    if (w->iconified == 2) {
        win_fake_uniconify(w);
        return 1;
    }
    if (w->kind == K_MAIN && !w->iconified) {
        int i, row = (my - w->work.g_y - 4 - cell_h - 4) / cell_h, n = 0;
        if (row < 0)
            return 0;
        for (i = 0; i < MAXWIN; i++) {
            VWIN *v = &wins[i];
            if (!v->used || v->kind == K_MAIN || v->handle <= 0)
                continue;
            if (n++ == row) {
                if (v->iconified && aes_iconify) {
                    GRECT r;
                    wind_get_grect(v->handle, WF_UNICONIFY, &r);
                    win_uniconify(v, &r);
                }
                wind_set(v->handle, WF_TOP, 0, 0, 0, 0);
                pal_check_top();
                return 1;
            }
        }
        return 0;
    }
    if (win_busy(w) && w->pdlg <= 0 && win_over_cancel(w, mx, my)) {
        do_cancel(w);
        return 1;
    }
    if (win_busy(w) && w->pdlg > 0) {       /* bring its dialog forward */
        wind_set(w->pdlg, WF_TOP, 0, 0, 0, 0);
        return 1;
    }
    if (clicks == 2 && w->kind == K_IMAGE && w->disp.fd_addr && !win_busy(w)) {
        win_set_zoom(w, w->zoom == ZOOM_FIT ? ZOOM_REAL : ZOOM_FIT);
        return 1;
    }
    return 0;
}

/* --------------------------------------------- preferences take effect */

void win_apply_prefs(void)
{
    int i;
    scrtd_ok = 0;
    for (i = 0; i < MAXWIN; i++) {
        VWIN *w = &wins[i];
        if (!w->used || w->handle <= 0 || win_busy(w))
            continue;
        if (w->kind == K_PDF) {
            render_cache_flush();            /* dither may have changed */
            w->prepst = 0;
        }
        if (w->kind == K_IMAGE && w->img.im.px)
            start_render(w);                 /* dither / palette changes */
        else if (w->kind == K_TEXT || w->kind == K_PDF) {
            win_relayout(w);
            win_update_info(w);
            win_redraw_all(w);
        }
    }
}

/* ---------------------------------------------------------------- saving */

static VWIN *op_win;

static int op_prog(PROG *p, long done, long total)
{
    static long last;
    long t = now_ms();
    (void)p;
    if (op_win->handle > 0)
        win_progress(op_win, done, total);
    if (t - last >= 60) {
        last = t;
        gui_coop_poll();
    }
    return op_win->closing || quitting;
}

/* Build PDF page images that the redraw left for later, with a progress
   dialog and Cancel. Called from the main loop only.                   */
void win_prepare_pending(void)
{
    int i;
    for (i = 0; i < MAXWIN; i++) {
        VWIN *w = &wins[i];
        PROG pg;
        long sx, sy;
        int ok;
        if (!w->used || !w->need_prep || w->handle <= 0)
            continue;
        if (w->kind != K_PDF || !w->npages || w->iconified || w->state == ST_ERROR) {
            w->need_prep = 0;
            continue;
        }
        if (win_busy(w) || coop_busy)
            continue;                         /* try again later */
        w->need_prep = 0;
        pdf_scale(w, &sx, &sy);
        if (!win_begin_op(w, ST_RENDER, "Rendering", &pg))
            continue;
        w->prepst = 3;                        /* building; a resize resets it */
        ok = pdf_build_page(vdih, w->blob, w->pageoff[w->page], sx, sy,
                            npens ? npens : 256, scr_planes == 1, &pg);
        if (w->prepst == 3)
            w->prepst = ok ? 1 : 2;           /* cancelled or no memory: boxes */
        dbg("window %d: page images built, ok %d", w->handle, ok);
        win_end_op(w);                        /* redraws */
        if (w->used)
            menu_update();
    }
}

/* at exit: every window goes, jobs have been stopped already */
void win_shutdown(void)
{
    int i;
    for (i = 0; i < MAXWIN; i++)
        if (wins[i].used) {
            if (wins[i].job) {
                wins[i].job->win = 0;
                free(wins[i].job);
                wins[i].job = 0;
            }
            win_destroy(&wins[i]);
        }
}

int win_begin_op(VWIN *w, int state, const char *verb, PROG *pg)
{
    if (win_busy(w) || coop_busy)
        return 0;
    w->state = state;
    w->closing = 0;
    begin_panel(w, verb);
    win_redraw_all(w);
    memset(pg, 0, sizeof *pg);
    pg->fn = op_prog;
    op_win = w;
    coop_busy = 1;
    return 1;
}

void win_end_op(VWIN *w)
{
    progdlg_close(w);
    coop_busy = 0;
    w->state = ST_IDLE;
    if (w->closing & 1) {
        win_destroy(w);
        return;
    }
    w->closing = 0;
    win_redraw_all(w);
}

static void tc_getrow(void *ctx, long y, u8 *rgb)
{
    VWIN *w = ctx;
    const u8 *s = (const u8 *)w->disp.fd_addr + y * w->disp.fd_wdwidth * 16L * dinfo.bytespp;
    long x;
    for (x = 0; x < w->img.dw; x++, s += dinfo.bytespp) {
        u32 p;
        int c, i;
        if (dinfo.bytespp == 2) p = dinfo.intel ? s[0] | (s[1] << 8) : (s[0] << 8) | s[1];
        else if (dinfo.bytespp == 3) p = ((u32)s[0] << 16) | (s[1] << 8) | s[2];
        else p = dinfo.intel ? s[0] | ((u32)s[1] << 8) | ((u32)s[2] << 16) | ((u32)s[3] << 24)
                             : ((u32)s[0] << 24) | ((u32)s[1] << 16) | (s[2] << 8) | s[3];
        for (c = 0; c < 3; c++) {
            int v = 0, nb = dinfo.nbits[c];
            for (i = 0; i < nb; i++)
                if (p & (1UL << dinfo.bitpos[c][i]))
                    v |= 0x80 >> i;
            if (nb && nb < 8)
                v |= v >> nb;               /* replicate into low bits */
            rgb[x * 3 + c] = (u8)v;
        }
    }
}

void win_save_img(VWIN *w, int dithered)
{
    char path[256], name[64], *d;
    PROG pg;
    int e;

    if (w->kind != K_IMAGE || !w->img.im.px || win_busy(w))
        return;
    strcpy(path, w->path);
    d = (char *)base_name(path);
    strcpy(name, d);
    *d = 0;
    strcat(path, "*.IMG");
    d = strrchr(name, '.');
    if (d) *d = 0;
    name[8] = 0;
    strcat(name, ".IMG");
    if (!file_select(path, name, dithered ? "Save dithered IMG" : "Save IMG (original depth)"))
        return;
    d = strrchr(path, '\\');
    if (d) d[1] = 0;
    strcat(path, name);
    if (Fattrib(path, 0, 0) >= 0 &&
        form_alert(2, "[2][The file already exists.|Replace it?][Replace|Cancel]") != 1)
        return;

    if (!win_begin_op(w, ST_SAVING, "Saving", &pg))
        return;
    if (!dithered)
        e = img_save(path, &w->img.im, &prefs, &pg);
    else if (dinfo.truecolor)
        e = img_save_rgbrows(path, w->img.dw, w->img.dh, tc_getrow, w, &pg);
    else if (dinfo.chunky) {
        /* bytes per pixel -> standard planes for the IMG writer */
        long wdw = w->disp.fd_wdwidth, h = w->disp.fd_h, y, psize;
        u16 *std = malloc(wdw * 2 * scr_planes * h);
        e = E_NOMEM;
        if (std) {
            for (y = 0; y < h; y++) {
                u16 *dst = std_row(std, y, h, wdw, scr_planes, &psize);
                chunky_to_planes((const u8 *)w->disp.fd_addr + y * wdw * 16L, wdw * 16,
                                 scr_planes, dst, psize, wdw);
            }
            e = img_save_planes(path, std, w->img.dw, h, wdw, scr_planes,
                                w->haspal ? w->pal : dinfo.sysrgb, &prefs, &pg);
            free(std);
        }
    } else {
        long wdw = w->disp.fd_wdwidth, h = w->disp.fd_h, y, psize;
        u16 *std = malloc(wdw * 2 * scr_planes * h);
        e = E_NOMEM;
        if (std) {
            for (y = 0; y < h; y += STD_BAND) {
                MFDB s, t;
                int rows = (int)(h - y > STD_BAND ? STD_BAND : h - y);
                s = w->disp;
                s.fd_addr = (u8 *)w->disp.fd_addr + y * wdw * 2 * scr_planes;
                s.fd_h = rows;
                s.fd_stand = 0;
                t = s;
                t.fd_addr = std_row(std, y, h, wdw, scr_planes, &psize);
                t.fd_stand = 1;
                vr_trnfm(vdih, &s, &t);
            }
            e = img_save_planes(path, std, w->img.dw, h, wdw, scr_planes,
                                w->haspal ? w->pal : dinfo.sysrgb, &prefs, &pg);
            free(std);
        }
    }
    win_end_op(w);
    if (e && e != E_CANCEL)
        alertf("[1][fview: could not save the file.|%.30s][ OK ]", fv_errstr(e));
}
