/* paltest.c - host test of the picture tools in palette.c (make paltest) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fview.h"
#include "palette.h"
/* host stubs */

static void *slots[RB_MAX];
void *res_alloc(int s, long n) { free(slots[s]); slots[s] = malloc(n); return slots[s]; }
void res_free(int s) { free(slots[s]); slots[s] = 0; }

int main(int argc, char **argv)
{
    IMAGE im, out, rs;
    PREFS pf;
    DISPINFO d;
    long x, y, n; int used, i, k;
    u8 pal[768]; int np;
    memset(&pf, 0, sizeof pf); pf.dither = DITH_FS; pf.serpentine = 1;
    /* RGB gradient 320x200 */
    memset(&im, 0, sizeof im); im.w = 320; im.h = 200; im.type = IT_RGB; im.stride = 960;
    im.px = malloc(960 * 200);
    for (y = 0; y < 200; y++) for (x = 0; x < 320; x++) { u8 *p = im.px + y*960 + x*3; p[0] = x*255/319; p[1] = y*255/199; p[2] = (x+y)&255; }
    n = img_count_real(&im, &used, 0); printf("real rgb colours %ld\n", n);
    if (img_reduce_colours(&im, &out, 16, &pf, 0)) { puts("reduce failed"); return 1; }
    printf("reduced: type %d ncolors %d\n", out.type, out.ncolors); { u8 *c = malloc(320*200); memcpy(c, out.px, 320*200); out.px = c; slots[RB_ORIG] = 0; }
    n = img_count_real(&out, &used, 0); printf("reduced real %ld used %d\n", n, used);
    memset(&d, 0, sizeof d); d.planes = 8; d.palmode = PAL_OPTIMISE;
    for (i = 0; i < 16; i++) d.reserved[i] = 1;
    n = img_count_seen(&out, 320, 200, &d, &pf, 0); printf("seen (dithered 16 col) %ld\n", n);
    pf.dither = DITH_NONE;
    n = img_count_seen(&out, 320, 200, &d, &pf, 0); printf("seen (no dither) %ld\n", n);
    { DISPINFO d4; memset(&d4,0,sizeof d4); d4.planes = 4; d4.palmode = PAL_OPTIMISE; d4.reserved[0]=d4.reserved[1]=1;
      pf.dither = DITH_FS; printf("rgb on 16 colours FS: seen %ld\n", img_count_seen(&im, 320, 200, &d4, &pf, 0));
      pf.dither = DITH_NONE; printf("rgb on 16 colours none: seen %ld\n", img_count_seen(&im, 320, 200, &d4, &pf, 0)); }
    pf.dither = DITH_FS;
    { IMAGE o2; IMAGE tmp = out; tmp.px = malloc(320*200); memcpy(tmp.px, out.px, 320*200);
      if (img_resize(&tmp, &rs, 100, 50, 1, 0)) { puts("resize fail"); return 1; }
      printf("resized indexed %ldx%ld type %d\n", rs.w, rs.h, rs.type);
      if (img_resize(&im, &o2, 640, 400, 1, 0)) return 1; printf("enlarged rgb %ldx%ld px0 %d,%d,%d\n", o2.w, o2.h, o2.px[0], o2.px[1], o2.px[2]);
    }
    if (pal_save_file("t.pal", out.pal, out.ncolors) || pal_load_file("t.pal", pal, &np)) return 1;
    printf("jasc round trip n=%d same=%d\n", np, !memcmp(pal, out.pal, np*3));
    if (pal_save_file("t.act", out.pal, out.ncolors) || pal_load_file("t.act", pal, &np)) return 1;
    printf("act round trip n=%d same=%d\n", np, !memcmp(pal, out.pal, np*3));
    if (pal_save_file("t.gpl", out.pal, out.ncolors) || pal_load_file("t.gpl", pal, &np)) return 1;
    printf("gpl round trip n=%d same=%d\n", np, !memcmp(pal, out.pal, np*3));
    { const IMAGE *v[2]; u8 sp[768]; v[0] = &im; v[1] = &out;
      if (shared_palette(v, 2, &d, sp, 0)) return 1;
      k = 0; for (i = 16; i < 256; i++) if (sp[i*3]|sp[i*3+1]|sp[i*3+2]) k++;
      printf("shared palette: %d non-black free entries\n", k); }
    /* UI pens free (Keep menu pens off): 16 colours, 3D greys at 8/9 */
    { static const char *nm[3] = { "gradient", "dark", "16-col indexed" };
      int t, ok = 1;
      for (t = 0; t < 3; t++) {
        DISPINFO du; IMAGE ti = im; u8 po[768]; DITHER *dt; int l0, l1;
        memset(&du, 0, sizeof du); du.planes = 4; du.palmode = PAL_OPTIMISE;
        du.ui_n = 4; du.ui_pix[0] = 0; du.ui_pix[1] = 15; du.ui_pix[2] = 8; du.ui_pix[3] = 9;
        if (t == 1) { ti.px = malloc(960*200); for (x = 0; x < 960*200; x++) ti.px[x] = (u8)(20 + (x % 40)); }
        if (t == 2) ti = out;      /* 16 colours: exact, all pens used */
        dt = dith_new(&ti, 320, 200, &du, DITH_FS, 1, 0, po, 0);
        if (!dt) { puts("dith_new failed"); return 1; }
        l0 = (77*po[0]+150*po[1]+29*po[2])>>8; l1 = (77*po[45]+150*po[46]+29*po[47])>>8;
        printf("ui pens %s: pen0 luma %d pen1 luma %d  grey8 %d,%d,%d\n", nm[t], l0, l1, po[24], po[25], po[26]);
        if (l0 - l1 < UI_CONTRAST) ok = 0;
        dith_free(dt);
      }
      printf("ui pens legible: %s\n", ok ? "OK" : "FAIL"); }
    (void)argc; (void)argv;
    return 0;
}
