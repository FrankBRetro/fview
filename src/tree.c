/* tree.c - object trees built at run time (menu, preferences dialog,
 * progress panel, icon) so fview needs no separate .RSC file. */
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
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

/* --------------------------------------------------------- dialog trees */

#ifndef WHITEBAK
#define WHITEBAK 0x0040
#endif

/* Turn a dialog tree laid out in characters into pixels, the same way on
 * every AES:
 * - rows on a 16-pixel font get 2 more pixels each, so neighbouring
 *   buttons' borders (outside the object on TOS, MultiTOS and MagiC 2D)
 *   don't touch;
 * - on/off and radio buttons (SELECTABLE, not EXIT) become MagiC-style
 *   check boxes and round radio buttons where the AES draws them
 *   (appl_getinfo 13: MagiC 3 and later, XaAES); elsewhere they stay
 *   buttons that go black when selected;
 * - other buttons get a little air above and below.
 * The root's own size is in characters too.                            */
void tree_finish(OBJECT *t, int n)
{
    int i, k, extra = cell_h >= 16 ? 2 : 0, rows = t[0].ob_height, right = 0;
    for (i = 0; i < n; i++)
        rsrc_obfix(t, i);
    for (i = 1; i < n; i++) {
        OBJECT *o = &t[i];
        if (o->ob_type == G_BUTTON && aes_xbutton && (o->ob_flags & SELECTABLE) &&
            !(o->ob_flags & (EXIT | TOUCHEXIT | DEFAULT))) {
            /* MagiC draws the box, then the text 3 characters in: one
               character wider than the button was, and the rest of the
               row moves along                                          */
            o->ob_state = (o->ob_state & 0x00FF) | WHITEBAK | 0xFF00;
            for (k = 1; k < n; k++)       /* plus half a character of gap */
                if (t[k].ob_y / cell_h == o->ob_y / cell_h && t[k].ob_x > o->ob_x)
                    t[k].ob_x += cell_w + cell_w / 2;
            o->ob_width += cell_w;
        } else if (o->ob_type == G_BUTTON) {
            o->ob_y += 2;                       /* a little air */
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

/* ----------------------------------------------------------------- menu */

static OBJECT menu[MENU_NOBJ];

/* Laid out the Atari way: the program's own menu first (About, then the
   accessories), File, View, the program's menus, Options last. Every
   item starts with two spaces (room for a tick), ends in its shortcut
   right-aligned with a space after it; "..." marks items that ask for
   more before they act; separators are disabled lines of dashes.
   Shortcuts: ^ Control, \001 (the arrow) Shift + Control.            */
static const char *titles[] = { " FView ", " File ", " View ", " Picture ", " Options " };
static const char *desk_items[] = {
    "  About FView...     ", "---------------------",
    "  Desk Accessory 1   ", "  Desk Accessory 2   ", "  Desk Accessory 3   ",
    "  Desk Accessory 4   ", "  Desk Accessory 5   ", "  Desk Accessory 6   "
};
static const char *file_items[] = {
    "  Open...                ^O ", "  Close                  ^U ",
    "----------------------------",
    "  Save IMG original...   ^S ", "  Save IMG as shown...  \001^S ",
    "----------------------------",
    "  Print...               ^P ",
    "----------------------------", "  Quit                   ^Q "
};
static const char *view_items[] = {
    "  Real size / 100%       ^1 ", "  Fit to window          ^2 ",
    "  Zoom in                 + ", "  Zoom out                - ",
    "----------------------------",
    "  Previous page          ^B ", "  Next page              ^N ",
    "  First page       Clr/Home ", "  Last page       \001Clr/Home ",
    "----------------------------", "  Cycle windows          ^W "
};
static const char *pic_items[] = {
    "  Duplicate              ^D ", "  Dithering...           ^T ",
    "  Reduce colours...      ^K ", "  Resize...              ^R ",
    "  Count colours          ^I ",
    "----------------------------",
    "  Load palette...        ^L ", "  Save palette...       \001^L ",
    "----------------------------",
    "  Share one palette      ^G "
};
static const char *opt_items[] = { "  Preferences...         ^E " };

OBJECT *build_menu(void)
{
    int i, idx, x, ncol = scr_w / (cell_w ? cell_w : 8);
    int tx[NMENUS];
    const char **lists[NMENUS];
    int counts[NMENUS];

    lists[0] = desk_items; counts[0] = 8;
    lists[1] = file_items; counts[1] = 9;
    lists[2] = view_items; counts[2] = 11;
    lists[3] = pic_items;  counts[3] = 10;
    lists[4] = opt_items;  counts[4] = 1;

    obj_add(menu, 0, -1, G_IBOX, 0, 0, 0L, 0, 0, ncol, 25);
    obj_add(menu, 1, 0, G_BOX, 0, 0, 0x00001100L, 0, 0, ncol, 0x0201);
    x = 0;
    for (i = 0; i < NMENUS; i++) {
        tx[i] = x;
        x += (int)strlen(titles[i]);
    }
    obj_add(menu, 2, 1, G_IBOX, 0, 0, 0L, 2, 0, x, 0x0301);
    for (i = 0; i < NMENUS; i++)
        obj_add(menu, 3 + i, 2, G_TITLE, 0, 0, (long)titles[i], tx[i], 0,
                (int)strlen(titles[i]), 0x0301);
    obj_add(menu, 3 + NMENUS, 0, G_IBOX, 0, 0, 0L, 0, 0x0301, ncol, 19);
    idx = 4 + NMENUS;
    for (i = 0; i < NMENUS; i++) {
        int k, w = 0, box = idx, bx = 2 + tx[i];
        for (k = 0; k < counts[i]; k++)
            if ((int)strlen(lists[i][k]) > w)
                w = (int)strlen(lists[i][k]);
        if (bx + w > ncol)
            bx = ncol - w;
        obj_add(menu, idx++, 3 + NMENUS, G_BOX, 0, 0, 0x00FF1100L, bx, 0, w, counts[i]);
        for (k = 0; k < counts[i]; k++) {
            int sep = lists[i][k][0] == '-';
            obj_add(menu, idx++, box, G_STRING, 0, sep ? DISABLED : 0,
                    (long)lists[i][k], 0, k, w, 1);
        }
    }
    menu[idx - 1].ob_flags |= LASTOB;
    for (i = 0; i < idx; i++)
        rsrc_obfix(menu, i);
    /* the drop-down area must cover the screen below the bar */
    menu[0].ob_width = scr_w;
    menu[1].ob_width = scr_w;
    return menu;
}

/* ------------------------------------------------------- progress panel */

#ifndef FL3DIND
#define FL3DIND 0x0200
#define FL3DBAK 0x0400
#endif
#define FL3DACT 0x0600

void build_progress(OBJECT *t, char *text, char *stat)
{
    /* bar colour: blue where the first 16 pens can't be redefined by a
       picture's palette (256 colours, true colour), black otherwise    */
    long fill = (scr_planes >= 8 && prefs.keep_sys) ? 0x00001174L : 0x00001171L;
    int bak = aes_3d ? FL3DBAK : 0;
    obj_add(t, 0, -1, G_BOX, bak, 0, aes_3d ? 0x00001100L : 0x00011100L, 0, 0, 100, 60);
    obj_add(t, 1, 0, G_STRING, 0, 0, (long)text, 8, 8, 80, cell_h);
    /* sunken trough for the bar on 3D AESes, a plain outline elsewhere */
    obj_add(t, 2, 0, G_BOX, aes_3d ? FL3DIND : 0, aes_3d ? SELECTED : 0,
            0x00FF1100L, 8, 8, 80, 12);
    obj_add(t, 3, 2, G_BOX, 0, 0, fill, 0, 0, 0, 12);
    obj_add(t, 4, 0, G_STRING, 0, 0, (long)stat, 8, 8, 80, cell_h);
    obj_add(t, 5, 0, G_BUTTON, SELECTABLE | EXIT | DEFAULT | LASTOB | (aes_3d ? FL3DACT : 0),
            0, (long)"Cancel", 8, 8, 10 * cell_w, cell_h + 4);
}

/* ----------------------------------------------------------------- icon */

static short ic_data[64], ic_mask[64];
static ICONBLK ic_blk;
static OBJECT ic_tree[2];

static void ic_set(int x, int y)
{
    if (x >= 0 && x < 32 && y >= 0 && y < 32)
        ic_data[y * 2 + (x >> 4)] |= (short)(0x8000U >> (x & 15));
}

OBJECT *build_icon(char *label)
{
    int x, y;
    memset(ic_data, 0, sizeof ic_data);
    /* picture frame */
    for (x = 2; x < 30; x++) { ic_set(x, 3); ic_set(x, 4); ic_set(x, 27); ic_set(x, 28); }
    for (y = 3; y < 29; y++) { ic_set(2, y); ic_set(3, y); ic_set(28, y); ic_set(29, y); }
    /* sun */
    for (y = -3; y <= 3; y++)
        for (x = -3; x <= 3; x++)
            if (x * x + y * y <= 10)
                ic_set(21 + x, 10 + y);
    /* mountains */
    for (y = 0; y < 12; y++)
        for (x = 0; x <= y; x++) {
            ic_set(11 - x, 14 + y); ic_set(11 + x, 14 + y);
            if (y > 4) { ic_set(19 - (x - 5), 14 + y); ic_set(19 + (x - 5), 14 + y); }
        }
    for (y = 0; y < 32; y++) {
        ic_mask[y * 2] = (y >= 3 && y <= 28) ? 0x3FFF : 0;
        ic_mask[y * 2 + 1] = (y >= 3 && y <= 28) ? (short)0xFFFC : 0;
    }
    memset(&ic_blk, 0, sizeof ic_blk);
    ic_blk.ib_pmask = ic_mask;
    ic_blk.ib_pdata = ic_data;
    ic_blk.ib_ptext = label;
    ic_blk.ib_char = 0x1000;
    ic_blk.ib_xicon = 16; ic_blk.ib_yicon = 0;
    ic_blk.ib_wicon = 32; ic_blk.ib_hicon = 32;
    ic_blk.ib_xtext = 0; ic_blk.ib_ytext = 32;
    ic_blk.ib_wtext = 64; ic_blk.ib_htext = 8;
    obj_add(ic_tree, 0, -1, G_IBOX, 0, 0, 0L, 0, 0, 64, 40);
    obj_add(ic_tree, 1, 0, G_ICON, LASTOB, 0, (long)&ic_blk, 0, 0, 64, 40);
    return ic_tree;
}

/* ------------------------------------------------------ preferences tree */

#define PF(m) offsetof(PREFS, m)

typedef struct {
    int  row;
    const char *label;
    int  field;          /* offset into PREFS, -1 = label/button        */
    int  val;            /* radio value, -1 = toggle                    */
    int  kind;           /* PB_* for action buttons                     */
} PITEM;

static const PITEM pitems[] = {
    { 0, "General",      -1, 0, 0 },
    { 0, "Start iconified", PF(start_iconified), -1, 0 },
    { 0, "MiNT workers", PF(use_workers), -1, 0 },
    { 1, "Dither",       -1, 0, 0 },
    { 1, "Floyd-S.",     PF(dither), DITH_FS, 0 },
    { 1, "Atkinson",     PF(dither), DITH_ATKINSON, 0 },
    { 1, "Ordered",      PF(dither), DITH_ORDERED, 0 },
    { 1, "None",         PF(dither), DITH_NONE, 0 },
    { 1, "Serpentine",   PF(serpentine), -1, 0 },
    { 2, "Open at",      -1, 0, 0 },
    { 2, "Real size",    PF(zoom), ZOOM_REAL, 0 },
    { 2, "Fit window",   PF(zoom), ZOOM_FIT, 0 },
    { 2, "Smooth shrink", PF(smooth), -1, 0 },
    { 3, "Palette",      -1, 0, 0 },
    { 3, "Per picture",  PF(palmode), PAL_OPTIMISE, 0 },
    { 3, "System",       PF(palmode), PAL_SYSTEM, 0 },
    { 3, "Keep GEM colours", PF(keep_sys), -1, 0 },
    { 4, "Background",   -1, 0, 0 },
    { 4, "White",        PF(bg), BG_WHITE, 0 },
    { 4, "Grey",         PF(bg), BG_GREY, 0 },
    { 4, "Black",        PF(bg), BG_BLACK, 0 },
    { 5, "GIF / PNG",    -1, 0, 0 },
    { 5, "GIF transparency", PF(gif_trans), -1, 0 },
    { 5, "PNG alpha",    PF(png_alpha), -1, 0 },
    { 6, "IFF / JPEG",   -1, 0, 0 },
    { 6, "IFF mask",     PF(iff_mask), -1, 0 },
    { 6, "HAM/EHB",      PF(iff_ham), -1, 0 },
    { 6, "JPEG grey on mono", PF(jpg_grey_mono), -1, 0 },
    { 7, "GEM IMG",      -1, 0, 0 },
    { 7, "System palette", PF(img_syspal), -1, 0 },
    { 7, "XIMG VDI pen order", PF(img_vdiorder), -1, 0 },
    { 8, "Text",         -1, 0, 0 },
    { 8, "Tab 4",        PF(tabw), 4, 0 },
    { 8, "Tab 8",        PF(tabw), 8, 0 },
    { 8, "Wrap long lines", PF(wrap), -1, 0 },
    { 9, "8-bit text",   -1, 0, 0 },
    { 9, "Atari",        PF(charset), CS_ATARI, 0 },
    { 9, "Latin-1",      PF(charset), CS_LATIN1, 0 },
    { 9, "CP1252",       PF(charset), CS_CP1252, 0 },
    { 10, "Markdown/RTF", -1, 0, 0 },
    { 10, "Show markup", PF(md_markup), -1, 0 },
    { 10, "RTF pictures", PF(rtf_pict), -1, 0 },
    { 11, "PDF",         -1, 0, 0 },
    { 11, "Fit width",   PF(pdf_zoom), PDFZ_FITW, 0 },
    { 11, "100%",        PF(pdf_zoom), PDFZ_100, 0 },
    { 11, "Graphics",    PF(pdf_gfx), -1, 0 },
    { 11, "Images",      PF(pdf_imgbox), -1, 0 },
    { 12, "Print size",  -1, 0, 0 },
    { 12, "Fit page",    PF(prt_scale), PRS_FIT, 0 },
    { 12, "Real size",   PF(prt_scale), PRS_REAL, 0 },
    { 12, "Header",      PF(prt_header), -1, 0 },
    { 13, "Print dither", -1, 0, 0 },
    { 13, "Floyd-S.",    PF(prt_dither), DITH_FS, 0 },
    { 13, "Atkinson",    PF(prt_dither), DITH_ATKINSON, 0 },
    { 13, "Ordered",     PF(prt_dither), DITH_ORDERED, 0 },
    { 14, "Paletted pics", -1, 0, 0 },
    { 14, "Dither them on 256+ colour screens", PF(dither_pal), -1, 0 },
    { 15, "Menu colours", -1, 0, 0 },
    { 15, "Keep menu pens for the desktop", PF(ui_pens), -1, 0 },
    { 16, "Save",        -1, 0, PB_SAVE },
    { 16, "Apply",       -1, 0, PB_APPLY },
    { 16, "Close",       -1, 0, PB_CLOSE },
};
#define NPITEMS ((int)(sizeof pitems / sizeof pitems[0]))

static OBJECT ptree[NPITEMS + 2];
static int pobj[NPITEMS];             /* object index of each item     */
static int pitem_of[NPITEMS + 2];     /* item of each object           */

OBJECT *build_prefs(void)
{
    int i, n = 1, xcol = 0, lastrow = -1, w = 0, labw = 0;
    for (i = 0; i < NPITEMS; i++)              /* labels: the longest + 2 */
        if (pitems[i].field < 0 && !pitems[i].kind &&
            (int)strlen(pitems[i].label) + 4 > labw)
            labw = (int)strlen(pitems[i].label) + 4;
    obj_add(ptree, 0, -1, G_BOX, 0, OUTLINED, 0x00021100L, 0, 0, 10, 18);
    for (i = 0; i < NPITEMS; i++) {
        const PITEM *p = &pitems[i];
        int len = (int)strlen(p->label);
        if (p->row != lastrow) {
            xcol = 2;
            lastrow = p->row;
        }
        pitem_of[n] = i;
        pobj[i] = n;
        if (p->field < 0 && !p->kind) {
            obj_add(ptree, n, 0, G_STRING, 0, 0, (long)p->label, xcol, p->row + 1, len, 1);
            xcol = labw;
        } else if (p->kind) {
            int bx = labw + (p->kind == PB_SAVE ? 4 : p->kind == PB_APPLY ? 15 : 26);
            obj_add(ptree, n, 0, G_BUTTON, SELECTABLE | EXIT | (p->kind == PB_SAVE ? DEFAULT : 0),
                    0, (long)p->label, bx, p->row + 1, 9, 1);
        } else {
            obj_add(ptree, n, 0, G_BUTTON, SELECTABLE | (p->val >= 0 ? RBUTTON : 0),
                    0, (long)p->label, xcol, p->row + 1, len + 2, 1);
            xcol += len + 3;
        }
        if (xcol > w) w = xcol;
        n++;
    }
    ptree[n - 1].ob_flags |= LASTOB;
    ptree[0].ob_width = w + 1;
    ptree[0].ob_height = pitems[NPITEMS - 1].row + 3;
    tree_finish(ptree, n);
    return ptree;
}

static char *pfield(PREFS *p, int off) { return (char *)p + off; }

void prefs_to_tree(OBJECT *t, const PREFS *p)
{
    int i;
    for (i = 0; i < NPITEMS; i++) {
        const PITEM *it = &pitems[i];
        OBJECT *o = &t[pobj[i]];
        if (it->field < 0)
            continue;
        {
            int v = *pfield((PREFS *)p, it->field);
            int on = it->val < 0 ? v != 0 : v == it->val;
            if (on) o->ob_state |= SELECTED; else o->ob_state &= ~SELECTED;
        }
    }
}

void tree_to_prefs(OBJECT *t, PREFS *p)
{
    int i;
    for (i = 0; i < NPITEMS; i++) {
        const PITEM *it = &pitems[i];
        int sel = (t[pobj[i]].ob_state & SELECTED) != 0;
        if (it->field < 0)
            continue;
        if (it->val < 0)
            *pfield(p, it->field) = (char)sel;
        else if (sel)
            *pfield(p, it->field) = (char)it->val;
    }
}

/* click on object: update states, return what kind of object it was.
   *redraw receives a list of objects whose state changed (-1 ends) */
int prefs_button_kind(int obj)
{
    int i;
    if (obj <= 0 || obj > NPITEMS)
        return PB_NONE;
    i = pitem_of[obj];
    if (pitems[i].kind)
        return pitems[i].kind;
    return pitems[i].field >= 0 ? PB_TOGGLE : PB_NONE;
}

/* radio siblings: same row and same field */
int prefs_radio_group(int obj, int *out, int max)
{
    int i = pitem_of[obj], k, n = 0;
    if (pitems[i].val < 0)
        return 0;
    for (k = 0; k < NPITEMS && n < max; k++)
        if (pitems[k].field == pitems[i].field && pitems[k].row == pitems[i].row)
            out[n++] = pobj[k];
    return n;
}
