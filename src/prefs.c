/*
 * prefs.c - FVIEW.INF and the non-modal preferences window.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "gemui.h"
#include "fvrsc.h"

int prefs_win;
static OBJECT *ptree;
static PREFS edit;                 /* what the dialog shows */

/* ------------------------------------------------------------ file */

typedef struct { const char *key; int off; } PKEY;
#define K(m) { #m, offsetof(PREFS, m) }
static const PKEY keys[] = {
    K(start_iconified), K(use_workers), K(dither), K(serpentine), K(zoom),
    K(smooth), K(palmode), K(keep_sys), K(ui_pens), K(bg), K(gif_trans), K(png_alpha),
    K(iff_mask), K(iff_ham), K(img_syspal), K(img_vdiorder), K(jpg_grey_mono),
    K(tabw), K(wrap), K(charset), K(md_markup), K(rtf_pict), K(pdf_zoom),
    K(pdf_gfx), K(pdf_imgbox), K(prt_scale), K(prt_dither), K(prt_header),
    K(dither_pal), K(lang),
    { 0, 0 }
};

static char inf_path[256];

static void find_inf(void)
{
    const char *home = getenv("HOME");
    FILE *f;
    if (home && *home) {
        size_t n = strlen(home);
        strcpy(inf_path, home);
        if (inf_path[n - 1] != '\\' && inf_path[n - 1] != '/')
            strcat(inf_path, "\\");
        strcat(inf_path, "FVIEW.INF");
        if ((f = fopen(inf_path, "r")) != 0) {
            fclose(f);
            return;
        }
    }
    /* next to the program (current directory at start-up) */
    strcpy(inf_path, "FVIEW.INF");
    if (shel_find(inf_path) && (f = fopen(inf_path, "r")) != 0) {
        fclose(f);
        return;
    }
    if (home && *home) {             /* save into HOME by default */
        size_t n = strlen(home);
        strcpy(inf_path, home);
        if (inf_path[n - 1] != '\\' && inf_path[n - 1] != '/')
            strcat(inf_path, "\\");
        strcat(inf_path, "FVIEW.INF");
    } else
        strcpy(inf_path, "FVIEW.INF");
}

void prefs_load(PREFS *p)
{
    FILE *f;
    char line[128];
    prefs_default(p);
    find_inf();
    f = fopen(inf_path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        int k;
        if (!eq || line[0] == '#')
            continue;
        *eq = 0;
        for (k = 0; keys[k].key; k++)
            if (!strcmp(line, keys[k].key)) {
                ((char *)p)[keys[k].off] = (char)atoi(eq + 1);
                break;
            }
    }
    fclose(f);
    if (p->tabw != 4 && p->tabw != 8)
        p->tabw = 8;
    if (p->lang < 0 || p->lang >= LANG_N)
        p->lang = LANG_EN;
}

int prefs_save(const PREFS *p)
{
    FILE *f = fopen(inf_path, "w");
    int k;
    if (!f)
        return 0;
    fprintf(f, "# fview " FV_VERSION " preferences\n");
    for (k = 0; keys[k].key; k++)
        fprintf(f, "%s=%d\n", keys[k].key, ((const char *)p)[keys[k].off]);
    return fclose(f) == 0;
}

/* ---------------------------------------------------------- window */

static GRECT pwork;

static void place_tree(void)
{
    wind_get_grect(prefs_win, WF_WORKXYWH, &pwork);
    ptree[0].ob_x = pwork.g_x + 3;
    ptree[0].ob_y = pwork.g_y + 3;
}

void prefs_open(void)
{
    GRECT wk, r;
    int kind = NAME | CLOSER | MOVER;
    if (prefs_win > 0) {
        wind_set(prefs_win, WF_TOP, 0, 0, 0, 0);
        return;
    }
    if (!ptree)
        ptree = build_prefs();
    edit = prefs;
    prefs_to_tree(ptree, &edit);
    wk.g_x = wk.g_y = 0;
    wk.g_w = ptree[0].ob_width + 6;
    wk.g_h = ptree[0].ob_height + 6;
    wind_calc_grect(WC_BORDER, kind, &wk, &r);
    r.g_x = desk.g_x + (desk.g_w - r.g_w) / 2;
    r.g_y = desk.g_y + (desk.g_h - r.g_h) / 2;
    if (r.g_x < desk.g_x) r.g_x = desk.g_x;
    if (r.g_y < desk.g_y) r.g_y = desk.g_y;
    prefs_win = wind_create_grect(kind, &r);
    if (prefs_win <= 0) {
        prefs_win = 0;
        return;
    }
    wind_set_str(prefs_win, WF_NAME, (char *)rs(S_W_PREFS));
    wind_open_grect(prefs_win, &r);
    place_tree();
}

void prefs_close(void)
{
    if (prefs_win > 0) {
        wind_close(prefs_win);
        wind_delete(prefs_win);
    }
    prefs_win = 0;
}

int prefs_handle(int handle) { return prefs_win > 0 && handle == prefs_win; }

void prefs_redraw(GRECT *area)
{
    GRECT r;
    short pxy[4];
    if (prefs_win <= 0)
        return;
    place_tree();
    wind_update(BEG_UPDATE);
    graf_mouse(M_OFF, 0);
    wind_get_grect(prefs_win, WF_FIRSTXYWH, &r);
    while (r.g_w && r.g_h) {
        if (rc_intersect(area, &r) && rc_intersect(&desk, &r)) {
            /* window background, then the tree */
            vsf_interior(vdih, FIS_SOLID);
            vsf_color(vdih, 0);
            vswr_mode(vdih, MD_REPLACE);
            pxy[0] = r.g_x; pxy[1] = r.g_y;
            pxy[2] = r.g_x + r.g_w - 1; pxy[3] = r.g_y + r.g_h - 1;
            vs_clip(vdih, 1, pxy);
            vr_recfl(vdih, pxy);
            vs_clip(vdih, 0, pxy);
            objc_draw_grect(ptree, 0, 8, &r);
        }
        wind_get_grect(prefs_win, WF_NEXTXYWH, &r);
    }
    graf_mouse(M_ON, 0);
    wind_update(END_UPDATE);
}

static void redraw_obj(int obj)
{
    short x, y;
    GRECT r;
    objc_offset(ptree, obj, &x, &y);
    r.g_x = x - 3; r.g_y = y - 3;
    r.g_w = ptree[obj].ob_width + 6;
    r.g_h = ptree[obj].ob_height + 6;
    prefs_redraw(&r);
}

void prefs_moved(GRECT *r)
{
    wind_set_grect(prefs_win, WF_CURRXYWH, r);
    place_tree();
}

void prefs_top(void)
{
    wind_set(prefs_win, WF_TOP, 0, 0, 0, 0);
}

void prefs_click(int mx, int my)
{
    int obj, kind;
    place_tree();
    obj = objc_find(ptree, 0, 8, mx, my);
    kind = prefs_button_kind(obj);
    if (kind == PB_NONE)
        return;
    if (kind == PB_TOGGLE) {
        int grp[8], n = prefs_radio_group(obj, grp, 8), i;
        if (n) {
            for (i = 0; i < n; i++)
                if (grp[i] != obj && (ptree[grp[i]].ob_state & SELECTED)) {
                    ptree[grp[i]].ob_state &= ~SELECTED;
                    redraw_obj(grp[i]);
                }
            ptree[obj].ob_state |= SELECTED;
        } else
            ptree[obj].ob_state ^= SELECTED;
        redraw_obj(obj);
        return;
    }
    /* Save / Apply / Close: show the press */
    ptree[obj].ob_state |= SELECTED;
    redraw_obj(obj);
    evnt_timer(80L);
    ptree[obj].ob_state &= ~SELECTED;
    redraw_obj(obj);
    if (kind == PB_CLOSE) {
        prefs_close();
        return;
    }
    tree_to_prefs(ptree, &edit);
    {
        PREFS old = prefs;
        int rerender = old.dither != edit.dither || old.serpentine != edit.serpentine ||
                       old.palmode != edit.palmode || old.keep_sys != edit.keep_sys || old.ui_pens != edit.ui_pens ||
                       old.smooth != edit.smooth;
        int relayout = old.pdf_zoom != edit.pdf_zoom || old.pdf_gfx != edit.pdf_gfx ||
                       old.pdf_imgbox != edit.pdf_imgbox;
        prefs = edit;
        if (old.lang != prefs.lang)          /* the resource file is loaded */
            alert_rs(S_AL_LANG);             /* at start-up only           */
        if (old.keep_sys != prefs.keep_sys || old.ui_pens != prefs.ui_pens ||
            old.palmode != prefs.palmode) {
            extern void disp_setup_reserved(void);
            disp_setup_reserved();
        }
        if (rerender || relayout)
            win_apply_prefs();
    }
    if (kind == PB_SAVE) {
        if (!prefs_save(&prefs))
            alert_rs(S_AL_PREFS_WRITE, inf_path);
        prefs_close();
    }
}
