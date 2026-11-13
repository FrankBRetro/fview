/*
 * wdlg.c - the resource file, and dialogs and alerts in windows of their
 * own, shared by FView and FConv.
 *
 * Nothing here waits for the user: a dialog or an alert is a window like
 * any other, the event loop hands its events here (wd_message,
 * wd_button, wd_key) and the answer comes back through a callback.
 * So pictures keep loading, printing goes on and windows can be moved
 * while a question is open - the programs' own multitasking keeps going.
 * Only when the AES has no window left does an alert fall back to
 * form_alert.
 *
 * Editable fields use objc_edit, and only while the dialog is the top
 * window (the cursor is drawn by XOR, which must not land on a window
 * that covers the field).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <osbind.h>
#include "gemui.h"
#include "wdlg.h"
#include "job.h"

#ifndef WHITEBAK
#define WHITEBAK 0x0040
#endif
#ifndef FL3DBAK
#define FL3DIND 0x0200
#define FL3DBAK 0x0400
#endif
#define FL3DACT 0x0600

/* the ALERT tree of both resource files (rsc/mkrsc.py alert_tree) */
enum { A_ROOT = 0, A_NOTE, A_QUEST, A_STOP, A_L1, A_B1 = A_L1 + 5, A_NOBJ = A_B1 + 3 };

/* ------------------------------------------------------ resource file */

static int rsc_loaded;

/* NAME.RSC, or name.rsc on a file system that minds the case */
static int try_load(const char *dir, const char *base, const char *suffix)
{
    char path[300], *p;
    sprintf(path, "%s%s%s.RSC", dir, base, suffix);
    if (Fattrib(path, 0, 0) < 0) {
        for (p = path + strlen(dir); *p; p++)
            if (*p >= 'A' && *p <= 'Z')
                *p += 'a' - 'A';
        if (Fattrib(path, 0, 0) < 0)
            return 0;
    }
    return rsrc_load(path) != 0;
}

int rsc_open(const char *base, int lang)
{
    static const char *suffix[LANG_N] = { "", "DE", "FR", "IT", "ES", "CZ" };
    char dir[256], path[300];
    job_app_dir(dir);
    if (lang > 0 && lang < LANG_N && try_load(dir, base, suffix[lang])) {
        rsc_loaded = 1;
        return 1;
    }
    if (try_load(dir, base, "")) {
        rsc_loaded = 1;
        return 1;
    }
    /* the AES's own search (current folder, PATH) as a last resort */
    sprintf(path, "%s.RSC", base);
    if (rsrc_load(path)) {
        rsc_loaded = 1;
        return 1;
    }
    return 0;
}

OBJECT *rs_tree(int tree)
{
    OBJECT *t = 0;
    rsrc_gaddr(R_TREE, tree, &t);
    return t;
}

const char *rs(int id)
{
    char **pp = 0;
    if (!rsc_loaded || !rsrc_gaddr(R_FRSTR, id, &pp) || !pp || !*pp)
        return "";
    return *pp;
}

int rsf(char *buf, int size, int id, ...)
{
    va_list ap;
    int n;
    va_start(ap, id);
    n = vsnprintf(buf, size, rs(id), ap);
    va_end(ap);
    return n;
}

/* the object's parent: the sibling chain ends at it */
static int obj_parent(const OBJECT *t, int obj)
{
    int i = obj;
    while (i > 0) {
        int nx = t[i].ob_next;
        if (nx < 0)
            return 0;
        if (t[nx].ob_tail == i)
            return nx;
        i = nx;
    }
    return 0;
}

static int tree_count(const OBJECT *t)
{
    int n = 0;
    while (!(t[n].ob_flags & LASTOB))
        n++;
    return n + 1;
}

/* The trees come from rsrc_load in pixels already. Then, the same way on
 * every AES:
 * - on/off and radio buttons (SELECTABLE, not EXIT) become MagiC-style
 *   check boxes and round radio buttons where the AES draws them
 *   (appl_getinfo 13: MagiC 3 and later, XaAES): MagiC puts the text 3
 *   characters in, so they get a character wider and the rest of the row
 *   moves along by one and a half (mkrsc.py allows for this);
 * - other buttons get a little air above and below;
 * - rows get up to 2 more pixels each on a 16-pixel font, so neighbouring
 *   buttons' borders don't touch, as long as the dialog still fits.    */
void tree_finish(OBJECT *t)
{
    int n = tree_count(t), i, k, rows, extra, right = 0;
    rows = cell_h ? t[0].ob_height / cell_h : 1;
    extra = cell_h >= 16 ? 2 : 0;
    while (extra && t[0].ob_height + rows * extra + 48 > desk.g_h)
        extra--;
    for (i = 1; i < n; i++) {
        OBJECT *o = &t[i];
        if (o->ob_type == G_BUTTON && aes_xbutton && (o->ob_flags & SELECTABLE) &&
            !(o->ob_flags & (EXIT | TOUCHEXIT | DEFAULT))) {
            o->ob_state = (o->ob_state & 0x00FF) | WHITEBAK | 0xFF00;
            for (k = 1; k < n; k++)
                if (t[k].ob_y / cell_h == o->ob_y / cell_h && t[k].ob_x > o->ob_x)
                    t[k].ob_x += cell_w + cell_w / 2;
            o->ob_width += cell_w;
        } else if (o->ob_type == G_BUTTON && o->ob_height > 4) {
            o->ob_y += 2;
            o->ob_height -= 2;
        }
    }
    for (i = 1; i < n; i++) {
        OBJECT *o = &t[i];
        o->ob_y += (o->ob_y / cell_h) * extra;
        if (o->ob_x + o->ob_width > right)
            right = o->ob_x + o->ob_width;
    }
    t[0].ob_height += rows * extra;
    if (t[0].ob_width < right + cell_w)
        t[0].ob_width = right + cell_w;
}

/* trees used as dialogs are finished once */
static unsigned char finished[32];

OBJECT *rs_dialog(int tree)
{
    OBJECT *t = rs_tree(tree);
    if (t && tree >= 0 && tree < 32 && !finished[tree]) {
        tree_finish(t);
        finished[tree] = 1;
    }
    return t;
}

void tree_place(OBJECT *t, int handle)
{
    GRECT wk;
    wind_get_grect(handle, WF_WORKXYWH, &wk);
    t[0].ob_x = wk.g_x + 3;
    t[0].ob_y = wk.g_y + 3;
}

static int is_top(int handle)
{
    short top = 0, d;
    wind_get(0, WF_TOP, &top, &d, &d, &d);
    return top == handle;
}

/* --------------------------------------------- non-modal dialog windows */

#define NSBUF 8

struct WDIALOG {
    WDIALOG     *next;
    int       handle;
    OBJECT   *t;
    int       n;
    TEDINFO  *ted;
    int       nted;
    char      sbuf[NSBUF][96];
    int       sobj[NSBUF];
    int       edit;            /* field with the cursor, 0 = none     */
    short     eidx;
    int       cursor;          /* the XOR cursor is on the screen     */
    WD_EXIT fn;
    void     *owner, *ctx;
    char      title[64];
    int       incb, dead;      /* in its callback; closed meanwhile   */
    /* alerts */
    ALERT_FN  afn;
    int       nbut;
};

static WDIALOG *dlgs;
static int ndlg;

static WDIALOG *find(int handle)
{
    WDIALOG *d;
    for (d = dlgs; d; d = d->next)
        if (d->handle == handle && !d->dead)
            return d;
    return 0;
}

int wd_find(int handle) { return handle > 0 && find(handle) != 0; }

static void cur_off(WDIALOG *d)
{
    if (d->cursor) {
        objc_edit(d->t, d->edit, 0, &d->eidx, ED_END);
        d->cursor = 0;
    }
}

static void cur_on(WDIALOG *d)
{
    if (d->edit > 0 && !d->cursor) {
        objc_edit(d->t, d->edit, 0, &d->eidx, ED_INIT);
        d->cursor = 1;
    }
}

/* draw the tree through the window's rectangle list, inside area */
static void draw(WDIALOG *d, const GRECT *area)
{
    GRECT r, a, wk;
    int top = is_top(d->handle);
    wind_get_grect(d->handle, WF_WORKXYWH, &wk);
    a = area ? *area : wk;
    tree_place(d->t, d->handle);
    wind_update(BEG_UPDATE);
    graf_mouse(M_OFF, 0);
    if (d->cursor) {
        if (top)
            cur_off(d);
        else {
            d->cursor = 0;          /* can't XOR it away: redraw it all */
            a = wk;
        }
    }
    wind_get_grect(d->handle, WF_FIRSTXYWH, &r);
    while (r.g_w && r.g_h) {
        if (rc_intersect(&a, &r) && rc_intersect(&desk, &r))
            objc_draw_grect(d->t, 0, 8, &r);
        wind_get_grect(d->handle, WF_NEXTXYWH, &r);
    }
    if (top)
        cur_on(d);
    graf_mouse(M_ON, 0);
    wind_update(END_UPDATE);
}

void wd_redraw_obj(WDIALOG *d, int obj)
{
    GRECT r;
    short x, y;
    if (!d || d->handle <= 0)
        return;
    tree_place(d->t, d->handle);
    objc_offset(d->t, obj, &x, &y);
    r.g_x = x - 3;
    r.g_y = y - 3;
    r.g_w = d->t[obj].ob_width + 6;
    r.g_h = d->t[obj].ob_height + 6;
    draw(d, &r);
}

/* the tree copied, no window yet */
static WDIALOG *wd_create(int tree, const char *title, int first_edit, WD_EXIT fn,
                         void *owner, void *ctx)
{
    OBJECT *src = rs_dialog(tree);
    WDIALOG *d;
    int n, i, k;
    if (!src)
        return 0;
    n = tree_count(src);
    for (i = k = 0; i < n; i++)
        if (src[i].ob_type == G_FTEXT || src[i].ob_type == G_FBOXTEXT ||
            src[i].ob_type == G_TEXT || src[i].ob_type == G_BOXTEXT)
            k++;
    d = calloc(1, sizeof(WDIALOG) + n * sizeof(OBJECT) + k * (sizeof(TEDINFO) + 16));
    if (!d)
        return 0;
    d->t = (OBJECT *)(d + 1);
    d->ted = (TEDINFO *)(d->t + n);
    memcpy(d->t, src, n * sizeof(OBJECT));
    d->n = n;
    /* own copies of the text objects, so two of a kind can be open */
    {
        char *buf = (char *)(d->ted + k);
        for (i = k = 0; i < n; i++) {
            OBJECT *o = &d->t[i];
            if (o->ob_type == G_FTEXT || o->ob_type == G_FBOXTEXT ||
                o->ob_type == G_TEXT || o->ob_type == G_BOXTEXT) {
                d->ted[k] = *src[i].ob_spec.tedinfo;
                strncpy(buf, d->ted[k].te_ptext, 15);
                d->ted[k].te_ptext = buf;
                if (d->ted[k].te_txtlen > 16)
                    d->ted[k].te_txtlen = 16;
                o->ob_spec.tedinfo = &d->ted[k];
                buf += 16;
                k++;
            }
        }
        d->nted = k;
    }
    for (i = 0; i < NSBUF; i++)
        d->sobj[i] = -1;
    d->edit = first_edit;
    d->fn = fn;
    d->owner = owner;
    d->ctx = ctx;
    snprintf(d->title, sizeof d->title, "%s", title ? title : "");
    d->next = dlgs;
    dlgs = d;
    ndlg++;
    return d;
}

/* a window the size of the tree, centred (a little lower for each one
   already open); 0 if the AES has none left (d is then freed)        */
static int wd_show(WDIALOG *d)
{
    GRECT wk, r;
    wk.g_x = wk.g_y = 0;
    wk.g_w = d->t[0].ob_width + 6;
    wk.g_h = d->t[0].ob_height + 6;
    wind_calc_grect(WC_BORDER, NAME | MOVER | CLOSER, &wk, &r);
    r.g_x = desk.g_x + (desk.g_w - r.g_w) / 2 + (ndlg - 1) * cell_w * 2;
    r.g_y = desk.g_y + (desk.g_h - r.g_h) / 3 + (ndlg - 1) * cell_h;
    if (r.g_x + r.g_w > desk.g_x + desk.g_w) r.g_x = desk.g_x + desk.g_w - r.g_w;
    if (r.g_y + r.g_h > desk.g_y + desk.g_h) r.g_y = desk.g_y + desk.g_h - r.g_h;
    if (r.g_x < desk.g_x) r.g_x = desk.g_x;
    if (r.g_y < desk.g_y) r.g_y = desk.g_y;
    d->handle = wind_create_grect(NAME | MOVER | CLOSER, &r);
    if (d->handle <= 0) {
        d->handle = 0;
        wd_close(d);
        return 0;
    }
    wind_set_str(d->handle, WF_NAME, d->title);
    wind_open_grect(d->handle, &r);
    return 1;
}

WDIALOG *wd_open(int tree, const char *title, int first_edit, WD_EXIT fn,
                void *owner, void *ctx)
{
    WDIALOG *d = wd_create(tree, title, first_edit, fn, owner, ctx);
    return d && wd_show(d) ? d : 0;
}

OBJECT *wd_tree(WDIALOG *d) { return d->t; }
void *wd_ctx(WDIALOG *d) { return d->ctx; }

char *wd_text(WDIALOG *d, int obj)
{
    int i;
    for (i = 0; i < NSBUF; i++)
        if (d->sobj[i] == obj)
            return d->sbuf[i];
    for (i = 0; i < NSBUF; i++)
        if (d->sobj[i] < 0) {
            d->sobj[i] = obj;
            d->sbuf[i][0] = 0;
            d->t[obj].ob_spec.free_string = d->sbuf[i];
            return d->sbuf[i];
        }
    return d->sbuf[NSBUF - 1];
}

void wd_set_field(WDIALOG *d, int obj, const char *s)
{
    TEDINFO *te = d->t[obj].ob_spec.tedinfo;
    int max = te->te_txtlen - 1;
    strncpy(te->te_ptext, s, max);
    te->te_ptext[max] = 0;
}

const char *wd_field(WDIALOG *d, int obj)
{
    return d->t[obj].ob_spec.tedinfo->te_ptext;
}

static void unlink_free(WDIALOG *d)
{
    WDIALOG **pp;
    for (pp = &dlgs; *pp; pp = &(*pp)->next)
        if (*pp == d) {
            *pp = d->next;
            break;
        }
    ndlg--;
    free(d);
}

void wd_close(WDIALOG *d)
{
    if (!d || d->dead)
        return;
    if (d->handle > 0) {
        wind_close(d->handle);
        wind_delete(d->handle);
        d->handle = 0;
    }
    d->dead = 1;
    if (!d->incb)
        unlink_free(d);
}

void wd_forget(void *owner)
{
    WDIALOG *d, *nx;
    if (!owner)
        return;
    for (d = dlgs; d; d = nx) {
        nx = d->next;
        if (d->owner == owner && !d->dead) {
            ALERT_FN fn = d->afn;
            void *ctx = d->ctx;
            d->afn = 0;
            wd_close(d);             /* frees d */
            if (fn)
                fn(0, ctx);          /* 0: dropped, free what it holds */
        }
    }
}

void wd_close_all(void)
{
    WDIALOG *d, *nx;
    for (d = dlgs; d; d = nx) {
        nx = d->next;
        wd_close(d);
    }
}

/* an exit button was chosen */
static void choose(WDIALOG *d, int obj)
{
    int close;
    if (d->incb)                   /* still answering (a job polls events) */
        return;
    if (obj > 0) {
        d->t[obj].ob_state |= SELECTED;
        wd_redraw_obj(d, obj);
    }
    d->incb = 1;
    close = d->fn ? d->fn(d, obj) : 1;
    d->incb = 0;
    if (d->dead) {                 /* closed while answering */
        unlink_free(d);
        return;
    }
    if (close)
        wd_close(d);
    else if (obj > 0) {
        d->t[obj].ob_state &= ~SELECTED;
        wd_redraw_obj(d, obj);
    }
}

static int find_flag(WDIALOG *d, int flag, int not)
{
    int i;
    for (i = 1; i < d->n; i++)
        if ((d->t[i].ob_flags & flag) && !(d->t[i].ob_flags & not) &&
            !(d->t[i].ob_flags & HIDETREE))
            return i;
    return -1;
}

/* the button that cancels: an exit button that isn't the default one,
   the last of them (alerts: the last button)                          */
static int cancel_obj(WDIALOG *d)
{
    int i, c = -1;
    for (i = 1; i < d->n; i++)
        if ((d->t[i].ob_flags & EXIT) && !(d->t[i].ob_flags & (DEFAULT | HIDETREE)))
            c = i;
    return c;
}

int wd_message(const short *msg)
{
    WDIALOG *d;
    if (msg[0] < WM_REDRAW || msg[0] > 40 || !(d = find(msg[3])))
        return 0;
    switch (msg[0]) {
    case WM_REDRAW:
        draw(d, (const GRECT *)&msg[4]);
        break;
    case WM_TOPPED:
        wind_set(d->handle, WF_TOP, 0, 0, 0, 0);
        if (d->edit > 0) {
            tree_place(d->t, d->handle);
            wind_update(BEG_UPDATE);
            cur_on(d);
            wind_update(END_UPDATE);
        }
        break;
    case WM_CLOSED:
        choose(d, -1);
        break;
    case WM_MOVED: {
        GRECT r = *(const GRECT *)&msg[4];
        if (d->cursor && is_top(d->handle)) {
            tree_place(d->t, d->handle);
            wind_update(BEG_UPDATE);
            cur_off(d);
            wind_update(END_UPDATE);
        }
        d->cursor = 0;
        wind_set_grect(d->handle, WF_CURRXYWH, &r);
        tree_place(d->t, d->handle);
        if (is_top(d->handle) && d->edit > 0) {
            wind_update(BEG_UPDATE);
            cur_on(d);
            wind_update(END_UPDATE);
        }
        break;
    }
    case 33:                                     /* WM_BOTTOMED */
        if (d->cursor && is_top(d->handle)) {
            tree_place(d->t, d->handle);
            wind_update(BEG_UPDATE);
            cur_off(d);
            wind_update(END_UPDATE);
        }
        wind_set(d->handle, WF_BOTTOM, 0, 0, 0, 0);
        break;
    }
    return 1;
}

int wd_button(int handle, int mx, int my, int clicks)
{
    WDIALOG *d = find(handle);
    OBJECT *o;
    int obj;
    (void)clicks;
    if (!d)
        return 0;
    if (d->incb)                       /* busy with its answer: wait */
        return 1;
    if (!is_top(handle)) {                       /* MagiC: clicks behind */
        wind_set(handle, WF_TOP, 0, 0, 0, 0);
        return 1;
    }
    tree_place(d->t, d->handle);
    obj = objc_find(d->t, 0, 8, mx, my);
    if (obj <= 0)
        return 1;
    o = &d->t[obj];
    if (o->ob_flags & EDITABLE) {
        wind_update(BEG_UPDATE);
        cur_off(d);
        d->edit = obj;
        cur_on(d);
        wind_update(END_UPDATE);
        return 1;
    }
    if (!(o->ob_flags & SELECTABLE) || (o->ob_state & DISABLED))
        return 1;
    if (o->ob_flags & EXIT) {
        int in;
        wind_update(BEG_UPDATE);
        wind_update(BEG_MCTRL);
        in = graf_watchbox(d->t, obj, SELECTED, 0);
        wind_update(END_MCTRL);
        wind_update(END_UPDATE);
        if (in)
            choose(d, obj);
        else {
            o->ob_state &= ~SELECTED;
            wd_redraw_obj(d, obj);
        }
    } else if (o->ob_flags & RBUTTON) {
        /* radio: the other radio buttons of the same parent go off */
        int i, par = obj_parent(d->t, obj);
        for (i = d->t[par].ob_head; i > 0 && i != par; i = d->t[i].ob_next)
            if (i != obj && (d->t[i].ob_flags & RBUTTON) && (d->t[i].ob_state & SELECTED)) {
                d->t[i].ob_state &= ~SELECTED;
                wd_redraw_obj(d, i);
            }
        o->ob_state |= SELECTED;
        wd_redraw_obj(d, obj);
    } else {
        o->ob_state ^= SELECTED;
        wd_redraw_obj(d, obj);
    }
    return 1;
}

int wd_key(int ks, int key)
{
    short top = 0, dd;
    WDIALOG *d;
    int scan = (key >> 8) & 0xFF, asc = key & 0xFF, obj;
    wind_get(0, WF_TOP, &top, &dd, &dd, &dd);
    if (!(d = find(top)))
        return 0;
    if (d->incb)
        return 1;
    if (ks & (K_CTRL | K_ALT))
        return 0;                       /* menu shortcuts still work */
    if (asc == 13 || scan == 0x72) {                 /* Return, Enter */
        if ((obj = find_flag(d, DEFAULT, 0)) > 0)
            choose(d, obj);
        return 1;
    }
    if (scan == 0x61 || (asc == 27 && d->edit <= 0)) {   /* Undo, Esc */
        if (d->nbut > 0)                 /* an alert: its last button */
            choose(d, A_B1 + d->nbut - 1);
        else if ((obj = cancel_obj(d)) > 0)
            choose(d, obj);
        return 1;
    }
    if (d->edit > 0) {
        tree_place(d->t, d->handle);
        wind_update(BEG_UPDATE);
        cur_on(d);
        if (asc == 9 || scan == 0x50 || scan == 0x48) {   /* next field */
            int i, n = d->n, back = scan == 0x48 || (ks & 3 && asc == 9), next = d->edit;
            for (i = 1; i < n; i++) {
                next = back ? (next > 1 ? next - 1 : n - 1) : (next < n - 1 ? next + 1 : 1);
                if ((d->t[next].ob_flags & EDITABLE) && !(d->t[next].ob_flags & HIDETREE))
                    break;
            }
            cur_off(d);
            d->edit = next;
            cur_on(d);
        } else
            objc_edit(d->t, d->edit, key, &d->eidx, ED_CHAR);
        wind_update(END_UPDATE);
    }
    return 1;
}

/* ------------------------------------------------------- alert windows */


int  wd_alert_tree = -1;            /* T_ALERT of the program's file */
const char *wd_app_title = "";      /* " FView " etc.                */

/* the window goes first, then the answer is acted on (which may take
   a while: saving, printing)                                         */
static int alert_exit(WDIALOG *d, int obj)
{
    int b = obj < 0 ? d->nbut : obj - A_B1 + 1;
    ALERT_FN fn = d->afn;
    void *ctx = d->ctx;
    d->afn = 0;
    wd_close(d);
    if (fn)
        fn(b, ctx);
    return 1;
}

/* split "[n][a|b][x|y]" */
static int alert_parse(const char *s, int *icon, char lines[5][48], int *nl,
                       char buts[3][24], int *nb)
{
    const char *p;
    int k = 0, n = 0;
    *nl = *nb = 0;
    if (s[0] != '[' || s[2] != ']' || s[3] != '[')
        return 0;
    *icon = s[1] - '0';
    for (p = s + 4; *p && *p != ']'; p++) {
        if (*p == '|') {
            lines[*nl][n] = 0;
            if (++*nl >= 5) return 0;
            n = 0;
        } else if (n < 47)
            lines[*nl][n++] = *p;
    }
    lines[(*nl)++][n] = 0;
    if (*p != ']' || p[1] != '[')
        return 0;
    for (p += 2, n = 0; *p && *p != ']'; p++) {
        if (*p == '|') {
            buts[k][n] = 0;
            if (++k >= 3) return 0;
            n = 0;
        } else if (n < 23)
            buts[k][n++] = *p;
    }
    buts[k][n] = 0;
    *nb = k + 1;
    return 1;
}

static void form_fallback(int def, const char *alert, ALERT_FN fn, void *ctx)
{
    int b = form_alert(def, (char *)alert);
    if (fn)
        fn(b, ctx);
}

void alert_show(int def, const char *alert, ALERT_FN fn, void *ctx, void *owner)
{
    char lines[5][48], buts[3][24];
    int icon = 0, nl, nb, i, maxl = 0, maxb = 0, tx, bw, tot, w, th, by;
    WDIALOG *d = 0;
    OBJECT *t;
    if (wd_alert_tree >= 0 && alert_parse(alert, &icon, lines, &nl, buts, &nb))
        d = wd_create(wd_alert_tree, wd_app_title, 0, alert_exit, owner, ctx);
    if (!d) {
        form_fallback(def, alert, fn, ctx);
        return;
    }
    d->afn = fn;
    d->nbut = nb;
    t = d->t;
    for (i = 0; i < nl; i++)
        if ((int)strlen(lines[i]) > maxl) maxl = (int)strlen(lines[i]);
    for (i = 0; i < nb; i++)
        if ((int)strlen(buts[i]) > maxb) maxb = (int)strlen(buts[i]);
    /* laid out in pixels like the AES's own alerts */
    for (i = A_NOTE; i <= A_STOP; i++)
        t[i].ob_flags |= HIDETREE;
    if (icon >= 1 && icon <= 3) {
        t[A_NOTE + icon - 1].ob_flags &= ~HIDETREE;
        t[A_NOTE + icon - 1].ob_x = 2 * cell_w;
        t[A_NOTE + icon - 1].ob_y = cell_h;
    }
    tx = (icon >= 1 && icon <= 3) ? 4 * cell_w + 32 : 2 * cell_w;
    for (i = 0; i < 5; i++) {
        OBJECT *o = &t[A_L1 + i];
        if (i < nl) {
            strcpy(wd_text(d, A_L1 + i), lines[i]);
            o->ob_x = tx;
            o->ob_y = cell_h + i * cell_h;
            o->ob_width = (int)strlen(lines[i]) * cell_w;
            o->ob_height = cell_h;
        } else
            o->ob_flags |= HIDETREE;
    }
    th = nl * cell_h;
    if (icon >= 1 && icon <= 3 && th < 32)
        th = 32;
    bw = (maxb + 2) * cell_w;
    tot = nb * bw + (nb - 1) * 2 * cell_w;
    w = tx + maxl * cell_w + 2 * cell_w;
    if (w < tot + 4 * cell_w)
        w = tot + 4 * cell_w;
    by = cell_h + th + cell_h;
    for (i = 0; i < 3; i++) {
        OBJECT *o = &t[A_B1 + i];
        if (i < nb) {
            strcpy(wd_text(d, A_B1 + i), buts[i]);
            o->ob_x = (w - tot) / 2 + i * (bw + 2 * cell_w);
            o->ob_y = by;
            o->ob_width = bw;
            o->ob_height = cell_h;
            o->ob_flags = (o->ob_flags & ~(DEFAULT | HIDETREE)) | SELECTABLE | EXIT |
                          (i + 1 == def ? DEFAULT : 0) | (aes_3d ? FL3DACT : 0);
        } else
            o->ob_flags |= HIDETREE;
    }
    t[0].ob_width = w;
    t[0].ob_height = by + cell_h + cell_h;
    if (aes_3d)
        t[0].ob_flags |= FL3DBAK;
    if (!wd_show(d))                        /* no window: the AES's own */
        form_fallback(def, alert, fn, ctx);
}

void alertf(const char *fmt, ...)
{
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    alert_show(1, buf, 0, 0, 0);
}

void alert_rs(int id, ...)
{
    char buf[400];
    va_list ap;
    va_start(ap, id);
    vsnprintf(buf, sizeof buf, rs(id), ap);
    va_end(ap);
    alert_show(1, buf, 0, 0, 0);
}

void alert_ask(int def, ALERT_FN fn, void *ctx, void *owner, int id, ...)
{
    char buf[400];
    va_list ap;
    va_start(ap, id);
    vsnprintf(buf, sizeof buf, rs(id), ap);
    va_end(ap);
    alert_show(def, buf, fn, ctx, owner);
}
