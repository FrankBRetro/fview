/* Copyright (c) 2026 Francis C Bullen. SPDX-License-Identifier: MIT (see LICENSE) */
/*
 * gem.h - the part of GEMLib's API these programs use, for PC GEM (FreeGEM /
 *         OpenGEM) with DJGPP. The front end (gem*.c) is compiled unchanged
 *         against this; pcgem/pgaes.c and pgvdi.c implement it on top of
 *         John Elliott's DJGPP GEM bindings (LIBGEM.A), which run GEM in
 *         real mode below 1 MB.
 *
 * Every GEMLib name is #defined to a pg_ function so it doesn't clash with
 * the bindings' own appl_init, objc_draw ... in LIBGEM.A.
 *
 * Object trees, TEDINFOs, MFDBs etc. live in normal 32-bit memory; the
 * layouts match GEM's (packed, 4-byte pointers in place of far pointers)
 * so the bindings' dj_tree_32to16() & co. can copy them down and back.
 */
#ifndef PCGEM_GEM_H
#define PCGEM_GEM_H

#define __CDECL
#define PCGEM 1

/* ---- structures ---------------------------------------------------------- */
#define PG_PACK __attribute__((packed))

typedef struct { short g_x, g_y, g_w, g_h; } GRECT;

typedef struct text_edinfo {
    char *te_ptext, *te_ptmplt, *te_pvalid;
    short te_font, te_fontid, te_just, te_color, te_fontsize, te_thickness, te_txtlen, te_tmplen;
} PG_PACK TEDINFO;

typedef struct icon_block {
    short *ib_pmask, *ib_pdata;
    char  *ib_ptext;
    short ib_char, ib_xchar, ib_ychar, ib_xicon, ib_yicon, ib_wicon, ib_hicon;
    short ib_xtext, ib_ytext, ib_wtext, ib_htext;
} PG_PACK ICONBLK;

typedef struct bit_block {
    short *bi_pdata;
    short bi_wb, bi_hl, bi_x, bi_y, bi_color;
} PG_PACK BITBLK;

struct parm_block;
typedef struct user_block {
    short (*ub_code)(struct parm_block *);
    long  ub_parm;
} PG_PACK USERBLK;

typedef union obspecptr {
    long      index;
    union obspecptr *indirect;
    struct { unsigned char character, framesize; unsigned short framecol:4, textcol:4, textmode:1, fillpattern:3, interiorcol:4; } obspec;
    TEDINFO  *tedinfo;
    ICONBLK  *iconblk;
    BITBLK   *bitblk;
    USERBLK  *userblk;
    char     *free_string;
} OBSPEC;

typedef struct object {
    short ob_next, ob_head, ob_tail;
    unsigned short ob_type, ob_flags, ob_state;
    OBSPEC ob_spec;
    short ob_x, ob_y, ob_width, ob_height;
} PG_PACK OBJECT;

typedef struct parm_block {
    OBJECT *pb_tree;
    short pb_obj, pb_prevstate, pb_currstate;
    short pb_x, pb_y, pb_w, pb_h, pb_xc, pb_yc, pb_wc, pb_hc;
    long  pb_parm;
} PG_PACK PARMBLK;

typedef struct memform {
    void *fd_addr;
    short fd_w, fd_h, fd_wdwidth, fd_stand, fd_nplanes, fd_r1, fd_r2, fd_r3;
} PG_PACK MFDB;

typedef struct { OBJECT *mn_tree; short mn_menu, mn_item, mn_scroll, mn_keystate; } MENU;

#ifndef PG_HOSTCHECK   /* 32-bit pointers: true with DJGPP, not on a 64-bit host */
typedef char pg_obj_size_ok[sizeof(OBJECT) == 24 && sizeof(TEDINFO) == 28 && sizeof(BITBLK) == 14 &&
                            sizeof(ICONBLK) == 34 && sizeof(MFDB) == 20 ? 1 : -1];
#endif

extern short aes_global[15];
#define gl_apid aes_global[2]

/* ---- AES constants -------------------------------------------------------- */
/* object types */
#define G_BOX 20
#define G_TEXT 21
#define G_BOXTEXT 22
#define G_IMAGE 23
#define G_USERDEF 24
#define G_IBOX 25
#define G_BUTTON 26
#define G_BOXCHAR 27
#define G_STRING 28
#define G_FTEXT 29
#define G_FBOXTEXT 30
#define G_ICON 31
#define G_TITLE 32
#define G_CICON 33
/* flags */
#define OF_NONE 0x0000
#define OF_SELECTABLE 0x0001
#define OF_DEFAULT 0x0002
#define OF_EXIT 0x0004
#define OF_EDITABLE 0x0008
#define OF_RBUTTON 0x0010
#define OF_LASTOB 0x0020
#define OF_TOUCHEXIT 0x0040
#define OF_HIDETREE 0x0080
#define OF_INDIRECT 0x0100
#define OF_FL3DIND 0x0200
#define OF_FL3DBAK 0x0400
#define OF_FL3DACT 0x0600
#define OF_FL3DMASK 0x0600
#define OF_SUBMENU 0x0800
#define NONE OF_NONE
#define SELECTABLE OF_SELECTABLE
#define DEFAULT OF_DEFAULT
#define EXIT OF_EXIT
#define EDITABLE OF_EDITABLE
#define RBUTTON OF_RBUTTON
#define LASTOB OF_LASTOB
#define TOUCHEXIT OF_TOUCHEXIT
#define HIDETREE OF_HIDETREE
#define INDIRECT OF_INDIRECT
#define FL3DIND 0x0200
#define FL3DBAK 0x0400
#define FL3DACT 0x0600
/* states */
#define OS_NORMAL 0x00
#define OS_SELECTED 0x01
#define OS_CROSSED 0x02
#define OS_CHECKED 0x04
#define OS_DISABLED 0x08
#define OS_OUTLINED 0x10
#define OS_SHADOWED 0x20
#define OS_WHITEBAK 0x40
#define OS_DRAW3D 0x80
#define NORMAL OS_NORMAL
#define SELECTED OS_SELECTED
#define CROSSED OS_CROSSED
#define CHECKED OS_CHECKED
#define DISABLED OS_DISABLED
#define OUTLINED OS_OUTLINED
#define SHADOWED OS_SHADOWED
#define ROOT 0
#define MAX_DEPTH 8
#define IBM 3
#define SMALL 5
#define TE_LEFT 0
#define TE_RIGHT 1
#define TE_CNTR 2
/* objc_edit */
#define ED_START 0
#define ED_INIT 1
#define ED_CHAR 2
#define ED_END 3
/* form_dial */
#define FMD_START 0
#define FMD_GROW 1
#define FMD_SHRINK 2
#define FMD_FINISH 3
/* graf_mouse */
#define ARROW 0
#define TEXT_CRSR 1
#define BUSYBEE 2
#define BUSY_BEE 2
#define HOURGLASS 2
#define POINT_HAND 3
#define FLAT_HAND 4
#define THIN_CROSS 5
#define THICK_CROSS 6
#define OUTLN_CROSS 7
#define USER_DEF 255
#define M_OFF 256
#define M_ON 257
#define M_SAVE 258
#define M_LAST 259
#define M_RESTORE 260
/* resources */
#define R_TREE 0
#define R_OBJECT 1
#define R_TEDINFO 2
#define R_ICONBLK 3
#define R_BITBLK 4
#define R_STRING 5
#define R_IMAGEDATA 6
#define R_OBSPEC 7
#define R_FRSTR 15
#define R_FRIMG 16
/* events */
#define MU_KEYBD 0x0001
#define MU_BUTTON 0x0002
#define MU_M1 0x0004
#define MU_M2 0x0008
#define MU_MESAG 0x0010
#define MU_TIMER 0x0020
#define K_RSHIFT 0x0001
#define K_LSHIFT 0x0002
#define K_CTRL 0x0004
#define K_ALT 0x0008
/* messages */
#define MN_SELECTED 10
#define WM_REDRAW 20
#define WM_TOPPED 21
#define WM_CLOSED 22
#define WM_FULLED 23
#define WM_ARROWED 24
#define WM_HSLID 25
#define WM_VSLID 26
#define WM_SIZED 27
#define WM_MOVED 28
#define WM_NEWTOP 29
#define WM_UNTOPPED 30
#define WM_ONTOP 31
#define WM_BOTTOMED 33
#define WM_ICONIFY 34
#define WM_UNICONIFY 35
#define WM_ALLICONIFY 36
#define AC_OPEN 40
#define AC_CLOSE 41
#define AP_TERM 50
#define AP_TFAIL 51
#define SHUT_COMPLETED 60
#define RESCHG_COMPLETED 61
#define AP_DRAGDROP 63
#define SH_WDRAW 72
#define CH_EXIT 90
#define WA_UPPAGE 0
#define WA_DNPAGE 1
#define WA_UPLINE 2
#define WA_DNLINE 3
#define WA_LFPAGE 4
#define WA_RTPAGE 5
#define WA_LFLINE 6
#define WA_RTLINE 7
#define WA_WHEEL 8
/* window parts */
#define NAME 0x0001
#define CLOSER 0x0002
#define FULLER 0x0004
#define MOVER 0x0008
#define INFO 0x0010
#define SIZER 0x0020
#define UPARROW 0x0040
#define DNARROW 0x0080
#define VSLIDE 0x0100
#define LFARROW 0x0200
#define RTARROW 0x0400
#define HSLIDE 0x0800
#define SMALLER 0x4000
/* wind_get/set fields */
#define WF_KIND 1
#define WF_NAME 2
#define WF_INFO 3
#define WF_WORKXYWH 4
#define WF_CURRXYWH 5
#define WF_PREVXYWH 6
#define WF_FULLXYWH 7
#define WF_HSLIDE 8
#define WF_VSLIDE 9
#define WF_TOP 10
#define WF_FIRSTXYWH 11
#define WF_NEXTXYWH 12
#define WF_NEWDESK 14
#define WF_HSLSIZE 15
#define WF_VSLSIZE 16
#define WF_SCREEN 17
#define WF_COLOR 18
#define WF_DCOLOR 19
#define WF_OWNER 20
#define WF_BEVENT 24
#define WF_BOTTOM 25
#define WF_ICONIFY 26
#define WF_UNICONIFY 27
#define WF_UNICONIFYXYWH 28
#define WF_TOOLBAR 30
#define WF_FTOOLBAR 31
#define WF_NTOOLBAR 32
#define WC_BORDER 0
#define WC_WORK 1
#define END_UPDATE 0
#define BEG_UPDATE 1
#define END_MCTRL 2
#define BEG_MCTRL 3
/* appl_getinfo */
#define AES_LARGEFONT 0
#define AES_SMALLFONT 1
#define AES_SYSTEM 2
#define AES_LANGUAGE 3
#define AES_PROCESS 4
#define AES_PCGEM 5
#define AES_INQUIRE 6
#define AES_MOUSE 8
#define AES_MENU 9
#define AES_SHELL 10
#define AES_WINDOW 11
#define AES_MESSAGE 12
#define AES_OBJECT 13
#define AES_FORM 14

/* ---- AES functions -------------------------------------------------------- */
#define appl_init      pg_appl_init
#define appl_exit      pg_appl_exit
#define appl_find      pg_appl_find
#define appl_write     pg_appl_write
#define appl_getinfo   pg_appl_getinfo
#define evnt_multi     pg_evnt_multi
#define evnt_timer     pg_evnt_timer
#define menu_bar       pg_menu_bar
#define menu_icheck    pg_menu_icheck
#define menu_ienable   pg_menu_ienable
#define menu_tnormal   pg_menu_tnormal
#define menu_popup     pg_menu_popup
#define objc_draw      pg_objc_draw
#define objc_find      pg_objc_find
#define objc_offset    pg_objc_offset
#define objc_edit      pg_objc_edit
#define objc_change    pg_objc_change
#define form_do        pg_form_do
#define form_dial      pg_form_dial
#define form_alert     pg_form_alert
#define form_center    pg_form_center
#define form_keybd     pg_form_keybd
#define form_button    pg_form_button
#define graf_handle    pg_graf_handle
#define graf_mouse     pg_graf_mouse
#define graf_mkstate   pg_graf_mkstate
#define graf_watchbox  pg_graf_watchbox
#define graf_slidebox  pg_graf_slidebox
#define scrp_read      pg_scrp_read
#define scrp_write     pg_scrp_write
#define fsel_input     pg_fsel_input
#define fsel_exinput   pg_fsel_exinput
#define wind_create    pg_wind_create
#define wind_open      pg_wind_open
#define wind_close     pg_wind_close
#define wind_delete    pg_wind_delete
#define wind_get       pg_wind_get
#define wind_set       pg_wind_set
#define wind_set_str   pg_wind_set_str
#define wind_find      pg_wind_find
#define wind_update    pg_wind_update
#define wind_calc      pg_wind_calc
#define rsrc_load      pg_rsrc_load
#define rsrc_free      pg_rsrc_free
#define rsrc_gaddr     pg_rsrc_gaddr
#define shel_write     pg_shel_write
#define shel_envrn     pg_shel_envrn

short appl_init(void);
short appl_exit(void);
short appl_find(const char *name);
short appl_write(short id, short len, void *buf);
short appl_getinfo(short type, short *o1, short *o2, short *o3, short *o4);
short evnt_multi(short type, short clicks, short mask, short state,
                 short m1flag, short m1x, short m1y, short m1w, short m1h,
                 short m2flag, short m2x, short m2y, short m2w, short m2h,
                 short *msg, unsigned long ms,
                 short *mx, short *my, short *mb, short *ks, short *key, short *nclicks);
short evnt_timer(unsigned long ms);
short menu_bar(OBJECT *tree, short show);
short menu_icheck(OBJECT *tree, short item, short check);
short menu_ienable(OBJECT *tree, short item, short enable);
short menu_tnormal(OBJECT *tree, short title, short normal);
short menu_popup(MENU *menu, short x, short y, MENU *out);
short objc_draw(OBJECT *tree, short start, short depth, short x, short y, short w, short h);
short objc_find(OBJECT *tree, short start, short depth, short x, short y);
short objc_offset(OBJECT *tree, short obj, short *x, short *y);
short objc_edit(OBJECT *tree, short obj, short ch, short *idx, short kind);
short objc_change(OBJECT *tree, short obj, short res, short x, short y, short w, short h, short state, short redraw);
short form_do(OBJECT *tree, short start);
short form_dial(short op, short lx, short ly, short lw, short lh, short bx, short by, short bw, short bh);
short form_alert(short def, const char *text);
short form_center(OBJECT *tree, short *x, short *y, short *w, short *h);
short form_keybd(OBJECT *tree, short obj, short next, short ch, short *onext, short *och);
short form_button(OBJECT *tree, short obj, short clicks, short *onext);
short graf_handle(short *wchar, short *hchar, short *wbox, short *hbox);
short graf_mouse(short form, void *shape);
short graf_mkstate(short *mx, short *my, short *mb, short *ks);
short graf_watchbox(OBJECT *tree, short obj, short instate, short outstate);
short graf_slidebox(OBJECT *tree, short parent, short obj, short vertical);
short scrp_read(char *path);
short scrp_write(const char *path);
short fsel_input(char *path, char *name, short *button);
short fsel_exinput(char *path, char *name, short *button, const char *title);
short wind_create(short kind, short x, short y, short w, short h);
short wind_open(short h, short x, short y, short w, short hh);
short wind_close(short h);
short wind_delete(short h);
short wind_get(short h, short field, short *o1, short *o2, short *o3, short *o4);
short wind_set(short h, short field, short i1, short i2, short i3, short i4);
short wind_set_str(short h, short field, const char *s);
short wind_find(short x, short y);
short wind_update(short mode);
short wind_calc(short type, short kind, short x, short y, short w, short h,
                short *ox, short *oy, short *ow, short *oh);
short rsrc_load(const char *name);
short rsrc_free(void);
short rsrc_gaddr(short type, short index, void *addr);
short shel_write(short mode, short graphic, short aes, const void *cmd, const void *tail);
short shel_envrn(char **value, const char *name);


/* ---- more of GEMLib (Fbench) --------------------------------------------- */
#define _AESversion    (aes_global[0])   /* 0x0120 on purpose: see pgaes.c */
extern short pg_aes_version;      /* the AES's real global[0] (0: not known) */
extern short pg_aes_numapps;      /* and its global[1] */
#define _AESnumapps    (aes_global[1])
#ifndef SWM_NEWMSG
#define SWM_NEWMSG     9
#endif
#ifndef NM_APTERM
#define NM_APTERM      0x0001
#endif
#define evnt_button    pg_evnt_button
#define menu_register  pg_menu_register
#define appl_yield     pg_appl_yield
#define _appl_yield    pg_appl_yield
#define rsrc_obfix     pg_rsrc_obfix
#define rc_intersect   pg_rc_intersect
#define shel_read      pg_shel_read
#define shel_find      pg_shel_find
short evnt_button(short clicks, short mask, short state, short *mx, short *my,
                  short *button, short *kstate);
short menu_register(short apid, const char *name);
short appl_yield(void);
short rsrc_obfix(OBJECT *tree, short obj);
short rc_intersect(const GRECT *src, GRECT *dst);
short shel_read(char *cmd, char *tail);
short shel_find(char *path);

/* GEMLib's GRECT forms */
static __inline__ short wind_get_grect(short h, short f, GRECT *r)
{ return wind_get(h, f, &r->g_x, &r->g_y, &r->g_w, &r->g_h); }
static __inline__ short wind_set_grect(short h, short f, const GRECT *r)
{ return wind_set(h, f, r->g_x, r->g_y, r->g_w, r->g_h); }
static __inline__ short wind_open_grect(short h, const GRECT *r)
{ return wind_open(h, r->g_x, r->g_y, r->g_w, r->g_h); }
static __inline__ short wind_create_grect(short kind, const GRECT *r)
{ return wind_create(kind, r->g_x, r->g_y, r->g_w, r->g_h); }
static __inline__ short wind_calc_grect(short type, short kind, const GRECT *in, GRECT *out)
{ return wind_calc(type, kind, in->g_x, in->g_y, in->g_w, in->g_h, &out->g_x, &out->g_y, &out->g_w, &out->g_h); }
static __inline__ short objc_draw_grect(OBJECT *t, short start, short depth, const GRECT *r)
{ return objc_draw(t, start, depth, r->g_x, r->g_y, r->g_w, r->g_h); }

/* ---- VDI ------------------------------------------------------------------ */
#include "vdi.h"

#endif
