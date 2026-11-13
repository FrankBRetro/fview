/*
 * picdlg.c - the Dithering window (non-modal, one picture window at a
 * time) and the Reduce colours / Resize dialogs (non-modal too: windows
 * of their own, see wdlg.c, one per picture if you like).
 *
 * The Dithering window shows the settings of the picture window it was
 * opened for; Apply renders that window again from its copy of the
 * picture, so each window can try its own dithering and palette. The
 * work itself runs as a job, with the window's progress bar and Cancel.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gemui.h"
#include "fvrsc.h"

/* ------------------------------------------------------ the Dithering window */

static OBJECT *dt;                 /* T_DITHER of the resource file  */
static int dwin;                  /* window handle, 0 = closed      */
static VWIN *dw;                  /* the picture window it is for   */
static char d_title[64], d_note[48], d_counts[64];

static void build(void)
{
    dt = rs_dialog(T_DITHER);
    dt[D_TITLE].ob_spec.free_string = d_title;
    dt[D_NOTE].ob_spec.free_string = d_note;
    dt[D_COUNTS].ob_spec.free_string = d_counts;
}

static void sel(int obj, int on)
{
    if (on) dt[obj].ob_state |= SELECTED;
    else dt[obj].ob_state &= ~SELECTED;
}

static void fill(void)
{
    const PREFS *p = &dw->wpf;
    rsf(d_title, sizeof d_title, S_D_PICTURE, win_base_name(dw->path),
        dw->copyno ? rs(S_D_COPY) : "");
    sel(D_FS, p->dither == DITH_FS);
    sel(D_ATK, p->dither == DITH_ATKINSON);
    sel(D_ORD, p->dither == DITH_ORDERED);
    sel(D_NONE, p->dither == DITH_NONE);
    sel(D_SERP, p->serpentine);
    sel(D_SMOOTH, p->smooth);
    sel(D_OWN, p->palmode == PAL_OPTIMISE);
    sel(D_SYS, p->palmode == PAL_SYSTEM);
    snprintf(d_note, sizeof d_note, "%s", share_pal ? rs(S_D_SHARING) :
             dinfo.truecolor ? rs(S_D_TRUECOL) : "");
    if (dw->cnt_real < 0)
        snprintf(d_counts, sizeof d_counts, "%s", rs(S_D_NOTCOUNT));
    else if (dw->cnt_seen >= 0)
        rsf(d_counts, sizeof d_counts, S_D_REALSEEN, dw->cnt_real, dw->cnt_seen);
    else
        rsf(d_counts, sizeof d_counts, S_D_REAL, dw->cnt_real);
}

static void place(void)
{
    GRECT wk;
    wind_get_grect(dwin, WF_WORKXYWH, &wk);
    dt[D_ROOT].ob_x = wk.g_x + 3;
    dt[D_ROOT].ob_y = wk.g_y + 3;
}

void picdlg_redraw(GRECT *area)
{
    GRECT r;
    short pxy[4];
    if (dwin <= 0)
        return;
    place();
    wind_update(BEG_UPDATE);
    graf_mouse(M_OFF, 0);
    wind_get_grect(dwin, WF_FIRSTXYWH, &r);
    while (r.g_w && r.g_h) {
        if (rc_intersect(area, &r) && rc_intersect(&desk, &r)) {
            vsf_interior(vdih, FIS_SOLID);
            vsf_color(vdih, 0);
            vswr_mode(vdih, MD_REPLACE);
            pxy[0] = r.g_x; pxy[1] = r.g_y;
            pxy[2] = r.g_x + r.g_w - 1; pxy[3] = r.g_y + r.g_h - 1;
            vs_clip(vdih, 1, pxy);
            vr_recfl(vdih, pxy);
            vs_clip(vdih, 0, pxy);
            objc_draw_grect(dt, 0, 8, &r);
        }
        wind_get_grect(dwin, WF_NEXTXYWH, &r);
    }
    graf_mouse(M_ON, 0);
    wind_update(END_UPDATE);
}

static void redraw_obj(int obj)
{
    short x, y;
    GRECT r;
    place();
    objc_offset(dt, obj, &x, &y);
    r.g_x = x - 3; r.g_y = y - 3;
    r.g_w = dt[obj].ob_width + 6;
    r.g_h = dt[obj].ob_height + 6;
    picdlg_redraw(&r);
}

static void redraw_all(void)
{
    GRECT wk;
    if (dwin <= 0)
        return;
    wind_get_grect(dwin, WF_WORKXYWH, &wk);
    picdlg_redraw(&wk);
}

void picdlg_open(VWIN *w)
{
    GRECT wk, r;
    int kind = NAME | CLOSER | MOVER;
    if (!w || w->kind != K_IMAGE)
        return;
    if (!dt)
        build();
    dw = w;
    fill();
    if (dwin > 0) {                       /* already open: now for w */
        wind_set(dwin, WF_TOP, 0, 0, 0, 0);
        redraw_all();
        return;
    }
    wk.g_x = wk.g_y = 0;
    wk.g_w = dt[D_ROOT].ob_width + 6;
    wk.g_h = dt[D_ROOT].ob_height + 6;
    wind_calc_grect(WC_BORDER, kind, &wk, &r);
    r.g_x = desk.g_x + desk.g_w - r.g_w - 8;
    r.g_y = desk.g_y + desk.g_h - r.g_h - 8;
    if (r.g_x < desk.g_x) r.g_x = desk.g_x;
    if (r.g_y < desk.g_y) r.g_y = desk.g_y;
    dwin = wind_create_grect(kind, &r);
    if (dwin <= 0) {
        dwin = 0;
        return;
    }
    wind_set_str(dwin, WF_NAME, (char *)rs(S_W_DITHER));
    wind_open_grect(dwin, &r);
    place();
}

void picdlg_close(void)
{
    if (dwin > 0) {
        wind_close(dwin);
        wind_delete(dwin);
    }
    dwin = 0;
    dw = 0;
}

int  picdlg_handle(int h) { return dwin > 0 && h == dwin; }
void picdlg_top(void) { wind_set(dwin, WF_TOP, 0, 0, 0, 0); }

void picdlg_moved(GRECT *r)
{
    wind_set_grect(dwin, WF_CURRXYWH, r);
    place();
}

void picdlg_refresh(VWIN *w)
{
    if (dwin > 0 && w == dw) {
        fill();
        redraw_all();
    }
}

void picdlg_forget(VWIN *w)
{
    if (dwin > 0 && w == dw)
        picdlg_close();
}

static void press(int obj)
{
    sel(obj, 1);
    redraw_obj(obj);
    evnt_timer(80L);
    sel(obj, 0);
    redraw_obj(obj);
}

static void radio(int obj, int first, int last)
{
    int i;
    for (i = first; i <= last; i++)
        if (i != obj && (dt[i].ob_state & SELECTED)) {
            sel(i, 0);
            redraw_obj(i);
        }
    sel(obj, 1);
    redraw_obj(obj);
}

void picdlg_click(int mx, int my)
{
    int obj;
    if (dwin <= 0 || !dw)
        return;
    place();
    obj = objc_find(dt, 0, 8, mx, my);
    switch (obj) {
    case D_FS: case D_ATK: case D_ORD: case D_NONE:
        radio(obj, D_FS, D_NONE);
        break;
    case D_OWN: case D_SYS:
        radio(obj, D_OWN, D_SYS);
        break;
    case D_SERP: case D_SMOOTH:
        dt[obj].ob_state ^= SELECTED;
        redraw_obj(obj);
        break;
    case D_COUNT:
        press(obj);
        win_count(dw);
        break;
    case D_REDUCE:
        press(obj);
        reduce_dialog(dw);
        break;
    case D_RESIZE:
        press(obj);
        resize_dialog(dw);
        break;
    case D_APPLY: {
        PREFS *p = &dw->wpf;
        press(obj);
        p->dither = dt[D_FS].ob_state & SELECTED ? DITH_FS :
                    dt[D_ATK].ob_state & SELECTED ? DITH_ATKINSON :
                    dt[D_ORD].ob_state & SELECTED ? DITH_ORDERED : DITH_NONE;
        p->serpentine = (dt[D_SERP].ob_state & SELECTED) != 0;
        p->smooth = (dt[D_SMOOTH].ob_state & SELECTED) != 0;
        p->palmode = dt[D_SYS].ob_state & SELECTED ? PAL_SYSTEM : PAL_OPTIMISE;
        dw->wpf_own = 1;
        win_rerender(dw);
        fill();
        redraw_all();
        break;
    }
    case D_CLOSE:
        press(obj);
        picdlg_close();
        break;
    }
}

/* ------------------------------------------- Reduce colours and Resize */

/* windows of their own (wdlg.c): nothing waits for them, and the picture
   they are for may close meanwhile (wd_forget closes them with it)    */

static long reduce_n = 16;

static int reduce_exit(WDIALOG *d, int obj)
{
    VWIN *w = wd_ctx(d);
    long n;
    if (obj != R_OK)
        return 1;
    n = atol(wd_field(d, R_FIELD));
    if (n < 2 || n > 256) {
        alert_rs(S_AL_RANGE, rs(S_N_COLOURS), 2L, 256L);
        return 0;                         /* stays open to correct it */
    }
    reduce_n = n;
    if (win_tool_ok(w))
        win_reduce(w, (int)n, 0, 0);
    return 1;
}

void reduce_dialog(VWIN *w)
{
    WDIALOG *d;
    char buf[12];
    if (!win_tool_ok(w))
        return;
    if (w->img.im.type == IT_INDEXED && reduce_n >= w->img.im.ncolors)
        reduce_n = w->img.im.ncolors > 2 ? w->img.im.ncolors / 2 : 2;
    d = wd_open(T_REDUCE, rs(S_W_REDUCE), R_FIELD, reduce_exit, w, w);
    if (!d) {
        alert_rs(S_AL_NOWIN);
        return;
    }
    rsf(wd_text(d, R_TITLE), 95, S_N_REDUCE, win_base_name(w->path));
    sprintf(buf, "%ld", reduce_n);
    wd_set_field(d, R_FIELD, buf);
}

static int keep = 1, smooth = 1;
/* a resize waiting for "needs a lot of memory": one per question */
typedef struct { VWIN *w; long nw, nh; int smooth; } BIGQ;

static void big_answer(int b, void *ctx)
{
    BIGQ *q = ctx;
    if (b == 1 && win_tool_ok(q->w))
        win_resize(q->w, q->nw, q->nh, q->smooth);
    free(q);
}

static int resize_exit(WDIALOG *d, int obj)
{
    VWIN *w = wd_ctx(d);
    OBJECT *t = wd_tree(d);
    long nw, nh, ow, oh;
    int sm;
    if (obj != Z_OK || !win_tool_ok(w))
        return 1;
    ow = w->img.im.w;
    oh = w->img.im.h;
    nw = atol(wd_field(d, Z_FW));
    nh = atol(wd_field(d, Z_FH));
    if (nw < 1 || nw > 16000) {
        alert_rs(S_AL_RANGE, rs(S_N_WIDTH), 1L, 16000L);
        return 0;
    }
    if (nh < 1 || nh > 16000) {
        alert_rs(S_AL_RANGE, rs(S_N_HEIGHT), 1L, 16000L);
        return 0;
    }
    keep = (t[Z_KEEP].ob_state & SELECTED) != 0;
    if (!(t[Z_SMOOTH].ob_flags & HIDETREE))
        smooth = (t[Z_SMOOTH].ob_state & SELECTED) != 0;
    if (keep) {
        /* the side that was changed decides; both changed: as typed */
        if (nw != ow && nh == oh)
            nh = (oh * nw + ow / 2) / ow;
        else if (nh != oh && nw == ow)
            nw = (ow * nh + oh / 2) / oh;
        if (nw < 1) nw = 1;
        if (nh < 1) nh = 1;
    }
    if (nw == ow && nh == oh)
        return 1;
    sm = w->img.im.type == IT_RGB && smooth;
    if ((double)nw * nh * (w->img.im.type == IT_RGB ? 3 : 1) > 8000000.0) {
        BIGQ *q = calloc(1, sizeof *q);
        if (!q)
            return 1;
        q->w = w;
        q->nw = nw;
        q->nh = nh;
        q->smooth = sm;
        alert_ask(1, big_answer, q, w, S_AL_BIGMEM);
        return 1;
    }
    win_resize(w, nw, nh, sm);
    return 1;
}

void resize_dialog(VWIN *w)
{
    WDIALOG *d;
    OBJECT *t;
    char buf[12];
    if (!win_tool_ok(w))
        return;
    d = wd_open(T_RESIZE, rs(S_W_RESIZE), Z_FW, resize_exit, w, w);
    if (!d) {
        alert_rs(S_AL_NOWIN);
        return;
    }
    t = wd_tree(d);
    rsf(wd_text(d, Z_TITLE), 95, S_N_RESIZE, win_base_name(w->path),
        w->img.im.w, w->img.im.h);
    sprintf(buf, "%ld", w->img.im.w);
    wd_set_field(d, Z_FW, buf);
    sprintf(buf, "%ld", w->img.im.h);
    wd_set_field(d, Z_FH, buf);
    if (keep) t[Z_KEEP].ob_state |= SELECTED;
    if (smooth) t[Z_SMOOTH].ob_state |= SELECTED;
    if (w->img.im.type != IT_RGB)           /* only true colour blends */
        t[Z_SMOOTH].ob_flags |= HIDETREE;
}
