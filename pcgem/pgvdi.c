/* Copyright (c) 2026 Francis C Bullen. SPDX-License-Identifier: MIT (see LICENSE) */
/*
 * pgvdi.c - GEMLib's VDI calls (as pcgem/include/vdi.h declares them) on
 *           PC GEM, through John Elliott's DJGPP GEM bindings.
 *
 * Bitmaps (MFDBs) live in normal 32-bit memory; the bindings' dj_vro_cpyfm
 * & co. move them to and from the screen in slices. An MFDB with no
 * address is the screen, as on the Atari.
 *
 * PC GEM's VDI has GDOS built in (fonts from ASSIGN.SYS), but only bitmap
 * fonts: vq_vgdos() says so, and the outline-font calls do what a GDOS
 * without outlines does. XGDOS for the PC (a separate resident program)
 * will add Speedo fonts underneath, as on the Atari.
 */
#include <stdlib.h>
#include <string.h>
#include <dpmi.h>
#include <sys/movedata.h>
#include "pgkit.h"

#ifdef EDLOG
void edlog(const char *fmt, ...);
#define PGLOG(...) edlog(__VA_ARGS__)
#else
#define PGLOG(...) ((void)0)
#endif

/* GEMLib's MFDB: the same layout, but fd_addr is a 32-bit pointer */
typedef struct { void *fd_addr; WORD fd_w, fd_h, fd_wdwidth, fd_stand, fd_nplanes, fd_r1, fd_r2, fd_r3; } FE_MFDB;

static short probe_h;        /* a workstation to ask XGDOS on (pg_v_opnvwk) */
static void raw_call(short *contrl, short *intin, short *ptsin, short *intout, short *ptsout);
typedef struct { short *control, *intin, *ptsin, *intout, *ptsout; } FE_VDIPB;

/* ---- the VDI's arrays ------------------------------------------------------
 * The bindings keep contrl/intin/ptsin/... in one small DOS block: intin
 * holds 80 words and ptsin 128 points, and nothing is checked - a longer
 * string or polygon runs on into the next DOS block and DOS's memory chain
 * is wrecked. They're replaced here by bigger ones, and every call is also
 * kept within what the driver says it takes (v_opnvwk's work_out). */
extern LPWORD pContrl, pIntin, pPtsin, pIntout, pPtsout;
extern LPLPTR pPblock;

#define MAXIN   1024                  /* words of intin: characters of text */
#define MAXPTS  2048                  /* points of ptsin */
static int max_in = 80, max_pts = 128; /* what may be passed: the bindings' until ours are in */

static void big_arrays(void)
{
    static int done;
    LPBYTE p;
    if (done) return;
    done = 1;
    vdi_alloc_arrays();               /* the bindings' own, and their parameter block */
    p = dos_alloc(24 + 2 * MAXIN + 4 * MAXPTS + 2 * 512 + 2 * 512 + 20);
    if (!p) return;                   /* keep theirs, and their limits */
    pContrl = (LPWORD)p;
    pIntin  = (LPWORD)(p + 24);
    pPtsin  = (LPWORD)(p + 24 + 2 * MAXIN);
    pIntout = (LPWORD)(p + 24 + 2 * MAXIN + 4 * MAXPTS);
    pPtsout = (LPWORD)(p + 24 + 2 * MAXIN + 4 * MAXPTS + 1024);
    pPblock = (LPLPTR)(p + 24 + 2 * MAXIN + 4 * MAXPTS + 2048);
    LSSET(pPblock,      pContrl);
    LSSET(pPblock + 4,  pIntin);
    LSSET(pPblock + 8,  pPtsin);
    LSSET(pPblock + 12, pIntout);
    LSSET(pPblock + 16, pPtsout);
    max_in = MAXIN; max_pts = MAXPTS;
}

/* the driver's own limits, from vq_extnd's extended answer: [14] vertices,
 * [15] intin words (-1: no limit) */
static void driver_limits(short h)
{
    WORD wo[57];
    memset(wo, 0, sizeof wo);
    vq_extnd(h, 1, wo);
    if (wo[14] > 0 && wo[14] < max_pts) max_pts = wo[14];
    if (wo[15] > 0 && wo[15] < max_in) max_in = wo[15];
    PGLOG("pg: VDI arrays %05lx, driver takes %d vertices, %d intin -> pieces of %d points, %d characters",
          (long)pContrl, wo[14], wo[15], max_pts, max_in);
}

void  pg_v_opnwk(short *wi, short *h, short *wo) { big_arrays(); v_opnwk(wi, h, wo); if (*h > 0) driver_limits(*h); }
void  pg_v_opnvwk(short *wi, short *h, short *wo)
{
    big_arrays();
    v_opnvwk(wi, h, wo);
    if (*h > 0) { driver_limits(*h); if (probe_h <= 0) probe_h = *h; }
}
void  pg_v_clswk(short h) { v_clswk(h); }
void  pg_v_clsvwk(short h) { v_clsvwk(h); }
void  pg_v_clrwk(short h) { v_clrwk(h); }
void  pg_v_updwk(short h) { v_updwk(h); }
/* Like most Atari programs, the front end turns clipping off with a NULL array;
 * the bindings copy the array regardless, and in DJGPP NULL is a page fault */
void pg_vs_clip(short h, short on, short *pxy)
{
    WORD none[4] = { 0, 0, 0, 0 };
    vs_clip(h, on, pxy ? pxy : none);
}
short pg_vswr_mode(short h, short m) { return vswr_mode(h, m); }
/* the screen's planes, asked once */
static short scr_planes_k;
static short pg_planes(short h)
{
    if (!scr_planes_k) {
        short ext[57];
        vq_extnd(h, 1, ext);
        scr_planes_k = ext[4] > 0 ? ext[4] : 1;
    }
    return scr_planes_k;
}

/* the FreeGEM 256-colour drivers (w64_drv.a86, w6x_drv.a86, w10_drv.a86
 * S_COLMAP) set the DAC with INT 10h AX=1010h but hand it the values in the
 * blue, green, red order of the VESA call that was commented out: red and
 * blue come out swapped. Swapped here first, so they come out right. (Their
 * own start-up colours don't go through it, and vq_color answers what was
 * asked, so nothing else needs it.) */
static unsigned char pen_swapped[256];        /* set here: vq_color swaps back */
void  pg_vs_color(short h, short i, const short *rgb)
{
    WORD v[3];
    v[0] = rgb[0]; v[1] = rgb[1]; v[2] = rgb[2];
    if (pg_planes(h) == 8) {
        v[0] = rgb[2]; v[2] = rgb[0];
        if (i >= 0 && i < 256) pen_swapped[i] = 1;
    }
    vs_color(h, i, v);
}
/* vq_color: asked for the colours really set (flag 1), the drivers answer
 * from their own table, which needn't be what's on the screen (the
 * 16-colour ones list the dark colours at 333). Put back from that, the
 * desktop changed. So the colour is read from the video card: the pen's
 * hardware index (the driver's answer in intout[0]), its DAC register (the
 * attribute palette, below 256 colours) and that register's 0-63 values. */
short pg_vq_color(short h, short i, short f, short *rgb)
{
    if (f) {
        short c[12], in[2], out[8];
        memset(c, 0, sizeof c);
        c[0] = 26; c[3] = 2; c[6] = h;
        in[0] = i; in[1] = f;
        out[0] = -1;
        raw_call(c, in, NULL, out, NULL);
        if (out[0] >= 0 && out[0] < 256) {
            __dpmi_regs r;
            int reg = out[0], k;
            if (pg_planes(h) < 8) {
                memset(&r, 0, sizeof r);
                r.x.ax = 0x1007; r.h.bl = (unsigned char)reg;
                __dpmi_int(0x10, &r);
                reg = r.h.bh;
            }
            memset(&r, 0, sizeof r);
            r.x.ax = 0x1015; r.x.bx = (unsigned short)reg;
            __dpmi_int(0x10, &r);
            rgb[0] = r.h.dh; rgb[1] = r.h.ch; rgb[2] = r.h.cl;
            for (k = 0; k < 3; k++) rgb[k] = (short)((rgb[k] & 63) * 1000L / 63);
            return i;
        }
    }
    vq_color(h, i, f, rgb);
    if (i >= 0 && i < 256 && pen_swapped[i]) { short t = rgb[0]; rgb[0] = rgb[2]; rgb[2] = t; }
    return i;
}
void  pg_vq_extnd(short h, short f, short *wo) { vq_extnd(h, f, wo); }

/* GEM/3 has no vq_scrninfo: say "no answer" the way the front end checks */
void  pg_vq_scrninfo(short h, short *wo) { (void)h; wo[0] = -1; wo[1] = -1; wo[2] = 0; }

short pg_vq_gdos(void) { return 1; }               /* PC GEM's VDI is a GDOS */

/* ---- XGDOS for PC GEM ---------------------------------------------------------
 * XGDOS.EXE puts itself in front of GEMVDI on INT EFh; its entry starts
 * with a short jump, GEMVDI's 'GEMVDI23',0 (GEM.EXE checks for it), then
 * 'XGDOS',0. With it
 * there, the Speedo calls are real VDI calls, as on the Atari. */
static int xgdos = -1;

static int xgdos_here(void)
{
    if (xgdos < 0) {
        __dpmi_raddr a;
        char sig[6] = "";
        if (!__dpmi_get_real_mode_interrupt_vector(0xEF, &a))
            dosmemget(a.segment * 16L + a.offset16 + 11, 6, sig);   /* after 'GEMVDI23',0 */
        if (!memcmp(sig, "XGDOS", 6)) xgdos = 1;
        else if (probe_h > 0) {
            /* GEM may have put its own INT EFh entry in front of XGDOS's:
             * ask instead. vqt_cachesize (255) is XGDOS's alone - GEMVDI
             * answers nothing */
            short c[12], in[1] = { 0 }, out[4], p[4];
            memset(c, 0, sizeof c);
            c[0] = 255; c[3] = 1; c[6] = probe_h;
            raw_call(c, in, NULL, out, p);
            xgdos = c[4] == 2;
        } else return 0;            /* no workstation yet: ask again later */
        PGLOG("pg: INT EFh at %04x:%04x; XGDOS %s", a.segment, a.offset16,
              xgdos ? "there: Speedo fonts" : "not there: GEM's bitmap fonts");
    }
    return xgdos;
}

long pg_vq_vgdos(void) { return xgdos_here() ? 0x5F46534DL : 0x5F464E54L; }   /* '_FSM' : '_FNT' */

/* a VDI call made by hand with the arrays given (word counts in contrl) */
static void raw_call(short *contrl, short *intin, short *ptsin, short *intout, short *ptsout)
{
    int ni = contrl[3], np = contrl[1];
    if (ni > max_in) ni = max_in;
    if (np > max_pts) np = max_pts;
    dosmemput(contrl, 24, pContrl);
    if (ni > 0) dosmemput(intin, 2 * ni, pIntin);
    if (np > 0) dosmemput(ptsin, 4 * np, pPtsin);
    vdi();
    dosmemget(pContrl, 24, contrl);
    if (intout && contrl[4] > 0) dosmemget(pIntout, 2 * (contrl[4] < 512 ? contrl[4] : 512), intout);
    if (ptsout && contrl[2] > 0) dosmemget(pPtsout, 4 * (contrl[2] < 256 ? contrl[2] : 256), ptsout);
}

static short simple(short h, short op, short nin, const short *in, short *out, short *pout)
{
    short c[12], o[64], p[16];
    memset(c, 0, sizeof c);
    c[0] = op; c[3] = nin; c[6] = h;
    raw_call(c, (short *)in, NULL, out ? out : o, pout ? pout : p);
    return c[4] > 0 ? (out ? out[0] : o[0]) : 0;
}

/* a long line in pieces, each starting where the last ended */
void pg_v_pline(short h, short n, short *pxy)
{
    while (n > max_pts) {
        v_pline(h, max_pts, pxy);
        pxy += 2 * (max_pts - 1);
        n -= max_pts - 1;
    }
    if (n >= 2) v_pline(h, n, pxy);
}

void pg_v_pmarker(short h, short n, short *pxy)
{
    while (n > 0) {
        short k = n > max_pts ? max_pts : n;
        v_pmarker(h, k, pxy);
        pxy += 2 * k; n -= k;
    }
}

/* a polygon can't be split: one bigger than the driver takes is cut short */
void pg_v_fillarea(short h, short n, short *pxy) { v_fillarea(h, n > max_pts ? max_pts : n, pxy); }
void  pg_v_circle(short h, short x, short y, short r) { v_circle(h, x, y, r); }
void  pg_vr_recfl(short h, short *pxy) { vr_recfl(h, pxy); }
/* text longer than intin takes, a piece at a time, each where the last
 * ended (left-aligned text) */
void pg_v_gtext(short h, short x, short y, const char *s)
{
    char piece[MAXIN + 1];
    size_t n = strlen(s);
    while ((long)n > max_in) {
        WORD ext[8];
        memcpy(piece, s, max_in); piece[max_in] = 0;
        v_gtext(h, x, y, (BYTE *)piece);
        vqt_extent(h, (BYTE *)piece, ext);
        x += ext[2] - ext[0];
        s += max_in; n -= max_in;
    }
    v_gtext(h, x, y, (BYTE *)s);
}

void pg_v_justified(short h, short x, short y, const char *s, short len, short ws, short cs)
{
    char piece[MAXIN + 1];
    size_t n = strlen(s);
    if ((long)n > max_in - 2) {       /* intin also holds the two flags */
        memcpy(piece, s, max_in - 2); piece[max_in - 2] = 0;
        s = piece;
    }
    v_justified(h, x, y, (BYTE *)s, len, ws, cs);
}

short pg_vsl_color(short h, short c) { return vsl_color(h, c); }
short pg_vsl_type(short h, short t) { return vsl_type(h, t); }
short pg_vsl_width(short h, short w) { return vsl_width(h, w); }
void  pg_vsl_ends(short h, short b, short e) { vsl_ends(h, b, e); }
void  pg_vsl_udsty(short h, short pat) { vsl_udsty(h, pat); }
short pg_vsm_type(short h, short t) { return vsm_type(h, t); }
short pg_vsm_color(short h, short c) { return vsm_color(h, c); }
short pg_vsf_interior(short h, short s) { return vsf_interior(h, s); }
short pg_vsf_color(short h, short c) { return vsf_color(h, c); }
short pg_vsf_style(short h, short s) { return vsf_style(h, s); }
short pg_vsf_perimeter(short h, short on) { return vsf_perimeter(h, on); }

short pg_vst_color(short h, short c) { return vst_color(h, c); }
/* the font each workstation has, for pg_vst_height */
static short cur_font[128];
short pg_vst_font(short h, short id)
{
    short r = vst_font(h, id);
    if (h >= 0 && h < 128) cur_font[h] = r;
    return r;
}
short pg_vst_point(short h, short pt, short *cw, short *ch, short *bw, short *bh)
{
    return vst_point(h, pt, cw, ch, bw, bh);
}
/* Atari programs ask for the 8x16 system font by height 13, its height on
 * the ST. PC GEM's 8x16 font is 14 high, so 13 gets the 8x8 font doubled
 * (blocky, twice as wide): ask for 14 instead. */
void  pg_vst_height(short h, short ht, short *cw, short *ch, short *bw, short *bh)
{
    short f = (h >= 0 && h < 128) ? cur_font[h] : 0;
    if ((f == 0 || f == 1) && ht == 13) ht = 14;
    vst_height(h, ht, cw, ch, bw, bh);
}
/* any size with XGDOS; else the nearest bitmap size, as vst_point gives it */
short pg_vst_arbpt(short h, short pt, short *cw, short *ch, short *bw, short *bh)
{
    if (xgdos_here()) {
        short out[4], pts[8], r;
        r = simple(h, 246, 1, &pt, out, pts);
        *cw = pts[0]; *ch = pts[1]; *bw = pts[2]; *bh = pts[3];
        return r;
    }
    return vst_point(h, pt, cw, ch, bw, bh);
}
short pg_vst_effects(short h, short e) { return vst_effects(h, e); }
void  pg_vst_alignment(short h, short hin, short vin, short *hout, short *vout)
{
    vst_alignment(h, hin, vin, hout, vout);
}
short pg_vst_load_fonts(short h, short sel) { return vst_load_fonts(h, sel); }
void  pg_vst_unload_fonts(short h, short sel) { vst_unload_fonts(h, sel); }
void  pg_vst_scratch(short h, short m) { if (xgdos_here()) simple(h, 244, 1, &m, NULL, NULL); }
/* not passed on: XGDOS would keep a pointer into the program's memory, which it
 * can't reach from real mode */
void  pg_vst_error(short h, short m, short *code) { (void)h; (void)m; if (code) *code = 0; }
short pg_vst_charmap(short h, short m) { return xgdos_here() ? simple(h, 236, 1, &m, NULL, NULL) : 0; }

/* the extent of a long string: its pieces' widths added up */
void pg_vqt_extent(short h, const char *s, short *pts)
{
    char piece[MAXIN + 1];
    size_t n = strlen(s);
    short w = 0, ht = 0;
    if ((long)n <= max_in) { vqt_extent(h, (BYTE *)s, pts); return; }
    while (n > 0) {
        WORD ext[8];
        size_t k = (long)n > max_in ? (size_t)max_in : n;
        memcpy(piece, s, k); piece[k] = 0;
        vqt_extent(h, (BYTE *)piece, ext);
        w += ext[2] - ext[0];
        if (ext[5] - ext[3] > ht) ht = ext[5] - ext[3];
        s += k; n -= k;
    }
    pts[0] = 0; pts[1] = 0; pts[2] = w; pts[3] = 0;
    pts[4] = w; pts[5] = ht; pts[6] = 0; pts[7] = ht;
}
short pg_vqt_width(short h, short ch, short *cw, short *ld, short *rd)
{
    return vqt_width(h, (BYTE)ch, cw, ld, rd);
}
short pg_vqt_name(short h, short n, char *name) { return vqt_name(h, n, (BYTE *)name); }

/* XGDOS: its advance; no outlines: the cell width */
void  pg_vqt_advance(short h, short ch, short *ax, short *ay, short *rx, short *ry)
{
    WORD cw = 0, ld, rd;
    if (xgdos_here()) {
        short pts[8];
        simple(h, 247, 1, &ch, NULL, pts);
        *ax = pts[0]; *ay = pts[1];
        if (rx) *rx = pts[2];
        if (ry) *ry = pts[3];
        return;
    }
    vqt_width(h, (BYTE)ch, &cw, &ld, &rd);
    *ax = cw; *ay = 0;
    if (rx) *rx = 0;
    if (ry) *ry = 0;
}

void  pg_v_getoutline(short h, short ch, short *xy, char *bez, short max, short *count)
{
    (void)h; (void)ch; (void)xy; (void)bez; (void)max;
    *count = 0;
}

static MFDB *kit_mfdb(FE_MFDB *m, MFDB *out)
{
    memcpy(out, m, sizeof *out);
    return out;
}

/* ---- bitmaps --------------------------------------------------------------
 * PC GEM's device format isn't one layout: planes one after another on a
 * 16-colour screen, a byte per pixel (packed) on the 256-colour drivers. The
 * bindings cut big bitmaps into strips assuming planes, so a packed one comes
 * back scrambled, and PC GEM's VDI doesn't copy from one bitmap in memory to
 * another. So a bitmap the program has turned into device format (vr_trnfm,
 * standard -> device, both in memory) is kept here in standard format - whose
 * layout is known - and marked; copying it to the screen transforms it a
 * strip at a time in DOS memory, then copies the strip. Device format really
 * read from the screen goes through the bindings as before. */
#define STD_MARK1 0x5047        /* fd_r1, fd_r2: "standard data inside" */
#define STD_MARK2 0x5354
static int std_marked(const FE_MFDB *m) { return m->fd_addr && m->fd_r1 == STD_MARK1 && m->fd_r2 == STD_MARK2; }
static long mfdb_bytes(const FE_MFDB *m) { return (long)m->fd_wdwidth * 2 * m->fd_h * (m->fd_nplanes > 0 ? m->fd_nplanes : 1); }

static LPBYTE strip_buf;        /* DOS memory: two MFDBs, then the two strips */
static long strip_len;
static int strip_logged;

/* as big as conventional memory allows, 64K down to 4K: GEM has most of it */
static int strip_alloc(void)
{
    long n;
    if (strip_buf) return 1;
    for (n = 65536L; n >= 4096L && !strip_buf; n /= 2)
        if ((strip_buf = dos_alloc(n)) != 0) strip_len = n;
    if (!strip_logged) { strip_logged = 1; PGLOG("pg: strip buffer %ld bytes at DOS %05lx", strip_len, (long)strip_buf); }
    return strip_buf != 0;
}

/* given back after every use: conventional memory is GEM's, and held from
 * start-up it left too little for the menu bar */
static void strip_free(void)
{
    if (strip_buf) dos_free(strip_buf);
    strip_buf = 0;
    strip_len = 0;
}

/* a 16-bit MFDB at d: data at mp (0: the screen), w x h, planes, standard? */
static void put_mfdb(LPBYTE d, LPBYTE mp, short w, short h, short wdw, short stand, short np)
{
    short m[8];
    if (mp) LSSET(d, mp); else _farpokel(_dos_ds, d, 0);
    m[0] = w; m[1] = h; m[2] = wdw; m[3] = stand; m[4] = np; m[5] = m[6] = m[7] = 0;
    dosmemput(m, 16, d + 4);
}

static void raw_blit(short h, short op, short nint, const short *in, const short *pxy, LPBYTE s, LPBYTE d)
{
    short c[12];
    memset(c, 0, sizeof c);
    c[0] = op; c[1] = pxy ? 4 : 0; c[3] = nint; c[6] = h;
    dosmemput(c, 24, pContrl);
    if (nint) dosmemput(in, 2 * nint, pIntin);
    if (pxy) dosmemput(pxy, 16, pPtsin);
    LSSET(pContrl + 14, s);
    LSSET(pContrl + 18, d);
    vdi();
}

/* copy rectangle pxy of the marked (standard format) bitmap src to the
 * screen: op 109 vro_cpyfm, 121 vrt_cpyfm (colors) */
/* standard -> device for rows y..y+n-1 of every plane, into a strip n rows
 * high, planes one after another.
 * Up to 4 planes (the 16-colour drivers): the standard planes with each
 * word's two bytes swapped - leftmost pixel in bit 7 of the first byte.
 * 8 planes (the FreeGEM 256-colour drivers, rasvx8.a86): a "plane" is not
 * a bit-plane - plane k holds whole pixel bytes, those of x = 8j + k, so
 * byte j of plane k's row is the pixel value at x = 8j + k. */
static int cpu_device(const FE_MFDB *src, long y, short n, unsigned char *out)
{
    long wdw = src->fd_wdwidth, plane = wdw * src->fd_h, rowb = wdw * 2, i, j;
    short np = src->fd_nplanes > 0 ? src->fd_nplanes : 1, p, k;
    const unsigned short *sp = (const unsigned short *)src->fd_addr;
    if (np != 8) {
        for (p = 0; p < np; p++) {
            const unsigned short *r = sp + p * plane + y * wdw;
            unsigned char *o = out + (long)p * n * rowb;
            for (i = 0; i < wdw * n; i++) { o[2 * i] = (unsigned char)(r[i] >> 8); o[2 * i + 1] = (unsigned char)r[i]; }
        }
        return 1;
    }
    for (i = 0; i < n; i++) {
        const unsigned short *r = sp + (y + i) * wdw;
        for (k = 0; k < 8; k++) {
            unsigned char *o = out + (long)k * n * rowb + i * rowb;
            for (j = 0; j < rowb; j++) {
                long x = 8 * j + k;
                unsigned short bit = (unsigned short)(0x8000 >> (x & 15));
                unsigned char v = 0;
                for (p = 0; p < 8; p++)
                    if (r[p * plane + (x >> 4)] & bit) v |= (unsigned char)(1 << p);
                o[j] = v;
            }
        }
    }
    return 1;
}

static unsigned char *cpu_buf;  /* a strip in device format, before it goes down */

/* under 8 pixels wide on the 256-colour drivers (whose copy runs away
 * below 8): the 8 screen pixels from bx are read into memory, the narrow
 * column's pixels put in, and the 8 written back - the others get back
 * what they had, so nothing beside it (another window) is touched */
static void narrow_to_screen(short h, short mode, const short *pxy, const FE_MFDB *src)
{
    long wdw = src->fd_wdwidth, plane = wdw * src->fd_h;
    const unsigned short *sp = (const unsigned short *)src->fd_addr;
    short sx = pxy[0] < pxy[2] ? pxy[0] : pxy[2], w = (short)((pxy[2] > pxy[0] ? pxy[2] - pxy[0] : pxy[0] - pxy[2]) + 1);
    short sy = pxy[1] < pxy[3] ? pxy[1] : pxy[3], hgt = (short)((pxy[3] > pxy[1] ? pxy[3] - pxy[1] : pxy[1] - pxy[3]) + 1);
    short dx = pxy[4] < pxy[6] ? pxy[4] : pxy[6], dy = pxy[5] < pxy[7] ? pxy[5] : pxy[7];
    short bx = dx, rows, y, n, q[8], in[1], ext[57], c, r, k;
    long room = strip_len - 64;
    LPBYTE md = strip_buf, scr = strip_buf + 20, bd = strip_buf + 64;
    unsigned char *buf;
    if (mode != 3) return;                       /* S_ONLY only */
    vq_extnd(h, 0, ext);
    if (bx + 7 > ext[0]) bx = (short)(ext[0] - 7);
    if (bx < 0) return;
    rows = (short)(room / 16);                   /* 8 planes of 2 bytes a row */
    if (rows < 1) return;
    buf = (unsigned char *)malloc(16L * rows);
    if (!buf) return;
    put_mfdb(scr, 0, 0, 0, 0, 0, 0);
    in[0] = 3;
    for (y = 0; y < hgt; y += rows) {
        n = (short)(hgt - y < rows ? hgt - y : rows);
        put_mfdb(md, bd, 16, n, 1, 0, 8);
        q[0] = bx; q[2] = (short)(bx + 7); q[1] = (short)(dy + y); q[3] = (short)(dy + y + n - 1);
        q[4] = 0; q[6] = 7; q[5] = 0; q[7] = (short)(n - 1);
        raw_blit(h, 109, 1, in, q, scr, md);             /* screen -> memory */
        dosmemget(bd, 16L * n, buf);
        for (r = 0; r < n; r++) {
            const unsigned short *row = sp + (long)(sy + y + r) * wdw;
            for (c = 0; c < w; c++) {
                long x = sx + c;
                unsigned short bit = (unsigned short)(0x8000 >> (x & 15));
                unsigned char v = 0;
                for (k = 0; k < 8; k++)
                    if (row[k * plane + (x >> 4)] & bit) v |= (unsigned char)(1 << k);
                buf[(long)(dx - bx + c) * 2 * n + r * 2] = v;   /* plane p, byte 0 of row r */
            }
        }
        dosmemput(buf, 16L * n, bd);
        q[0] = 0; q[2] = 7; q[1] = 0; q[3] = (short)(n - 1);
        q[4] = bx; q[6] = (short)(bx + 7); q[5] = (short)(dy + y); q[7] = (short)(dy + y + n - 1);
        raw_blit(h, 109, 1, in, q, md, scr);             /* and back */
    }
    free(buf);
}

static void std_to_screen(short h, short op, short mode, const short *pxy, const FE_MFDB *src, const short *colors)
{
    long row = (long)src->fd_wdwidth * 2, plane = row * src->fd_h;
    short np = src->fd_nplanes > 0 ? src->fd_nplanes : 1;
    short y0 = pxy[1] < pxy[3] ? pxy[1] : pxy[3], y1 = pxy[1] < pxy[3] ? pxy[3] : pxy[1];
    short dy = (pxy[5] < pxy[7] ? pxy[5] : pxy[7]) - y0;
    long room;
    short rows, y, p, in[3];
    LPBYTE ms, md, bs, bd, scr;
    if (!strip_alloc()) return;
    /* the 256-colour drivers' memory -> screen copy (rasvx8.a86 ms_getrow)
     * runs away on anything narrower than 8 pixels: such a sliver is left */
    if (np == 8 && (pxy[2] > pxy[0] ? pxy[2] - pxy[0] : pxy[0] - pxy[2]) < 7) {
        narrow_to_screen(h, mode, pxy, src);
        strip_free();
        return;
    }
    room = (strip_len - 64) / 2;
    rows = (short)(room / (row * np));
    PGLOG("pg: std_to_screen op %d %dx%d %d planes, rows %d-%d, %d rows a strip", op,
          src->fd_w, src->fd_h, np, pxy[1], pxy[3], rows);
    if (rows < 1) { strip_free(); return; }
    ms = strip_buf; md = strip_buf + 20; scr = strip_buf + 40;
    bs = strip_buf + 64; bd = bs + room;
    put_mfdb(scr, 0, 0, 0, 0, 0, 0);
    if (y0 < 0) y0 = 0;
    if (y1 >= src->fd_h) y1 = src->fd_h - 1;
    for (y = y0; y <= y1; y += rows) {
        short n = (short)(y1 - y + 1 < rows ? y1 - y + 1 : rows), q[8];
        put_mfdb(md, bd, src->fd_w, n, src->fd_wdwidth, 0, np);
        if (!cpu_buf) cpu_buf = (unsigned char *)malloc(room);
        if (cpu_buf && cpu_device(src, y, n, cpu_buf)) {
            dosmemput(cpu_buf, n * row * np, bd);           /* converted here */
        } else {
            for (p = 0; p < np; p++)
                dosmemput((char *)src->fd_addr + p * plane + y * row, n * row, bs + p * n * row);
            put_mfdb(ms, bs, src->fd_w, n, src->fd_wdwidth, 1, np);
            raw_blit(h, 110, 0, NULL, NULL, ms, md);         /* vr_trnfm, the strip */
        }
        q[0] = pxy[0]; q[2] = pxy[2]; q[1] = 0; q[3] = (short)(n - 1);
        q[4] = pxy[4]; q[6] = pxy[6]; q[5] = (short)(y + dy); q[7] = (short)(y + dy + n - 1);
        in[0] = mode;
        if (colors) { in[1] = colors[0]; in[2] = colors[1]; }
        raw_blit(h, op, (short)(colors ? 3 : 1), in, q, md, scr);
    }
    strip_free();
}

/* v_get_pixel: the FreeGEM drivers answer the pen in "pel" too, not the
 * pixel value (256 colours: pen 1, black, reads 1 though it is pixel 255;
 * 16 colours the same). The pixel is copied off the screen instead. On an
 * 8-plane screen the 8 pixels from x & ~7 are copied to a 16x1 bitmap in DOS
 * memory: in their memory format pixel k is the first byte of plane k (2
 * bytes a plane). Never less than 8 wide: rasvx8.a86's sm_putrow works out
 * a byte count per plane that goes negative below that, and runs away. */
short pg_v_get_pixel(short h, short x, short y, short *pel, short *idx)
{
    short r = v_get_pixel(h, x, y, pel, idx), q[8], in[1];
    unsigned char b;
    LPBYTE md, scr, bd;
    short np = pg_planes(h), k;
    unsigned char w[16];
    if (np < 2 || np > 8 || !strip_alloc()) return r;
    md = strip_buf; scr = strip_buf + 20; bd = strip_buf + 64;
    put_mfdb(scr, 0, 0, 0, 0, 0, 0);
    put_mfdb(md, bd, 16, 1, 1, 0, np);
    in[0] = 3;                                  /* S_ONLY */
    if (np == 8) {
        q[0] = (short)(x & ~7); q[2] = (short)(q[0] + 7); q[1] = q[3] = y;
        q[4] = q[5] = q[7] = 0; q[6] = 7;
        raw_blit(h, 109, 1, in, q, scr, md);
        dosmemget(bd + 2 * (x & 7), 1, &b);
    } else {
        /* the 16-colour drivers: bit-planes, each word's bytes swapped
         * (leftmost pixel in bit 7 of the first byte); 16 pixels copied */
        q[0] = (short)(x & ~15); q[2] = (short)(q[0] + 15); q[1] = q[3] = y;
        q[4] = q[5] = q[7] = 0; q[6] = 15;
        raw_blit(h, 109, 1, in, q, scr, md);
        dosmemget(bd, 2 * np, w);
        b = 0;
        for (k = 0; k < np; k++)
            if (w[2 * k + ((x & 15) >> 3)] & (0x80 >> (x & 7))) b |= (unsigned char)(1 << k);
    }
    strip_free();
    *pel = b;
    return r;
}

/* a 32-bit bitmap (fd_addr set) goes through the bindings' slicing copies;
 * two screen MFDBs need nothing moved */
void pg_vro_cpyfm(short h, short mode, short *pxy, FE_MFDB *src, FE_MFDB *dst)
{
    MFDB s, d;
    if (std_marked(src) && !dst->fd_addr) { std_to_screen(h, 109, mode, pxy, src, NULL); return; }
    if (dst->fd_addr) dst->fd_r1 = dst->fd_r2 = 0;      /* real device data from now on */
    kit_mfdb(src, &s); kit_mfdb(dst, &d);
    if (src->fd_addr || dst->fd_addr) dj_vro_cpyfm(h, mode, pxy, &s, &d);
    else vro_cpyfm(h, mode, pxy, &s, &d);
}

void pg_vrt_cpyfm(short h, short mode, short *pxy, FE_MFDB *src, FE_MFDB *dst, short *colors)
{
    MFDB s, d;
    if (std_marked(src) && !dst->fd_addr) { std_to_screen(h, 121, mode, pxy, src, colors); return; }
    if (dst->fd_addr) dst->fd_r1 = dst->fd_r2 = 0;
    kit_mfdb(src, &s); kit_mfdb(dst, &d);
    if (src->fd_addr || dst->fd_addr) dj_vrt_cpyfm(h, mode, pxy, &s, &d, colors);
    else vrt_cpyfm(h, mode, pxy, &s, &d, colors);
}

void pg_vr_trnfm(short h, FE_MFDB *src, FE_MFDB *dst)
{
    MFDB s, d;
    if (src->fd_addr && dst->fd_addr && src->fd_stand && !dst->fd_stand) {
        /* standard -> device in memory: kept as standard, marked */
        if (dst->fd_addr != src->fd_addr) memmove(dst->fd_addr, src->fd_addr, mfdb_bytes(src));
        dst->fd_r1 = STD_MARK1; dst->fd_r2 = STD_MARK2;
        return;
    }
    if (std_marked(src) && dst->fd_addr && dst->fd_stand) {     /* and back */
        if (dst->fd_addr != src->fd_addr) memmove(dst->fd_addr, src->fd_addr, mfdb_bytes(src));
        return;
    }
    kit_mfdb(src, &s); kit_mfdb(dst, &d);
    dj_vr_trnfm(h, &s, &d);
}

/* gemrend.c asks vqt_fontinfo this way (opcode 131); answer it from the
 * bindings, in the standard reply layout. Other raw calls get nothing. */
void pg_vdi(FE_VDIPB *pb)
{
    if (pb->control[0] == 131) {
        WORD mn, mx, dist[5], mw, eff[3];
        vqt_font_info(pb->control[6], &mn, &mx, dist, &mw, eff);
        pb->intout[0] = mn; pb->intout[1] = mx;
        pb->ptsout[0] = mw;
        pb->ptsout[1] = dist[0]; pb->ptsout[3] = dist[1]; pb->ptsout[5] = dist[2];
        pb->ptsout[7] = dist[3]; pb->ptsout[9] = dist[4];
        pb->ptsout[2] = eff[0]; pb->ptsout[4] = eff[1]; pb->ptsout[6] = eff[2];
        pb->control[2] = 5; pb->control[4] = 2;
        return;
    }
    if (!xgdos_here()) { pb->control[2] = pb->control[4] = 0; return; }
    if (pb->control[0] == 243) {        /* v_getoutline: its two arrays must be in DOS memory */
        short max = pb->intin[1], in[6];
        short *xy, n;
        char *bez;
        LPBYTE dxy, dbez;
        memcpy(&xy, &pb->intin[2], sizeof xy);
        memcpy(&bez, &pb->intin[2 + sizeof(void *) / 2], sizeof bez);
        if (max <= 0 || max > 4096) max = 4096;
        dxy = dos_alloc(4L * max);
        dbez = dos_alloc(max);
        if (!dxy || !dbez) {
            if (dxy) dos_free(dxy);
            if (dbez) dos_free(dbez);
            pb->control[4] = 1; pb->intout[0] = 0;
            return;
        }
        in[0] = pb->intin[0]; in[1] = max;
        in[2] = (short)(dxy & 15); in[3] = (short)(dxy >> 4);       /* far pointers: offset, segment */
        in[4] = (short)(dbez & 15); in[5] = (short)(dbez >> 4);
        pb->control[3] = 6;
        raw_call(pb->control, in, pb->ptsin, pb->intout, pb->ptsout);
        n = pb->control[4] > 0 ? pb->intout[0] : 0;
        if (n > 0 && n <= max) {
            dosmemget(dxy, 4L * n, xy);
            dosmemget(dbez, n, bez);
        }
        dos_free(dxy); dos_free(dbez);
        return;
    }
    raw_call(pb->control, pb->intin, pb->ptsin, pb->intout, pb->ptsout);
}

/* more of the VDI (Fbench): straight to the bindings */
void  pg_v_bar(short h, short *pxy) { v_bar(h, pxy); }
void  pg_v_arc(short h, short x, short y, short r, short b, short e) { v_arc(h, x, y, r, b, e); }
void  pg_v_pieslice(short h, short x, short y, short r, short b, short e) { v_pieslice(h, x, y, r, b, e); }
void  pg_v_ellipse(short h, short x, short y, short xr, short yr) { v_ellipse(h, x, y, xr, yr); }
void  pg_v_ellarc(short h, short x, short y, short xr, short yr, short b, short e) { v_ellarc(h, x, y, xr, yr, b, e); }
void  pg_v_ellpie(short h, short x, short y, short xr, short yr, short b, short e) { v_ellpie(h, x, y, xr, yr, b, e); }
void  pg_v_rbox(short h, short *pxy) { v_rbox(h, pxy); }
void  pg_v_rfbox(short h, short *pxy) { v_rfbox(h, pxy); }
void  pg_v_contourfill(short h, short x, short y, short c) { v_contourfill(h, x, y, c); }
void  pg_v_show_c(short h, short reset) { v_show_c(h, reset); }
void  pg_v_hide_c(short h) { v_hide_c(h); }
short pg_vsm_height(short h, short ht) { return vsm_height(h, ht); }
short pg_vst_rotation(short h, short a) { return vst_rotation(h, a); }
void  pg_vsf_udpat(short h, short *pat, short planes) { vsf_udpat(h, pat, planes); }
void  pg_vqt_attributes(short h, short *attr) { vqt_attributes(h, attr); }
short pg_vqt_fontinfo(short h, short *mn, short *mx, short *dist, short *mw, short *eff)
{
    return vqt_font_info(h, mn, mx, dist, mw, eff);
}
