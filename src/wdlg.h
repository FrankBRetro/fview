/* wdlg.h - resource files and non-modal dialog windows (wdlg.c), shared
 * by FView and FConv. */
#ifndef WDLG_H
#define WDLG_H

/* ------------------------------------------------------ resource file */

/* the languages, in the order of the Preferences' Language buttons */
enum { LANG_EN = 0, LANG_DE, LANG_FR, LANG_IT, LANG_ES, LANG_CZ, LANG_N };

/* load <base>.RSC for English or <base>DE.RSC etc. from the program's
   folder (English if the language's file is missing); 0 if none loads */
int     rsc_open(const char *base, int lang);
OBJECT *rs_tree(int tree);
const char *rs(int id);                     /* free string id           */
int     rsf(char *buf, int size, int id, ...);   /* rs(id) as a format  */
/* chars -> pixels done by rsrc_load; this adds MagiC check boxes, air
   around buttons and row spacing that suits the screen (once per tree) */
void    tree_finish(OBJECT *t);
void    tree_place(OBJECT *t, int handle);   /* root into the work area  */
OBJECT *rs_dialog(int tree);                 /* rs_tree, finished once   */

extern int aes_xbutton;          /* AES draws check boxes / round buttons */

/* --------------------------------------------- non-modal dialog windows */

typedef struct WDIALOG WDIALOG;
/* an exit button was chosen (obj), or the window's closer (-1): return
   1 to close the dialog, 0 to keep it open                             */
typedef int (*WD_EXIT)(WDIALOG *d, int obj);

/* a copy of resource tree `tree` in a window of its own; first_edit is
   the editable field with the cursor (0: none). owner: the document it
   belongs to (closed with it, see wd_forget), or 0.                  */
WDIALOG   *wd_open(int tree, const char *title, int first_edit, WD_EXIT fn,
                  void *owner, void *ctx);
OBJECT *wd_tree(WDIALOG *d);
void   *wd_ctx(WDIALOG *d);
char   *wd_text(WDIALOG *d, int obj);   /* the object's own text buffer (95) */
void    wd_set_field(WDIALOG *d, int obj, const char *s);
const char *wd_field(WDIALOG *d, int obj);
void    wd_redraw_obj(WDIALOG *d, int obj);
void    wd_close(WDIALOG *d);
void    wd_forget(void *owner);      /* close the owner's, no callbacks */
void    wd_close_all(void);
int     wd_find(int handle);         /* 1 if the window is one of ours */

/* the event loop offers every event here first; 1 = taken */
int     wd_message(const short *msg);
int     wd_button(int handle, int mx, int my, int clicks);
int     wd_key(int kstate, int key);

/* ------------------------------------------------------- alert windows */

extern int  wd_alert_tree;          /* the program's T_ALERT         */
extern const char *wd_app_title;    /* window title of its alerts     */

/* button 1..3 that was chosen (the last one for the window's closer),
   or 0 when the alert's owner closed first: free ctx then too        */
typedef void (*ALERT_FN)(int button, void *ctx);

/* a GEM alert string ([icon][lines][buttons]) shown in a window: nothing
   waits for it. fn (may be 0) gets the answer. Without a free window it
   falls back to form_alert.                                            */
void alert_show(int def, const char *alert, ALERT_FN fn, void *ctx, void *owner);
void alertf(const char *fmt, ...);                      /* a notice      */
void alert_rs(int id, ...);                             /* notice, rs(id) */
void alert_ask(int def, ALERT_FN fn, void *ctx, void *owner, int id, ...);

#endif
