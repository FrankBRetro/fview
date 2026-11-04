/* vdistub.c - records VDI calls made by render.c as text, so a script can
 * draw them; lets the text and PDF renderers be checked on a host.   */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../src/gemui.h"
#include "fontwidths.h"

FILE *vlog;
static int cur_font = 1, cur_px = 16;
static int cur_h = 13, txt_col = 1, eff, lcol = 1, lwid = 1, fcol = 1, finter = 1,
           fstyle = 0, valign = 0;
short syspal[256][3] = {
    {1000,1000,1000},{0,0,0},{1000,0,0},{0,1000,0},{0,0,1000},{0,1000,1000},
    {1000,1000,0},{1000,0,1000},{752,752,752},{502,502,502},{502,0,0},
    {0,502,0},{0,0,502},{0,502,502},{502,502,0},{502,0,502}
};

static void cell(int h, short *cw, short *ch)
{
    int c = h >= 13 ? h * 16 / 13 : h + 2;
    *ch = c;
    *cw = c / 2;
}

void vst_height(VdiHdl hd, short h, short *a, short *b, short *cw, short *ch)
{
    (void)hd;
    cur_h = h;
    cur_px = h * 4 / 3;     /* outline fonts: height is about 3/4 of the em */
    cell(h, cw, ch);
    *a = *cw; *b = *ch;
}
void vst_alignment(VdiHdl h, short hor, short ver, short *a, short *b)
{ (void)h; valign = ver; *a = hor; *b = ver; }
short vst_color(VdiHdl h, short c) { (void)h; return txt_col = c; }
short vswr_mode(VdiHdl h, short m) { (void)h; return m; }
short vst_effects(VdiHdl h, short e) { (void)h; return eff = e; }
/* fake GDOS: font 1 is the system font, 2.. proportional outline fonts */
static const char *gnames[] = { "System Font", "Swiss 721", "Swiss 721 Bold", "Swiss 721 Italic",
    "Dutch 801 Roman", "Dutch 801 Bold", "Dutch 801 Italic", "American Garamond",
    "American Garamond Bold", "Monospace 821", 0 };
short vst_font(VdiHdl h, short f) { (void)h; cur_font = f; return f; }
short vq_gdos(void) { return 1; }
long vq_vgdos(void) { return GDOS_FSM; }
short vst_load_fonts(VdiHdl h, short s) { (void)h; (void)s; return 9; }
void vst_unload_fonts(VdiHdl h, short s) { (void)h; (void)s; }
short vqt_name(VdiHdl h, short i, char *name) { (void)h; strcpy(name, gnames[i - 1]); return i; }
static void set_pt(int pt, short *cw, short *ch, short *cellw, short *cellh)
{
    cur_px = pt * 91 / 72;
    if (cur_font == 1) { cell(pt >= 10 ? 13 : 6, cw, ch); cur_px = *ch; }
    else { *ch = cur_px; *cw = cur_px / 2; }
    *cellw = *cw; *cellh = *ch;
    cur_h = *ch;
}
short vst_point(VdiHdl h, short pt, short *a, short *b, short *c, short *d) { (void)h; set_pt(pt, a, b, c, d); return pt; }
short vst_arbpt(VdiHdl h, short pt, short *a, short *b, short *c, short *d) { (void)h; set_pt(pt, a, b, c, d); return pt; }
static int char_w(int c)
{
    if (cur_font == 1) { short cw, ch; cell(cur_h, &cw, &ch); return cw; }
    if (cur_font == 10) return cur_px * 600 / 1000;
    return (cur_font >= 5 ? serif_w : sans_w)[c & 255] * cur_px / 1000 + ((eff & 1) ? 1 : 0);
}
void vqt_extent(VdiHdl h, const char *s, short ext[])
{
    int w = 0;
    (void)h;
    for (; *s; s++) w += char_w((unsigned char)*s);
    memset(ext, 0, 16);
    ext[2] = ext[4] = (short)w;
    ext[5] = ext[7] = (short)cur_px;
}
void vqt_fontinfo(VdiHdl h, short *mn, short *mx, short dist[], short *mw, short effs[])
{
    (void)h; *mn = 32; *mx = 255; *mw = cur_px;
    dist[0] = (short)(cur_px / 4); dist[1] = (short)(cur_px / 5); dist[2] = (short)(cur_px / 2);
    dist[3] = (short)(cur_px * 3 / 4); dist[4] = (short)(cur_px * 4 / 5);
    effs[0] = effs[1] = effs[2] = 0;
}

void v_gtext(VdiHdl h, short x, short y, const char *s)
{
    short cw, ch;
    int w = 0;
    const char *p;
    (void)h;
    cell(cur_h, &cw, &ch);
    for (p = s; *p; p++) w += char_w((unsigned char)*p);
    fprintf(vlog, "T %d %d %d %d %d %d %d %d %d %d %s\n", x, y, w, ch,
            txt_col, eff, valign, cur_h, cur_font, cur_px, s);
}
void v_justified(VdiHdl h, short x, short y, const char *s, short len, short ws, short cs)
{
    short cw, ch;
    (void)h; (void)ws; (void)cs;
    cell(cur_h, &cw, &ch);
    fprintf(vlog, "T %d %d %d %d %d %d %d %d %d %d %s\n", x, y, len, ch, txt_col, eff,
            valign, cur_h, cur_font, cur_px, s);
}
short vsl_color(VdiHdl h, short c) { (void)h; return lcol = c; }
short vsl_width(VdiHdl h, short w) { (void)h; return lwid = w; }
short vsl_type(VdiHdl h, short t) { (void)h; return t; }
void v_pline(VdiHdl h, short n, short *p)
{
    int i;
    (void)h;
    fprintf(vlog, "L %d %d %d", lcol, lwid, n);
    for (i = 0; i < n * 2; i++) fprintf(vlog, " %d", p[i]);
    fputc('\n', vlog);
}
/* raster functions: vr_trnfm converts standard -> interleaved like an ST
   screen; blits are written out as PPM files for drawlog.py           */
int vdih = 1, scr_planes = 4, scr_w = 640, scr_h = 400, pix_w_um = 278, pix_h_um = 278;
PREFS prefs;
DISPINFO dinfo;
static int nblit;
static const u8 pal16[48] = { 255,255,255, 255,0,0, 0,255,0, 255,255,0, 0,0,255, 255,0,255,
    0,255,255, 170,170,170, 85,85,85, 255,128,128, 128,255,128, 255,255,128, 128,128,255,
    255,128,255, 128,255,255, 0,0,0 };   /* by pixel value: 0 white, 15 black */

void vr_trnfm(VdiHdl h, MFDB *s, MFDB *d)
{
    long wdw = s->fd_wdwidth, n = wdw * s->fd_h, i, p, np = s->fd_nplanes;
    u16 *src = s->fd_addr, *dst = d->fd_addr, *tmp;
    (void)h;
    if (np == 1) {
        if (src != dst) memcpy(dst, src, n * 2);
        return;
    }
    tmp = malloc(n * np * 2);
    for (i = 0; i < n; i++)
        for (p = 0; p < np; p++)
            tmp[i * np + p] = src[p * n + i];
    memcpy(dst, tmp, n * np * 2);
    free(tmp);
}

static int pixel_of(MFDB *m, int x, int y)
{
    const u16 *g = (const u16 *)m->fd_addr + ((long)y * m->fd_wdwidth + (x >> 4)) * m->fd_nplanes;
    int p, v = 0, bit = 15 - (x & 15);
    for (p = 0; p < m->fd_nplanes; p++) v |= ((g[p] >> bit) & 1) << p;
    return v;
}

static void blit_out(short *pxy, MFDB *m, int mono, short *col, int trans)
{
    char fn[256];
    FILE *f;
    int x, y, w = pxy[2] - pxy[0] + 1, h = pxy[3] - pxy[1] + 1;
    sprintf(fn, "/tmp/claude-0/-home-claude/27f3716f-36f8-5e4e-8742-866ed226c668/scratchpad/blit%03d.ppm", nblit++);
    f = fopen(fn, "wb");
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            int v = pixel_of(m, pxy[0] + x, pxy[1] + y);
            const u8 *c;
            if (mono) c = v ? (const u8 *)"\0\0\0" : (const u8 *)"\377\377\376";
            else c = pal16 + (v & 15) * 3;
            fwrite(c, 1, 3, f);
        }
    fclose(f);
    fprintf(vlog, "B %d %d %d %d %d %s\n", pxy[4], pxy[5], w, h, trans, fn);
    (void)col;
}

void vro_cpyfm(VdiHdl h, short mode, short pxy[], MFDB *s, MFDB *d)
{ (void)h; (void)mode; (void)d; blit_out(pxy, s, s->fd_nplanes == 1, 0, 0); }
void vrt_cpyfm(VdiHdl h, short mode, short pxy[], MFDB *s, MFDB *d, short col[])
{ (void)h; (void)d; blit_out(pxy, s, 1, col, mode == MD_TRANS); }
short rc_intersect(const GRECT *r1, GRECT *r2)
{
    int x = r1->g_x > r2->g_x ? r1->g_x : r2->g_x, y = r1->g_y > r2->g_y ? r1->g_y : r2->g_y;
    int x2 = r1->g_x + r1->g_w < r2->g_x + r2->g_w ? r1->g_x + r1->g_w : r2->g_x + r2->g_w;
    int y2 = r1->g_y + r1->g_h < r2->g_y + r2->g_h ? r1->g_y + r1->g_h : r2->g_y + r2->g_h;
    r2->g_x = x; r2->g_y = y; r2->g_w = x2 - x; r2->g_h = y2 - y;
    return x2 > x && y2 > y;
}

short vsf_interior(VdiHdl h, short s) { (void)h; return finter = s; }
short vsf_color(VdiHdl h, short c) { (void)h; return fcol = c; }
short vsf_style(VdiHdl h, short s) { (void)h; return fstyle = s; }
short vsf_perimeter(VdiHdl h, short v) { (void)h; return v; }
void v_fillarea(VdiHdl h, short n, short *p)
{
    int i;
    (void)h;
    fprintf(vlog, "F %d %d %d %d", fcol, finter, fstyle, n);
    for (i = 0; i < n * 2; i++) fprintf(vlog, " %d", p[i]);
    fputc('\n', vlog);
}

void dbg(const char *fmt, ...) { (void)fmt; }
