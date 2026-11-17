/* Copyright (c) 2026 Francis C Bullen. SPDX-License-Identifier: MIT (see LICENSE) */
/*
 * pgaes.c - GEMLib's AES calls (as pcgem/include/gem.h declares them) on
 *           PC GEM, through John Elliott's DJGPP GEM bindings.
 *
 * The program's object trees live in normal 32-bit memory. For each call that
 * takes a tree, the tree is copied down to DOS memory (where GEM can see
 * it), the call is made, and the objects' states and the edited texts are
 * copied back. Program-drawn (USERDEF) objects are given real-mode
 * callbacks for the call's length: GEM calls back into a trampoline here,
 * which calls the program's drawing code with the 32-bit tree and parameter.
 *
 * objc_find and objc_offset only read the tree, so they're done here.
 *
 * aes_global[0] says 0x0120 (an AES like TOS 1.x's): the front end then
 * takes its most careful paths - no 3D objects, no appl_getinfo, no
 * WF_BOTTOM/ICONIFY, menus kept small - which is what GEM/3 can do.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/movedata.h>
#include "pgkit.h"

#ifdef EDLOG
void edlog(const char *fmt, ...);
#define PGLOG(...) edlog(__VA_ARGS__)
static int pg_calls;            /* tree calls logged: the first 200 only */
#define PGLOG_CALL(...) do { if (pg_calls < 200) { pg_calls++; edlog(__VA_ARGS__); } } while (0)
#else
#define PGLOG(...) ((void)0)
#define PGLOG_CALL(...) ((void)0)
#endif

short aes_global[15];
short pg_aes_version, pg_aes_numapps;    /* what this GEM's AES really says */
short pg_wchar = 8, pg_hchar = 16, pg_scr_w = 640, pg_scr_h = 480;

/* ---- trees down to GEM and back --------------------------------------- */

typedef struct { WORD (*code)(PARMBLK *); LONG parm; } FE_USERBLK;    /* the program's USERBLK */
typedef struct { OBJECT *tree; FE_USERBLK *ub; } UDCTX;

struct pg_low {
    int n;
    DJUSERBLK *djb;
    UDCTX *ctx;
    LONG *spec;                   /* the 32-bit ob_specs of the USERDEF objects */
    WORD *obj;
};

/* GEM wants a USERDEF drawn: give the program its own tree and parameter */
static WORD ud_trampoline(PARMBLK *pb)
{
    UDCTX *c = (UDCTX *)pb->pb_parm;
    pb->pb_tree = (LPTREE)(long)c->tree;
    pb->pb_parm = c->ub->parm;
    return c->ub->code(pb);
}

LPTREE pg_tree_down(OBJECT *tree, PG_LOW **keep)
{
    LPTREE tr;
    PG_LOW *k;
    int i, n = 0;
    *keep = NULL;
    for (i = 0;; i++) {
        if ((tree[i].ob_type & 0xff) == G_USERDEF) n++;
        if (tree[i].ob_flags & LASTOB) break;
    }
    {
        LONG sz = dj_tree_size32(tree);
        tr = dos_alloc(sz);
        PGLOG_CALL("pg: tree %p: %d objects, %ld bytes -> DOS %05lx, %d USERDEF", (void *)tree, i + 1, sz, (long)tr, n);
        if (!tr) return 0;
    }
    dj_tree_32to16(tree, tr);
    if (!n) return tr;
    k = (PG_LOW *)calloc(1, sizeof *k);
    if (k) {
        k->djb = (DJUSERBLK *)calloc(n, sizeof *k->djb);
        k->ctx = (UDCTX *)calloc(n, sizeof *k->ctx);
        k->spec = (LONG *)calloc(n, sizeof *k->spec);
        k->obj = (WORD *)calloc(n, sizeof *k->obj);
    }
    if (!k || !k->djb || !k->ctx || !k->spec || !k->obj) {
        if (k) { free(k->djb); free(k->ctx); free(k->spec); free(k->obj); free(k); }
        return tr;                /* drawn without them, rather than not at all */
    }
    for (i = 0;; i++) {
        if ((tree[i].ob_type & 0xff) == G_USERDEF && tree[i].ob_spec) {
            int j = k->n;
            k->ctx[j].tree = tree;
            k->ctx[j].ub = (FE_USERBLK *)tree[i].ob_spec;
            k->djb[j].ub_code = ud_trampoline;
            k->djb[j].ub_parm = (LONG)&k->ctx[j];
            k->spec[j] = tree[i].ob_spec;
            k->obj[j] = (WORD)i;
            if (dj_userdef(tr, (WORD)i, &k->djb[j])) k->n++;
        }
        if (tree[i].ob_flags & LASTOB) break;
    }
    *keep = k;
    return tr;
}

/* The objects' states (and positions, which form_center sets) and the
 * edited texts back from GEM's copy. Not dj_tree_16to32s(): that copies
 * te_txtlen + 1 bytes, but te_txtlen already counts the final 0, so it
 * wrote one byte past every text buffer. */
static void tree_back(LPTREE tr, OBJECT *tree)
{
    int i;
    for (i = 0;; i++) {
        LONG spec = tree[i].ob_spec;
        LPTREE o16 = tr + 24L * i;
        dosmemget(o16, sizeof(OBJECT), &tree[i]);
        tree[i].ob_spec = spec;
        switch (tree[i].ob_type & 0xff) {
        case G_TEXT: case G_BOXTEXT: case G_FTEXT: case G_FBOXTEXT: {
            TEDINFO *te = (TEDINFO *)spec;
            int n = te ? te->te_txtlen : 0;
            if (n > 0 && te->te_ptext) {
                LONG ted16 = LSGET(o16 + 12);
                dosmemget(LSGET(ted16), n, (void *)te->te_ptext);
                ((char *)te->te_ptext)[n - 1] = 0;
            }
            break;
        }
        }
        if (tree[i].ob_flags & LASTOB) break;
    }
}

void pg_tree_up(OBJECT *tree, LPTREE tr, PG_LOW *k, int copyback)
{
    int j;
    if (copyback) tree_back(tr, tree);
    if (k) {
        for (j = k->n - 1; j >= 0; j--) {
            dj_unuserdef(&k->djb[j]);
            tree[k->obj[j]].ob_spec = k->spec[j];
        }
        free(k->djb); free(k->ctx); free(k->spec); free(k->obj); free(k);
    }
    dos_free(tr);
}

#define TREE_CALL(tree, copyback, call) do { \
        PG_LOW *k_; LPTREE tr = pg_tree_down(tree, &k_); \
        if (!tr) return 0; \
        PGLOG_CALL("pg: %s", #call); \
        rv = (call); \
        PGLOG_CALL("pg:   -> %d", rv); \
        pg_tree_up(tree, tr, k_, copyback); \
    } while (0)

/* ---- application --------------------------------------------------------- */

short pg_appl_init(void)
{
    WORD wc, hc, wb, hb, x, y, w, h;
    short id = appl_init(NULL);
    extern LPWORD pGlobal;              /* the bindings' global array, in DOS memory */
    pg_aes_version = pg_aes_numapps = 0;
    if (id >= 0 && pGlobal) {
        dosmemget(pGlobal, 2, &pg_aes_version);
        dosmemget(pGlobal + 2, 2, &pg_aes_numapps);
    }
    memset(aes_global, 0, sizeof aes_global);
    aes_global[0] = 0x0120;
    aes_global[1] = 1;
    aes_global[2] = id;
    if (id < 0) return id;
    graf_handle(&wc, &hc, &wb, &hb);
    pg_wchar = wc; pg_hchar = hc;
    wind_get(0, WF_CXYWH, &x, &y, &w, &h);    /* the desk's window: the whole screen */
    pg_scr_w = x + w; pg_scr_h = y + h;
    return id;
}

short pg_appl_exit(void) { return appl_exit(); }
short pg_appl_find(const char *name) { return appl_find((BYTE *)name); }
short pg_appl_write(short id, short len, void *buf) { return appl_write(id, len, (WORD *)buf); }

short pg_appl_getinfo(short type, short *o1, short *o2, short *o3, short *o4)
{
    (void)type;
    *o1 = *o2 = *o3 = *o4 = 0;
    return 0;                     /* not asked for: the front end only asks AES 4 */
}

short pg_evnt_multi(short type, short clicks, short mask, short state,
                    short m1flag, short m1x, short m1y, short m1w, short m1h,
                    short m2flag, short m2x, short m2y, short m2w, short m2h,
                    short *msg, unsigned long ms,
                    short *mx, short *my, short *mb, short *ks, short *key, short *nclicks)
{
    UWORD x = 0, y = 0, b = 0, s = 0, k = 0, c = 0;
    WORD dummy[8];
    short r = evnt_multi((UWORD)type, (UWORD)clicks, (UWORD)mask, (UWORD)state,
                         (UWORD)m1flag, (UWORD)m1x, (UWORD)m1y, (UWORD)m1w, (UWORD)m1h,
                         (UWORD)m2flag, (UWORD)m2x, (UWORD)m2y, (UWORD)m2w, (UWORD)m2h,
                         msg ? msg : dummy, (UWORD)(ms & 0xffff), (UWORD)(ms >> 16),
                         &x, &y, &b, &s, &k, &c);
    if (mx) *mx = x;
    if (my) *my = y;
    if (mb) *mb = b;
    if (ks) *ks = s;
    if (key) *key = k;
    if (nclicks) *nclicks = c;
    return r;
}

short pg_evnt_timer(unsigned long ms) { return evnt_timer((UWORD)(ms & 0xffff), (UWORD)(ms >> 16)); }
short pg_evnt_button(short clicks, short mask, short state, short *mx, short *my,
                     short *button, short *kstate)
{
    return evnt_button(clicks, (UWORD)mask, (UWORD)state, mx, my, button, kstate);
}
short pg_appl_yield(void) { appl_yield(); return 1; }

/* the Desk menu entry: GEM keeps the string's address, so it stays in DOS
 * memory for as long as the program runs */
short pg_menu_register(short apid, const char *name)
{
    LPBYTE p = dj_string_addr((char *)name);
    return p ? menu_register(apid, (LPVOID)p) : -1;
}

/* ---- menus: GEM keeps its own copy of the bar (dj_menu_bar) ------------- */

static OBJECT *menu32;

short pg_menu_bar(OBJECT *tree, short show)
{
    if (!show) { menu32 = NULL; return dj_menu_bar(NULL, 0); }
    menu32 = tree;
    return dj_menu_bar(tree, 1);
}

static void set_state(OBJECT *tree, short obj, unsigned short bit, int on)
{
    if (!tree) return;
    if (on) tree[obj].ob_state |= bit; else tree[obj].ob_state &= ~bit;
}

short pg_menu_icheck(OBJECT *tree, short item, short check)
{
    set_state(tree, item, CHECKED, check);
    return tree == menu32 ? dj_menu_icheck(item, check) : 1;
}

short pg_menu_ienable(OBJECT *tree, short item, short enable)
{
    set_state(tree, item, DISABLED, !enable);
    return tree == menu32 ? dj_menu_ienable(item, enable) : 1;
}

short pg_menu_tnormal(OBJECT *tree, short title, short normal)
{
    set_state(tree, title, SELECTED, !normal);
    return tree == menu32 ? dj_menu_tnormal(title, normal) : 1;
}

short pg_menu_popup(void *menu, short x, short y, void *out)
{
    (void)menu; (void)x; (void)y; (void)out;
    return 0;                     /* AES 4.1 only: the front end doesn't ask GEM/3 */
}

/* ---- objects ------------------------------------------------------------- */

static short parent_of(OBJECT *t, short obj)
{
    short n;
    if (obj <= 0) return -1;
    for (;;) {
        n = t[obj].ob_next;
        if (n < 0) return -1;
        if (t[n].ob_tail == obj) return n;
        obj = n;
    }
}

short pg_objc_offset(OBJECT *t, short obj, short *x, short *y)
{
    short ax = 0, ay = 0;
    while (obj >= 0) {
        ax += (short)t[obj].ob_x; ay += (short)t[obj].ob_y;     /* the kit's fields are unsigned */
        obj = parent_of(t, obj);
    }
    *x = ax; *y = ay;
    return 1;
}

static short find_in(OBJECT *t, short obj, short depth, short mx, short my, short px, short py)
{
    short x = px + (short)t[obj].ob_x, y = py + (short)t[obj].ob_y, found, c;
    if (t[obj].ob_flags & HIDETREE) return -1;
    if (mx < x || my < y || mx >= x + (short)t[obj].ob_width || my >= y + (short)t[obj].ob_height) return -1;
    found = obj;
    if (depth > 0)
        for (c = t[obj].ob_head; c >= 0 && c != obj; c = t[c].ob_next) {
            short r = find_in(t, c, depth - 1, mx, my, x, y);
            if (r >= 0) found = r;        /* later children are drawn on top */
        }
    return found;
}

short pg_objc_find(OBJECT *t, short start, short depth, short mx, short my)
{
    short px = 0, py = 0, p = parent_of(t, start);
    if (p >= 0) pg_objc_offset(t, p, &px, &py);
    return find_in(t, start, depth, mx, my, px, py);
}

short pg_objc_draw(OBJECT *tree, short start, short depth, short x, short y, short w, short h)
{
    short rv;
    TREE_CALL(tree, 0, objc_draw(tr, start, depth, x, y, w, h));
    return rv;
}

short pg_objc_edit(OBJECT *tree, short obj, short ch, short *idx, short kind)
{
    short rv;
    WORD i = *idx;
    TREE_CALL(tree, 1, objc_edit(tr, obj, ch, &i, kind));
    *idx = i;
    return rv;
}

short pg_objc_change(OBJECT *tree, short obj, short res, short x, short y, short w, short h, short state, short redraw)
{
    short rv;
    TREE_CALL(tree, 1, objc_change(tr, obj, res, x, y, w, h, state, redraw));
    return rv;
}

short pg_form_do(OBJECT *tree, short start)
{
    short rv;
    TREE_CALL(tree, 1, form_do((LPVOID)tr, start));
    return rv;
}

short pg_form_center(OBJECT *tree, short *x, short *y, short *w, short *h)
{
    short rv;
    WORD cx, cy, cw, ch;
    TREE_CALL(tree, 1, form_center(tr, &cx, &cy, &cw, &ch));
    *x = cx; *y = cy; *w = cw; *h = ch;
    return rv;
}

short pg_form_keybd(OBJECT *tree, short obj, short next, short ch, short *onext, short *och)
{
    short rv;
    WORD n, c;
    TREE_CALL(tree, 1, form_keybd((LPVOID)tr, obj, next, ch, &n, &c));
    *onext = n; *och = c;
    return rv;
}

short pg_form_button(OBJECT *tree, short obj, short clicks, short *onext)
{
    short rv;
    WORD n;
    TREE_CALL(tree, 1, form_button((LPVOID)tr, obj, clicks, &n));
    *onext = n;
    return rv;
}

short pg_graf_watchbox(OBJECT *tree, short obj, short instate, short outstate)
{
    short rv;
    TREE_CALL(tree, 1, graf_watchbox(tr, obj, (UWORD)instate, (UWORD)outstate));
    return rv;
}

short pg_graf_slidebox(OBJECT *tree, short parent, short obj, short vertical)
{
    short rv;
    TREE_CALL(tree, 0, graf_slidebox(tr, parent, obj, vertical));
    return rv;
}

short pg_form_dial(short op, short lx, short ly, short lw, short lh, short bx, short by, short bw, short bh)
{
    return form_dial(op, lx, ly, lw, lh, bx, by, bw, bh);
}

short pg_form_alert(short def, const char *text)
{
    PGLOG("pg: form_alert %s", text);
    return dj_form_alert(def, "%s", text);
}

/* ---- graphics -------------------------------------------------------------- */

short pg_graf_handle(short *wc, short *hc, short *wb, short *hb)
{
    WORD a, b, c, d;
    short r = graf_handle(&a, &b, &c, &d);
    *wc = a; *hc = b; *wb = c; *hb = d;
    return r;
}

short pg_graf_mouse(short form, void *shape)
{
    (void)shape;                  /* the program's own pointer shapes: GEM's arrow instead, for now */
    if (form == USER_DEF) form = ARROW;
    return graf_mouse(form, 0L);
}

short pg_graf_mkstate(short *mx, short *my, short *mb, short *ks)
{
    WORD a, b, c, d;
    graf_mkstate(&a, &b, &c, &d);
    *mx = a; *my = b; *mb = c; *ks = d;
    return 1;
}

/* ---- clipboard, file selector, shell ----------------------------------- */

short pg_scrp_read(char *path) { return scrp_read(path); }
short pg_scrp_write(const char *path) { return scrp_write((char *)path); }

short pg_fsel_input(char *path, char *name, short *button)
{
    WORD b = 0;
    short r = fsel_input(path, name, &b);
    *button = b;
    return r;
}

short pg_fsel_exinput(char *path, char *name, short *button, const char *title)
{
    (void)title;
    return pg_fsel_input(path, name, button);
}

short pg_shel_write(short mode, short graphic, short aes, const void *cmd, const void *tail)
{
    (void)mode; (void)graphic; (void)aes; (void)cmd; (void)tail;
    return 0;                     /* GEM/3 can't start a program alongside */
}

/* the program's own path (DJGPP keeps it in argv[0]) and an empty tail */
short pg_shel_read(char *cmd, char *tail)
{
    extern char **__crt0_argv;
    char *p;
    strcpy(cmd, __crt0_argv && __crt0_argv[0] ? __crt0_argv[0] : "");
    for (p = cmd; *p; p++) if (*p == '/') *p = '\\';
    if (tail) tail[0] = tail[1] = 0;
    return 1;
}

/* a file in the current folder, else beside the program; the full path
 * goes back in path (as the AES's shel_find does with its search) */
short pg_shel_find(char *path)
{
    char t[260], *s;
    const char *name = path;
    FILE *f;
    if ((s = strrchr(path, '\\')) || (s = strrchr(path, '/'))) name = s + 1;
    if ((f = fopen(path, "rb")) != NULL) { fclose(f); return 1; }
    pg_shel_read(t, NULL);
    if ((s = strrchr(t, '\\')) != NULL) {
        strcpy(s + 1, name);
        if ((f = fopen(t, "rb")) != NULL) { fclose(f); strcpy(path, t); return 1; }
    }
    return 0;
}

short pg_shel_envrn(char **value, const char *name)
{
    char n[64];
    size_t l = strlen(name);
    if (l >= sizeof n) l = sizeof n - 1;
    memcpy(n, name, l); n[l] = 0;
    if (l && n[l - 1] == '=') n[l - 1] = 0;          /* GEMLib style: "PATH=" */
    *value = getenv(n);
    return 1;
}

/* ---- windows ------------------------------------------------------------- */

#define MAXWIN 64
static LPBYTE wstr[MAXWIN][2];    /* each window's name and info line, in DOS memory */

short pg_wind_create(short kind, short x, short y, short w, short h)
{
    short r = wind_create((UWORD)kind, x, y, w, h);
    PGLOG("pg: wind_create %04x %d,%d %dx%d -> %d", kind, x, y, w, h, r);
    return r;
}
short pg_wind_open(short h, short x, short y, short w, short hh) { return wind_open(h, x, y, w, hh); }
short pg_wind_close(short h) { return wind_close(h); }

short pg_wind_delete(short h)
{
    short r = wind_delete(h);
    if (h >= 0 && h < MAXWIN) {
        if (wstr[h][0]) dos_free(wstr[h][0]);
        if (wstr[h][1]) dos_free(wstr[h][1]);
        wstr[h][0] = wstr[h][1] = 0;
    }
    return r;
}

short pg_wind_get(short h, short field, short *o1, short *o2, short *o3, short *o4)
{
    WORD a = 0, b = 0, c = 0, d = 0;
    short r = wind_get(h, field, &a, &b, &c, &d);
    if (o1) *o1 = a;
    if (o2) *o2 = b;
    if (o3) *o3 = c;
    if (o4) *o4 = d;
    return r;
}

short pg_wind_set(short h, short field, short i1, short i2, short i3, short i4)
{
    return wind_set(h, field, i1, i2, i3, i4);
}

/* GEM keeps the pointer, so the text stays in DOS memory until it's replaced */
short pg_wind_set_str(short h, short field, const char *s)
{
    int slot = field == WF_NAME ? 0 : field == WF_INFO ? 1 : -1;
    LPBYTE p = dj_string_addr((char *)s);
    short r;
    PGLOG("pg: wind_set_str %d %d '%s' -> DOS %05lx", h, field, s, (long)p);
    if (!p) return 0;
    r = dj_wind_setl(h, field, p, 0L);
    if (slot >= 0 && h >= 0 && h < MAXWIN) {
        if (wstr[h][slot]) dos_free(wstr[h][slot]);
        wstr[h][slot] = p;
    }
    return r;
}

short pg_wind_find(short x, short y) { return wind_find(x, y); }
short pg_wind_update(short mode) { return wind_update(mode); }

short pg_wind_calc(short type, short kind, short x, short y, short w, short h,
                   short *ox, short *oy, short *ow, short *oh)
{
    WORD a, b, c, d;
    short r = wind_calc(type, (UWORD)kind, x, y, w, h, &a, &b, &c, &d);
    *ox = a; *oy = b; *ow = c; *oh = d;
    return r;
}
