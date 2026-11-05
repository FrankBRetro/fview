/*
 * fconv.c - FConv, batch picture converter for TOS / MultiTOS / MiNT.
 *
 * Starts as an icon. Pictures and folders dropped on it (AP_DRAGDROP),
 * sent with VA_START or given on the command line are converted to GEM
 * IMG files in the destination folder, one after the other, with a
 * non-modal progress dialog (all files, this file, Cancel).
 *
 * The work is done by the same job system as FView: under MiNT with
 * U:\SHM each file is converted by its own FVWORK.TTP at nice 19, so
 * FConv stays responsive; on plain TOS the jobs run cooperatively.
 *
 * Export formats (Format menu): the picture's own depth, 24-bit true
 * colour, the screen's colours (dithered; 24-bit on true colour screens)
 * or mono. Settings are kept in FCONV.INF next to the program.
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

#undef APPNAME
#define APPNAME "FConv"

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

long _stksize = 64 * 1024L;

/* what the shared code (job.c, the decoders) expects to find */
int  vdih, ap_id;
int  scr_w, scr_h, scr_planes, pix_w_um, pix_h_um;
int  cell_w, cell_h;
GRECT desk;
PREFS prefs;
DISPINFO dinfo;
int  quitting;
int  aes_iconify, aes_3d;
extern const char *job_app_name, *job_log_name;

/* ------------------------------------------------------------ settings */

typedef struct {
    int  pdf;                    /* write PDF files instead of IMG       */
    int  xfmt;                   /* XF_*                                 */
    int  fit;                    /* shrink to the screen size            */
    int  replace;                /* overwrite existing IMG files         */
    int  subdirs;                /* folders: include subfolders          */
    char dest[256];              /* destination folder, ends in '\'      */
} CSET;

static CSET cs = { 0, XF_SCREEN, 0, 0, 1, "" };
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

static int fc_file_select(char *path, char *name, const char *title)
{
    short btn = 0, ok;
    if (_AESversion >= 0x0140)
        ok = fsel_exinput(path, name, &btn, title);
    else
        ok = fsel_input(path, name, &btn);
    return ok && btn;
}

/* the folder FCONV.PRG was started from (ends in a backslash) */
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
    strcat(p, "FCONV.INF");
}

static void settings_save(void)
{
    char p[300];
    FILE *f;
    settings_path(p);
    f = fopen(p, "w");
    if (!f)
        return;
    fprintf(f, "FCONV 1\noutput=%s\nformat=%d\nfit=%d\nreplace=%d\nsubfolders=%d\n"
            "dither=%d\nserpentine=%d\nsmooth=%d\npalette=%d\ndest=%s\n",
            cs.pdf ? "pdf" : "img", cs.xfmt, cs.fit, cs.replace, cs.subdirs, prefs.dither,
            prefs.serpentine, prefs.smooth, prefs.palmode, cs.dest);
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
        if (!strcmp(line, "output")) cs.pdf = !strcmp(v, "pdf");
        else if (!strcmp(line, "format") && n >= XF_ORIGINAL && n <= XF_MONO) cs.xfmt = n;
        else if (!strcmp(line, "fit")) cs.fit = n != 0;
        else if (!strcmp(line, "replace")) cs.replace = n != 0;
        else if (!strcmp(line, "subfolders")) cs.subdirs = n != 0;
        else if (!strcmp(line, "dither") && n >= DITH_FS && n <= DITH_NONE) prefs.dither = (char)n;
        else if (!strcmp(line, "serpentine")) prefs.serpentine = n != 0;
        else if (!strcmp(line, "smooth")) prefs.smooth = n != 0;
        else if (!strcmp(line, "palette")) prefs.palmode = n ? PAL_SYSTEM : PAL_OPTIMISE;
        else if (!strcmp(line, "dest")) {
            strncpy(cs.dest, v, sizeof cs.dest - 2);
            cs.dest[sizeof cs.dest - 2] = 0;
        }
    }
    fclose(f);
}

/* ------------------------------------------------------------- display */

/* What "screen colours" means: the screen's depth and, on palette
 * screens, its current colours. Pixel values follow the standard VDI
 * layout, which is also what an IMG file's palette is written in.     */
static void disp_init(void)
{
    short out[57], rgb[3];
    int i, n;
    memset(&dinfo, 0, sizeof dinfo);
    vq_extnd(vdih, 1, out);
    scr_planes = out[4];
    dinfo.planes = scr_planes;
    dinfo.truecolor = scr_planes > 8;
    dinfo.pix_w = pix_w_um > 0 ? pix_w_um : 278;
    dinfo.pix_h = pix_h_um > 0 ? pix_h_um : 278;
    if (dinfo.truecolor)
        return;
    dinfo.fixed_palette = scr_planes == 1 || !out[5];
    n = scr_planes >= 8 ? 256 : 1 << scr_planes;
    for (i = 0; i < n; i++) {
        int pix = vdi2pix(i, scr_planes);
        vq_color(vdih, i, 1, rgb);
        dinfo.sysrgb[pix * 3]     = (u8)(rgb[0] * 255L / 1000);
        dinfo.sysrgb[pix * 3 + 1] = (u8)(rgb[1] * 255L / 1000);
        dinfo.sysrgb[pix * 3 + 2] = (u8)(rgb[2] * 255L / 1000);
    }
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
static char scr_item[40];
static const char *titles[NTITLE] = { " FConv ", " File ", " Format ", " Dither " };
static const char *m_desk[] = {
    "  About FConv...     ", "---------------------",
    "  Desk Accessory 1   ", "  Desk Accessory 2   ", "  Desk Accessory 3   ",
    "  Desk Accessory 4   ", "  Desk Accessory 5   ", "  Desk Accessory 6   "
};
static const char *m_file[] = {
    "  Convert files...      ^O ", "  Destination folder... ^D ",
    "---------------------------", "  Quit                  ^Q "
};
static const char *m_fmt[] = {
    "  GEM IMG files             ", "  PDF files                 ",
    "----------------------------",
    "  Original depth            ", "  True colour (24 bit)      ", scr_item,
    "  Mono (1 bit)              ", "----------------------------",
    "  Fit to screen size        ", "  Replace existing files    ",
    "  Include subfolders        "
};
static const char *m_dith[] = {
    "  Floyd-Steinberg     ", "  Atkinson            ", "  Ordered             ",
    "  None                ", "----------------------",
    "  Serpentine          ", "  Smooth shrink       ", "----------------------",
    "  Palette per picture ", "  System palette      "
};
enum {                                       /* item numbers inside menus */
    D_ABOUT = 0,
    F_CONVERT = 0, F_DEST = 1, F_QUIT = 3,
    X_IMG = 0, X_PDF = 1,
    X_ORIG = 3, X_TRUE, X_SCREEN, X_MONO, X_FIT = 8, X_REPLACE, X_SUBDIRS,
    T_FS = 0, T_ATK, T_ORD, T_NONE, T_SERP = 5, T_SMOOTH, T_OPT = 8, T_SYS
};
static const char **m_lists[NTITLE] = { m_desk, m_file, m_fmt, m_dith };
static const int m_counts[NTITLE] = { 8, 4, 11, 10 };
#define MENU_OBJS (8 + 8 + 4 + 11 + 10 + NTITLE + 8)
static OBJECT menu[MENU_OBJS];
static int m_first[NTITLE];                  /* object of each menu's item 0 */

static OBJECT *fc_build_menu(void)
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

static void fc_menu_update(void)
{
    int i;
    menu_icheck(menu, MI(2, X_IMG), !cs.pdf);
    menu_icheck(menu, MI(2, X_PDF), cs.pdf);
    for (i = 0; i <= XF_MONO; i++)
        menu_icheck(menu, MI(2, X_ORIG + i), cs.xfmt == i);
    menu_icheck(menu, MI(2, X_FIT), cs.fit);
    menu_icheck(menu, MI(2, X_REPLACE), cs.replace);
    menu_icheck(menu, MI(2, X_SUBDIRS), cs.subdirs);
    for (i = 0; i <= DITH_NONE; i++)
        menu_icheck(menu, MI(3, T_FS + i), prefs.dither == i);
    menu_icheck(menu, MI(3, T_SERP), prefs.serpentine);
    menu_icheck(menu, MI(3, T_SMOOTH), prefs.smooth);
    menu_icheck(menu, MI(3, T_OPT), prefs.palmode == PAL_OPTIMISE);
    menu_icheck(menu, MI(3, T_SYS), prefs.palmode == PAL_SYSTEM);
    /* the palette choice only matters for the screen's colours */
    menu_ienable(menu, MI(3, T_OPT), !dinfo.truecolor && !dinfo.fixed_palette);
    menu_ienable(menu, MI(3, T_SYS), !dinfo.truecolor);
}

/* ------------------------------------------------------------ the queue */

static const char *fmt_name(void);
static char **queue;                         /* input files               */
static int  qn, qcap, qpos;                  /* count, room, next to do   */
static int  n_ok, n_fail, n_skip, skip_shown;
static char first_err[90];
static int  running, cancelling, finished;   /* conversion state          */
static int  want_next;                       /* start the next file       */
static JOB *cur;                             /* job in progress           */
static int  cur_dith;                        /* this file has 3 phases    */

static void queue_add(const char *p)
{
    char *s;
    if (qn == qcap) {
        int nc = qcap ? qcap * 2 : 64;
        char **nq = realloc(queue, nc * sizeof(char *));
        if (!nq)
            return;
        queue = nq;
        qcap = nc;
    }
    s = malloc(strlen(p) + 1);
    if (!s)
        return;
    strcpy(s, p);
    queue[qn++] = s;
}

static void queue_clear(void)
{
    int i;
    for (i = 0; i < qn; i++)
        free(queue[i]);
    qn = qpos = 0;
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

static void log_add(const char *fmt, ...);
static const char *base_name(const char *p);

static void add_file(const char *p, int told)
{
    if (FT_IS_GRAPHIC(ftype_detect(p)))
        queue_add(p);
    else {
        n_skip++;
        if (told)                            /* dropped on its own */
            log_add("%s: not a picture, skipped", base_name(p));
    }
}

/* every picture in a folder (and its subfolders if switched on) */
static void scan_dir(const char *dir, int depth)
{
    char base[256], full[300];
    size_t n = strlen(dir);
    if (n > 240 || depth > 8)
        return;
    strcpy(base, dir);
    if (n && base[n - 1] != '\\' && base[n - 1] != '/')
        strcat(base, "\\");
    if (have_mint) {
        /* Dopendir/Dreaddir: long names on MiNT file systems */
        long h = Dopendir(base, 0);
        char buf[260];
        if ((h & 0xFF000000L) == 0xFF000000L)
            return;
        while (Dreaddir(sizeof buf, h, buf) == 0) {
            const char *nm = buf + 4;          /* after the inode number */
            if (!strcmp(nm, ".") || !strcmp(nm, ".."))
                continue;
            if (strlen(base) + strlen(nm) >= sizeof full)
                continue;
            strcpy(full, base);
            strcat(full, nm);
            if (is_dir(full)) {
                if (cs.subdirs)
                    scan_dir(full, depth + 1);
            } else
                add_file(full, 0);
        }
        Dclosedir(h);
    } else {
        _DTA dta, *old = Fgetdta();
        char pat[260];
        Fsetdta(&dta);
        strcpy(pat, base);
        strcat(pat, "*.*");
        if (Fsfirst(pat, FA_DIR) == 0) {
            do {
                const char *nm = dta.dta_name;
                if (!strcmp(nm, ".") || !strcmp(nm, ".."))
                    continue;
                strcpy(full, base);
                strcat(full, nm);
                if (dta.dta_attribute & FA_DIR) {
                    if (cs.subdirs) {
                        /* the recursion uses its own DTA */
                        scan_dir(full, depth + 1);
                        Fsetdta(&dta);
                    }
                } else
                    add_file(full, 0);
            } while (Fsnext() == 0);
        }
        Fsetdta(old);
    }
}

static void add_path(const char *p)
{
    if (!*p)
        return;
    graf_mouse(BUSYBEE, 0);
    if (is_dir(p)) {
        int before = qn;
        scan_dir(p, 0);
        log_add("%s: %d picture%s found", p, qn - before, qn - before == 1 ? "" : "s");
    } else
        add_file(p, 1);
    graf_mouse(ARROW, 0);
}

/* "ARGS": names separated by spaces, quoted with ' ('' is a quote) or " */
static void add_args(char *s)
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
            add_path(name);
    }
}

/* ------------------------------------------------------------------ log */

/* What was converted, how long it took and what went wrong: shown in the
 * main window (open the icon) and appended to FCONV.TXT in the
 * destination folder.                                                 */
#define LOGN 200
#define LOGW 96
static char logl[LOGN][LOGW];
static long lognum;                          /* lines ever added          */
static long logtop;                          /* first line shown          */
static long batch_t0, cur_t0;                /* start times (ms)          */
static void log_changed(int was_bottom);
static int  log_visible(void);

static void log_to_file(const char *s)
{
    char p[300];
    FILE *f;
    if (!cs.dest[0] || strlen(cs.dest) > 280)
        return;
    strcpy(p, cs.dest);
    strcat(p, "FCONV.TXT");
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
static long f_done, f_total = 1;             /* this file's progress       */

static void build_dialog(void)
{
    long fill = (scr_planes >= 4) ? 0x00001174L : 0x00001171L;
    int bak = aes_3d ? FL3DBAK : 0, pad = PD_PAD, pw = 40 * cell_w;
    int bh = cell_h > 10 ? 12 : 8, btnh = cell_h + 6, btnw = 11 * cell_w, y;
    obj_add(ptree, P_ROOT, -1, G_BOX, bak, 0, aes_3d ? 0x00001100L : 0x00011100L,
            0, 0, pw + 2 * pad, 10);
    /* all files */
    y = pad;
    obj_add(ptree, P_TOTAL, 0, G_STRING, 0, 0, (long)t_total, pad, y, pw, cell_h);
    y += cell_h + 6;
    obj_add(ptree, P_TBOX, 0, G_BOX, aes_3d ? FL3DIND : 0, aes_3d ? SELECTED : 0,
            0x00FF1100L, pad, y, pw, bh);
    obj_add(ptree, P_TBAR, P_TBOX, G_BOX, 0, 0, fill, 0, 0, 0, bh);
    /* this file */
    y += bh + cell_h;
    obj_add(ptree, P_FILE, 0, G_STRING, 0, 0, (long)t_file, pad, y, pw, cell_h);
    y += cell_h + 6;
    obj_add(ptree, P_FBOX, 0, G_BOX, aes_3d ? FL3DIND : 0, aes_3d ? SELECTED : 0,
            0x00FF1100L, pad, y, pw, bh);
    obj_add(ptree, P_FBAR, P_FBOX, G_BOX, 0, 0, fill, 0, 0, 0, bh);
    /* status and button on a row of their own, clear of the bars (3D
       buttons draw a few pixels outside their box)                     */
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
    long tot = qn > 0 ? qn : 1, fd = f_total > 0 ? f_done * 1000L / f_total : 0;
    long t;
    if (fd > 1000) fd = 1000;
    t = finished ? 1000 : (qpos * 1000L + (cur ? fd : 0)) / tot;
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
    strcpy(pd_title, " FConv ");
    wind_set_str(pdlg, WF_NAME, pd_title);
    wind_open_grect(pdlg, &r);
    /* draw now, as the redraw message may wait behind the work */
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

static void set_total_text(void)
{
    if (finished)
        sprintf(t_total, "%s: %d converted", cancelling ? "Cancelled" : "Done", n_ok);
    else
        sprintf(t_total, "File %d of %d", qpos + 1 > qn ? qn : qpos + 1, qn);
}

static void set_stat_text(int pct)
{
    if (finished) {
        if (n_fail)
            sprintf(t_stat, "%d failed: %.24s", n_fail, first_err);
        else if (n_skip)
            sprintf(t_stat, "%d not pictures, skipped", n_skip);
        else
            strcpy(t_stat, "");
    } else if (cancelling)
        strcpy(t_stat, "Cancelling...");
    else if (n_fail)
        sprintf(t_stat, "%3d%%   %d failed", pct, n_fail);
    else
        sprintf(t_stat, "%3d%%", pct);
}

static const char *base_name(const char *p)
{
    const char *b = p, *s;
    for (s = p; *s; s++)
        if (*s == '\\' || *s == '/' || *s == ':')
            b = s + 1;
    return b;
}

/* --------------------------------------------------- converting a file */

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

static int exists(const char *p)
{
    return Fattrib(p, 0, 0) >= 0;
}

/* destination name: NAME.IMG, or NAME1.IMG ... if that is taken (and
   never the source file itself)                                       */
static int out_name(const char *src, char *out)
{
    char stem[64];
    const char *b = base_name(src), *dot = strrchr(b, '.');
    size_t n = dot ? (size_t)(dot - b) : strlen(b);
    int k;
    if (n > 60) n = 60;
    memcpy(stem, b, n);
    stem[n] = 0;
    if (!n)
        strcpy(stem, "PICTURE");
    sprintf(out, "%s%s.%s", cs.dest, stem, cs.pdf ? "PDF" : "IMG");
    if (!same_path(out, src) && (cs.replace || !exists(out)))
        return 1;
    for (k = 1; k < 100; k++) {
        char st[64];
        /* keep 8.3 names valid on TOS file systems */
        int keep = k < 10 ? 7 : 6;
        strcpy(st, stem);
        if ((int)strlen(st) > keep && !have_mint)
            st[keep] = 0;
        sprintf(out, "%s%s%d.%s", cs.dest, st, k, cs.pdf ? "PDF" : "IMG");
        if (!same_path(out, src) && !exists(out))
            return 1;
    }
    return 0;
}

static void finish_all(void)
{
    char t[24];
    running = 0;
    finished = 1;
    skip_shown = n_skip;
    log_add("%s: %d converted, %d failed, %d skipped in %s", cancelling ? "Cancelled" : "Done",
            n_ok, n_fail, n_skip, secs(now_ms() - batch_t0, t));
    cur = 0;
    strcpy(t_btn, "OK");
    ptree[P_BTN].ob_state &= ~SELECTED;
    strcpy(t_file, "");
    set_total_text();
    set_stat_text(100);
    dlg_draw(P_ROOT, 0);
}

/* start the job for the next file in the queue */
static void start_next(void)
{
    want_next = 0;
    if (cur || !running)
        return;
    while (qpos < qn && !cancelling) {
        const char *src = queue[qpos];
        char out[300];
        JOB *j;
        int ft = ftype_detect(src);
        if (!FT_IS_GRAPHIC(ft)) {
            n_skip++;
            qpos++;
            continue;
        }
        if (!out_name(src, out)) {
            n_fail++;
            if (!first_err[0])
                sprintf(first_err, "%.20s: no free name", base_name(src));
            log_add("%s: failed, no free output name", base_name(src));
            qpos++;
            continue;
        }
        j = calloc(1, sizeof(JOB));
        if (!j) {
            n_fail++;
            qpos++;
            continue;
        }
        j->q.op = OP_CONVERT;
        strncpy(j->q.path, src, sizeof j->q.path - 1);
        strncpy(j->q.out, out, sizeof j->q.out - 1);
        j->q.ftype = ft;
        j->q.pf = prefs;
        j->q.disp = dinfo;
        j->q.disp.palmode = prefs.palmode;
        j->q.xfmt = cs.xfmt;
        j->q.outpdf = cs.pdf;
        j->q.maxw = cs.fit ? scr_w : 0;
        j->q.maxh = cs.fit ? scr_h : 0;
        j->win = (VWIN *)&cur;               /* any non-null tag */
        cur_dith = cs.xfmt == XF_MONO || (cs.xfmt == XF_SCREEN && !dinfo.truecolor);
        f_done = 0;
        f_total = 1;
        cur = j;
        cur_t0 = now_ms();
        sprintf(t_file, "Reading %.30s", base_name(src));
        set_total_text();
        set_stat_text((int)(qpos * 100L / (qn ? qn : 1)));
        dlg_draw(P_ROOT, 0);
        dbg("convert %s -> %s, format %d", src, out, cs.xfmt);
        if (!job_start(j)) {
            free(j);
            cur = 0;
            n_fail++;
            qpos++;
            continue;
        }
        return;
    }
    finish_all();
}

/* the GUI side of job.c */
void win_job_finished(VWIN *w, JOB *j)
{
    (void)w;
    char t[24];
    if (j->res.err == E_CANCEL || j->cancel) {
        cancelling = 1;
        log_add("%s: cancelled", base_name(j->q.path));
    } else if (j->res.err) {
        n_fail++;
        if (!first_err[0])
            sprintf(first_err, "%.20s: %.40s", base_name(j->q.path),
                    j->res.detail[0] ? j->res.detail : fv_errstr(j->res.err));
        log_add("%s: failed, %s%s%s", base_name(j->q.path), fv_errstr(j->res.err),
                j->res.detail[0] ? ": " : "", j->res.detail);
    } else {
        n_ok++;
        log_add("%s -> %s  %s", base_name(j->q.path), base_name(j->q.out),
                secs(now_ms() - cur_t0, t));
    }
    {
        int i;
        for (i = 0; i < RB_MAX; i++)
            job_release(&j->res, i);
    }
    if (cur == j)
        cur = 0;
    free(j);
    qpos++;
    if (cancelling) {
        qpos = qn;
        finish_all();
    } else
        want_next = 1;                       /* from the main loop */
}

void win_stage(VWIN *w, int stage)
{
    const char *verb;
    (void)w;
    if (!cur)
        return;
    verb = stage == 0 ? "Reading" : (stage == 1 && cur_dith) ? "Dithering" : "Writing";
    sprintf(t_file, "%s %.30s", verb, base_name(cur->q.path));
    dlg_draw(P_FILE, 0);
}

/* FArchive's jobs send log lines; FView's and FConv's do not */
void win_note(VWIN *w, int kind, const char *text)
{
    (void)w;
    (void)kind;
    (void)text;
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
    pct = (int)((qpos * 100L + fp) / (qn ? qn : 1));
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
        qpos = qn;
        finish_all();
    }
}

static int choose_dest(void)
{
    char path[300], name[64] = "";
    char *s;
    if (cs.dest[0])
        strcpy(path, cs.dest);
    else {
        strcpy(path, app_folder);
    }
    strcat(path, "*.*");
    if (!fc_file_select(path, name, "Destination folder (OK)"))
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

/* new files were added: start (or carry on) */
static void begin(void)
{
    show_log();                              /* open the icon: the log */
    if (!qn || qpos >= qn) {
        if (n_skip && !running) {
            alertf("[1][FConv: none of these|are pictures FConv can read.][ OK ]");
            n_skip = 0;
        }
        return;
    }
    if (!cs.dest[0] && !choose_dest()) {
        queue_clear();
        n_skip = 0;
        return;
    }
    if (running)
        return;                              /* added to the running batch */
    /* a new batch: forget the files of the last one */
    if (qpos > 0) {
        int i;
        for (i = 0; i < qpos; i++)
            free(queue[i]);
        memmove(queue, queue + qpos, (qn - qpos) * sizeof(char *));
        qn -= qpos;
        qpos = 0;
    }
    if (finished)
        n_skip -= skip_shown;
    skip_shown = 0;
    batch_t0 = now_ms();
    {
        time_t now = time(0);
        struct tm *tm = localtime(&now);
        char when[32];
        if (tm)
            sprintf(when, "%04d-%02d-%02d %02d:%02d", tm->tm_year + 1900, tm->tm_mon + 1,
                    tm->tm_mday, tm->tm_hour, tm->tm_min);
        else
            strcpy(when, "");
        log_add("%s  %d file%s to %s, %s %s", when, qn, qn == 1 ? "" : "s", cs.dest,
                cs.pdf ? "PDF" : "IMG", fmt_name());
    }
    running = 1;
    finished = cancelling = 0;
    n_ok = n_fail = 0;
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
        queue_clear();
        n_skip = skip_shown = 0;
    } else
        cancel_all();
}

/* ------------------------------------------------------- the main window */

static int  mainw;                           /* window handle             */
static int  iconified;                       /* 1 AES, 2 our own          */
static GRECT main_full;                      /* size before iconifying    */
static short ic_data[64], ic_mask[64];
static ICONBLK ic_blk;
static OBJECT ic_tree[2];
static char ic_label[] = "FConv";

static void ic_set(int x, int y)
{
    if (x >= 0 && x < 32 && y >= 0 && y < 32)
        ic_data[y * 2 + (x >> 4)] |= (short)(0x8000U >> (x & 15));
}

/* a picture with an arrow into a page */
static void fc_build_icon(void)
{
    int x, y;
    memset(ic_data, 0, sizeof ic_data);
    for (x = 1; x < 15; x++) { ic_set(x, 6); ic_set(x, 19); }
    for (y = 6; y < 20; y++) { ic_set(1, y); ic_set(14, y); }
    for (y = 0; y < 8; y++)                   /* mountain */
        for (x = 0; x <= y; x++) { ic_set(7 - x, 11 + y); ic_set(7 + x, 11 + y); }
    ic_set(11, 9); ic_set(12, 9); ic_set(11, 10); ic_set(12, 10);   /* sun */
    for (x = 15; x < 20; x++) ic_set(x, 13);  /* arrow */
    for (y = 0; y < 4; y++) { ic_set(19 - y, 10 + y); ic_set(19 - y, 16 - y); }
    for (y = 4; y < 28; y++) { ic_set(21, y); ic_set(30, y); }      /* page */
    for (x = 21; x < 31; x++) { ic_set(x, 4); ic_set(x, 27); }
    for (y = 8; y < 25; y += 3)
        for (x = 23; x < 29; x++) ic_set(x, y);
    for (y = 0; y < 32; y++) {
        ic_mask[y * 2] = (y >= 4 && y <= 27) ? (short)0x7FFF : 0;
        ic_mask[y * 2 + 1] = (y >= 4 && y <= 27) ? (short)0xFFFE : 0;
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

static const char *fmt_name(void)
{
    static char b[40];
    switch (cs.xfmt) {
    case XF_ORIGINAL: return "original depth";
    case XF_TRUE:     return "true colour (24 bit)";
    case XF_MONO:     return "mono (1 bit)";
    }
    if (dinfo.truecolor) return "screen colours (24 bit)";
    sprintf(b, "screen colours (%d)", scr_planes >= 8 ? 256 : 1 << scr_planes);
    return b;
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
    v_gtext(vdih, x, y, "FConv " FV_VERSION " (" FV_CPU ") - drop pictures or folders here");
    vst_effects(vdih, 0);
    y += cell_h + 6;
    sprintf(line, "Format:       %s, %s", cs.pdf ? "PDF" : "GEM IMG", fmt_name());
    v_gtext(vdih, x, y, line); y += cell_h;
    sprintf(line, "Dither:       %s%s", prefs.dither == DITH_FS ? "Floyd-Steinberg" :
            prefs.dither == DITH_ATKINSON ? "Atkinson" : prefs.dither == DITH_ORDERED ?
            "ordered" : "none", prefs.serpentine && prefs.dither != DITH_ORDERED &&
            prefs.dither != DITH_NONE ? ", serpentine" : "");
    v_gtext(vdih, x, y, line); y += cell_h;
    sprintf(line, "Destination:  %.50s", cs.dest[0] ? cs.dest : "(asked for first)");
    v_gtext(vdih, x, y, line); y += cell_h;
    sprintf(line, "Fit to screen %s, replace %s, subfolders %s",
            cs.fit ? "on" : "off", cs.replace ? "on" : "off", cs.subdirs ? "on" : "off");
    v_gtext(vdih, x, y, line); y += cell_h;
    sprintf(line, "%s", !have_mint ? "TOS: converting inside FConv" :
            !have_worker ? "MiNT: FVWORK.TTP not found, converting inside FConv" :
            "MiNT: converting in background workers");
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
            v_gtext(vdih, x, y, "(converted files are listed here)");
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

static void post_redraw(void);

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

static void post_redraw(void)
{
    short msg[8];
    GRECT wk;
    if (mainw <= 0)
        return;
    wind_get_grect(mainw, WF_WORKXYWH, &wk);
    msg[0] = WM_REDRAW; msg[1] = ap_id; msg[2] = 0; msg[3] = mainw;
    msg[4] = wk.g_x; msg[5] = wk.g_y; msg[6] = wk.g_w; msg[7] = wk.g_h;
    appl_write(ap_id, 16, msg);
}

/* the icon sits at the bottom left, to the right of FView's */
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
    r->g_x = desk.g_x + r->g_w + 8;
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
        wind_set_str(mainw, WF_NAME, " FConv ");
    } else {
        iconified = 0;
        make_window(MAINK | (aes_iconify ? SMALLER : 0), &main_full, " FConv ");
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
    wk.g_w = 62 * cell_w;
    wk.g_h = (HDR_LINES + 12) * cell_h + 20;
    wind_calc_grect(WC_BORDER, k, &wk, &r);
    if (r.g_w > desk.g_w) r.g_w = desk.g_w;
    if (r.g_h > desk.g_h) r.g_h = desk.g_h;
    r.g_x = desk.g_x + (desk.g_w - r.g_w) / 2;
    r.g_y = desk.g_y + (desk.g_h - r.g_h) / 3;
    main_full = r;
    make_window(k, &r, " FConv ");
    if (mainw <= 0)
        return;
    to_icon();
}

/* ------------------------------------------------------------- messages */

static void about(void)
{
    char buf[256];
    sprintf(buf, "[1][FConv " FV_VERSION " - picture converter|"
            FV_CPU " version|"
            "Drop pictures or folders on|the icon: IMG or PDF files.|"
            "%s][ OK ]",
            !have_mint ? "TOS: converting in FConv" :
            !have_worker ? "MiNT: FVWORK.TTP not found" :
                           "MiNT: background workers");
    form_alert(1, buf);
}

static void do_convert_files(void)
{
    static char path[300], name[64];
    char full[400], *s;
    if (!path[0]) {
        strcpy(path, app_folder);
        strcat(path, "*.*");
    }
    name[0] = 0;
    if (!fc_file_select(path, name, "Convert picture"))
        return;
    strcpy(full, path);
    s = strrchr(full, '\\');
    if (s) s[1] = 0;
    if (name[0])
        strcat(full, name);
    else if (s)
        s[0] = 0;                            /* no name: the whole folder */
    add_path(full);
    begin();
}

static void changed(void)
{
    settings_save();
    fc_menu_update();
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
        if (k == F_CONVERT) do_convert_files();
        else if (k == F_DEST) { choose_dest(); changed(); }
        else if (k == F_QUIT) quitting = 1;
        break;
    case 2:
        if (k == X_IMG) cs.pdf = 0;
        else if (k == X_PDF) cs.pdf = 1;
        else if (k >= X_ORIG && k <= X_MONO) cs.xfmt = k - X_ORIG;
        else if (k == X_FIT) cs.fit = !cs.fit;
        else if (k == X_REPLACE) cs.replace = !cs.replace;
        else if (k == X_SUBDIRS) cs.subdirs = !cs.subdirs;
        changed();
        break;
    case 3:
        if (k >= T_FS && k <= T_NONE) prefs.dither = (char)(k - T_FS);
        else if (k == T_SERP) prefs.serpentine = !prefs.serpentine;
        else if (k == T_SMOOTH) prefs.smooth = !prefs.smooth;
        else if (k == T_OPT) prefs.palmode = PAL_OPTIMISE;
        else if (k == T_SYS) prefs.palmode = PAL_SYSTEM;
        changed();
        break;
    }
    menu_tnormal(menu, title, 1);
}

/* MultiTOS drag & drop */
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
            /* a pipe cannot seek: read the rest and drop it */
            long left = hlen - (long)sizeof hdr;
            Fread((short)fd, sizeof hdr, hdr);
            while (left > 0) {
                char junk[64];
                long k = left > 64 ? 64 : left;
                if (Fread((short)fd, k, junk) != k)
                    break;
                left -= k;
            }
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
                add_args(buf);
                begin();
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
    if (copy[0]) {
        add_args(copy);
        begin();
    }
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
    add_args(copy);
    begin();
}

static void av_hello(void)
{
    char *srv = getenv("AVSERVER");
    static char myname[9] = "FCONV   ";
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

static void message(short *msg)
{
    GRECT *r = (GRECT *)&msg[4];
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
    if (mainw <= 0 || msg[3] != mainw)
        return;
    switch (msg[0]) {
    case WM_REDRAW:  main_redraw(r); break;
    case WM_TOPPED:  wind_set(mainw, WF_TOP, 0, 0, 0, 0); break;
    case WM_MOVED:
        wind_set_grect(mainw, WF_CURRXYWH, r);
        if (!iconified)
            main_full = *r;
        break;
    case WM_CLOSED:
        /* closing the window makes it the icon again; closing the icon
           (or File > Quit) ends FConv                                 */
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

static void button(int mx, int my, int clicks)
{
    int h = wind_find(mx, my);
    (void)clicks;
    if (pdlg > 0 && h == pdlg) {
        dlg_place();
        if (objc_find(ptree, 0, 8, mx, my) == P_BTN && !(ptree[P_BTN].ob_state & SELECTED))
            dlg_button();
        else
            wind_set(pdlg, WF_TOP, 0, 0, 0, 0);
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
    if (c == 15) do_convert_files();         /* ^O */
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
        if (ev & MU_BUTTON) button(mx, my, clicks);
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
        form_alert(1, "[3][FConv: no VDI workstation.][ Quit ]");
        appl_exit();
        return 1;
    }
    scr_w = work_out[0] + 1;
    scr_h = work_out[1] + 1;
    pix_w_um = work_out[3];
    pix_h_um = work_out[4];
    wind_get_grect(0, WF_WORKXYWH, &desk);

    prefs_default(&prefs);
    prefs.use_workers = 1;
    prefs.keep_sys = 0;
    find_app_folder();
    settings_load();
    disp_init();
    job_app_name = "fconv " FV_VERSION " (" FV_CPU ")";
    job_log_name = "FCONV.LOG";
    job_init_system();
    aes_iconify = detect_iconify();
    aes_3d = detect_3d();
    dbg("AES %04x screen %dx%d planes %d truecolor %d, format %d dest '%s'",
        _AESversion, scr_w, scr_h, scr_planes, dinfo.truecolor, cs.xfmt, cs.dest);

    if (_AESversion >= 0x0400)
        shel_write(9, 1, 0, 0L, 0L);          /* AP_TERM */
    if (_AESnumapps != 1 || _AESversion >= 0x0400)
        menu_register(ap_id, "  FConv");

    if (dinfo.truecolor)
        strcpy(scr_item, "  Screen colours (24 bit)   ");
    else
        sprintf(scr_item, "  Screen colours (%3d)      ",
                scr_planes >= 8 ? 256 : 1 << scr_planes);
    fc_build_menu();
    menu_bar(menu, 1);
    fc_menu_update();
    fc_build_icon();
    build_dialog();
    graf_mouse(ARROW, 0);
    av_hello();
    open_main();

    for (i = 1; i < argc; i++)
        add_path(argv[i]);
    if (argc > 1)
        begin();

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
    if (mainw > 0) {
        wind_close(mainw);
        wind_delete(mainw);
    }
    menu_bar(menu, 0);
    v_clsvwk(vdih);
    appl_exit();
    return 0;
}
