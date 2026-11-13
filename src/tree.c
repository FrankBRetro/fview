/* tree.c - FView's object trees, from its resource file (FVIEW.RSC or a
 * translation, see rsc/mkrsc.py): the menu bar, the Preferences tree,
 * the progress panel and the icon of iconified windows. */
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "gemui.h"
#include "fvrsc.h"

#ifndef FL3DIND
#define FL3DIND 0x0200
#define FL3DBAK 0x0400
#endif
#define FL3DACT 0x0600

/* ----------------------------------------------------------------- menu */

/* Laid out the Atari way by mkrsc.py: the program's own menu first
   (About, then the accessories), File, View, Picture, Options last;
   two spaces before every item, its shortcut right-aligned with a space
   after it, "..." on items that ask for more, separators of dashes. The
   bar and the drop-downs are made as wide as the screen here.         */
OBJECT *build_menu(void)
{
    OBJECT *m = rs_tree(T_MENU);
    int box;
    if (!m)
        return 0;
    m[0].ob_width = scr_w;
    m[1].ob_width = scr_w;
    m[MB_0 - 1].ob_width = scr_w;             /* the drop-downs' area */
    /* a drop-down that would run off a narrow screen moves left */
    for (box = m[MB_0 - 1].ob_head; box > 0 && box != MB_0 - 1; box = m[box].ob_next)
        if (m[box].ob_x + m[box].ob_width > scr_w)
            m[box].ob_x = scr_w - m[box].ob_width;
    return m;
}

/* ------------------------------------------------------- progress panel */

void build_progress(OBJECT *t, char *text, char *stat)
{
    OBJECT *src = rs_tree(T_PROGRESS);
    /* bar colour: blue where the first 16 pens can't be redefined by a
       picture's palette (256 colours, true colour), black otherwise    */
    long fill = (scr_planes >= 8 && prefs.keep_sys) ? 0x00001174L : 0x00001171L;
    memcpy(t, src, PROGRESS_NOBJ * sizeof(OBJECT));
    t[0].ob_flags = aes_3d ? FL3DBAK : 0;
    t[0].ob_spec.index = aes_3d ? 0x00001100L : 0x00011100L;
    t[1].ob_spec.free_string = text;
    /* sunken trough for the bar on 3D AESes, a plain outline elsewhere */
    t[2].ob_flags = aes_3d ? FL3DIND : 0;
    t[2].ob_state = aes_3d ? SELECTED : 0;
    t[3].ob_spec.index = fill;
    t[4].ob_spec.free_string = stat;
    t[5].ob_flags = SELECTABLE | EXIT | DEFAULT | LASTOB | (aes_3d ? FL3DACT : 0);
    t[5].ob_width = 10 * cell_w;
    t[5].ob_height = cell_h + 4;
}

/* ----------------------------------------------------------------- icon */

OBJECT *build_icon(char *label)
{
    OBJECT *t = rs_tree(T_ICON);
    if (t)
        t[1].ob_spec.iconblk->ib_ptext = label;
    return t;
}

/* ------------------------------------------------------ preferences tree */

#define PF(m) offsetof(PREFS, m)

/* what each object of the Preferences tree sets: line n is object n */
typedef struct {
    int  field;          /* offset into PREFS, -1 = label/button        */
    int  val;            /* radio value, -1 = on/off                    */
    int  kind;           /* PB_* for action buttons                     */
} PITEM;

static const PITEM pitems[] = {
#include "fvprefs.inc"
};
#define NPITEMS ((int)(sizeof pitems / sizeof pitems[0]))

OBJECT *build_prefs(void)
{
    return rs_dialog(T_PREFS);
}

static char *pfield(PREFS *p, int off) { return (char *)p + off; }

void prefs_to_tree(OBJECT *t, const PREFS *p)
{
    int i;
    for (i = 1; i < NPITEMS; i++) {
        const PITEM *it = &pitems[i];
        if (it->field < 0)
            continue;
        {
            int v = *pfield((PREFS *)p, it->field);
            int on = it->val < 0 ? v != 0 : v == it->val;
            if (on) t[i].ob_state |= SELECTED; else t[i].ob_state &= ~SELECTED;
        }
    }
}

void tree_to_prefs(OBJECT *t, PREFS *p)
{
    int i;
    for (i = 1; i < NPITEMS; i++) {
        const PITEM *it = &pitems[i];
        int sel = (t[i].ob_state & SELECTED) != 0;
        if (it->field < 0)
            continue;
        if (it->val < 0)
            *pfield(p, it->field) = (char)sel;
        else if (sel)
            *pfield(p, it->field) = (char)it->val;
    }
}

/* what kind of object was clicked */
int prefs_button_kind(int obj)
{
    if (obj <= 0 || obj >= NPITEMS)
        return PB_NONE;
    if (pitems[obj].kind)
        return pitems[obj].kind;
    return pitems[obj].field >= 0 ? PB_TOGGLE : PB_NONE;
}

/* radio siblings: the buttons that set the same field */
int prefs_radio_group(int obj, int *out, int max)
{
    int k, n = 0;
    if (obj <= 0 || obj >= NPITEMS || pitems[obj].val < 0)
        return 0;
    for (k = 1; k < NPITEMS && n < max; k++)
        if (pitems[k].field == pitems[obj].field && pitems[k].val >= 0)
            out[n++] = k;
    return n;
}
