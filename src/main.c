/*
 * main.c - fview: application start-up, menus, event loop, drag & drop.
 *
 * fview starts with its menu bar and an iconified main window that
 * accepts dropped files (AP_DRAGDROP), VA_START from desktops and file
 * names on the command line. Each file opens in its own window.
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

#ifndef VA_START
#define VA_START      0x4711
#define AV_STARTED    0x4738
#define AV_PROTOKOLL  0x4700
#define VA_PROTOSTATUS 0x4701
#endif
#ifndef WM_UNICONIFY
#define WM_UNICONIFY  35
#define WM_ALLICONIFY 36
#endif
#ifndef WF_UNICONIFY
#define WF_UNICONIFY  27
#endif
#define DD_OK    0
#define DD_NAK   1
#define DD_EXT   2

/* stb_image and the PDF interpreter want more than mintlib's default */
long _stksize = 64 * 1024L;

int  vdih, ap_id;
int  scr_w, scr_h, scr_planes, pix_w_um, pix_h_um;
int  cell_w, cell_h;
GRECT desk;
PREFS prefs;
DISPINFO dinfo;
int  quitting;
FONTSET scrfonts;
int  aes_iconify;
int  aes_3d;
int  screen_interleaved;

static OBJECT *menu;
static int scr_sysfonts;
static void main_to_icon(VWIN *w);
static VWIN *mainw;
static int bee_on;
static char last_dir[256];

/* --------------------------------------------------------------- utils */

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

int file_select(char *path, char *name, const char *title)
{
    short btn = 0, ok;
    if (_AESversion >= 0x0140)
        ok = fsel_exinput(path, name, &btn, title);
    else
        ok = fsel_input(path, name, &btn);
    return ok && btn;
}

/* --------------------------------------------------------- the display */

void disp_setup_reserved(void)
{
    int i, n = dinfo.truecolor ? 0 : (scr_planes >= 8 ? 256 : 1 << scr_planes);
    memset(dinfo.reserved, 0, sizeof dinfo.reserved);
    dinfo.palmode = prefs.palmode;
    for (i = 0; i < n; i++) {
        int pix = vdi2pix(i, scr_planes);
        if (dinfo.fixed_palette || prefs.palmode == PAL_SYSTEM)
            dinfo.reserved[pix] = 1;
        else if (prefs.keep_sys && i < (n >= 256 ? 16 : 2))
            dinfo.reserved[pix] = 1;
    }
    pal_restore();
    pal_invalidate();
}


/* ------------------------------------------------------- format probe */

/* Measure the screen format instead of trusting the driver's description:
 * draw n pixels on a one pixel high strip at the top left of the screen
 * (saved first and put back), copy it into memory and look at the bits.
 * mode 0: pixel i is VDI pen i. mode 1: pen 1 is set to rgb[i] for
 * pixel i (true colour). raw gets the device-format strip; vals (palette
 * screens) the pixel value of each pixel after vr_trnfm.                */
static int probe_strip(int n, int mode, const short (*rgb)[3], u8 *raw, int *vals)
{
    MFDB scr, sv, rd, st;
    short pxy[8], r4[4], keep[3];
    int w16 = (n + 15) & ~15, wdw = w16 / 16, i, p;
    long bytes = (long)wdw * 2 * scr_planes;
    u8 *bsv = calloc(1, bytes), *brd = calloc(1, bytes), *bst = calloc(1, bytes);
    if (!bsv || !brd || !bst || w16 > scr_w) {
        free(bsv); free(brd); free(bst);
        return 0;
    }
    memset(&scr, 0, sizeof scr);
    memset(&sv, 0, sizeof sv);
    sv.fd_addr = bsv; sv.fd_w = (short)w16; sv.fd_h = 1;
    sv.fd_wdwidth = (short)wdw; sv.fd_stand = 0; sv.fd_nplanes = (short)scr_planes;
    rd = sv; rd.fd_addr = brd;
    pxy[0] = 0; pxy[1] = 0; pxy[2] = (short)(w16 - 1); pxy[3] = 0;
    pxy[4] = 0; pxy[5] = 0; pxy[6] = (short)(w16 - 1); pxy[7] = 0;

    wind_update(BEG_UPDATE);
    graf_mouse(M_OFF, 0);
    vs_clip(vdih, 0, pxy);
    vro_cpyfm(vdih, S_ONLY, pxy, &scr, &sv);            /* save */
    vswr_mode(vdih, MD_REPLACE);
    vsf_interior(vdih, 1);
    vsf_perimeter(vdih, 0);
    if (mode == 1)
        vq_color(vdih, 1, 0, keep);
    for (i = 0; i < w16; i++) {
        if (mode == 1)
            vs_color(vdih, 1, (short *)rgb[i < n ? i : 0]);
        vsf_color(vdih, mode == 1 ? 1 : (i < n ? i : 0));
        r4[0] = r4[2] = (short)i;
        r4[1] = r4[3] = 0;
        vr_recfl(vdih, r4);
    }
    vro_cpyfm(vdih, S_ONLY, pxy, &scr, &rd);            /* read back */
    vro_cpyfm(vdih, S_ONLY, pxy, &sv, &scr);            /* restore */
    if (mode == 1)
        vs_color(vdih, 1, keep);
    vsf_color(vdih, 1);
    graf_mouse(M_ON, 0);
    wind_update(END_UPDATE);

    memcpy(raw, brd, bytes);
    if (vals) {
        st = rd;
        st.fd_addr = bst;
        st.fd_stand = 1;
        vr_trnfm(vdih, &rd, &st);
        for (i = 0; i < n; i++) {
            int v = 0;
            for (p = 0; p < scr_planes; p++)
                if (((u16 *)bst)[p * wdw + (i >> 4)] & (0x8000 >> (i & 15)))
                    v |= 1 << p;
            vals[i] = v;
        }
    }
    free(bsv); free(brd); free(bst);
    return 1;
}

static int all_distinct(const int *v, int n)
{
    static u8 seen[256];
    int i;
    memset(seen, 0, sizeof seen);
    for (i = 0; i < n; i++) {
        if (v[i] < 0 || v[i] > 255 || seen[v[i]])
            return 0;
        seen[v[i]] = 1;
    }
    return 1;
}

/* a channel mask: contiguous run of bits -> MSB-first bit list */
static int mask_bits(u32 m, int *pos)
{
    int hi = -1, lo = -1, b, n = 0;
    for (b = 31; b >= 0; b--)
        if (m & (1UL << b)) {
            if (hi < 0) hi = b;
            lo = b;
        }
    if (hi < 0)
        return 0;
    for (b = hi; b >= lo; b--) {
        if (!(m & (1UL << b)))
            return 0;                   /* not contiguous */
        if (n < 16)
            pos[n++] = b;
    }
    return n;
}

static void probe_palette(int pal_known_interleaved)
{
    static u8 raw[256 * 2 * 8 / 16 * 2];
    static int vals[256], rb[256];
    int n = scr_planes >= 8 ? 256 : 1 << scr_planes, i, *map = 0;
    if (scr_planes < 2 || scr_planes > 8 || !probe_strip(n, 0, 0, raw, vals))
        return;
    if (scr_planes == 8 && !pal_known_interleaved) {
        for (i = 0; i < n; i++)
            rb[i] = raw[i];
        if (all_distinct(rb, n)) {
            dinfo.chunky = 1;           /* one byte per pixel */
            map = rb;
        }
    }
    if (!map && all_distinct(vals, n))
        map = vals;
    dbg("palette probe: chunky %d, pen 0 -> %d, 1 -> %d, 2 -> %d, 15 -> %d, 255 -> %d",
        dinfo.chunky, map ? map[0] : -1, map ? map[1] : -1, map ? map[2] : -1,
        map && n > 15 ? map[15] : -1, map && n > 255 ? map[255] : -1);
    if (!map)
        return;
    memset(dinfo.sysrgb, 0, sizeof dinfo.sysrgb);
    for (i = 0; i < n; i++) {
        short rgb[3];
        pal_set_pen2pix(i, map[i]);
        vq_color(vdih, i, 1, rgb);
        dinfo.sysrgb[map[i] * 3]     = (u8)(rgb[0] * 255L / 1000);
        dinfo.sysrgb[map[i] * 3 + 1] = (u8)(rgb[1] * 255L / 1000);
        dinfo.sysrgb[map[i] * 3 + 2] = (u8)(rgb[2] * 255L / 1000);
    }
}

static void probe_truecolor(void)
{
    static const short col[4][3] = { {1000,0,0}, {0,1000,0}, {0,0,1000}, {0,0,0} };
    static u8 raw[16 * 4];
    u32 v[4], m[3];
    int i, k, bpp = dinfo.bytespp, swap, nb[3], pos[3][16];
    if (bpp < 2 || bpp > 4 || !probe_strip(4, 1, col, raw, 0))
        return;
    for (swap = 0; swap < 2; swap++) {
        int ok = 1;
        if (swap && bpp != 2)
            break;
        for (i = 0; i < 4; i++) {
            const u8 *s = raw + i * bpp;
            u32 p = 0;
            for (k = 0; k < bpp; k++)
                p = (p << 8) | s[swap ? bpp - 1 - k : k];
            v[i] = p;
        }
        for (i = 0; i < 3; i++) {
            m[i] = v[i] & ~v[3];
            nb[i] = mask_bits(m[i], pos[i]);
            if (!nb[i]) ok = 0;
        }
        if (ok && ((m[0] & m[1]) || (m[0] & m[2]) || (m[1] & m[2])))
            ok = 0;
        dbg("true colour probe%s: red %lx green %lx blue %lx black %lx -> %s",
            swap ? " (bytes swapped)" : "", v[0], v[1], v[2], v[3], ok ? "ok" : "no");
        if (ok) {
            for (i = 0; i < 3; i++) {
                dinfo.nbits[i] = nb[i];
                for (k = 0; k < nb[i]; k++)
                    dinfo.bitpos[i][k] = pos[i][k];
            }
            dinfo.intel = swap;
            return;
        }
    }
}

static void disp_init(void)
{
    short out[57], scr[272];
    long edDI = 0;
    int i, n;

    memset(&dinfo, 0, sizeof dinfo);
    dinfo.pix_w = pix_w_um > 0 ? pix_w_um : 278;
    dinfo.pix_h = pix_h_um > 0 ? pix_h_um : 278;
    vq_extnd(vdih, 1, out);
    scr_planes = out[4];
    dinfo.planes = scr_planes;
    dinfo.truecolor = scr_planes > 8;
    dinfo.fixed_palette = scr_planes == 1 || !out[5];   /* no colour table */

    memset(scr, 0, sizeof scr);
    if (Getcookie(C_EdDI, &edDI) == C_FOUND)
        vq_scrninfo(vdih, scr);

    if (dinfo.truecolor) {
        int c;
        dinfo.fixed_palette = 1;
        if (edDI && scr[0] == 2 && scr[8] > 0) {
            dinfo.bytespp = scr_planes == 15 ? 2 : (scr_planes + 7) / 8;
            for (c = 0; c < 3; c++) {
                int nb = scr[8 + c], k;
                if (nb > 16) nb = 16;
                dinfo.nbits[c] = nb;
                /* the table lists bit numbers from the least significant
                   intensity bit up; we keep them most significant first */
                for (k = 0; k < nb; k++)
                    dinfo.bitpos[c][k] = scr[16 + c * 16 + (nb - 1 - k)];
            }
            dinfo.intel = (scr[14] & 0x80) != 0;   /* bit 7: bytes swapped */
        } else {
            /* Falcon style defaults */
            if (scr_planes == 16 || scr_planes == 15) {
                dinfo.bytespp = 2;
                dinfo.nbits[0] = 5; dinfo.nbits[1] = scr_planes == 16 ? 6 : 5; dinfo.nbits[2] = 5;
                for (i = 0; i < 5; i++) {
                    dinfo.bitpos[0][i] = (scr_planes == 16 ? 15 : 14) - i;
                    dinfo.bitpos[2][i] = 4 - i;
                }
                for (i = 0; i < dinfo.nbits[1]; i++)
                    dinfo.bitpos[1][i] = (scr_planes == 16 ? 10 : 9) - i;
            } else {
                dinfo.bytespp = scr_planes == 24 ? 3 : 4;
                for (c = 0; c < 3; c++) {
                    dinfo.nbits[c] = 8;
                    for (i = 0; i < 8; i++)
                        dinfo.bitpos[c][i] = 23 - c * 8 - i;
                }
            }
        }
    }

    /* ST-style interleaved bitplanes, the layout fview can check and,
       if need be, write to directly */
    screen_interleaved = !dinfo.truecolor && (!edDI || scr[0] == 0);
    pal_init();
    /* current palette by pixel value */
    n = dinfo.truecolor ? 0 : (scr_planes >= 8 ? 256 : 1 << scr_planes);
    for (i = 0; i < n; i++) {
        short rgb[3];
        int pix = vdi2pix(i, scr_planes);
        if (edDI && scr[1] == 1 && scr[16 + i] >= 0 && scr[16 + i] < 256) {
            pix = scr[16 + i];            /* EdDI: VDI index -> pixel value */
            pal_set_pen2pix(i, pix);
        }
        vq_color(vdih, i, 1, rgb);
        dinfo.sysrgb[pix * 3]     = (u8)(rgb[0] * 255L / 1000);
        dinfo.sysrgb[pix * 3 + 1] = (u8)(rgb[1] * 255L / 1000);
        dinfo.sysrgb[pix * 3 + 2] = (u8)(rgb[2] * 255L / 1000);
    }
    dbg("screen: EdDI %d format %d clut %d bits %d flags %x, bytes/pixel %d, intel %d,"
        " red %d bits from %d, green %d from %d, blue %d from %d", edDI ? 1 : 0, scr[0], scr[1],
        scr[2], scr[14], dinfo.bytespp, dinfo.intel, dinfo.nbits[0], dinfo.bitpos[0][0],
        dinfo.nbits[1], dinfo.bitpos[1][0], dinfo.nbits[2], dinfo.bitpos[2][0]);
    /* check against what the screen really does */
    if (dinfo.truecolor)
        probe_truecolor();
    else
        probe_palette(screen_interleaved && edDI && scr[0] == 0);
    if (dinfo.truecolor)
        dbg("screen after probe: intel %d, red %d bits from %d, green %d from %d, blue %d from %d",
            dinfo.intel, dinfo.nbits[0], dinfo.bitpos[0][0], dinfo.nbits[1], dinfo.bitpos[1][0],
            dinfo.nbits[2], dinfo.bitpos[2][0]);
    disp_setup_reserved();
}

/* ------------------------------------------------------------- pointer */

/* The busy bee is shown only while the mouse is over one of fview's own
 * windows that is busy (never over other programs, and the arrow comes
 * back over the Cancel button). We only ever reset what we set.        */
void update_pointer(void)
{
    short mx, my, mb, ks;
    int want = 0;
    VWIN *w;
    graf_mkstate(&mx, &my, &mb, &ks);
    w = win_find(wind_find(mx, my));
    if (w && win_busy(w) && !w->iconified && !win_over_cancel(w, mx, my)) {
        GRECT wk;
        wind_get_grect(w->handle, WF_WORKXYWH, &wk);
        want = mx >= wk.g_x && my >= wk.g_y &&
               mx < wk.g_x + wk.g_w && my < wk.g_y + wk.g_h;
    }
    if (mb)                       /* don't change shape while dragging */
        return;
    if (want && !bee_on) {
        graf_mouse(BUSYBEE, 0);
        bee_on = 1;
    } else if (!want && bee_on) {
        graf_mouse(ARROW, 0);
        bee_on = 0;
    }
}

/* ---------------------------------------------------------------- menu */

static VWIN *cur_win(void)
{
    VWIN *w = win_top();
    if (w && w->kind != K_MAIN)
        return w;
    return 0;
}

void menu_update(void)
{
    VWIN *w = cur_win();
    int img = w && w->kind == K_IMAGE && w->disp.fd_addr && !win_busy(w);
    int pdf = w && w->kind == K_PDF && w->npages && !win_busy(w);
    int any = w && !win_busy(w) && w->state != ST_ERROR;
    if (!menu)
        return;
    menu_ienable(menu, MI_CLOSE, w != 0);
    menu_ienable(menu, MI_SAVEORIG, img);
    menu_ienable(menu, MI_SAVEDITH, img);
    menu_ienable(menu, MI_PRINT, any);
    menu_ienable(menu, MI_REAL, img || pdf);
    menu_ienable(menu, MI_FIT, img || pdf);
    menu_ienable(menu, MI_ZOOMIN, pdf);
    menu_ienable(menu, MI_ZOOMOUT, pdf);
    menu_icheck(menu, MI_REAL, (img && w->zoom == ZOOM_REAL) || (pdf && w->pdfzoom == 100));
    menu_icheck(menu, MI_FIT, (img && w->zoom == ZOOM_FIT) || (pdf && w->pdfzoom == 0));
    menu_ienable(menu, MI_PREVPG, pdf && w->page > 0);
    menu_ienable(menu, MI_NEXTPG, pdf && w->page < w->npages - 1);
    menu_ienable(menu, MI_FIRSTPG, pdf);
    menu_ienable(menu, MI_LASTPG, pdf);
}

static void about(void)
{
    char buf[256];
    sprintf(buf, "[1][FView " FV_VERSION "  - file viewer|"
            FV_CPU " version|"
            "Pictures, text, Markdown,|RTF, PDF, GEM metafiles|"
            "%s][ OK ]",
            !have_mint ? "TOS: cooperative loading" :
            !have_shm ? "MiNT: no U:\\SHM, cooperative" :
            !have_worker ? "MiNT: FVWORK.TTP not found" :
            prefs.use_workers ? "MiNT: background workers" :
                                "MiNT: workers switched off");
    form_alert(1, buf);
}

static void cycle_windows(void)
{
    VWIN *top = win_top();
    int i, start = 0;
    if (top)
        start = (int)(top - wins) + 1;
    for (i = 0; i < MAXWIN; i++) {
        VWIN *w = &wins[(start + i) % MAXWIN];
        if (w->used && w->handle > 0 && w != top) {
            wind_set(w->handle, WF_TOP, 0, 0, 0, 0);
            break;
        }
    }
    pal_check_top();
    menu_update();
}

static void do_open(void)
{
    char path[256], name[64];
    if (!last_dir[0]) {
        last_dir[0] = Dgetdrv() + 'A';
        last_dir[1] = ':';
        Dgetpath(last_dir + 2, 0);
        strcat(last_dir, "\\");
    }
    strcpy(path, last_dir);
    strcat(path, "*.*");
    name[0] = 0;
    if (!file_select(path, name, "fview: open file"))
        return;
    {
        char *s = strrchr(path, '\\');
        if (s) s[1] = 0;
        strcpy(last_dir, path);
        strcat(path, name);
    }
    if (name[0])
        open_file(path);
}

/* Quitting is a two step affair: stop everything here, tear down from
 * the main loop, so a cooperative job or a save/print in progress can
 * unwind first.                                                        */
static void quit(void)
{
    quitting = 1;
    job_kill_all();
}

static void do_menu(int title, int item)
{
    VWIN *w = cur_win();
    switch (item) {
    case MI_ABOUT:    about(); break;
    case MI_OPEN:     do_open(); break;
    case MI_CLOSE:    if (w) win_close(w); break;
    case MI_SAVEORIG: if (w) win_save_img(w, 0); break;
    case MI_SAVEDITH: if (w) win_save_img(w, 1); break;
    case MI_PRINT:    if (w) print_window(w); break;
    case MI_QUIT:     quit(); break;
    case MI_REAL:     if (w && w->kind == K_PDF) win_pdf_zoom(w, 100);
                      else if (w) win_set_zoom(w, ZOOM_REAL);
                      break;
    case MI_FIT:      if (w && w->kind == K_PDF) win_pdf_zoom(w, 0);
                      else if (w) win_set_zoom(w, ZOOM_FIT);
                      break;
    case MI_ZOOMIN:   if (w) win_pdf_zoom(w, 1); break;
    case MI_ZOOMOUT:  if (w) win_pdf_zoom(w, -1); break;
    case MI_PREVPG:   if (w) win_page(w, w->page - 1); break;
    case MI_NEXTPG:   if (w) win_page(w, w->page + 1); break;
    case MI_FIRSTPG:  if (w) win_page(w, 0); break;
    case MI_LASTPG:   if (w) win_page(w, w->npages - 1); break;
    case MI_CYCLE:    cycle_windows(); break;
    case MI_PREFS:    prefs_open(); break;
    }
    menu_tnormal(menu, title, 1);
    menu_update();
}

/* ---------------------------------------------------------- open files */

void open_file(const char *path)
{
    VWIN *w;
    if (quitting)
        return;
    w = win_new();
    if (!w) {
        alertf("[1][fview: too many windows open.][ OK ]");
        return;
    }
    win_load(w, path);
    if (mainw && !mainw->iconified)
        win_redraw_all(mainw);
}

/* "ARGS" format: names separated by spaces, names with spaces in single
 * quotes ('' is a quote). Double quotes are accepted as well.          */
void open_files_from_string(char *s)
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
            open_file(name);
    }
}

/* MultiTOS drag & drop protocol */
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
            Fclose((short)fd);
            if (size > 0) {
                buf[size] = 0;
                open_files_from_string(buf);
            }
            free(buf);
            return;
        }
        c = DD_EXT;                   /* ask for another type */
        Fwrite((short)fd, 1, &c);
    }
    Fclose((short)fd);
}

static void va_start_msg(short *msg)
{
    char *args = *(char **)&msg[3];
    short reply[8];
    if (args) {
        char copy[1024];
        strncpy(copy, args, sizeof copy - 1);
        copy[sizeof copy - 1] = 0;
        open_files_from_string(copy);
    }
    memset(reply, 0, sizeof reply);
    reply[0] = AV_STARTED;
    reply[1] = ap_id;
    reply[3] = msg[3];
    reply[4] = msg[4];
    appl_write(msg[1], 16, reply);
}

/* AV desktops report files dragged onto one of our windows with
   VA_DRAGACCWIND (msg[3] window, msg[6]/[7] the names)              */
static void drag_acc_wind(short *msg)
{
    char *args = *(char **)&msg[6];
    char copy[1024];
    if (!args)
        return;
    strncpy(copy, args, sizeof copy - 1);
    copy[sizeof copy - 1] = 0;
    dbg("VA_DRAGACCWIND window %d: '%s'", msg[3], copy);
    open_files_from_string(copy);
}

static void av_hello(void)
{
    char *srv = getenv("AVSERVER");
    static char myname[9] = "FVIEW   ";
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
    msg[3] = 0x0012;              /* VA_START, quoted names */
    *(char **)&msg[6] = myname;
    appl_write(id, 16, msg);
}

/* ------------------------------------------------------------ messages */

void gui_dispatch_message(short *msg)
{
    VWIN *w = win_find(msg[3]);
    GRECT *r = (GRECT *)&msg[4];

    if (msg[0] >= WM_REDRAW && msg[0] <= 40 && prefs_handle(msg[3])) {
        switch (msg[0]) {
        case WM_REDRAW: prefs_redraw(r); break;
        case WM_TOPPED: prefs_top(); break;
        case WM_CLOSED: prefs_close(); break;
        case WM_MOVED:  prefs_moved(r); break;
        }
        return;
    }
    switch (msg[0]) {
    case MN_SELECTED:
        do_menu(msg[3], msg[4]);
        return;
    case AP_TERM:
        quit();
        return;
    case AP_DRAGDROP:
        drag_drop(msg);
        return;
    case VA_START:
        va_start_msg(msg);
        return;
    case 0x4725:                    /* VA_DRAGACCWIND */
        drag_acc_wind(msg);
        return;
    }
    if (!w) {
        VWIN *dw = win_find_dlg(msg[3]);       /* a progress dialog */
        if (!dw)
            return;
        switch (msg[0]) {
        case WM_REDRAW: win_dlg_redraw(dw, r); break;
        case WM_TOPPED: wind_set(msg[3], WF_TOP, 0, 0, 0, 0); break;
        case WM_MOVED:  wind_set_grect(msg[3], WF_CURRXYWH, r); break;
        case WM_CLOSED: if (dw->job) job_cancel(dw->job); break;
        }
        return;
    }
    switch (msg[0]) {
    case WM_REDRAW:
        win_redraw(w, r);
        break;
    case WM_TOPPED:
        wind_set(w->handle, WF_TOP, 0, 0, 0, 0);
        pal_check_top();
        menu_update();
        break;
    case WM_ONTOP:
    case WM_UNTOPPED:
        pal_check_top();
        menu_update();
        break;
    case WM_CLOSED:
        if (w->kind == K_MAIN) {
            /* closing the main window turns it back into the icon;
               File > Quit ends fview */
            if (!w->iconified)
                main_to_icon(w);
            else
                quit();
        } else
            win_close(w);
        menu_update();
        break;
    case WM_FULLED:
        if (w->kind != K_MAIN && !w->iconified) win_fulled(w);
        break;
    case WM_ARROWED:
        if (!w->iconified) win_arrow(w, msg[4]);
        break;
    case WM_HSLID:
        if (!w->iconified) win_hslide(w, msg[4]);
        break;
    case WM_VSLID:
        if (!w->iconified) win_vslide(w, msg[4]);
        break;
    case WM_SIZED:
        if (w->kind != K_MAIN && !w->iconified) win_sized(w, r);
        break;
    case WM_MOVED:
        win_moved(w, r);
        break;
    case WM_ICONIFY:
    case WM_ALLICONIFY:
        win_iconify(w, r);
        break;
    case WM_UNICONIFY:
        win_uniconify(w, r);
        break;
    case WM_BOTTOMED:
        wind_set(w->handle, WF_BOTTOM, 0, 0, 0, 0);
        pal_check_top();
        break;
    }
}

/* ---------------------------------------------------------- keyboard */

void gui_handle_key(int ks, int key)
{
    VWIN *w = cur_win();
    int scan = (key >> 8) & 0xFF, asc = key & 0xFF;
    int ctrl = ks & K_CTRL, shift = ks & (K_LSHIFT | K_RSHIFT);

    if (ctrl) {
        switch (asc | 0x40) {
        case 'O': do_open(); return;
        case 'U': if (w) win_close(w); menu_update(); return;
        case 'Q': quit(); return;
        case 'S': if (w) win_save_img(w, 0); return;
        case 'D': if (w) win_save_img(w, 1); return;
        case 'P': if (w) print_window(w); return;
        case 'B': if (w) win_page(w, w->page - 1); menu_update(); return;
        case 'N': if (w) win_page(w, w->page + 1); menu_update(); return;
        case 'W': cycle_windows(); return;
        case 'E': prefs_open(); return;
        }
        if (scan == 0x02 || scan == 0x78) {
            if (w && w->kind == K_PDF) win_pdf_zoom(w, 100);
            else if (w) win_set_zoom(w, ZOOM_REAL);
            menu_update();
            return;
        }
        if (scan == 0x03 || scan == 0x79) {
            if (w && w->kind == K_PDF) win_pdf_zoom(w, 0);
            else if (w) win_set_zoom(w, ZOOM_FIT);
            menu_update();
            return;
        }
    }
    if (scan == 0x62) {                    /* Help */
        about();
        return;
    }
    if (!w || w->iconified || win_busy(w))
        return;
    switch (scan) {
    case 0x48: win_arrow(w, shift ? WA_UPPAGE : WA_UPLINE); break;
    case 0x50: win_arrow(w, shift ? WA_DNPAGE : WA_DNLINE); break;
    case 0x4B: win_arrow(w, shift ? WA_LFPAGE : WA_LFLINE); break;
    case 0x4D: win_arrow(w, shift ? WA_RTPAGE : WA_RTLINE); break;
    case 0x47:                             /* Clr/Home */
        if (w->kind == K_PDF)
            win_page(w, shift ? w->npages - 1 : 0);
        else
            win_scroll_to(w, 0, shift ? w->doch : 0);
        menu_update();
        break;
    default:
        if (asc == ' ')
            win_arrow(w, shift ? WA_UPPAGE : WA_DNPAGE);
        else if (w->kind == K_PDF && (asc == '+' || asc == '-')) {
            win_pdf_zoom(w, asc == '+' ? 1 : -1);    /* main or keypad +/- */
            menu_update();
        }
    }
}

void gui_handle_button(int mx, int my, int mb, int ks, int clicks)
{
    int h = wind_find(mx, my);
    VWIN *w;
    (void)mb; (void)ks;
    if (prefs_handle(h)) {
        prefs_click(mx, my);
        return;
    }
    w = win_find(h);
    if (w)
        win_click(w, mx, my, clicks);
    else if ((w = win_find_dlg(h)) != 0)
        win_dlg_click(w, mx, my);
}

/* --------------------------------------------------------- event loop */

static int in_coop;

int gui_coop_poll(void)
{
    short msg[8], mx, my, mb, ks, key, clicks, ev;
    if (in_coop)
        return 0;
    in_coop = 1;
    {
        /* handle everything that is waiting (redraws first of all), not
           just one event, so windows catch up while a file loads      */
        int n = 0;
        do {
            ev = evnt_multi(MU_MESAG | MU_KEYBD | MU_BUTTON | MU_TIMER,
                            0x102, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            msg, 0L, &mx, &my, &mb, &ks, &key, &clicks);
            if (ev & MU_MESAG)
                gui_dispatch_message(msg);
            if (ev & MU_BUTTON)
                gui_handle_button(mx, my, mb, ks, clicks);
            if (ev & MU_KEYBD)
                gui_handle_key(ks, key);
        } while ((ev & (MU_MESAG | MU_KEYBD | MU_BUTTON)) && ++n < 16 && !quitting);
    }
    job_poll();
    update_pointer();
    pal_check_top();
    in_coop = 0;
    return 0;
}

/* outer rectangle for the main window as an icon: bottom left of the
   desktop, where desktop icons usually live */
static void icon_rect(GRECT *r)
{
    GRECT wk;
    if (aes_iconify) {
        r->g_w = 72;
        r->g_h = 72;
    } else {
        /* our own icon-sized window: room for the 32x32 icon + label */
        wk.g_x = wk.g_y = 0;
        wk.g_w = 72;
        wk.g_h = 44;
        wind_calc_grect(WC_BORDER, NAME | CLOSER | MOVER, &wk, r);
    }
    r->g_x = desk.g_x;
    r->g_y = desk.g_y + desk.g_h - r->g_h;
}

/* Iconify the main window: with the AES when it can, otherwise by
   shrinking it to an icon-sized window that shows the same icon.     */
static void main_to_icon(VWIN *w)
{
    GRECT r;
    icon_rect(&r);
    if (aes_iconify)
        win_iconify(w, &r);
    else
        win_fake_iconify(w, &r);
}

static void open_main_window(void)
{
    mainw = win_new();
    if (!mainw)
        return;
    strcpy(mainw->title, " FView ");
    strcpy(mainw->path, "fview");
    if (!win_open(mainw, K_MAIN))
        return;
    if (prefs.start_iconified)
        main_to_icon(mainw);
    else
        win_post_redraw(mainw);
}

/* 3D objects: AES 3.40 (TOS 4) and later, or whatever appl_getinfo says */
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
    /* AES 4.1 (MultiTOS) and later always have WF_ICONIFY, whatever
       appl_getinfo says */
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
    short work_in[11], work_out[57], d = 0;
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
        scr_sysfonts = work_out[10];
    }
    (void)d;
    if (vdih <= 0) {
        form_alert(1, "[3][fview: no VDI workstation.][ Quit ]");
        appl_exit();
        return 1;
    }
    scr_w = work_out[0] + 1;
    scr_h = work_out[1] + 1;
    pix_w_um = work_out[3];
    pix_h_um = work_out[4];
    wind_get_grect(0, WF_WORKXYWH, &desk);

    prefs_load(&prefs);
    disp_init();
    job_init_system();
    fonts_load(&scrfonts, vdih, scr_sysfonts);   /* GDOS fonts, if any */
    aes_iconify = detect_iconify();
    aes_3d = detect_3d();
    dbg("AES %04x apps %d iconify %d 3d %d screen %dx%d planes %d truecolor %d cell %dx%d",
        _AESversion, _AESnumapps, aes_iconify, aes_3d, scr_w, scr_h, scr_planes,
        dinfo.truecolor, cell_w, cell_h);

    /* tell the AES what we understand */
    if (_AESversion >= 0x0400)
        shel_write(9, 1, 0, 0L, 0L);          /* AP_TERM */
    if (_AESnumapps != 1 || _AESversion >= 0x0400)
        menu_register(ap_id, "  FView");

    menu = build_menu();
    menu_bar(menu, 1);
    menu_update();
    graf_mouse(ARROW, 0);
    av_hello();

    open_main_window();

    /* files given on the command line */
    for (i = 1; i < argc; i++)
        open_file(argv[i]);

    while (!quitting) {
        short msg[8], mx, my, mb, ks, key, clicks, ev;
        long timer = job_any_active() ? 100L : 400L;
        ev = evnt_multi(MU_MESAG | MU_KEYBD | MU_BUTTON | MU_TIMER,
                        0x102, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                        msg, timer, &mx, &my, &mb, &ks, &key, &clicks);
        if (ev & MU_MESAG)
            gui_dispatch_message(msg);
        if (ev & MU_BUTTON)
            gui_handle_button(mx, my, mb, ks, clicks);
        if (ev & MU_KEYBD)
            gui_handle_key(ks, key);
        job_poll();
        job_run_pending();          /* TOS / workers off: one at a time */
        win_prepare_pending();      /* PDF page images, with progress */
        update_pointer();
        pal_check_top();
    }

    if (bee_on)
        graf_mouse(ARROW, 0);
    prefs_close();
    win_shutdown();
    fonts_unload(&scrfonts);
    pal_restore();
    menu_bar(menu, 0);
    v_clsvwk(vdih);
    appl_exit();
    return 0;
}
