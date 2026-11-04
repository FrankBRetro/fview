/* convtest.c - host test of FConv's conversion core:
 *   convtest in.pic out.img xfmt [maxw maxh [planes]]
 * xfmt 0 original, 1 true colour, 2 screen (planes: 1/2/4/8, 16 = true
 * colour; default 4), 3 mono. Then reads out.img back and writes
 * out.img.ppm so the result can be looked at.                         */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/fview.h"

static void *blocks[RB_MAX];
void *res_alloc(int slot, long size) { free(blocks[slot]); return blocks[slot] = malloc(size); }
void res_free(int slot) { free(blocks[slot]); blocks[slot] = 0; }
void dbg(const char *fmt, ...) { (void)fmt; }

static int prog(PROG *p, long done, long total) { (void)p; (void)done; (void)total; return 0; }

int main(int argc, char **argv)
{
    PREFS pf;
    DISPINFO d;
    PROG pg;
    IMAGE im;
    int e, xf, planes = argc > 6 ? atoi(argv[6]) : 4, i;
    long mw = argc > 5 ? atol(argv[4]) : 0, mh = argc > 5 ? atol(argv[5]) : 0;
    char fn[300];
    FILE *f;
    if (argc < 4) return 2;
    xf = atoi(argv[3]);
    prefs_default(&pf);
    memset(&d, 0, sizeof d);
    memset(&pg, 0, sizeof pg);
    pg.fn = prog;
    if (planes > 8) {
        d.truecolor = 1; d.planes = planes; d.bytespp = 2;
    } else {
        /* a palette screen: the standard VDI colours by pixel value */
        static const u8 st16[16][3] = { {255,255,255},{255,0,0},{0,255,0},{255,255,0},
            {0,0,255},{255,0,255},{0,255,255},{170,170,170},{85,85,85},{255,85,85},
            {85,255,85},{255,255,85},{85,85,255},{255,85,255},{85,255,255},{0,0,0} };
        d.planes = planes;
        for (i = 0; i < 256; i++) {
            const u8 *c = st16[i & 15];
            if (planes == 1) c = i ? st16[15] : st16[0];
            memcpy(d.sysrgb + i * 3, c, 3);
        }
        d.fixed_palette = planes == 1;
    }
    {
        size_t n = strlen(argv[2]);
        int pdf = n > 4 && !strcmp(argv[2] + n - 4, ".pdf");
        e = convert_picture(argv[1], ftype_detect(argv[1]), argv[2], xf, pdf, mw, mh,
                            &pf, &d, &pg);
        printf("convert: %s %s\n", fv_errstr(e), fv_errdetail);
        if (e) return 1;
        if (pdf) return 0;               /* checked with pdftoppm instead */
    }
    img_init(&im);
    e = dec_img(argv[2], &im, &pf, 0, &pg);
    printf("read back: %s, %ldx%ld, %s, aspect %d:%d\n", fv_errstr(e), im.w, im.h,
           im.fmtinfo, im.aspx, im.aspy);
    if (e) return 1;
    sprintf(fn, "%s.ppm", argv[2]);
    f = fopen(fn, "wb");
    fprintf(f, "P6\n%ld %ld\n255\n", im.w, im.h);
    {
        u8 *row = malloc(im.w * 3);
        long y;
        for (y = 0; y < im.h; y++) {
            img_get_rgb_row(&im, y, row);
            fwrite(row, 3, im.w, f);
        }
        free(row);
    }
    fclose(f);
    return 0;
}
