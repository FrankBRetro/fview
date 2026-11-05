/*
 * farchive.c - FArchive, archiver for TOS / MultiTOS / MiNT.
 *
 * Starts as an icon, like FConv. Archives dropped on it (AP_DRAGDROP,
 * VA_START, VA_DRAGACCWIND or the command line) are extracted into the
 * destination folder or opened in a window of their own; other files and
 * folders dropped on it go into a new archive. Files dropped on an
 * archive's window are added to it. The formats: ZIP, LHA, ZOO, ARC, LZX,
 * TAR, TAR.GZ and GZ, with real compression in each.
 *
 * Names in archives that are not valid 8.3 names are renamed when they
 * are extracted to a FAT (TOS) drive, with a line in the log; drives with
 * long names (MiNT file systems) get the names as they are.
 *
 * The work is done by the job system shared with FView and FConv: under
 * MiNT with U:\SHM each task runs in its own FAWORK.TTP at nice 19, so
 * FArchive stays responsive; on plain TOS the jobs run cooperatively.
 * Settings live in FARCHIVE.INF next to the program; the log is also kept
 * in FARCHIVE.TXT, but only on hard disks (never on A: or B:).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <osbind.h>
#include <mintbind.h>
#include <mint/cookie.h>
#include "gemui.h"
#include "archive.h"

#undef APPNAME
#define APPNAME "FArchive"

#ifndef VA_START
#define VA_START      0x4711
#define AV_STARTED    0x4738
#define AV_PROTOKOLL  0x4700
#endif
#ifndef VA_DRAGACCWIND
#define VA_DRAGACCWIND 0x4725   /* AV desktops: files dropped on a window */
#endif
#ifndef AP_DRAGDROP
#define AP_DRAGDROP   63
#endif
#ifndef WM_ICONIFY
#define WM_ICONIFY    34
#define WM_UNICONIFY  35
#define WM_ALLICONIFY 36
#endif
#ifndef WF_ICONIFY
#define WF_ICONIFY    26
#define WF_UNICONIFY  27
#endif
#define DD_OK    0
#define DD_NAK   1
#define DD_EXT   2
#ifndef FA_DIR
#define FA_DIR   0x10
#endif
#ifndef K_LSHIFT
#define K_RSHIFT 1
#define K_LSHIFT 2
#endif
#ifndef K_CTRL
#define K_CTRL   4
#endif

long _stksize = 64 * 1024L;

/* what the shared code (job.c) expects to find */
int  vdih, ap_id;
int  scr_w, scr_h, scr_planes, pix_w_um, pix_h_um;
int  cell_w, cell_h;
GRECT desk;
PREFS prefs;
DISPINFO dinfo;
int  quitting;
int  aes_iconify, aes_3d;
extern const char *job_app_name, *job_log_name, *job_worker_name;

/* ------------------------------------------------------------ settings */

typedef struct {
    int  fmt;                    /* AF_* for new archives                */
    int  xdrop;                  /* extract dropped archives (else open) */
    int  folder;                 /* a folder per archive when extracting */
    int  paths;                  /* keep the archive's folders           */
    int  replace;                /* overwrite existing files             */
    int  subdirs;                /* adding folders: include subfolders   */
    int  rename83;               /* 8.3 names on FAT drives              */
    int  best;                   /* best (slower) compression            */
    char dest[256];              /* destination folder, ends in '\'      */
} ASET;

static ASET cs = { AF_ZIP, 1, 1, 1, 0, 1, 1, 0, "" };
static char app_folder[256];

long now_ms(void)
{
    return (long)(clock() * 1000L / CLOCKS_PER_SEC);
}

void alertf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsprintf(buf, fmt, ap);
    va_end(ap);
    form_alert(1, buf);
}

static int fa_file_select(char *path, char *name, const char *title)
{
    short btn = 0, ok;
    if (_AESversion >= 0x0140)
        ok = fsel_exinput(path, name, &btn, title);
    else
        ok = fsel_input(path, name, &btn);
    return ok && btn;
}

/* the folder FARCHIVE.PRG was started from (ends in a backslash) */
static void find_app_folder(void)
{
    char cmd[256], tail[128], *s;
    cmd[0] = 0;
    if (shel_read(cmd, tail) && cmd[0] &&
        ((s = strrchr(cmd, '\\')) != 0 || (s = strrchr(cmd, '/')) != 0) &&
        (cmd[1] == ':' || cmd[0] == '\\' || cmd[0] == '/')) {
        s[1] = 0;
        strcpy(app_folder, cmd);
        return;
    }
    app_folder[0] = Dgetdrv() + 'A';
    app_folder[1] = ':';
    Dgetpath(app_folder + 2, 0);
    strcat(app_folder, "\\");
}

static void settings_path(char *p)
{
    strcpy(p, app_folder);
    strcat(p, "FARCHIVE.INF");
}

static void settings_save(void)
{
    char p[300];
    FILE *f;
    settings_path(p);
    f = fopen(p, "w");
    if (!f)
        return;
    fprintf(f, "FARCHIVE 1\nformat=%d\nextract=%d\nfolder=%d\npaths=%d\nreplace=%d\n"
            "subfolders=%d\nrename83=%d\nbest=%d\ndest=%s\n",
            cs.fmt, cs.xdrop, cs.folder, cs.paths, cs.replace, cs.subdirs, cs.rename83,
            cs.best, cs.dest);
    fclose(f);
}

static void settings_load(void)
{
    char p[300], line[300];
    FILE *f;
    settings_path(p);
    f = fopen(p, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *v = strchr(line, '=');
        int n;
        if (!v)
            continue;
        *v++ = 0;
        v[strcspn(v, "\r\n")] = 0;
        n = atoi(v);
        if (!strcmp(line, "format") && n > AF_NONE && n < AF_COUNT) cs.fmt = n;
        else if (!strcmp(line, "extract")) cs.xdrop = n != 0;
        else if (!strcmp(line, "folder")) cs.folder = n != 0;
        else if (!strcmp(line, "paths")) cs.paths = n != 0;
        else if (!strcmp(line, "replace")) cs.replace = n != 0;
        else if (!strcmp(line, "subfolders")) cs.subdirs = n != 0;
        else if (!strcmp(line, "rename83")) cs.rename83 = n != 0;
        else if (!strcmp(line, "best")) cs.best = n != 0;
        else if (!strcmp(line, "dest")) {
            strncpy(cs.dest, v, sizeof cs.dest - 2);
            cs.dest[sizeof cs.dest - 2] = 0;
        }
    }
    fclose(f);
}

/* ---------------------------------------------------------- object trees */

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

/* ----------------------------------------------------------------- menu */

#define NTITLE 4
static const char *titles[NTITLE] = { " FArchive ", " File ", " Format ", " Options " };
static const char *m_desk[] = {
    "  About FArchive...  ", "---------------------",
    "  Desk Accessory 1   ", "  Desk Accessory 2   ", "  Desk Accessory 3   ",
    "  Desk Accessory 4   ", "  Desk Accessory 5   ", "  Desk Accessory 6   "
};
static const char *m_file[] = {
    "  Open archive...        ^O ", "  New archive...         ^N ",
    "----------------------------",
    "  Extract archive...     ^E ", "  Extract selected       ^X ",
    "  Add files...           ^A ", "  Select all             ^S ",
    "  Close archive          ^U ",
    "----------------------------",
    "  Destination folder...  ^D ",
    "----------------------------", "  Quit                   ^Q "
};
static const char *m_fmt[] = {
    "  ZIP (deflate)        ", "  LHA (lh5)            ", "  ZOO (LZW)            ",
    "  ARC (crunched)       ", "  LZX (Amiga)          ", "  TAR                  ",
    "  TAR.GZ               ", "  GZ (one file)        ",
    "-----------------------", "  Best compression     "
};
static const char *m_opt[] = {
    "  Extract dropped archives ", "  Folder per archive       ",
    "  Keep folders             ", "  Replace existing files   ",
    "  Include subfolders       ", "  8.3 names on FAT drives  "
};
enum {                                       /* item numbers inside menus */
    D_ABOUT = 0,
    F_OPEN = 0, F_NEW = 1, F_EXTRACT = 3, F_XSEL, F_ADD, F_SELALL, F_CLOSE, F_DEST = 9,
    F_QUIT = 11,
    X_FIRST = 0, X_BEST = 9,
    O_XDROP = 0, O_FOLDER, O_PATHS, O_REPLACE, O_SUBDIRS, O_83
};
static const char **m_lists[NTITLE] = { m_desk, m_file, m_fmt, m_opt };
static const int m_counts[NTITLE] = { 8, 12, 10, 6 };
#define MENU_OBJS (8 + 8 + 12 + 10 + 6 + NTITLE + 8)
static OBJECT menu[MENU_OBJS];
static int m_first[NTITLE];                  /* object of each menu's item 0 */

static OBJECT *fa_build_menu(void)
{
    int i, idx, x, ncol = scr_w / (cell_w ? cell_w : 8), tx[NTITLE];
    obj_add(menu, 0, -1, G_IBOX, 0, 0, 0L, 0, 0, ncol, 25);
    obj_add(menu, 1, 0, G_BOX, 0, 0, 0x00001100L, 0, 0, ncol, 0x0201);
    x = 0;
    for (i = 0; i < NTITLE; i++) {
        tx[i] = x;
        x += (int)strlen(titles[i]);
    }
    obj_add(menu, 2, 1, G_IBOX, 0, 0, 0L, 2, 0, x, 0x0301);
    for (i = 0; i < NTITLE; i++)
        obj_add(menu, 3 + i, 2, G_TITLE, 0, 0, (long)titles[i], tx[i], 0,
                (int)strlen(titles[i]), 0x0301);
    obj_add(menu, 3 + NTITLE, 0, G_IBOX, 0, 0, 0L, 0, 0x0301, ncol, 19);
    idx = 4 + NTITLE;
    for (i = 0; i < NTITLE; i++) {
        int k, w = 0, box = idx, bx = 2 + tx[i];
        for (k = 0; k < m_counts[i]; k++)
            if ((int)strlen(m_lists[i][k]) > w)
                w = (int)strlen(m_lists[i][k]);
        if (bx + w > ncol)
            bx = ncol - w;
        obj_add(menu, idx++, 3 + NTITLE, G_BOX, 0, 0, 0x00FF1100L, bx, 0, w, m_counts[i]);
        m_first[i] = idx;
        for (k = 0; k < m_counts[i]; k++) {
            int sep = m_lists[i][k][0] == '-';
            obj_add(menu, idx++, box, G_STRING, 0, sep ? DISABLED : 0,
                    (long)m_lists[i][k], 0, k, w, 1);
        }
    }
    menu[idx - 1].ob_flags |= LASTOB;
    for (i = 0; i < idx; i++)
        rsrc_obfix(menu, i);
    menu[0].ob_width = scr_w;
    menu[1].ob_width = scr_w;
    return menu;
}

#define MI(m, k) (m_first[m] + (k))

static struct AWIN *top_awin(void);

static void fa_menu_update(void)
{
    int i;
    struct AWIN *t = top_awin();
    for (i = 0; i < AF_COUNT - 1; i++)
        menu_icheck(menu, MI(2, X_FIRST + i), cs.fmt == i + 1);
    menu_icheck(menu, MI(2, X_BEST), cs.best);
    menu_icheck(menu, MI(3, O_XDROP), cs.xdrop);
    menu_icheck(menu, MI(3, O_FOLDER), cs.folder);
    menu_icheck(menu, MI(3, O_PATHS), cs.paths);
    menu_icheck(menu, MI(3, O_REPLACE), cs.replace);
    menu_icheck(menu, MI(3, O_SUBDIRS), cs.subdirs);
    menu_icheck(menu, MI(3, O_83), cs.rename83);
    menu_ienable(menu, MI(1, F_XSEL), t != 0);
    menu_ienable(menu, MI(1, F_ADD), t != 0);
    menu_ienable(menu, MI(1, F_SELALL), t != 0);
    menu_ienable(menu, MI(1, F_CLOSE), t != 0);
}

/* ------------------------------------------------------------- helpers */

static const char *base_name(const char *p)
{
    const char *b = p, *s;
    for (s = p; *s; s++)
        if ((*s == '\\' || *s == '/' || *s == ':') && s[1])
            b = s + 1;
    return b;
}

static int is_dir(const char *p)
{
    char t[256];
    long a;
    size_t n = strlen(p);
    if (n == 0 || n > 250)
        return 0;
    strcpy(t, p);
    if (n > 3 && (t[n - 1] == '\\' || t[n - 1] == '/'))
        t[n - 1] = 0;
    if (n <= 3 && t[1] == ':')                /* a whole drive */
        return 1;
    a = Fattrib(t, 0, 0);
    return a >= 0 && (a & FA_DIR);
}

static int exists(const char *p)
{
    return Fattrib(p, 0, 0) >= 0;
}

static int same_path(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x == '/') x = '\\';
        if (y == '/') y = '\\';
        if (x != y)
            return 0;
    }
    return *a == *b;
}

/* a floppy: no log file there */
static int on_floppy(const char *p)
{
    int d = p[0] & 0xDF;
    if (p[1] == ':')
        return d == 'A' || d == 'B';
    d = Dgetdrv();
    return d < 2;
}

/* "12345", "1234K", "12.3M" in 6 columns */
static char *size_str(long n, char *b)
{
    if (n < 100000L)
        sprintf(b, "%6ld", n);
    else if (n < 10000000L)
        sprintf(b, "%5ldK", (n + 512) / 1024);
    else
        sprintf(b, "%4ld.%ldM", n / 1048576L, (n % 1048576L) * 10 / 1048576L);
    return b;
}

static char *date_str(u32 dt, char *b)
{
    unsigned d = (unsigned)(dt >> 16), t = (unsigned)(dt & 0xFFFF);
    if (!dt)
        strcpy(b, "                ");
    else
        sprintf(b, "%04d-%02d-%02d %02d:%02d", 1980 + (d >> 9), (d >> 5) & 15, d & 31,
                t >> 11, (t >> 5) & 63);
    return b;
}

/* "1.8 s", "2 min 5 s" */
static char *secs(long ms, char *b)
{
    if (ms < 0) ms = 0;
    if (ms < 60000L)
        sprintf(b, "%ld.%ld s", ms / 1000, (ms % 1000) / 100);
    else
        sprintf(b, "%ld min %ld s", ms / 60000L, (ms % 60000L) / 1000);
    return b;
}

/* ------------------------------------------------------------------ log */

/* What was done and what went wrong: shown in the main window (open the
 * icon) and appended to FARCHIVE.TXT - in the destination folder, else
 * next to the program - but only on a hard disk, never on A: or B:.  */
#define LOGN 200
#define LOGW 96
static char logl[LOGN][LOGW];
static long lognum;                          /* lines ever added          */
static long logtop;                          /* first line shown          */
static void log_changed(int was_bottom);
static int  log_visible(void);

static void log_to_file(const char *s)
{
    char p[300];
    FILE *f;
    const char *dir = cs.dest[0] && !on_floppy(cs.dest) ? cs.dest :
                      !on_floppy(app_folder) ? app_folder : 0;
    if (!dir || strlen(dir) > 280)
        return;
    strcpy(p, dir);
    strcat(p, "FARCHIVE.TXT");
    f = fopen(p, "ab");
    if (!f)
        return;
    fputs(s, f);
    fputs("\r\n", f);                       /* Atari text files */
    fclose(f);
}

static void log_add(const char *fmt, ...)
{
    char b[400];
    va_list ap;
    int was_bottom = logtop >= lognum - log_visible();
    va_start(ap, fmt);
    vsprintf(b, fmt, ap);
    va_end(ap);
    strncpy(logl[lognum % LOGN], b, LOGW - 1);
    logl[lognum % LOGN][LOGW - 1] = 0;
    lognum++;
    log_to_file(b);
    dbg("log: %s", b);
    log_changed(was_bottom);
}

/* ----------------------------------------------------- archive windows */

typedef struct {
    char *name;
    long  usize, csize;
    u32   dt;
    u8    flags;                             /* 1 folder, 2 unsupported   */
    u8    sel;
    char  m[9];
} MEMB;

#define MAXAW 8
typedef struct AWIN {
    int   handle;
    char  path[256];
    int   fmt;
    int   isnew;                             /* not written yet           */
    MEMB *e;
    int   n;
    long  usize, csize;
    long  top;
    int   anchor;                            /* last clicked row          */
    int   busy;                              /* a task is using it        */
    char  title[80], info[100];
    GRECT full;
} AWIN;

static AWIN aw[MAXAW];

#define AWK (NAME | CLOSER | MOVER | SIZER | FULLER | INFO | UPARROW | DNARROW | VSLIDE)

static AWIN *find_aw(int handle)
{
    int i;
    for (i = 0; i < MAXAW; i++)
        if (aw[i].handle > 0 && aw[i].handle == handle)
            return &aw[i];
    return 0;
}

static AWIN *find_aw_path(const char *path)
{
    int i;
    for (i = 0; i < MAXAW; i++)
        if (aw[i].handle > 0 && same_path(aw[i].path, path))
            return &aw[i];
    return 0;
}

static AWIN *top_awin(void)
{
    short h = 0, d;
    wind_get(0, WF_TOP, &h, &d, &d, &d);
    return find_aw(h);
}

static void aw_free_list(AWIN *w)
{
    int i;
    for (i = 0; i < w->n; i++)
        free(w->e[i].name);
    free(w->e);
    w->e = 0;
    w->n = 0;
    w->usize = w->csize = 0;
}

static int aw_rows(AWIN *w)
{
    GRECT wk;
    int n;
    wind_get_grect(w->handle, WF_WORKXYWH, &wk);
    n = (wk.g_h - cell_h - 6) / cell_h;
    return n < 1 ? 1 : n;
}

static void aw_slider(AWIN *w)
{
    int vis = aw_rows(w);
    if (w->top > w->n - vis) w->top = w->n - vis;
    if (w->top < 0) w->top = 0;
    if (w->n <= vis) {
        wind_set(w->handle, WF_VSLSIZE, 1000, 0, 0, 0);
        wind_set(w->handle, WF_VSLIDE, 0, 0, 0, 0);
    } else {
        wind_set(w->handle, WF_VSLSIZE, (short)(1000L * vis / w->n), 0, 0, 0);
        wind_set(w->handle, WF_VSLIDE, (short)(1000L * w->top / (w->n - vis)), 0, 0, 0);
    }
}

static void aw_set_info(AWIN *w)
{
    char a[12], b[12];
    int nsel = 0, i;
    for (i = 0; i < w->n; i++)
        nsel += w->e[i].sel;
    if (w->busy)
        strcpy(w->info, " Working...");
    else if (w->isnew)
        sprintf(w->info, " New %s archive: drop files here to add them", af_name(w->fmt));
    else {
        size_str(w->usize, a);
        size_str(w->csize, b);
        sprintf(w->info, " %s, %d member%s, %s bytes, %s packed", af_name(w->fmt),
                w->n, w->n == 1 ? "" : "s", a, b);
        if (nsel)
            sprintf(w->info + strlen(w->info), ", %d selected", nsel);
    }
    wind_set_str(w->handle, WF_INFO, w->info);
}

static void post_redraw_h(int h)
{
    short msg[8];
    GRECT wk;
    if (h <= 0)
        return;
    wind_get_grect(h, WF_WORKXYWH, &wk);
    msg[0] = WM_REDRAW; msg[1] = ap_id; msg[2] = 0; msg[3] = h;
    msg[4] = wk.g_x; msg[5] = wk.g_y; msg[6] = wk.g_w; msg[7] = wk.g_h;
    appl_write(ap_id, 16, msg);
}

static void aw_draw(AWIN *w, GRECT *clip)
{
    GRECT wk;
    short pxy[4], d;
    int y, x, cols, vis, i, namew;
    char line[300], s1[12], s2[12], dt[20];
    wind_get_grect(w->handle, WF_WORKXYWH, &wk);
    pxy[0] = clip->g_x; pxy[1] = clip->g_y;
    pxy[2] = clip->g_x + clip->g_w - 1; pxy[3] = clip->g_y + clip->g_h - 1;
    vsf_interior(vdih, 1);
    vsf_color(vdih, 0);
    vswr_mode(vdih, MD_REPLACE);
    vr_recfl(vdih, pxy);
    vst_height(vdih, cell_h > 10 ? 13 : 6, &d, &d, &d, &d);
    vst_alignment(vdih, 0, 5, &d, &d);
    vswr_mode(vdih, MD_TRANS);
    cols = wk.g_w / cell_w - 1;
    namew = cols - 41;                       /* size packed method date */
    if (namew < 12) namew = 12;
    x = wk.g_x + cell_w / 2;
    y = wk.g_y + 2;
    vst_color(vdih, 1);
    vst_effects(vdih, 1);
    sprintf(line, "%-*.*s %6s %6s %-7s %s", namew, namew, "Name", "Size", "Packed",
            "Method", "Date");
    v_gtext(vdih, x, y, line);
    vst_effects(vdih, 0);
    {
        short ln[4];
        ln[0] = wk.g_x; ln[2] = wk.g_x + wk.g_w - 1;
        ln[1] = ln[3] = y + cell_h + 1;
        vsl_color(vdih, 1);
        vsl_width(vdih, 1);
        v_pline(vdih, 2, ln);
    }
    y += cell_h + 4;
    vis = aw_rows(w);
    if (!w->n) {
        v_gtext(vdih, x, y, w->busy ? "(reading the archive...)" :
                w->isnew ? "(empty: drop files on this window)" : "(no members)");
        return;
    }
    for (i = (int)w->top; i < w->n && i < w->top + vis + 1; i++) {
        MEMB *e = &w->e[i];
        const char *nm = e->name;
        int l = (int)strlen(nm);
        char shown[260];
        if (l > namew) {                     /* keep the end of long paths */
            strcpy(shown, "...");
            strcat(shown, nm + l - (namew - 3));
        } else
            strcpy(shown, nm);
        if (e->flags & 1)
            sprintf(line, "%-*.*s %6s %6s %-7s %s", namew, namew, shown, "", "", "folder",
                    date_str(e->dt, dt));
        else
            sprintf(line, "%-*.*s %s %s %-7.7s %s", namew, namew, shown,
                    size_str(e->usize, s1), size_str(e->csize, s2),
                    (e->flags & 2) ? "?" : e->m, date_str(e->dt, dt));
        if (e->sel) {
            short r[4];
            r[0] = wk.g_x; r[1] = y - 1;
            r[2] = wk.g_x + wk.g_w - 1; r[3] = y + cell_h - 2;
            vsf_color(vdih, 1);
            vswr_mode(vdih, MD_REPLACE);
            vr_recfl(vdih, r);
            vswr_mode(vdih, MD_TRANS);
            vst_color(vdih, 0);
        } else
            vst_color(vdih, (e->flags & 2) && scr_planes > 1 ? 2 : 1);
        v_gtext(vdih, x, y, line);
        y += cell_h;
    }
    vst_color(vdih, 1);
}

static void aw_redraw(AWIN *w, GRECT *area)
{
    GRECT r, a = *area;
    if (w->handle <= 0 || !rc_intersect(&desk, &a))
        return;
    wind_update(BEG_UPDATE);
    graf_mouse(M_OFF, 0);
    wind_get_grect(w->handle, WF_FIRSTXYWH, &r);
    while (r.g_w && r.g_h) {
        if (rc_intersect(&a, &r)) {
            short clip[4];
            clip[0] = r.g_x; clip[1] = r.g_y;
            clip[2] = r.g_x + r.g_w - 1; clip[3] = r.g_y + r.g_h - 1;
            vs_clip(vdih, 1, clip);
            aw_draw(w, &r);
        }
        wind_get_grect(w->handle, WF_NEXTXYWH, &r);
    }
    {
        short none[4] = { 0, 0, 0, 0 };
        vs_clip(vdih, 0, none);
    }
    graf_mouse(M_ON, 0);
    wind_update(END_UPDATE);
}

static void aw_changed(AWIN *w)
{
    aw_slider(w);
    aw_set_info(w);
    post_redraw_h(w->handle);
}

/* make the list from a job's listing blob */
static u32 get4(const u8 *p)
{
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

static void aw_take_list(AWIN *w, const u8 *b, long len)
{
    long n, i;
    const u8 *p = b + 18, *end = b + len;
    aw_free_list(w);
    if (len < 18 || memcmp(b, "FAL1", 4))
        return;
    w->fmt = b[4] << 8 | b[5];
    n = (long)get4(b + 6);
    w->usize = (long)get4(b + 10);
    w->csize = (long)get4(b + 14);
    w->e = calloc(n + 1, sizeof(MEMB));
    if (!w->e)
        return;
    for (i = 0; i < n && p + 22 <= end; i++) {
        MEMB *e = &w->e[i];
        int nl = p[21];
        if (p + 22 + nl > end)
            break;
        e->usize = (long)get4(p);
        e->csize = (long)get4(p + 4);
        e->dt = get4(p + 8);
        e->flags = p[12];
        memcpy(e->m, p + 13, 8);
        e->m[8] = 0;
        e->name = malloc(nl + 1);
        if (!e->name)
            break;
        memcpy(e->name, p + 22, nl);
        e->name[nl] = 0;
        p += 22 + nl;
        w->n++;
    }
    w->isnew = 0;
}

static void aw_close(AWIN *w)
{
    if (w->handle > 0) {
        wind_close(w->handle);
        wind_delete(w->handle);
    }
    aw_free_list(w);
    w->handle = 0;
    fa_menu_update();
}

static void queue_list(AWIN *w);

static AWIN *aw_open(const char *path, int fmt, int isnew)
{
    AWIN *w = find_aw_path(path);
    GRECT wk, r;
    int i, slot = -1;
    if (w) {
        wind_set(w->handle, WF_TOP, 0, 0, 0, 0);
        return w;
    }
    for (i = 0; i < MAXAW; i++)
        if (aw[i].handle <= 0) {
            slot = i;
            break;
        }
    if (slot < 0) {
        alertf("[1][FArchive: close an archive|window first.][ OK ]");
        return 0;
    }
    w = &aw[slot];
    memset(w, 0, sizeof *w);
    strncpy(w->path, path, sizeof w->path - 1);
    w->fmt = fmt;
    w->isnew = isnew;
    w->anchor = -1;
    wk.g_x = wk.g_y = 0;
    wk.g_w = 72 * cell_w;
    wk.g_h = 16 * cell_h + 8;
    wind_calc_grect(WC_BORDER, AWK, &wk, &r);
    if (r.g_w > desk.g_w) r.g_w = desk.g_w;
    if (r.g_h > desk.g_h) r.g_h = desk.g_h;
    r.g_x = desk.g_x + (desk.g_w - r.g_w) / 2 + slot * 16 - 48;
    r.g_y = desk.g_y + 16 + slot * 16;
    if (r.g_x < desk.g_x) r.g_x = desk.g_x;
    if (r.g_y + r.g_h > desk.g_y + desk.g_h) r.g_y = desk.g_y + desk.g_h - r.g_h;
    w->handle = wind_create_grect(AWK, &r);
    if (w->handle <= 0) {
        w->handle = 0;
        alertf("[1][FArchive: no more windows.][ OK ]");
        return 0;
    }
    sprintf(w->title, " %.60s ", path);
    wind_set_str(w->handle, WF_NAME, w->title);
    aw_set_info(w);
    wind_open_grect(w->handle, &r);
    w->full = r;
    aw_slider(w);
    if (!isnew)
        queue_list(w);
    fa_menu_update();
    return w;
}

static int aw_count_sel(AWIN *w)
{
    int i, n = 0;
    for (i = 0; i < w->n; i++)
        n += w->e[i].sel;
    return n;
}

/* ------------------------------------------------------------ the tasks */

typedef struct {
    int   op;                                /* OP_ARC_*                  */
    char  path[256];                         /* the archive               */
    char  out[256];                          /* extract into              */
    char  list[256];                         /* list file, deleted after  */
    int   afmt;
    int   nitems;                            /* files being added         */
    int   handle;                            /* the archive's window      */
} TASK;

static TASK *tasks;
static int  tn, tcap, tpos;                  /* count, room, next to do   */
static int  n_ok, n_fail, n_skip, n_ren, n_tfail;
static char first_err[90];
static int  running, cancelling, finished;
static int  want_next;
static JOB *cur;
static long batch_t0, cur_t0;
static int  item_i, item_n;                  /* member now being done      */
static int  list_seq;

static TASK *task_new(int op, const char *path)
{
    TASK *t;
    if (tn == tcap) {
        int nc = tcap ? tcap * 2 : 16;
        TASK *nt = realloc(tasks, nc * sizeof(TASK));
        if (!nt)
            return 0;
        tasks = nt;
        tcap = nc;
    }
    t = &tasks[tn++];
    memset(t, 0, sizeof *t);
    t->op = op;
    strncpy(t->path, path, sizeof t->path - 1);
    return t;
}

/* a temporary list file next to the program (or in the destination) */
static FILE *list_file(char *path)
{
    const char *dir = app_folder;
    FILE *f;
    int k;
    for (k = 0; k < 100; k++) {
        sprintf(path, "%sFARC%04d.LST", dir, (list_seq++) % 10000);
        if (!exists(path))
            break;
    }
    f = fopen(path, "wb");
    if (!f && cs.dest[0]) {
        sprintf(path, "%sFARC%04d.LST", cs.dest, (list_seq++) % 10000);
        f = fopen(path, "wb");
    }
    return f;
}

static void begin(void);

/* the folder an archive is extracted into */
static void extract_dest(const char *arc, char *out)
{
    strcpy(out, cs.dest);
    if (cs.folder) {
        char stem[64];
        const char *b = base_name(arc), *dot = strrchr(b, '.');
        size_t n = dot && dot != b ? (size_t)(dot - b) : strlen(b);
        if (n > 60) n = 60;
        memcpy(stem, b, n);
        stem[n] = 0;
        /* the folder's name: 8.3 on FAT drives */
        if (cs.rename83 && !afs_long_names(cs.dest) && n > 8)
            stem[8] = 0;
        strcat(out, stem);
        strcat(out, "\\");
    }
}

static int choose_dest(void);

static void queue_extract(const char *arc, AWIN *w, int selected)
{
    TASK *t;
    if (!cs.dest[0] && !choose_dest())
        return;
    t = task_new(OP_ARC_EXTRACT, arc);
    if (!t)
        return;
    extract_dest(arc, t->out);
    if (w)
        t->handle = w->handle;
    if (w && selected && aw_count_sel(w)) {
        FILE *f = list_file(t->list);
        int i;
        if (!f) {
            tn--;
            log_add("Cannot write a list file next to FArchive");
            return;
        }
        for (i = 0; i < w->n; i++)
            putc(w->e[i].sel, f);
        fclose(f);
        t->nitems = aw_count_sel(w);
    }
}

static void queue_list(AWIN *w)
{
    TASK *t = task_new(OP_ARC_LIST, w->path);
    if (!t)
        return;
    t->handle = w->handle;
    w->busy = 1;
    aw_set_info(w);
    begin();
}

/* names[] go into the archive; afmt for a new one */
static void queue_add(const char *arc, int afmt, char **names, int n, AWIN *w)
{
    TASK *t;
    FILE *f;
    int i;
    if (!n)
        return;
    t = task_new(OP_ARC_ADD, arc);
    if (!t)
        return;
    t->afmt = afmt;
    t->nitems = n;
    if (w)
        t->handle = w->handle;
    f = list_file(t->list);
    if (!f) {
        tn--;
        log_add("Cannot write a list file next to FArchive");
        return;
    }
    for (i = 0; i < n; i++) {
        fputs(names[i], f);
        fputs("\n", f);
    }
    fclose(f);
}

/* ------------------------------------------------------ progress dialog */

#ifndef FL3DIND
#define FL3DIND 0x0200
#define FL3DBAK 0x0400
#endif
#define FL3DACT 0x0600
#define PD_PAD  16

enum { P_ROOT = 0, P_TOTAL, P_TBOX, P_TBAR, P_FILE, P_FBOX, P_FBAR, P_STAT, P_BTN, P_NOBJ };
static OBJECT ptree[P_NOBJ];
static char t_total[80], t_file[80], t_stat[80], t_btn[12];
static char pd_title[40];
static int  pdlg;                            /* window handle, 0 = closed  */
static long f_done, f_total = 1;             /* this task's progress       */

static void build_dialog(void)
{
    long fill = (scr_planes >= 4) ? 0x00001174L : 0x00001171L;
    int bak = aes_3d ? FL3DBAK : 0, pad = PD_PAD, pw = 40 * cell_w;
    int bh = cell_h > 10 ? 12 : 8, btnh = cell_h + 6, btnw = 11 * cell_w, y;
    obj_add(ptree, P_ROOT, -1, G_BOX, bak, 0, aes_3d ? 0x00001100L : 0x00011100L,
            0, 0, pw + 2 * pad, 10);
    y = pad;
    obj_add(ptree, P_TOTAL, 0, G_STRING, 0, 0, (long)t_total, pad, y, pw, cell_h);
    y += cell_h + 6;
    obj_add(ptree, P_TBOX, 0, G_BOX, aes_3d ? FL3DIND : 0, aes_3d ? SELECTED : 0,
            0x00FF1100L, pad, y, pw, bh);
    obj_add(ptree, P_TBAR, P_TBOX, G_BOX, 0, 0, fill, 0, 0, 0, bh);
    y += bh + cell_h;
    obj_add(ptree, P_FILE, 0, G_STRING, 0, 0, (long)t_file, pad, y, pw, cell_h);
    y += cell_h + 6;
    obj_add(ptree, P_FBOX, 0, G_BOX, aes_3d ? FL3DIND : 0, aes_3d ? SELECTED : 0,
            0x00FF1100L, pad, y, pw, bh);
    obj_add(ptree, P_FBAR, P_FBOX, G_BOX, 0, 0, fill, 0, 0, 0, bh);
    y += bh + cell_h + 4;
    obj_add(ptree, P_STAT, 0, G_STRING, 0, 0, (long)t_stat, pad, y + (btnh - cell_h) / 2,
            pw - btnw - 2 * cell_w, cell_h);
    obj_add(ptree, P_BTN, 0, G_BUTTON, SELECTABLE | EXIT | DEFAULT | LASTOB |
            (aes_3d ? FL3DACT : 0), 0, (long)t_btn, pad + pw - btnw, y, btnw, btnh);
    ptree[P_ROOT].ob_height = y + btnh + pad;
}

static void dlg_place(void)
{
    GRECT wk;
    if (pdlg <= 0)
        return;
    wind_get_grect(pdlg, WF_WORKXYWH, &wk);
    ptree[P_ROOT].ob_x = wk.g_x;
    ptree[P_ROOT].ob_y = wk.g_y;
}

static void dlg_bars(void)
{
    int pw = ptree[P_TBOX].ob_width;
    long tot = tn > 0 ? tn : 1, fd = f_total > 0 ? f_done * 1000L / f_total : 0;
    long t;
    if (fd > 1000) fd = 1000;
    t = finished ? 1000 : (tpos * 1000L + (cur ? fd : 0)) / tot;
    if (t > 1000) t = 1000;
    ptree[P_TBAR].ob_width = (int)(pw * t / 1000);
    ptree[P_FBAR].ob_width = finished ? pw : cur ? (int)(pw * fd / 1000) : 0;
}

/* draw one object (with its children) through the rectangle list */
static void dlg_draw(int obj, GRECT *area)
{
    GRECT r, o;
    short x, y;
    if (pdlg <= 0)
        return;
    dlg_place();
    dlg_bars();
    objc_offset(ptree, obj, &x, &y);
    o.g_x = x - 3; o.g_y = y - 3;
    o.g_w = ptree[obj].ob_width + 6;
    o.g_h = ptree[obj].ob_height + 6;
    if (area && !rc_intersect(area, &o))
        return;
    wind_update(BEG_UPDATE);
    graf_mouse(M_OFF, 0);
    wind_get_grect(pdlg, WF_FIRSTXYWH, &r);
    while (r.g_w && r.g_h) {
        if (rc_intersect(&o, &r) && rc_intersect(&desk, &r))
            /* always from the root, clipped to the object: text objects
               are transparent, so the background must be redrawn under
               them or the new text lands on top of the old one         */
            objc_draw_grect(ptree, 0, 8, &r);
        wind_get_grect(pdlg, WF_NEXTXYWH, &r);
    }
    graf_mouse(M_ON, 0);
    wind_update(END_UPDATE);
}

static void dlg_open(void)
{
    GRECT wk, r;
    int k = NAME | MOVER;
    if (pdlg > 0) {
        wind_set(pdlg, WF_TOP, 0, 0, 0, 0);
        dlg_draw(P_ROOT, 0);
        return;
    }
    build_dialog();
    wk.g_x = wk.g_y = 0;
    wk.g_w = ptree[P_ROOT].ob_width;
    wk.g_h = ptree[P_ROOT].ob_height;
    wind_calc_grect(WC_BORDER, k, &wk, &r);
    r.g_x = desk.g_x + (desk.g_w - r.g_w) / 2;
    r.g_y = desk.g_y + (desk.g_h - r.g_h) / 2;
    pdlg = wind_create_grect(k, &r);
    if (pdlg <= 0) {
        pdlg = 0;
        return;
    }
    strcpy(pd_title, " FArchive ");
    wind_set_str(pdlg, WF_NAME, pd_title);
    wind_open_grect(pdlg, &r);
    dlg_draw(P_ROOT, 0);
}

static void dlg_close(void)
{
    if (pdlg > 0) {
        wind_close(pdlg);
        wind_delete(pdlg);
    }
    pdlg = 0;
}

static const char *op_verb(int op)
{
    return op == OP_ARC_LIST ? "Reading" : op == OP_ARC_EXTRACT ? "Extracting" : "Adding to";
}

static void set_total_text(void)
{
    if (finished)
        sprintf(t_total, "%s: %d file%s", cancelling ? "Cancelled" : "Done", n_ok,
                n_ok == 1 ? "" : "s");
    else if (tpos < tn)
        sprintf(t_total, "%d of %d: %s %.24s", tpos + 1, tn, op_verb(tasks[tpos].op),
                base_name(tasks[tpos].path));
    else
        strcpy(t_total, "");
}

static void set_stat_text(int pct)
{
    if (finished) {
        if (n_fail || n_tfail)
            sprintf(t_stat, "%d failed: %.24s", n_fail + n_tfail, first_err);
        else if (n_ren)
            sprintf(t_stat, "%d renamed for 8.3", n_ren);
        else if (n_skip)
            sprintf(t_stat, "%d skipped", n_skip);
        else
            strcpy(t_stat, "");
    } else if (cancelling)
        strcpy(t_stat, "Cancelling...");
    else if (n_fail || n_tfail)
        sprintf(t_stat, "%3d%%   %d failed", pct, n_fail + n_tfail);
    else
        sprintf(t_stat, "%3d%%", pct);
}

/* ------------------------------------------------------ running tasks */

static void finish_all(void)
{
    char t[24];
    int i, only_lists = 1;
    for (i = 0; i < tn; i++)
        if (tasks[i].op != OP_ARC_LIST)
            only_lists = 0;
    running = 0;
    finished = 1;
    cur = 0;
    if (only_lists && !cancelling && !n_tfail) {
        /* just reading archives: no report, no dialog to close */
        dlg_close();
        finished = 0;
        tn = tpos = 0;
        return;
    }
    log_add("%s: %d done, %d failed, %d skipped, %d renamed in %s",
            cancelling ? "Cancelled" : "Finished", n_ok, n_fail + n_tfail, n_skip, n_ren,
            secs(now_ms() - batch_t0, t));
    cur = 0;
    strcpy(t_btn, "OK");
    strcpy(t_file, "");
    set_total_text();
    set_stat_text(100);
    dlg_draw(P_ROOT, 0);
}

static void start_next(void)
{
    want_next = 0;
    if (cur || !running)
        return;
    while (tpos < tn && !cancelling) {
        TASK *t = &tasks[tpos];
        JOB *j = calloc(1, sizeof(JOB));
        if (!j) {
            n_tfail++;
            tpos++;
            continue;
        }
        j->q.op = t->op;
        strncpy(j->q.path, t->path, sizeof j->q.path - 1);
        strncpy(j->q.out, t->out, sizeof j->q.out - 1);
        strncpy(j->q.list, t->list, sizeof j->q.list - 1);
        j->q.afmt = t->afmt;
        j->q.aflags = (cs.replace ? AJ_REPLACE : 0) | (cs.rename83 ? AJ_RENAME83 : 0) |
                      (cs.subdirs ? AJ_SUBDIRS : 0) | (cs.paths ? AJ_PATHS : 0) |
                      (cs.best ? AJ_BEST : 0);
        j->q.pf = prefs;
        j->win = (VWIN *)&cur;               /* any non-null tag */
        f_done = 0;
        f_total = 1;
        item_i = item_n = 0;
        cur = j;
        cur_t0 = now_ms();
        if (t->op == OP_ARC_EXTRACT)
            log_add("Extracting %s to %s", t->path, t->out);
        else if (t->op == OP_ARC_ADD)
            log_add("Adding %d item%s to %s", t->nitems, t->nitems == 1 ? "" : "s", t->path);
        strcpy(t_file, "");
        set_total_text();
        set_stat_text((int)(tpos * 100L / (tn ? tn : 1)));
        dlg_draw(P_ROOT, 0);
        dbg("task %d: op %d %s -> %s", tpos, t->op, t->path, t->out);
        if (!job_start(j)) {
            free(j);
            cur = 0;
            n_tfail++;
            tpos++;
            continue;
        }
        return;
    }
    finish_all();
}

/* the GUI side of job.c */
void win_job_finished(VWIN *vw, JOB *j)
{
    TASK *t = tpos < tn ? &tasks[tpos] : 0;
    AWIN *w = t ? find_aw(t->handle) : 0;
    char tm[24];
    (void)vw;
    if (j->res.err == E_CANCEL || j->cancel) {
        cancelling = 1;
        log_add("%s: cancelled", base_name(j->q.path));
    } else if (j->res.err) {
        n_tfail++;
        if (!first_err[0])
            sprintf(first_err, "%.20s: %.40s", base_name(j->q.path),
                    j->res.detail[0] ? j->res.detail : fv_errstr(j->res.err));
        log_add("%s: failed, %s%s%s", base_name(j->q.path), fv_errstr(j->res.err),
                j->res.detail[0] ? ": " : "", j->res.detail);
    } else if (j->q.op != OP_ARC_LIST)
        log_add("%s: %ld done, %ld failed, %ld skipped, %ld renamed  %s", base_name(j->q.path),
                j->res.acount[0], j->res.acount[1], j->res.acount[2], j->res.acount[3],
                secs(now_ms() - cur_t0, tm));
    if (j->q.op != OP_ARC_LIST) {
        n_ok += (int)j->res.acount[0];
        n_fail += (int)j->res.acount[1];
        n_skip += (int)j->res.acount[2];
        n_ren += (int)j->res.acount[3];
    }
    if (j->q.op == OP_ARC_LIST && w) {
        w->busy = 0;
        if (!j->res.err && j->res.ptr[RB_DOC])
            aw_take_list(w, j->res.ptr[RB_DOC], j->res.doclen);
        else if (j->res.err && !j->cancel) {
            /* not an archive after all: drop the window */
            aw_close(w);
            w = 0;
        }
        if (w)
            aw_changed(w);
    }
    {
        int i;
        for (i = 0; i < RB_MAX; i++)
            job_release(&j->res, i);
    }
    if (t && t->list[0])
        Fdelete(t->list);
    /* the archive changed: read it again */
    if (j->q.op == OP_ARC_ADD && !j->res.err) {
        AWIN *aw2 = find_aw_path(j->q.path);
        if (aw2) {
            aw2->isnew = 0;
            queue_list(aw2);
        }
    }
    if (cur == j)
        cur = 0;
    free(j);
    tpos++;
    if (cancelling) {
        int k;
        for (k = tpos; k < tn; k++) {
            AWIN *x = find_aw(tasks[k].handle);
            if (tasks[k].list[0])
                Fdelete(tasks[k].list);
            if (x) {
                x->busy = 0;
                aw_set_info(x);
            }
        }
        tpos = tn;
        finish_all();
    } else
        want_next = 1;                       /* from the main loop */
}

void win_stage(VWIN *w, int stage)
{
    (void)w;
    (void)stage;
}

/* log lines and "now doing" from the job */
void win_note(VWIN *w, int kind, const char *text)
{
    (void)w;
    if (kind == NOTE_LOG) {
        log_add("  %s", text);
        return;
    }
    if (kind == NOTE_ITEM && cur) {
        int i = 0, n = 0;
        const char *s = text;
        i = atoi(s);
        while (*s && *s != ' ') s++;
        if (*s) s++;
        n = atoi(s);
        while (*s && *s != ' ') s++;
        if (*s) s++;
        item_i = i;
        item_n = n;
        if (n > 0)
            sprintf(t_file, "%d/%d %.30s", i, n, base_name(s));
        else
            sprintf(t_file, "%.36s", base_name(s));
        dlg_draw(P_FILE, 0);
    }
}

void win_progress(VWIN *w, long done, long total)
{
    static int lastpct = -1, lastf = -1;
    int pct, fp;
    (void)w;
    if (total <= 0) total = 1;
    f_done = done;
    f_total = total;
    fp = (int)(done * 100 / total);
    pct = (int)((tpos * 100L + fp) / (tn ? tn : 1));
    if (pct == lastpct && fp == lastf)
        return;
    lastpct = pct;
    lastf = fp;
    set_stat_text(pct);
    dlg_draw(P_TBOX, 0);
    dlg_draw(P_FBOX, 0);
    dlg_draw(P_STAT, 0);
}

static void cancel_all(void)
{
    if (!running || cancelling)
        return;
    cancelling = 1;
    ptree[P_BTN].ob_state |= SELECTED;
    set_stat_text(0);
    dlg_draw(P_BTN, 0);
    dlg_draw(P_STAT, 0);
    if (cur)
        job_cancel(cur);
    else {
        tpos = tn;
        finish_all();
    }
}

static int choose_dest(void)
{
    char path[300], name[64] = "";
    char *s;
    if (cs.dest[0])
        strcpy(path, cs.dest);
    else
        strcpy(path, app_folder);
    strcat(path, "*.*");
    if (!fa_file_select(path, name, "Destination folder (OK)"))
        return 0;
    s = strrchr(path, '\\');
    if (!s)
        return 0;
    s[1] = 0;
    strcpy(cs.dest, path);
    settings_save();
    return 1;
}

static void show_log(void);

/* tasks were queued: start (or carry on) */
static void begin(void)
{
    int i, quiet = 1;
    if (tpos >= tn)
        return;
    for (i = tpos; i < tn; i++)
        if (tasks[i].op != OP_ARC_LIST)
            quiet = 0;
    if (!quiet)
        show_log();                          /* open the icon: the log */
    if (running)
        return;                              /* added to the running batch */
    /* a new batch: forget the tasks of the last one */
    if (tpos > 0) {
        memmove(tasks, tasks + tpos, (tn - tpos) * sizeof(TASK));
        tn -= tpos;
        tpos = 0;
    }
    batch_t0 = now_ms();
    if (!quiet) {
        time_t now = time(0);
        struct tm *tm = localtime(&now);
        char when[32];
        if (tm)
            sprintf(when, "%04d-%02d-%02d %02d:%02d", tm->tm_year + 1900, tm->tm_mon + 1,
                    tm->tm_mday, tm->tm_hour, tm->tm_min);
        else
            strcpy(when, "");
        log_add("%s  %d task%s", when, tn, tn == 1 ? "" : "s");
    }
    running = 1;
    finished = cancelling = 0;
    n_ok = n_fail = n_skip = n_ren = n_tfail = 0;
    first_err[0] = 0;
    cur = 0;
    strcpy(t_btn, "Cancel");
    ptree[P_BTN].ob_state = 0;
    t_file[0] = 0;
    set_total_text();
    set_stat_text(0);
    dlg_open();
    ptree[P_BTN].ob_state = 0;
    want_next = 1;
}

static void dlg_button(void)
{
    if (finished) {
        dlg_close();
        finished = 0;
        tn = tpos = 0;
    } else
        cancel_all();
}

/* ------------------------------------------------- what was dropped */

static char **loose;                         /* not archives: to be added  */
static int  nloose, caploose;

static void loose_add(const char *p)
{
    char *s;
    if (nloose == caploose) {
        int nc = caploose ? caploose * 2 : 32;
        char **nl = realloc(loose, nc * sizeof(char *));
        if (!nl)
            return;
        loose = nl;
        caploose = nc;
    }
    s = malloc(strlen(p) + 1);
    if (!s)
        return;
    strcpy(s, p);
    loose[nloose++] = s;
}

static void loose_clear(void)
{
    int i;
    for (i = 0; i < nloose; i++)
        free(loose[i]);
    nloose = 0;
}

static int new_archive_name(char *out, const char *suggest);

/* one name dropped on the icon or given on the command line */
static void drop_path(const char *p)
{
    char t[256];
    size_t n;
    if (!*p)
        return;
    strncpy(t, p, sizeof t - 1);
    t[sizeof t - 1] = 0;
    n = strlen(t);
    if (n > 3 && (t[n - 1] == '\\' || t[n - 1] == '/'))
        t[n - 1] = 0;
    if (!is_dir(t)) {
        int fmt = af_detect(t);
        if (fmt != AF_NONE) {
            if (cs.xdrop)
                queue_extract(t, 0, 0);
            else
                aw_open(t, fmt, 0);
            return;
        }
    }
    loose_add(t);
}

/* files that are not archives were dropped: put them in a new one */
static void loose_done(AWIN *into)
{
    char arc[300];
    if (!nloose)
        return;
    if (into) {
        queue_add(into->path, into->fmt, loose, nloose, into);
    } else if (new_archive_name(arc, nloose == 1 ? loose[0] : 0)) {
        int fmt = af_from_name(arc);
        if (exists(arc) && af_detect(arc) != AF_NONE)
            fmt = af_detect(arc);
        if (fmt == AF_NONE)
            fmt = cs.fmt;
        queue_add(arc, fmt, loose, nloose, find_aw_path(arc));
    }
    loose_clear();
}

/* "ARGS": names separated by spaces, quoted with ' ('' is a quote) or " */
static void each_arg(char *s, void (*fn)(const char *))
{
    char name[256];
    while (*s) {
        int n = 0;
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
            s++;
        if (!*s)
            break;
        if (*s == '\'' || *s == '"') {
            char q = *s++;
            while (*s) {
                if (*s == q) {
                    if (q == '\'' && s[1] == '\'') {
                        if (n < 255) name[n++] = '\'';
                        s += 2;
                        continue;
                    }
                    s++;
                    break;
                }
                if (n < 255) name[n++] = *s;
                s++;
            }
        } else
            while (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') {
                if (n < 255) name[n++] = *s;
                s++;
            }
        name[n] = 0;
        if (n)
            fn(name);
    }
}

/* names dropped: on an archive window they are added to it, on the
   icon (or the log) archives are extracted / opened and the rest packed */
static void dropped(char *args, int handle)
{
    AWIN *w = find_aw(handle);
    graf_mouse(BUSYBEE, 0);
    if (w)
        each_arg(args, loose_add);
    else
        each_arg(args, drop_path);
    graf_mouse(ARROW, 0);
    if (w && nloose) {
        int i;
        /* not the archive itself */
        for (i = 0; i < nloose; i++)
            if (same_path(loose[i], w->path)) {
                free(loose[i]);
                loose[i--] = loose[--nloose];
            }
    }
    loose_done(w);
    begin();
}

/* ------------------------------------------------------- the main window */

static int  mainw;                           /* window handle             */
static int  iconified;                       /* 1 AES, 2 our own          */
static GRECT main_full;                      /* size before iconifying    */
static short ic_data[64], ic_mask[64];
static ICONBLK ic_blk;
static OBJECT ic_tree[2];
static char ic_label[] = "FArchive";

static void ic_set(int x, int y)
{
    if (x >= 0 && x < 32 && y >= 0 && y < 32)
        ic_data[y * 2 + (x >> 4)] |= (short)(0x8000U >> (x & 15));
}

/* a box with a strap and a zip */
static void fa_build_icon(void)
{
    int x, y;
    memset(ic_data, 0, sizeof ic_data);
    for (x = 3; x < 29; x++) { ic_set(x, 9); ic_set(x, 28); ic_set(x, 13); }
    for (y = 9; y < 29; y++) { ic_set(3, y); ic_set(28, y); }
    for (x = 6; x < 26; x++) ic_set(x, 4);   /* lid */
    for (y = 4; y < 10; y++) { ic_set(6 - (y - 4) / 2, y); ic_set(25 + (y - 4) / 2, y); }
    for (y = 13; y < 28; y++) {              /* zip down the middle */
        ic_set(15, y); ic_set(16, y);
        if (y & 1) { ic_set(14, y); ic_set(17, y); }
    }
    for (x = 13; x < 19; x++) { ic_set(x, 15); ic_set(x, 18); }    /* pull */
    ic_set(13, 16); ic_set(13, 17); ic_set(18, 16); ic_set(18, 17);
    for (y = 0; y < 32; y++) {
        ic_mask[y * 2] = (y >= 4 && y <= 28) ? (short)0x1FFF : 0;
        ic_mask[y * 2 + 1] = (y >= 4 && y <= 28) ? (short)0xFFF8 : 0;
        if (y >= 9 && y <= 28) {
            ic_mask[y * 2] = 0x1FFF;
            ic_mask[y * 2 + 1] = (short)0xFFF8;
        }
    }
    memset(&ic_blk, 0, sizeof ic_blk);
    ic_blk.ib_pmask = ic_mask;
    ic_blk.ib_pdata = ic_data;
    ic_blk.ib_ptext = ic_label;
    ic_blk.ib_char = 0x1000;
    ic_blk.ib_xicon = 16; ic_blk.ib_yicon = 0;
    ic_blk.ib_wicon = 32; ic_blk.ib_hicon = 32;
    ic_blk.ib_xtext = 0; ic_blk.ib_ytext = 32;
    ic_blk.ib_wtext = 64; ic_blk.ib_htext = 8;
    obj_add(ic_tree, 0, -1, G_IBOX, 0, 0, 0L, 0, 0, 64, 40);
    obj_add(ic_tree, 1, 0, G_ICON, LASTOB, 0, (long)&ic_blk, 0, 0, 64, 40);
}

static void draw_main(GRECT *clip)
{
    GRECT wk;
    short pxy[4], d;
    char line[300];
    int x, y;
    wind_get_grect(mainw, WF_WORKXYWH, &wk);
    pxy[0] = clip->g_x; pxy[1] = clip->g_y;
    pxy[2] = clip->g_x + clip->g_w - 1; pxy[3] = clip->g_y + clip->g_h - 1;
    vsf_interior(vdih, 1);
    vsf_color(vdih, 0);
    vswr_mode(vdih, MD_REPLACE);
    vr_recfl(vdih, pxy);
    if (iconified) {
        ic_tree[0].ob_x = wk.g_x;
        ic_tree[0].ob_y = wk.g_y;
        ic_tree[0].ob_width = wk.g_w;
        ic_tree[0].ob_height = wk.g_h;
        ic_tree[1].ob_x = (wk.g_w - ic_tree[1].ob_width) / 2;
        ic_tree[1].ob_y = (wk.g_h - ic_tree[1].ob_height) / 2;
        if (ic_tree[1].ob_y < 0) ic_tree[1].ob_y = 0;
        objc_draw_grect(ic_tree, 0, 2, clip);
        return;
    }
    vst_height(vdih, cell_h > 10 ? 13 : 6, &d, &d, &d, &d);
    vst_alignment(vdih, 0, 5, &d, &d);
    vst_color(vdih, 1);
    vswr_mode(vdih, MD_TRANS);
    x = wk.g_x + 8;
    y = wk.g_y + 4;
    vst_effects(vdih, 1);
    v_gtext(vdih, x, y, "FArchive " FV_VERSION " (" FV_CPU ") - drop archives or files here");
    vst_effects(vdih, 0);
    y += cell_h + 6;
    sprintf(line, "New archives: %s%s", af_name(cs.fmt), cs.best ? ", best compression" : "");
    v_gtext(vdih, x, y, line); y += cell_h;
    sprintf(line, "Dropped archives are %s", cs.xdrop ? "extracted" : "opened");
    v_gtext(vdih, x, y, line); y += cell_h;
    sprintf(line, "Destination:  %.50s", cs.dest[0] ? cs.dest : "(asked for first)");
    v_gtext(vdih, x, y, line); y += cell_h;
    sprintf(line, "Folder per archive %s, keep folders %s, replace %s, 8.3 names %s",
            cs.folder ? "on" : "off", cs.paths ? "on" : "off", cs.replace ? "on" : "off",
            cs.rename83 ? "on" : "off");
    v_gtext(vdih, x, y, line); y += cell_h;
    sprintf(line, "%s", !have_mint ? "TOS: working inside FArchive" :
            !have_worker ? "MiNT: FAWORK.TTP not found, working inside FArchive" :
            "MiNT: working in background workers");
    v_gtext(vdih, x, y, line); y += cell_h + 3;
    {
        short ln[4];
        long i, first = lognum > LOGN ? lognum - LOGN : 0;
        int vis = log_visible();
        ln[0] = wk.g_x; ln[2] = wk.g_x + wk.g_w - 1;
        ln[1] = ln[3] = y;
        vsl_color(vdih, 1);
        vsl_width(vdih, 1);
        v_pline(vdih, 2, ln);
        y += 4;
        if (!lognum)
            v_gtext(vdih, x, y, "(what FArchive does is listed here)");
        for (i = logtop < first ? first : logtop; i < lognum && i < logtop + vis; i++) {
            v_gtext(vdih, x, y, logl[i % LOGN]);
            y += cell_h;
        }
    }
}

#define HDR_LINES 6                          /* title + 5 lines above the log */

static int log_visible(void)
{
    GRECT wk;
    int n;
    if (mainw <= 0 || iconified)
        return 20;
    wind_get_grect(mainw, WF_WORKXYWH, &wk);
    n = (wk.g_h - (4 + HDR_LINES * cell_h + 6 + 7)) / cell_h;
    return n < 1 ? 1 : n;
}

static void clamp_top(void)
{
    long first = lognum > LOGN ? lognum - LOGN : 0, maxtop = lognum - log_visible();
    if (maxtop < first) maxtop = first;
    if (logtop > maxtop) logtop = maxtop;
    if (logtop < first) logtop = first;
}

static void set_slider(void)
{
    long first = lognum > LOGN ? lognum - LOGN : 0, avail = lognum - first;
    int vis = log_visible();
    if (mainw <= 0 || iconified)
        return;
    clamp_top();
    if (avail <= vis) {
        wind_set(mainw, WF_VSLSIZE, 1000, 0, 0, 0);
        wind_set(mainw, WF_VSLIDE, 0, 0, 0, 0);
    } else {
        wind_set(mainw, WF_VSLSIZE, (short)(1000L * vis / avail), 0, 0, 0);
        wind_set(mainw, WF_VSLIDE, (short)(1000L * (logtop - first) / (avail - vis)), 0, 0, 0);
    }
}

static void post_redraw(void)
{
    post_redraw_h(mainw);
}

static void log_changed(int was_bottom)
{
    if (was_bottom)
        logtop = lognum - log_visible();
    clamp_top();
    if (mainw > 0 && !iconified) {
        set_slider();
        post_redraw();
    }
}

static void main_redraw(GRECT *area)
{
    GRECT r, a = *area;
    if (mainw <= 0 || !rc_intersect(&desk, &a))
        return;
    wind_update(BEG_UPDATE);
    graf_mouse(M_OFF, 0);
    wind_get_grect(mainw, WF_FIRSTXYWH, &r);
    while (r.g_w && r.g_h) {
        if (rc_intersect(&a, &r)) {
            short clip[4];
            clip[0] = r.g_x; clip[1] = r.g_y;
            clip[2] = r.g_x + r.g_w - 1; clip[3] = r.g_y + r.g_h - 1;
            vs_clip(vdih, 1, clip);
            draw_main(&r);
        }
        wind_get_grect(mainw, WF_NEXTXYWH, &r);
    }
    {
        short none[4] = { 0, 0, 0, 0 };
        vs_clip(vdih, 0, none);
    }
    graf_mouse(M_ON, 0);
    wind_update(END_UPDATE);
}

/* the icon sits at the bottom left, to the right of FView's and FConv's */
static void icon_rect(GRECT *r)
{
    GRECT wk;
    if (aes_iconify) {
        r->g_w = 72;
        r->g_h = 72;
    } else {
        wk.g_x = wk.g_y = 0;
        wk.g_w = 72;
        wk.g_h = 44;
        wind_calc_grect(WC_BORDER, NAME | CLOSER | MOVER, &wk, r);
    }
    r->g_x = desk.g_x + 2 * (r->g_w + 8);
    r->g_y = desk.g_y + desk.g_h - r->g_h;
}

#define MAINK (NAME | CLOSER | MOVER | SIZER | FULLER | UPARROW | DNARROW | VSLIDE)
#define ICONK (NAME | CLOSER | MOVER)

/* (re)create the main window with the given gadgets */
static void make_window(int kind, GRECT *r, const char *title)
{
    if (mainw > 0) {
        wind_close(mainw);
        wind_delete(mainw);
    }
    mainw = wind_create_grect(kind, r);
    if (mainw <= 0) {
        mainw = 0;
        return;
    }
    wind_set_str(mainw, WF_NAME, title);
    wind_open_grect(mainw, r);
}

static void to_icon(void)
{
    GRECT r;
    icon_rect(&r);
    if (aes_iconify) {
        wind_get_grect(mainw, WF_CURRXYWH, &main_full);
        wind_set_grect(mainw, WF_ICONIFY, &r);
        iconified = 1;
        wind_set_str(mainw, WF_NAME, APPNAME);
    } else {
        /* no WF_ICONIFY: a small window of its own shows the icon */
        wind_get_grect(mainw, WF_CURRXYWH, &main_full);
        iconified = 2;
        make_window(ICONK, &r, APPNAME);
    }
    post_redraw();
}

static void from_icon(GRECT *r)
{
    if (iconified == 1) {
        wind_set_grect(mainw, WF_UNICONIFY, r);
        iconified = 0;
        wind_set_str(mainw, WF_NAME, " FArchive ");
    } else {
        iconified = 0;
        make_window(MAINK | (aes_iconify ? SMALLER : 0), &main_full, " FArchive ");
    }
    logtop = lognum - log_visible();
    set_slider();
    post_redraw();
}

/* open the icon into the log window (and bring it forward) */
static void show_log(void)
{
    if (mainw <= 0)
        return;
    if (iconified == 1) {
        GRECT r = main_full;
        if (r.g_w <= 0 || r.g_h <= 0)
            wind_get_grect(mainw, WF_UNICONIFY, &r);
        from_icon(&r);
    } else if (iconified == 2)
        from_icon(&main_full);
    wind_set(mainw, WF_TOP, 0, 0, 0, 0);
    if (pdlg > 0)
        wind_set(pdlg, WF_TOP, 0, 0, 0, 0);  /* the progress stays in front */
}

static void open_main(void)
{
    GRECT wk, r;
    int k = MAINK | (aes_iconify ? SMALLER : 0);
    wk.g_x = wk.g_y = 0;
    wk.g_w = 66 * cell_w;
    wk.g_h = (HDR_LINES + 12) * cell_h + 20;
    wind_calc_grect(WC_BORDER, k, &wk, &r);
    if (r.g_w > desk.g_w) r.g_w = desk.g_w;
    if (r.g_h > desk.g_h) r.g_h = desk.g_h;
    r.g_x = desk.g_x + (desk.g_w - r.g_w) / 2;
    r.g_y = desk.g_y + (desk.g_h - r.g_h) / 3;
    main_full = r;
    make_window(k, &r, " FArchive ");
    if (mainw <= 0)
        return;
    to_icon();
}

/* ------------------------------------------------------------- commands */

static void about(void)
{
    char buf[300];
    sprintf(buf, "[1][FArchive " FV_VERSION " - archiver|"
            FV_CPU " version|"
            "ZIP LHA ZOO ARC LZX TAR GZ.|Drop archives or files on the icon.|"
            "%s][ OK ]",
            !have_mint ? "TOS: working in FArchive" :
            !have_worker ? "MiNT: FAWORK.TTP not found" :
                           "MiNT: background workers");
    form_alert(1, buf);
}

static char sel_path[300];

static int pick_file(char *full, const char *title, int allow_folder)
{
    static char name[64];
    char *s;
    if (!sel_path[0]) {
        strcpy(sel_path, cs.dest[0] ? cs.dest : app_folder);
        strcat(sel_path, "*.*");
    }
    name[0] = 0;
    if (!fa_file_select(sel_path, name, title))
        return 0;
    strcpy(full, sel_path);
    s = strrchr(full, '\\');
    if (s) s[1] = 0;
    if (name[0])
        strcat(full, name);
    else if (allow_folder && s)
        s[0] = 0;                            /* no name: the whole folder */
    else
        return 0;
    return 1;
}

static void do_open(void)
{
    char full[300];
    int fmt;
    if (!pick_file(full, "Open archive", 0))
        return;
    fmt = af_detect(full);
    if (fmt == AF_NONE) {
        alertf("[1][FArchive: %.30s|is not an archive FArchive|can read.][ OK ]",
               base_name(full));
        return;
    }
    aw_open(full, fmt, 0);
}

static void do_extract_file(void)
{
    char full[300];
    if (!pick_file(full, "Extract archive", 0))
        return;
    if (af_detect(full) == AF_NONE) {
        alertf("[1][FArchive: %.30s|is not an archive FArchive|can read.][ OK ]",
               base_name(full));
        return;
    }
    queue_extract(full, 0, 0);
    begin();
}

/* a name for a new archive, with the extension of the format */
static int new_archive_name(char *out, const char *suggest)
{
    char path[300], name[64], *s;
    int f;
    if (cs.dest[0])
        strcpy(path, cs.dest);
    else
        strcpy(path, app_folder);
    strcat(path, "*.");
    strcat(path, af_ext(cs.fmt));
    if (suggest) {
        const char *b = base_name(suggest), *dot = strrchr(b, '.');
        size_t n = dot && dot != b ? (size_t)(dot - b) : strlen(b);
        if (n > 8) n = 8;
        memcpy(name, b, n);
        name[n] = 0;
    } else
        strcpy(name, "ARCHIVE");
    strcat(name, ".");
    strcat(name, af_ext(cs.fmt));
    if (!fa_file_select(path, name, "Name of the new archive"))
        return 0;
    s = strrchr(path, '\\');
    if (!s || !name[0])
        return 0;
    s[1] = 0;
    strcpy(out, path);
    strcat(out, name);
    f = af_from_name(out);
    if (f == AF_NONE && !strchr(name, '.')) {
        strcat(out, ".");
        strcat(out, af_ext(cs.fmt));
    }
    return 1;
}

static void do_new(void)
{
    char arc[300];
    int fmt;
    if (!new_archive_name(arc, 0))
        return;
    if (exists(arc)) {
        fmt = af_detect(arc);
        if (fmt == AF_NONE) {
            alertf("[1][FArchive: %.30s|exists and is not an archive.][ OK ]", base_name(arc));
            return;
        }
        aw_open(arc, fmt, 0);
        return;
    }
    fmt = af_from_name(arc);
    aw_open(arc, fmt != AF_NONE ? fmt : cs.fmt, 1);
}

static void do_add_files(void)
{
    AWIN *w = top_awin();
    char full[300];
    char *one[1];
    if (!w)
        return;
    if (!pick_file(full, "Add to the archive (no name: folder)", 1))
        return;
    one[0] = full;
    queue_add(w->path, w->fmt, one, 1, w);
    begin();
}

static void do_extract_sel(void)
{
    AWIN *w = top_awin();
    if (!w || w->isnew)
        return;
    queue_extract(w->path, w, 1);
    begin();
}

static void select_all(AWIN *w, int on)
{
    int i;
    if (!w)
        return;
    for (i = 0; i < w->n; i++)
        w->e[i].sel = (u8)on;
    aw_changed(w);
}

static void changed(void)
{
    settings_save();
    fa_menu_update();
    if (!iconified)
        post_redraw();
}

static void do_menu(int title, int item)
{
    int t = title - 3, k = item - m_first[t >= 0 && t < NTITLE ? t : 0];
    switch (t) {
    case 0:
        if (k == D_ABOUT) about();
        break;
    case 1:
        if (k == F_OPEN) do_open();
        else if (k == F_NEW) do_new();
        else if (k == F_EXTRACT) do_extract_file();
        else if (k == F_XSEL) do_extract_sel();
        else if (k == F_ADD) do_add_files();
        else if (k == F_SELALL) select_all(top_awin(), 1);
        else if (k == F_CLOSE) { AWIN *w = top_awin(); if (w) aw_close(w); }
        else if (k == F_DEST) { choose_dest(); changed(); }
        else if (k == F_QUIT) quitting = 1;
        break;
    case 2:
        if (k >= X_FIRST && k < X_FIRST + AF_COUNT - 1) cs.fmt = k - X_FIRST + 1;
        else if (k == X_BEST) cs.best = !cs.best;
        changed();
        break;
    case 3:
        if (k == O_XDROP) cs.xdrop = !cs.xdrop;
        else if (k == O_FOLDER) cs.folder = !cs.folder;
        else if (k == O_PATHS) cs.paths = !cs.paths;
        else if (k == O_REPLACE) cs.replace = !cs.replace;
        else if (k == O_SUBDIRS) cs.subdirs = !cs.subdirs;
        else if (k == O_83) cs.rename83 = !cs.rename83;
        changed();
        break;
    }
    menu_tnormal(menu, title, 1);
}

/* ------------------------------------------------------------- messages */

/* MultiTOS drag & drop: msg[3] is the window it was dropped on */
static void drag_drop(short *msg)
{
    char pipe[24], ext[32], hdr[256];
    short hlen;
    long fd, size;
    char c;
    int tries;
    strcpy(pipe, "U:\\PIPE\\DRAGDROP.AA");
    pipe[17] = (char)(msg[7] >> 8);
    pipe[18] = (char)(msg[7] & 0xFF);
    fd = Fopen(pipe, 2);
    dbg("AP_DRAGDROP window %d, pipe %s -> %ld", msg[3], pipe, fd);
    if (fd < 0)
        return;
    c = DD_OK;
    Fwrite((short)fd, 1, &c);
    memset(ext, 0, sizeof ext);
    memcpy(ext, "ARGS", 4);
    Fwrite((short)fd, 32, ext);
    for (tries = 0; tries < 8; tries++) {
        char type[5];
        if (Fread((short)fd, 2, &hlen) != 2 || hlen < 8)
            break;
        if (hlen > (short)sizeof hdr) {
            Fread((short)fd, sizeof hdr, hdr);
            Fseek(hlen - sizeof hdr, (short)fd, 1);
        } else
            Fread((short)fd, hlen, hdr);
        memcpy(type, hdr, 4);
        type[4] = 0;
        size = ((long)(u8)hdr[4] << 24) | ((long)(u8)hdr[5] << 16) |
               ((u8)hdr[6] << 8) | (u8)hdr[7];
        dbg("drag & drop offers '%s', %ld bytes", type, size);
        if (!strcmp(type, "ARGS") && size > 0 && size < 32768L) {
            char *buf = malloc(size + 1);
            if (!buf) {
                c = DD_NAK;
                Fwrite((short)fd, 1, &c);
                break;
            }
            c = DD_OK;
            Fwrite((short)fd, 1, &c);
            size = Fread((short)fd, size, buf);
            Fclose((short)fd);             /* let the desktop go first */
            if (size > 0) {
                buf[size] = 0;
                dropped(buf, msg[3]);
            }
            free(buf);
            return;
        }
        c = DD_EXT;
        Fwrite((short)fd, 1, &c);
    }
    Fclose((short)fd);
}

static void va_start_msg(short *msg)
{
    char *args = *(char **)&msg[3];
    short reply[8];
    char copy[1024];
    copy[0] = 0;
    if (args) {
        strncpy(copy, args, sizeof copy - 1);
        copy[sizeof copy - 1] = 0;
    }
    memset(reply, 0, sizeof reply);
    reply[0] = AV_STARTED;
    reply[1] = ap_id;
    reply[3] = msg[3];
    reply[4] = msg[4];
    appl_write(msg[1], 16, reply);
    if (copy[0])
        dropped(copy, 0);
}

/* AV desktops (Thing, Jinnee, TeraDesk...) report files dragged onto one
   of our windows - the icon too - with VA_DRAGACCWIND: msg[3] window,
   msg[4]/[5] mouse position, msg[6]/[7] the names                     */
static void drag_acc_wind(short *msg)
{
    char *args = *(char **)&msg[6];
    char copy[1024];
    if (!args)
        return;
    strncpy(copy, args, sizeof copy - 1);
    copy[sizeof copy - 1] = 0;
    dbg("VA_DRAGACCWIND window %d: '%s'", msg[3], copy);
    dropped(copy, msg[3]);
}

static void av_hello(void)
{
    char *srv = getenv("AVSERVER");
    static char myname[9] = "FARCHIVE";
    short msg[8], id;
    char name[9];
    if (!srv)
        return;
    memset(name, ' ', 8);
    name[8] = 0;
    memcpy(name, srv, strlen(srv) > 8 ? 8 : strlen(srv));
    id = appl_find(name);
    if (id < 0)
        return;
    memset(msg, 0, sizeof msg);
    msg[0] = AV_PROTOKOLL;
    msg[1] = ap_id;
    msg[3] = 0x0012;                         /* VA_START, quoted names */
    *(char **)&msg[6] = myname;
    appl_write(id, 16, msg);
}

static void aw_message(AWIN *w, short *msg)
{
    GRECT *r = (GRECT *)&msg[4];
    switch (msg[0]) {
    case WM_REDRAW:  aw_redraw(w, r); break;
    case WM_TOPPED:  wind_set(w->handle, WF_TOP, 0, 0, 0, 0); fa_menu_update(); break;
    case WM_CLOSED:  aw_close(w); break;
    case WM_MOVED:   wind_set_grect(w->handle, WF_CURRXYWH, r); w->full = *r; break;
    case WM_SIZED: {
        GRECT n = *r;
        if (n.g_w < 30 * cell_w) n.g_w = 30 * cell_w;
        if (n.g_h < 6 * cell_h) n.g_h = 6 * cell_h;
        wind_set_grect(w->handle, WF_CURRXYWH, &n);
        aw_changed(w);
        break;
    }
    case WM_FULLED: {
        GRECT now;
        wind_get_grect(w->handle, WF_CURRXYWH, &now);
        if (now.g_w == desk.g_w && now.g_h == desk.g_h && w->full.g_w)
            now = w->full;
        else {
            w->full = now;
            now = desk;
        }
        wind_set_grect(w->handle, WF_CURRXYWH, &now);
        aw_changed(w);
        break;
    }
    case WM_ARROWED: {
        int vis = aw_rows(w);
        switch (msg[4]) {
        case 0: w->top -= vis; break;
        case 1: w->top += vis; break;
        case 2: w->top--; break;
        case 3: w->top++; break;
        }
        aw_changed(w);
        break;
    }
    case WM_VSLID: {
        int vis = aw_rows(w);
        if (w->n > vis)
            w->top = (long)msg[4] * (w->n - vis) / 1000;
        aw_changed(w);
        break;
    }
    }
}

static void message(short *msg)
{
    GRECT *r = (GRECT *)&msg[4];
    AWIN *w;
    if (msg[0] != WM_REDRAW)
        dbg("message %04x from %d: %d %d %d %d %d", (unsigned short)msg[0], msg[1],
            msg[3], msg[4], msg[5], msg[6], msg[7]);
    switch (msg[0]) {
    case MN_SELECTED:  do_menu(msg[3], msg[4]); return;
    case AP_TERM:      quitting = 1; return;
    case AP_DRAGDROP:  drag_drop(msg); return;
    case VA_START:     va_start_msg(msg); return;
    case VA_DRAGACCWIND: drag_acc_wind(msg); return;
    }
    if (pdlg > 0 && msg[3] == pdlg) {
        switch (msg[0]) {
        case WM_REDRAW: dlg_draw(P_ROOT, r); break;
        case WM_TOPPED: wind_set(pdlg, WF_TOP, 0, 0, 0, 0); break;
        case WM_MOVED:  wind_set_grect(pdlg, WF_CURRXYWH, r); break;
        }
        return;
    }
    if ((w = find_aw(msg[3])) != 0) {
        aw_message(w, msg);
        return;
    }
    if (mainw <= 0 || msg[3] != mainw)
        return;
    switch (msg[0]) {
    case WM_REDRAW:  main_redraw(r); break;
    case WM_TOPPED:  wind_set(mainw, WF_TOP, 0, 0, 0, 0); fa_menu_update(); break;
    case WM_MOVED:
        wind_set_grect(mainw, WF_CURRXYWH, r);
        if (!iconified)
            main_full = *r;
        break;
    case WM_CLOSED:
        /* closing the window makes it the icon again; closing the icon
           (or File > Quit) ends FArchive                              */
        if (iconified)
            quitting = 1;
        else
            to_icon();
        break;
    case WM_ICONIFY:
    case WM_ALLICONIFY:
        if (!iconified) {
            wind_get_grect(mainw, WF_CURRXYWH, &main_full);
            wind_set_grect(mainw, WF_ICONIFY, r);
            iconified = 1;
            wind_set_str(mainw, WF_NAME, APPNAME);
            post_redraw();
        }
        break;
    case WM_UNICONIFY:
        if (iconified == 1)
            from_icon(r);
        break;
    case WM_SIZED:
        if (!iconified) {
            GRECT n = *r;
            if (n.g_w < 30 * cell_w) n.g_w = 30 * cell_w;
            if (n.g_h < (HDR_LINES + 4) * cell_h) n.g_h = (HDR_LINES + 4) * cell_h;
            wind_set_grect(mainw, WF_CURRXYWH, &n);
            main_full = n;
            clamp_top();
            set_slider();
            post_redraw();
        }
        break;
    case WM_FULLED:
        if (!iconified) {
            static GRECT before;
            GRECT now;
            wind_get_grect(mainw, WF_CURRXYWH, &now);
            if (now.g_w == desk.g_w && now.g_h == desk.g_h && before.g_w)
                now = before;
            else {
                before = now;
                now = desk;
            }
            wind_set_grect(mainw, WF_CURRXYWH, &now);
            main_full = now;
            clamp_top();
            set_slider();
            post_redraw();
        }
        break;
    case WM_ARROWED:
        if (!iconified) {
            int vis = log_visible();
            switch (msg[4]) {
            case 0: logtop -= vis; break;        /* page up   */
            case 1: logtop += vis; break;        /* page down */
            case 2: logtop--; break;             /* line up   */
            case 3: logtop++; break;             /* line down */
            }
            clamp_top();
            set_slider();
            post_redraw();
        }
        break;
    case WM_VSLID:
        if (!iconified) {
            long first = lognum > LOGN ? lognum - LOGN : 0, avail = lognum - first;
            int vis = log_visible();
            if (avail > vis)
                logtop = first + (long)msg[4] * (avail - vis) / 1000;
            clamp_top();
            set_slider();
            post_redraw();
        }
        break;
    }
}

/* a click in an archive window: select it (Shift: add or take away one,
   Control: everything from the last click), double click: extract it  */
static void aw_click(AWIN *w, int mx, int my, int clicks, int ks)
{
    GRECT wk;
    int row, i;
    wind_get_grect(w->handle, WF_WORKXYWH, &wk);
    if (my < wk.g_y + cell_h + 4) {
        wind_set(w->handle, WF_TOP, 0, 0, 0, 0);
        return;
    }
    row = (int)w->top + (my - wk.g_y - cell_h - 4) / cell_h;
    (void)mx;
    if (row < 0 || row >= w->n) {
        if (!(ks & (K_LSHIFT | K_RSHIFT | K_CTRL)))
            select_all(w, 0);
        return;
    }
    if ((ks & K_CTRL) && w->anchor >= 0 && w->anchor < w->n) {
        int a = w->anchor < row ? w->anchor : row, b = w->anchor < row ? row : w->anchor;
        for (i = a; i <= b; i++)
            w->e[i].sel = 1;
    } else if (ks & (K_LSHIFT | K_RSHIFT)) {
        w->e[row].sel = !w->e[row].sel;
    } else {
        for (i = 0; i < w->n; i++)
            w->e[i].sel = 0;
        w->e[row].sel = 1;
    }
    if (!(ks & K_CTRL))
        w->anchor = row;
    aw_changed(w);
    if (clicks >= 2 && !(w->e[row].flags & 2)) {
        for (i = 0; i < w->n; i++)
            w->e[i].sel = i == row;
        queue_extract(w->path, w, 1);
        begin();
    }
}

static void button(int mx, int my, int clicks, int ks)
{
    int h = wind_find(mx, my);
    AWIN *w;
    if (pdlg > 0 && h == pdlg) {
        dlg_place();
        if (objc_find(ptree, 0, 8, mx, my) == P_BTN && !(ptree[P_BTN].ob_state & SELECTED))
            dlg_button();
        else
            wind_set(pdlg, WF_TOP, 0, 0, 0, 0);
        return;
    }
    if ((w = find_aw(h)) != 0) {
        aw_click(w, mx, my, clicks, ks);
        return;
    }
    if (h == mainw && iconified == 2) {      /* our own icon: open it */
        GRECT r;
        wind_get_grect(mainw, WF_CURRXYWH, &r);
        from_icon(&r);
    }
}

static void key(int ks, int k)
{
    int c = k & 0xFF;
    (void)ks;
    if (c == 15) do_open();                  /* ^O */
    else if (c == 14) do_new();              /* ^N */
    else if (c == 5) do_extract_file();      /* ^E */
    else if (c == 24) do_extract_sel();      /* ^X */
    else if (c == 1) do_add_files();         /* ^A */
    else if (c == 19) select_all(top_awin(), 1);     /* ^S */
    else if (c == 21) { AWIN *w = top_awin(); if (w) aw_close(w); }   /* ^U */
    else if (c == 4) { choose_dest(); changed(); }   /* ^D */
    else if (c == 17) quitting = 1;          /* ^Q */
    else if (c == 27 && pdlg > 0) dlg_button();      /* Esc */
}

static void events(long timer, int all)
{
    short msg[8], mx, my, mb, ks, k, clicks, ev;
    int n = 0;
    do {
        ev = evnt_multi(MU_MESAG | MU_KEYBD | MU_BUTTON | MU_TIMER,
                        0x102, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                        msg, timer, &mx, &my, &mb, &ks, &k, &clicks);
        if (ev & MU_MESAG) message(msg);
        if (ev & MU_BUTTON) button(mx, my, clicks, ks);
        if (ev & MU_KEYBD) key(ks, k);
        timer = 0;
    } while (all && (ev & (MU_MESAG | MU_KEYBD | MU_BUTTON)) && ++n < 16 && !quitting);
}

/* called by cooperative jobs (plain TOS) while they work */
int gui_coop_poll(void)
{
    static int in;
    if (in)
        return 0;
    in = 1;
    events(0L, 1);
    job_poll();
    in = 0;
    return 0;
}

/* ----------------------------------------------------------------- main */

static int detect_3d(void)
{
    short o1 = 0, o2 = 0, o3 = 0, o4 = 0;
    if ((_AESversion >= 0x0400 || appl_find("?AGI") >= 0) &&
        appl_getinfo(13, &o1, &o2, &o3, &o4))
        return o1 != 0;
    return _AESversion >= 0x0340;
}

static int detect_iconify(void)
{
    short o1 = 0, o2 = 0, o3 = 0, o4 = 0;
    if (_AESversion >= 0x0410)
        return 1;
    if (_AESversion < 0x0400 && appl_find("?AGI") < 0)
        return 0;
    if (!appl_getinfo(11, &o1, &o2, &o3, &o4))
        return 0;
    return (o1 & 0x80) != 0;
}

int main(int argc, char **argv)
{
    short work_in[11], work_out[57];
    int i;

    ap_id = appl_init();
    if (ap_id < 0)
        return 1;
    {
        short cw, ch, bw, bh, h;
        h = graf_handle(&cw, &ch, &bw, &bh);
        cell_w = cw;
        cell_h = ch;
        for (i = 0; i < 10; i++) work_in[i] = 1;
        work_in[10] = 2;
        v_opnvwk(work_in, &h, work_out);
        vdih = h;
    }
    if (vdih <= 0) {
        form_alert(1, "[3][FArchive: no VDI workstation.][ Quit ]");
        appl_exit();
        return 1;
    }
    scr_w = work_out[0] + 1;
    scr_h = work_out[1] + 1;
    pix_w_um = work_out[3];
    pix_h_um = work_out[4];
    wind_get_grect(0, WF_WORKXYWH, &desk);
    {
        short out[57];
        vq_extnd(vdih, 1, out);
        scr_planes = out[4];
    }

    prefs_default(&prefs);
    prefs.use_workers = 1;
    find_app_folder();
    settings_load();
    job_app_name = "farchive " FV_VERSION " (" FV_CPU ")";
    job_log_name = "FARCHIVE.LOG";
    job_worker_name = "FAWORK.TTP";
    job_init_system();
    aes_iconify = detect_iconify();
    aes_3d = detect_3d();
    dbg("AES %04x screen %dx%d planes %d, format %d dest '%s'",
        _AESversion, scr_w, scr_h, scr_planes, cs.fmt, cs.dest);

    if (_AESversion >= 0x0400)
        shel_write(9, 1, 0, 0L, 0L);          /* AP_TERM */
    if (_AESnumapps != 1 || _AESversion >= 0x0400)
        menu_register(ap_id, "  FArchive");

    fa_build_menu();
    menu_bar(menu, 1);
    fa_menu_update();
    fa_build_icon();
    build_dialog();
    graf_mouse(ARROW, 0);
    av_hello();
    open_main();

    for (i = 1; i < argc; i++)
        drop_path(argv[i]);
    if (argc > 1) {
        loose_done(0);
        begin();
    }

    while (!quitting) {
        events(job_any_active() || want_next ? 100L : 400L, 0);
        job_poll();
        if (want_next)
            start_next();
        job_run_pending();                   /* TOS: the job runs here */
        if (want_next)
            start_next();
    }

    if (cur)
        job_cancel(cur);
    job_kill_all();
    dlg_close();
    for (i = 0; i < MAXAW; i++)
        if (aw[i].handle > 0)
            aw_close(&aw[i]);
    if (mainw > 0) {
        wind_close(mainw);
        wind_delete(mainw);
    }
    menu_bar(menu, 0);
    v_clsvwk(vdih);
    appl_exit();
    return 0;
}
