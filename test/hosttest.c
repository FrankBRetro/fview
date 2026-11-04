/* hosttest.c - run fview's decoders, ditherer and IMG writer on a host.
 *   hosttest <file> <outprefix> [planes]
 * Writes <outprefix>_orig.ppm (decoded), <outprefix>_disp.ppm (as it
 * would appear on a display with <planes> planes), <outprefix>.img and a
 * round-trip of that IMG; documents are dumped as text.               */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/fview.h"
#include "../src/doc.h"

static void *blocks[RB_MAX];
void *res_alloc(int slot, long size) { free(blocks[slot]); return blocks[slot] = malloc(size); }
void res_free(int slot) { free(blocks[slot]); blocks[slot] = 0; }

static int prog(PROG *p, long d, long t) { (void)p; (void)d; (void)t; return 0; }

static void write_ppm(const char *fn, const IMAGE *im)
{
    FILE *f = fopen(fn, "wb");
    u8 *row = malloc(im->w * 3);
    long y;
    fprintf(f, "P6\n%ld %ld\n255\n", im->w, im->h);
    for (y = 0; y < im->h; y++) {
        img_get_rgb_row(im, y, row);
        fwrite(row, 1, im->w * 3, f);
    }
    free(row);
    fclose(f);
}

static void dump_text(const u8 *b, long len)
{
    long n = rd_u32(b + 4), i, o = 12;
    printf("flags %lu, %ld paragraphs\n", (unsigned long)rd_u32(b + 8), n);
    if (rd_u32(b + 8) & DOCF_RICH) {        /* font and colour tables */
        int nf = rd_u16(b + o), k;
        o += 2;
        for (k = 0; k < nf; k++) {
            printf("font %d: family %d '%.*s'\n", k, b[o], b[o + 1], b + o + 2);
            o += 2 + b[o + 1];
        }
        k = rd_u16(b + o);
        printf("%d colours, text width %lu twips\n", k, (unsigned long)rd_u32(b + o + 2 + k * 3));
        o += 2 + k * 3 + 4;
    }
    for (i = 0; i < n && o < len; i++) {
        unsigned l = rd_u16(b + o), k;
        int st = b[o + 2], ind = b[o + 3];
        printf("[%d/%d] ", st, ind);
        for (k = 0; k < l; k++) {
            u8 c = b[o + 4 + k];
            if (c == TA_MARK) { printf("<%d>", b[o + 4 + k + 1]); k++; }
            else if (c == FM_FONT) { printf("<f%d %d>", b[o + 5 + k], b[o + 6 + k]); k += 2; }
            else if (c == FM_COLOR) { printf("<c%d>", b[o + 5 + k]); k++; }
            else if (c >= 0x80) printf("{%02X}", c);
            else putchar(c);
        }
        putchar('\n');
        o += 4 + l;
    }
}

static void dump_pdf(const u8 *b, long len)
{
    long np = rd_u32(b + 4), o = 8, p;
    printf("%ld pages\n", np);
    for (p = 0; p < np && o < len; p++) {
        long sz = rd_u32(b + o), q = o + 4, end = o + 4 + sz, ni, i;
        printf("--- page %ld box %.1f %.1f %.1f %.1f\n", p + 1,
               rd_i32(b + q) / 16.0, rd_i32(b + q + 4) / 16.0,
               rd_i32(b + q + 8) / 16.0, rd_i32(b + q + 12) / 16.0);
        ni = rd_u32(b + q + 16);
        q += 20;
        for (i = 0; i < ni && q < end; i++) {
            int k = b[q];
            if (k == PI_TEXT) {
                unsigned n = rd_u16(b + q + 20);
                printf("T %6.1f %6.1f w%6.1f s%4.1f a%d '%.*s'\n", rd_i32(b + q + 4) / 16.0,
                       rd_i32(b + q + 8) / 16.0, rd_i32(b + q + 12) / 16.0,
                       rd_u16(b + q + 16) / 16.0, b[q + 18], (int)n, b + q + 22);
                q += 22 + n;
            } else if (k == PI_LINE || k == PI_FILL) {
                unsigned n = rd_u16(b + q + 6);
                printf("%c rgb(%d,%d,%d) %u pts (%.1f,%.1f)\n", k == PI_LINE ? 'L' : 'F',
                       b[q + 1], b[q + 2], b[q + 3], n, rd_i32(b + q + 8) / 16.0,
                       rd_i32(b + q + 12) / 16.0);
                q += 8 + n * 8;
            } else if (k == PI_IMAGE) {
                long ilen = rd_u32(b + q + 26);
                printf("I %.1f %.1f %.1f %.1f fmt %d flags %d %ux%u %ld bytes\n", rd_i32(b + q + 4) / 16.0,
                       rd_i32(b + q + 8) / 16.0, rd_i32(b + q + 12) / 16.0, rd_i32(b + q + 16) / 16.0,
                       b[q + 20], b[q + 21], rd_u16(b + q + 22), rd_u16(b + q + 24), ilen);
                q += 30 + ilen;
            } else { printf("bad item %d\n", k); break; }
        }
        o = end;
    }
}

int main(int argc, char **argv)
{
    PREFS pf;
    PROG pg;
    DISPINFO d;
    IMAGE im;
    int t, e, planes = argc > 3 ? atoi(argv[3]) : 4, i;
    char fn[512];

    prefs_default(&pf);
    memset(&pg, 0, sizeof pg);
    pg.fn = prog;
    memset(&d, 0, sizeof d);
    d.planes = planes;
    d.truecolor = planes > 8;
    /* ST-like system palette: VDI 16 colours on their pixel values */
    {
        static const u8 v16[48] = { 255,255,255, 0,0,0, 255,0,0, 0,255,0, 0,0,255, 0,255,255,
            255,255,0, 255,0,255, 192,192,192, 128,128,128, 128,0,0, 0,128,0, 0,0,128,
            0,128,128, 128,128,0, 128,0,128 };
        int n = planes >= 8 ? 256 : 1 << planes;
        for (i = 0; i < n; i++) {
            int pix = vdi2pix(i, planes);
            if (i < 16) memcpy(d.sysrgb + pix * 3, v16 + i * 3, 3);
            else d.sysrgb[pix * 3] = d.sysrgb[pix * 3 + 1] = d.sysrgb[pix * 3 + 2] = i;
            if (i < (planes >= 8 ? 16 : 2)) d.reserved[pix] = 1;
        }
    }
    if (d.truecolor) {   /* RGB565 */
        d.bytespp = 2;
        d.nbits[0] = 5; d.nbits[1] = 6; d.nbits[2] = 5;
        for (i = 0; i < 5; i++) d.bitpos[0][i] = 15 - i, d.bitpos[2][i] = 4 - i;
        for (i = 0; i < 6; i++) d.bitpos[1][i] = 10 - i;
    }

    t = ftype_detect(argv[1]);
    printf("type: %s\n", ftype_name(t));
    img_init(&im);
    switch (t) {
    case FT_GIF:  e = dec_gif(argv[1], &im, &pf, &pg); break;
    case FT_IFF:  e = dec_iff(argv[1], &im, &pf, &pg); break;
    case FT_BMP:  e = dec_bmp(argv[1], &im, &pf, &pg); break;
    case FT_IMG:  e = dec_img(argv[1], &im, &pf, &d, &pg); break;
    case FT_PNG: case FT_JPEG: e = dec_stb(argv[1], &im, &pf, &d, &pg); break;
    case FT_DEGAS: case FT_DEGASC: case FT_NEO: case FT_SPU: case FT_SPC: case FT_SPS:
    case FT_PCS: case FT_PBX: e = dec_st(argv[1], t, &im, &pf, &pg); break;
    case FT_GEM: {
        u8 *b; long len;
        e = dec_gem(argv[1], &pf, &pg, &b, &len);
        printf("result %s %s\n", fv_errstr(e), fv_errdetail);
        if (!e) dump_pdf(b, len);
        return e != 0;
    }
    case FT_PDF: {
        u8 *b; long len;
        e = dec_pdf(argv[1], &pf, &pg, &b, &len);
        printf("result %s %s\n", fv_errstr(e), fv_errdetail);
        if (!e) dump_pdf(b, len);
        return e != 0;
    }
    default: {
        u8 *b; long len;
        e = dec_text(argv[1], t, &pf, &pg, &b, &len);
        printf("result %s\n", fv_errstr(e));
        if (!e) dump_text(b, len);
        return e != 0;
    }
    }
    printf("result %s %s\n", fv_errstr(e), fv_errdetail);
    if (e) return 1;
    printf("%ldx%ld %s ncol %d bits %d trans %d  [%s]\n", im.w, im.h,
           im.type == IT_RGB ? "rgb" : "indexed", im.ncolors, im.orig_bits, im.trans, im.fmtinfo);
    sprintf(fn, "%s_orig.ppm", argv[2]);
    write_ppm(fn, &im);

    /* render for display, scaled to fit 320x200 when argv[4] given */
    {
        u8 pal[768], *buf;
        long wdw, tw = im.w, th = im.h, y, x;
        if (argc > 4) img_fit(im.w, im.h, atol(argv[4]), atol(argv[4]) * 5 / 8, &tw, &th);
        e = render_display(&im, tw, th, &d, &pf, pal, &buf, &wdw, &pg);
        printf("render %s %ldx%ld\n", fv_errstr(e), tw, th);
        if (!e) {
            FILE *f;
            sprintf(fn, "%s_disp.ppm", argv[2]);
            f = fopen(fn, "wb");
            fprintf(f, "P6\n%ld %ld\n255\n", tw, th);
            for (y = 0; y < th; y++)
                for (x = 0; x < tw; x++) {
                    if (d.truecolor) {
                        u8 *p = buf + (y * wdw * 16 + x) * 2;
                        unsigned v = (p[0] << 8) | p[1];
                        fputc(((v >> 11) & 31) << 3, f); fputc(((v >> 5) & 63) << 2, f); fputc((v & 31) << 3, f);
                    } else {
                        int p, v = 0;
                        long ps;
                        u16 *s = std_row((u16 *)buf, y, th, wdw, planes, &ps);
                        for (p = 0; p < planes; p++)
                            if (s[p * ps + x / 16] & (0x8000 >> (x & 15))) v |= 1 << p;
                        fwrite(pal + v * 3, 1, 3, f);
                    }
                }
            fclose(f);
            if (!d.truecolor) {
                sprintf(fn, "%s_disp.img", argv[2]);
                e = img_save_planes(fn, (u16 *)buf, tw, th, wdw, planes, pal, &pf, &pg);
                printf("save dithered: %s\n", fv_errstr(e));
            }
        }
    }
    sprintf(fn, "%s.img", argv[2]);
    e = img_save(fn, &im, &pf, &pg);
    printf("save original: %s\n", fv_errstr(e));
    {
        IMAGE im2;
        long y;
        u8 *a = malloc(im.w * 3), *b = malloc(im.w * 3);
        long diff = 0;
        IMAGE keep = im;
        keep.px = malloc(im.stride * im.h);
        memcpy(keep.px, im.px, im.stride * im.h);
        img_init(&im2);
        e = dec_img(fn, &im2, &pf, &d, &pg);
        printf("reload img: %s %ldx%ld bits %d\n", fv_errstr(e), im2.w, im2.h, im2.orig_bits);
        for (y = 0; !e && y < im.h; y++) {
            img_get_rgb_row(&keep, y, a);
            img_get_rgb_row(&im2, y, b);
            for (i = 0; i < im.w * 3; i++) diff += abs(a[i] - b[i]) > 4;
        }
        printf("round trip differing samples: %ld\n", diff);
    }
    return 0;
}

void dbg(const char *fmt, ...) { (void)fmt; }
