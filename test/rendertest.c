/* rendertest.c - lay out / draw a document through render.c into a VDI
 * call log:  rendertest <file> <width> [page]                         */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/gemui.h"

extern FILE *vlog;
static void *blocks[RB_MAX];
void *res_alloc(int slot, long size) { free(blocks[slot]); return blocks[slot] = malloc(size); }
void res_free(int slot) { free(blocks[slot]); blocks[slot] = 0; }

int main(int argc, char **argv)
{
    PREFS pf;
    u8 *b;
    long len;
    int t = ftype_detect(argv[1]), width = atoi(argv[2]), e;
    GRECT clip = { 0, 0, 2000, 3000 };
    prefs_default(&pf);
    prefs = pf;
    vlog = stdout;
    {   /* a 16 colour ST-like display, pixel 0 white, 15 black */
        extern DISPINFO dinfo;
        static const u8 pal16[48] = { 255,255,255, 255,0,0, 0,255,0, 255,255,0, 0,0,255, 255,0,255,
            0,255,255, 170,170,170, 85,85,85, 255,128,128, 128,255,128, 255,255,128, 128,128,255,
            255,128,255, 128,255,255, 0,0,0 };
        memset(&dinfo, 0, sizeof dinfo);
        dinfo.planes = 4;
        memcpy(dinfo.sysrgb, pal16, 48);
    }
    {
        static FONTSET fs;
        fonts_load(&fs, 1, 1);
    }
    if (t == FT_PDF || t == FT_GEM) {
        long *offs, n, pw, ph, x0, y0, sx, sy, page = argc > 3 ? atol(argv[3]) : 0;
        e = t == FT_GEM ? dec_gem(argv[1], &pf, 0, &b, &len) : dec_pdf(argv[1], &pf, 0, &b, &len);
        if (e) return 1;
        n = pdf_index(b, len, &offs);
        pdf_page_size(b, offs[page], &pw, &ph, &x0, &y0);
        sx = (long)width * 16000L / pw;
        sy = sx;
        printf("P %d %ld\n", width, ph * sy / 16000L);
        pdf_prepare_page(1, b, offs[page], 0, 0, sx, sy, 16, argc > 4, &clip);
        pdf_draw_page(1, b, offs[page], 0, 0, sx, sy, 16, argc > 4, &clip);
        fprintf(stderr, "%ld pages\n", n);
    } else {
        TXTDEV td;
        TLINE *lines;
        long n, i;
        e = dec_text(argv[1], t, &pf, 0, &b, &len);
        if (e) return 1;
        txt_measure(&td, 1, 13, 16, 278);
        n = txt_layout(b, len, &td, width, &lines, 0);
        printf("P %d %ld\n", width, lines[n - 1].y + lines[n - 1].h + 16);
        fprintf(stderr, "%ld lines, rich %d\n", n, lines[0].rich);
        for (i = 0; i < n; i++)
            txt_draw_line(&td, b, &lines[i], MARGIN, MARGIN + (int)lines[i].y, width);
    }
    return 0;
}
