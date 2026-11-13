/*
 * picdlg.c - the Dithering window (non-modal, one picture window at a
 * time) and the small Reduce colours / Resize dialogs.
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

static void obj_add(OBJECT *t, int idx, int parent, int type, int flags,
                    int state, long spec, int x, int y, int w, int h)
{
    OBJECT *o = &t[idx];
    o->ob_next = o->ob_head = o->ob_tail = -1;
    o->ob_type = type;
    o->ob_flags = flags;
    o->ob_state = state;
    o->ob_spec.index = spec;
    o->ob_x = x; o->ob_y = y; o->ob_width = w; o->ob_height = h;
    if (parent >= 0) {
        OBJECT *p = &t[parent];
        if (p->ob_head < 0)
            p->ob_head = idx;
        else
            t[p->ob_tail].ob_next = idx;
        p->ob_tail = idx;
        o->ob_next = parent;
    }
}

/* ------------------------------------------------------ the Dithering window */

enum {
    D_ROOT = 0, D_TITLE,
    D_LDITH, D_FS, D_ATK, D_ORD, D_NONE, D_SERP, D_SMOOTH,
    D_LPAL, D_OWN, D_SYS, D_NOTE,
    D_LCOL, D_COUNTS,
    D_COUNT, D_REDUCE, D_RESIZE, D_APPLY, D_CLOSE,
    D_NOBJ
};

static OBJECT dt[D_NOBJ];
static int dwin;                  /* window handle, 0 = closed      */
static VWIN *dw;                  /* the picture window it is for   */
static char d_title[64], d_note[48], d_counts[64];

static void build(void)
{
    int r = 1;
    obj_add(dt, D_ROOT, -1, G_BOX, 0, OUTLINED, 0x00021100L, 0, 0, 52, 11);
    obj_add(dt, D_TITLE, 0, G_STRING, 0, 0, (long)d_title, 2, r, 48, 1);
    r += 2;
    obj_add(dt, D_LDITH, 0, G_STRING, 0, 0, (long)"Dither", 2, r, 8, 1);
    obj_add(dt, D_FS, 0, G_BUTTON, SELECTABLE | RBUTTON, 0, (long)"Floyd-S.", 11, r, 10, 1);
    obj_add(dt, D_ATK, 0, G_BUTTON, SELECTABLE | RBUTTON, 0, (long)"Atkinson", 22, r, 10, 1);
    obj_add(dt, D_ORD, 0, G_BUTTON, SELECTABLE | RBUTTON, 0, (long)"Ordered", 33, r, 9, 1);
    obj_add(dt, D_NONE, 0, G_BUTTON, SELECTABLE | RBUTTON, 0, (long)"None", 43, r, 7, 1);
    r += 1;
    obj_add(dt, D_SERP, 0, G_BUTTON, SELECTABLE, 0, (long)"Serpentine", 11, r, 12, 1);
    obj_add(dt, D_SMOOTH, 0, G_BUTTON, SELECTABLE, 0, (long)"Smooth shrink", 24, r, 15, 1);
    r += 2;
    obj_add(dt, D_LPAL, 0, G_STRING, 0, 0, (long)"Palette", 2, r, 8, 1);
    obj_add(dt, D_OWN, 0, G_BUTTON, SELECTABLE | RBUTTON, 0, (long)"Its own", 11, r, 9, 1);
    obj_add(dt, D_SYS, 0, G_BUTTON, SELECTABLE | RBUTTON, 0, (long)"System", 21, r, 8, 1);
    obj_add(dt, D_NOTE, 0, G_STRING, 0, 0, (long)d_note, 30, r, 21, 1);
    r += 2;
    obj_add(dt, D_LCOL, 0, G_STRING, 0, 0, (long)"Colours", 2, r, 8, 1);
    obj_add(dt, D_COUNTS, 0, G_STRING, 0, 0, (long)d_counts, 11, r, 40, 1);
    r += 2;
    obj_add(dt, D_COUNT, 0, G_BUTTON, SELECTABLE | EXIT, 0, (long)"Count", 2, r, 8, 1);
    obj_add(dt, D_REDUCE, 0, G_BUTTON, SELECTABLE | EXIT, 0, (long)"Reduce...", 11, r, 11, 1);
    obj_add(dt, D_RESIZE, 0, G_BUTTON, SELECTABLE | EXIT, 0, (long)"Resize...", 23, r, 11, 1);
    obj_add(dt, D_APPLY, 0, G_BUTTON, SELECTABLE | EXIT | DEFAULT, 0, (long)"Apply", 35, r, 8, 1);
    obj_add(dt, D_CLOSE, 0, G_BUTTON, SELECTABLE | EXIT | LASTOB, 0, (long)"Close", 44, r, 7, 1);
    dt[D_ROOT].ob_height = r + 2;
    tree_finish(dt, D_NOBJ);
}

static void sel(int obj, int on)
{
    if (on) dt[obj].ob_state |= SELECTED;
    else dt[obj].ob_state &= ~SELECTED;
}

static void fill(void)
{
    const PREFS *p = &dw->wpf;
    sprintf(d_title, "Picture: %.38s%s", win_base_name(dw->path),
            dw->copyno ? " (copy)" : "");
    sel(D_FS, p->dither == DITH_FS);
    sel(D_ATK, p->dither == DITH_ATKINSON);
    sel(D_ORD, p->dither == DITH_ORDERED);
    sel(D_NONE, p->dither == DITH_NONE);
    sel(D_SERP, p->serpentine);
    sel(D_SMOOTH, p->smooth);
    sel(D_OWN, p->palmode == PAL_OPTIMISE);
    sel(D_SYS, p->palmode == PAL_SYSTEM);
    strcpy(d_note, share_pal ? "(sharing one palette)" :
                   dinfo.truecolor ? "(true colour screen)" : "");
    if (dw->cnt_real < 0)
        strcpy(d_counts, "not counted (Count)");
    else if (dw->cnt_seen >= 0)
        sprintf(d_counts, "%ld real, about %ld seen", dw->cnt_real, dw->cnt_seen);
    else
        sprintf(d_counts, "%ld real", dw->cnt_real);
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
    if (!dt[D_ROOT].ob_width)
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
    wind_set_str(dwin, WF_NAME, " Dithering ");
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

/* ----------------------------------------------------------- modal dialogs */

/* a dialog with up to two number fields and up to two switches, OK and
   Cancel, drawn and run by the AES (form_do). Returns 1 for OK.       */
typedef struct {
    const char *title;
    const char *label[2];
    long  *val[2];
    long   lo[2], hi[2];
    const char *sw[2];
    int   *swval[2];
} NUMDLG;

static int run_dialog(NUMDLG *nd)
{
    OBJECT t[12];
    TEDINFO te[2];
    char text[2][8], tmpl[2][8], valid[2][8];
    int n = 1, r = 1, i, f[2] = { -1, -1 }, s[2] = { -1, -1 }, ok, cancel, b;
    short x, y, w, h;
    memset(t, 0, sizeof t);
    memset(te, 0, sizeof te);
    obj_add(t, 0, -1, G_BOX, 0, OUTLINED, 0x00021100L, 0, 0, 36, 8);
    obj_add(t, n++, 0, G_STRING, 0, 0, (long)nd->title, 2, r, 32, 1);
    r += 2;
    for (i = 0; i < 2; i++) {
        if (!nd->label[i])
            continue;
        sprintf(text[i], "%ld", *nd->val[i]);
        strcpy(tmpl[i], "_____");
        strcpy(valid[i], "99999");
        te[i].te_ptext = text[i];
        te[i].te_ptmplt = tmpl[i];
        te[i].te_pvalid = valid[i];
        te[i].te_font = 3;
        te[i].te_just = 0;
        te[i].te_color = 0x1180;
        te[i].te_thickness = -1;
        te[i].te_txtlen = 6;
        te[i].te_tmplen = 6;
        obj_add(t, n++, 0, G_STRING, 0, 0, (long)nd->label[i], 2, r, 14, 1);
        f[i] = n;
        obj_add(t, n++, 0, G_FBOXTEXT, EDITABLE, 0, (long)&te[i], 17, r, 7, 1);
        r += 2;
    }
    for (i = 0; i < 2; i++) {
        if (!nd->sw[i])
            continue;
        s[i] = n;
        obj_add(t, n++, 0, G_BUTTON, SELECTABLE, *nd->swval[i] ? SELECTED : 0,
                (long)nd->sw[i], 2, r, (int)strlen(nd->sw[i]) + 2, 1);
        r += 1;
    }
    r += 1;
    ok = n;
    obj_add(t, n++, 0, G_BUTTON, SELECTABLE | EXIT | DEFAULT, 0, (long)"OK", 15, r, 8, 1);
    cancel = n;
    obj_add(t, n++, 0, G_BUTTON, SELECTABLE | EXIT | LASTOB, 0, (long)"Cancel", 25, r, 9, 1);
    t[0].ob_height = r + 2;
    tree_finish(t, n);
    form_center(t, &x, &y, &w, &h);
    wind_update(BEG_UPDATE);
    form_dial(FMD_START, 0, 0, 0, 0, x, y, w, h);
    objc_draw(t, 0, 8, x, y, w, h);
    b = form_do(t, f[0] > 0 ? f[0] : 0) & 0x7FFF;
    form_dial(FMD_FINISH, 0, 0, 0, 0, x, y, w, h);
    wind_update(END_UPDATE);
    if (b != ok)
        return 0;
    for (i = 0; i < 2; i++) {
        if (f[i] > 0) {
            long v = atol(text[i]);
            if (v < nd->lo[i] || v > nd->hi[i]) {
                alertf("[1][%s must be from|%ld to %ld.][ OK ]", nd->label[i],
                       nd->lo[i], nd->hi[i]);
                return 0;
            }
            *nd->val[i] = v;
        }
        if (s[i] > 0)
            *nd->swval[i] = (t[s[i]].ob_state & SELECTED) != 0;
    }
    (void)cancel;
    return 1;
}

int modal_number(const char *title, const char *label, long *v, long lo, long hi)
{
    NUMDLG nd;
    memset(&nd, 0, sizeof nd);
    nd.title = title;
    nd.label[0] = label;
    nd.val[0] = v;
    nd.lo[0] = lo;
    nd.hi[0] = hi;
    return run_dialog(&nd);
}

void reduce_dialog(VWIN *w)
{
    static long n = 16;
    char title[48];
    if (!win_tool_ok(w))
        return;
    if (w->img.im.type == IT_INDEXED && n >= w->img.im.ncolors)
        n = w->img.im.ncolors > 2 ? w->img.im.ncolors / 2 : 2;
    sprintf(title, "Reduce the colours of %.12s", win_base_name(w->path));
    if (!modal_number(title, "Colours (2-256)", &n, 2, 256))
        return;
    win_reduce(w, (int)n, 0, 0);
}

void resize_dialog(VWIN *w)
{
    static int keep = 1, smooth = 1;
    long nw, nh, ow, oh;
    char title[48];
    NUMDLG nd;
    if (!win_tool_ok(w))
        return;
    ow = nw = w->img.im.w;
    oh = nh = w->img.im.h;
    sprintf(title, "Resize %.20s (%ldx%ld)", win_base_name(w->path), ow, oh);
    memset(&nd, 0, sizeof nd);
    nd.title = title;
    nd.label[0] = "Width (pixels)";
    nd.val[0] = &nw;
    nd.lo[0] = 1;
    nd.hi[0] = 16000;
    nd.label[1] = "Height (pixels)";
    nd.val[1] = &nh;
    nd.lo[1] = 1;
    nd.hi[1] = 16000;
    nd.sw[0] = "Keep the proportions";
    nd.swval[0] = &keep;
    nd.sw[1] = w->img.im.type == IT_RGB ? "Smooth (average / blend)" : 0;
    nd.swval[1] = &smooth;
    if (!run_dialog(&nd))
        return;
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
        return;
    if ((double)nw * nh * (w->img.im.type == IT_RGB ? 3 : 1) > 8000000.0 &&
        form_alert(1, "[2][That picture needs a lot|of memory. Resize anyway?][Resize|Cancel]") != 1)
        return;
    win_resize(w, nw, nh, w->img.im.type == IT_RGB && smooth);
}
