/* gemui.h - GEM side of fview */
#ifndef GEMUI_H
#define GEMUI_H

#define __GEMLIB_OLDNAMES 1      /* SELECTED, LASTOB, ... */
#include <gem.h>
#include <gemx.h>
#include "fview.h"
#include "doc.h"

/* window records: allocated as needed and kept for reuse, so a VWIN
   pointer stays valid; how many windows can be open is up to the AES */
#define MAXWIN   256
#define APPNAME  "FView"
#define MARGIN   8

/* ------------------------------------------------------------------ jobs */

#include "job.h"

void job_init_system(void);      /* detects MiNT, shared memory, worker  */
int  job_start(JOB *j);          /* queue or start                       */
void job_cancel(JOB *j);
void job_poll(void);             /* read worker progress / results       */
int  job_any_active(void);
void job_run_pending(void);      /* TOS: run one queued job cooperatively */
void job_release(JOBRES *r, int slot);   /* free one result block        */
void job_kill_all(void);
extern int have_mint, have_shm, have_worker;
extern int coop_busy;            /* a cooperative job is executing       */

/* -------------------------------------------------------------- windows */

enum { ST_IDLE = 0, ST_LOADING, ST_RENDER, ST_SAVING, ST_PRINTING, ST_ERROR };

typedef struct {
    long  para;                  /* offset of the paragraph record       */
    u16   start, len;            /* byte range inside the paragraph text */
    u8    style, indent;         /* style: PS_* | align << 4             */
    u8    attr;                  /* attribute in effect at start         */
    u8    first;                 /* first line of the paragraph          */
    long  y;                     /* top in pixels                        */
    short h;                     /* height                               */
    /* rich text only */
    u8    rich, last;            /* last line of its paragraph           */
    u8    font, size, color;     /* state at the start of the line       */
    short base;                  /* baseline below the top               */
    short w;                     /* width of the text                    */
    short nsp;                   /* spaces between words (justify)       */
    short avail;                 /* width the line was laid out in       */
} TLINE;

/* GDOS fonts of one workstation */
#define MAXGF 96
typedef struct {
    int   handle, n, gdos, loaded, arb;
    short id[MAXGF];
    char  name[MAXGF][34], norm[MAXGF][34], face[MAXGF][34];
    u8    bold[MAXGF], ital[MAXGF], fam[MAXGF];
} FONTSET;

void     fonts_load(FONTSET *fs, int handle, int sysfonts);
void     fonts_unload(FONTSET *fs);
FONTSET *fonts_for(int handle);
int      font_pick(FONTSET *fs, const char *name, int family, int attr, int *fx);
int      font_size(FONTSET *fs, int handle, int halfpts);
extern FONTSET scrfonts;

typedef struct VWIN {
    int    used;
    int    handle;
    int    kind;
    int    ftype;
    int    state;
    int    iconified;            /* 1 by the AES, 2 our icon-sized window */
    char   path[256];
    char   title[80];
    char   info[160];
    char   errmsg[120];
    GRECT  work;
    GRECT  full;
    long   xoff, yoff;           /* scroll position (pixels)            */
    long   docw, doch;           /* content size (pixels)               */
    /* progress panel */
    OBJECT prog[7];
    char   ptext[64], pstat[40], ptitle[64];
    int    pdlg;                 /* progress dialog window, 0 = none     */
    long   pdone, ptotal;
    int    lastpct;
    JOB   *job;
    int    closing;              /* close once the job has stopped      */
    int    want_rerender;
    /* image */
    JOBRES img;                  /* original + palette                  */
    MFDB   disp;                 /* device format display image         */
    u8    *dispmem;
    int    zoom;                 /* ZOOM_REAL / ZOOM_FIT                */
    int    haspal;
    u8     pal[256 * 3];
    PREFS  wpf;                  /* this window's dithering and palette  */
    int    wpf_own;              /* set in its Dithering dialog: global
                                    preference changes leave it alone    */
    long   cnt_real, cnt_seen;   /* Count colours (cnt_real < 0: not yet) */
    int    cnt_used;             /* palette entries the pixels use        */
    int    copyno;               /* Duplicate: 2, 3, ...                  */
    /* documents */
    u8    *blob;
    long   bloblen;
    TLINE *lines;
    long   nlines;
    int    laywidth;             /* width used for the current layout   */
    /* pdf */
    long  *pageoff;
    long   npages, page;
    int    pdfzoom;              /* 0 = fit width, else percent          */
    int    prepst;               /* page images: 0 to do, 1 done, 2 boxes */
    int    need_prep;            /* build them (with progress) from the loop */
    int    need_layout;          /* lay a long text out (with progress) */
} VWIN;

extern VWIN *wins[MAXWIN];
extern int   nwins;           /* records allocated so far */
extern int  vdih, ap_id;
extern int  scr_w, scr_h, scr_planes, pix_w_um, pix_h_um;
extern int  cell_w, cell_h;      /* system font cell                     */
extern GRECT desk;
extern PREFS prefs;
extern DISPINFO dinfo;
extern int  quitting;
extern int  aes_iconify;         /* AES supports WF_ICONIFY             */
extern int  aes_3d;
extern int  aes_xbutton;      /* AES draws check boxes/round radio buttons */
extern int  aes_magic;        /* MagiC (MagX cookie)                      */
void tree_finish(OBJECT *t, int n);   /* chars -> pixels, buttons, rows */
extern int  screen_interleaved;  /* ST-style interleaved planes          */              /* AES draws 3D objects (TOS 4 / AES 3.4+) */

/* main.c */
void gui_dispatch_message(short *msg);
void gui_handle_key(int kstate, int key);
void gui_handle_button(int mx, int my, int mb, int ks, int clicks);
int  gui_coop_poll(void);        /* one non-blocking event pass         */
void update_pointer(void);
void open_files_from_string(char *s);
void open_file(const char *path);
void alertf(const char *fmt, ...);
int  file_select(char *path, char *name, const char *title);
int  prefs_radio_group(int obj, int *out, int max);
void menu_update(void);

/* win.c */
VWIN *win_new(void);
VWIN *win_find(int handle);
VWIN *win_top(void);
int   win_open(VWIN *w, int kind);
void  win_close(VWIN *w);
void  win_redraw(VWIN *w, GRECT *area);
void  win_redraw_all(VWIN *w);
void  win_post_redraw(VWIN *w);
void  win_sized(VWIN *w, GRECT *r);
void  win_moved(VWIN *w, GRECT *r);
void  win_fulled(VWIN *w);
void  win_arrow(VWIN *w, int what);
void  win_hslide(VWIN *w, int pos);
void  win_vslide(VWIN *w, int pos);
void  win_scroll_to(VWIN *w, long x, long y);
void  win_set_sliders(VWIN *w);
void  win_iconify(VWIN *w, GRECT *r);
void  win_uniconify(VWIN *w, GRECT *r);
void  win_fake_iconify(VWIN *w, GRECT *r);
void  win_fake_uniconify(VWIN *w);
void  win_progress(VWIN *w, long done, long total);
void  win_stage(VWIN *w, int stage);
void  win_note(VWIN *w, int kind, const char *text);   /* FArchive's log lines */
void  win_load(VWIN *w, const char *path);
void  win_job_finished(VWIN *w, JOB *j);
void  win_set_zoom(VWIN *w, int zoom);
void  win_page(VWIN *w, long page);
void  win_pdf_zoom(VWIN *w, int step);   /* -1 out, +1 in, 0 fit, 100 */
void  win_free_content(VWIN *w);
void  win_status(VWIN *w, const char *text);   /* progress panel's status line */
int   win_click(VWIN *w, int mx, int my, int clicks);
void  win_relayout(VWIN *w);
void  win_update_info(VWIN *w);
void  win_apply_prefs(void);
int   win_busy(VWIN *w);
int   win_over_cancel(VWIN *w, int mx, int my);
VWIN *win_find_dlg(int handle);
void  win_dlg_redraw(VWIN *w, GRECT *area);
void  win_dlg_click(VWIN *w, int mx, int my);
void  progdlg_close(VWIN *w);
void  win_save_img(VWIN *w, int dithered);
int   win_begin_op(VWIN *w, int state, const char *verb, PROG *pg);
void  win_end_op(VWIN *w);
void  win_shutdown(void);
/* picture tools (win.c) */
void  win_duplicate(VWIN *w);
void  win_reduce(VWIN *w, int ncol, const u8 *pal, int npal);  /* pal: map onto it */
void  win_resize(VWIN *w, long nw, long nh, int smooth);
void  win_count(VWIN *w);
void  win_rerender(VWIN *w);     /* after its wpf changed                */
void  win_load_palette(VWIN *w);
void  win_save_palette(VWIN *w);
void  win_share_palette(int on);
void  win_share_update(void);    /* windows came or went: new shared one */
int   win_tool_ok(VWIN *w);      /* a picture the tools can work on now  */
extern int share_pal;            /* Picture > Share one palette           */
/* picdlg.c: the Dithering window and the Reduce / Resize dialogs */
void  picdlg_open(VWIN *w);
void  picdlg_close(void);
int   picdlg_handle(int handle);
void  picdlg_redraw(GRECT *r);
void  picdlg_click(int mx, int my);
void  picdlg_moved(GRECT *r);
void  picdlg_top(void);
void  picdlg_refresh(VWIN *w);   /* w's settings or counts changed       */
void  picdlg_forget(VWIN *w);    /* w is going                           */
void  reduce_dialog(VWIN *w);
void  resize_dialog(VWIN *w);
int   modal_number(const char *title, const char *label, long *v, long lo, long hi);
void  win_prepare_pending(void);
const char *win_base_name(const char *p);

/* palette */
void  pal_init(void);
void  pal_check_top(void);
void  pal_restore(void);
void  pal_invalidate(void);
void  pal_set_pen2pix(int pen, int pix);

/* text and pdf drawing (shared by screen and printer) */
typedef struct {
    int handle;
    int cw[PS_BLANK + 1], ch[PS_BLANK + 1];   /* cell size per style  */
    int base[PS_BLANK + 1];                    /* baseline offset      */
    int height[PS_BLANK + 1];                  /* vst_height value     */
    int ncolors;
    int mono;
    /* rich text */
    FONTSET *fs;
    int um_w;                                  /* pixel width, um      */
    const u8 *doc;                             /* document the picks are for */
    short pick[64][4], pfx[64][4];             /* font id / effects per font, style */
    int cur_font, cur_size, cur_attr, cur_color; /* state set on the VDI */
    int asc, desc;                             /* metrics of that state */
} TXTDEV;

void  txt_measure(TXTDEV *td, int handle, int base_height, int ncolors, int um_w);
int   txt_is_rich(const u8 *blob);
long  txt_layout(const u8 *blob, long len, TXTDEV *td, int width_px,
                 TLINE **lines, PROG *pg);
void  txt_draw_line(TXTDEV *td, const u8 *blob, const TLINE *l, int x, int y,
                    int width);
long  pdf_index(const u8 *blob, long len, long **offs);
void  pdf_page_size(const u8 *blob, long off, long *w16, long *h16, long *x0, long *y0);
void  pdf_draw_page(int handle, const u8 *blob, long off, int ox, int oy,
                    long sx1000, long sy1000, int ncolors, int mono,
                    const GRECT *clip);
int   pen_for_rgb(const u8 *rgb, int ncolors);
void  pdf_prepare_page(int handle, const u8 *blob, long off, int ox, int oy,
                      long sx1000, long sy1000, int ncolors, int mono,
                      const GRECT *clip);
void  render_cache_flush(void);
long  pdf_page_missing(int handle, const u8 *blob, long off, long sx1000, long sy1000,
                      int ncolors, int mono);
int   pdf_build_page(int handle, const u8 *blob, long off, long sx1000, long sy1000,
                   int ncolors, int mono, PROG *pg);
extern int render_nobuild, render_boxes;
long  txt_vis(const u8 *s, long n);

/* tree.c */
OBJECT *build_menu(void);
void    build_progress(OBJECT *t, char *text, char *stat);
OBJECT *build_prefs(void);
OBJECT *build_icon(char *label);
void    prefs_to_tree(OBJECT *t, const PREFS *p);
void    tree_to_prefs(OBJECT *t, PREFS *p);
int     prefs_button_kind(int obj);     /* PB_* */
enum { PB_NONE = 0, PB_SAVE, PB_APPLY, PB_CLOSE, PB_TOGGLE };

/* menu object indices (must match build_menu() in tree.c) */
#define NMENUS 5
enum {
    MT_DESK = 3, MT_FILE, MT_VIEW, MT_PIC, MT_OPTS,
    MI_ABOUT = 10,
    MI_OPEN = 19, MI_CLOSE, MI_SEP1, MI_SAVEORIG, MI_SAVEDITH, MI_SEP2,
    MI_PRINT, MI_SEP3, MI_QUIT,
    MI_REAL = 29, MI_FIT, MI_ZOOMIN, MI_ZOOMOUT, MI_SEP4, MI_PREVPG,
    MI_NEXTPG, MI_FIRSTPG, MI_LASTPG, MI_SEP5, MI_CYCLE,
    MI_DUP = 41, MI_DITHER, MI_REDUCE, MI_RESIZE, MI_COUNT, MI_SEP6,
    MI_LOADPAL, MI_SAVEPAL, MI_SEP7, MI_SHAREPAL,
    MI_PREFS = 52,
    MENU_NOBJ = 53
};

/* prefs.c */
void prefs_load(PREFS *p);
int  prefs_save(const PREFS *p);
void prefs_open(void);
void prefs_close(void);
int  prefs_handle(int handle);
void prefs_redraw(GRECT *r);
void prefs_click(int mx, int my);
void prefs_moved(GRECT *r);
void prefs_top(void);
extern int prefs_win;

/* print.c */
void print_window(VWIN *w);

/* misc */
int  gem_handle_ok(void);
/* main.c */
void disp_reserved_for(DISPINFO *d, int palmode);   /* pens a picture keeps */
const char *fv_temp_dir(void);   /* for temporary files, ends in '\'    */
void win_share_pending(void);
long now_ms(void);

#endif
