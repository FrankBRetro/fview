/* palette.h - picture tools (palette.c): palette files, colour reduction,
 * remapping, resizing, colour counts, shared palettes. GEM-free.       */
#ifndef PALETTE_H
#define PALETTE_H
#include "fview.h"

/* palette files: JASC-PAL (.PAL), GIMP (.GPL), Adobe colour table (.ACT),
   raw RGB triples, Atari hardware words (16 x 0RGB), and the palette of
   Degas and NEOchrome pictures. Saving picks the
   format by extension: .ACT, .GPL, anything else JASC-PAL.            */
int  pal_load_file(const char *path, u8 *pal, int *n);
int  pal_save_file(const char *path, const u8 *pal, int n);

/* 15-bit histogram of a picture (sampled on big ones) */
int  quant_hist_add(u32 *hist, const IMAGE *im, PROG *pg, long base, long total);
/* median cut: up to want colours from a histogram; count, -1 on failure */
int  quant_from_hist(const u32 *hist, int want, u8 *out, PROG *pg);
/* the best want colours for a picture (its own if they already fit)   */
int  quant_palette(const IMAGE *im, int want, u8 *pal, PROG *pg);

/* new pictures in RB_ORIG, with the header in dst (src stays as it is):
   map onto the n colours of pal with pf's dithering                   */
int  img_remap(const IMAGE *src, IMAGE *dst, const u8 *pal, int n,
               const PREFS *pf, PROG *pg);
/* the picture's best n colours, then remapped onto them               */
int  img_reduce_colours(const IMAGE *src, IMAGE *dst, int n, const PREFS *pf,
                        PROG *pg);
/* nw x nh pixels; paletted pictures are scaled by nearest pixel, true
   colour ones averaged (smaller) or blended (larger) when smooth      */
int  img_resize(const IMAGE *src, IMAGE *dst, long nw, long nh, int smooth,
                PROG *pg);

/* different 24-bit colours (-1 cancelled, -2 no memory); for paletted
   pictures *used_entries gets the palette entries the pixels use      */
long img_count_real(const IMAGE *im, int *used_entries, PROG *pg);
/* colours seen when shown tw x th on display d with pf's dithering    */
long img_count_seen(const IMAGE *im, long tw, long th, const DISPINFO *d,
                    const PREFS *pf, PROG *pg);

/* one palette (by pixel value) for several pictures on display d      */
int  shared_palette(const IMAGE *const *ims, int n, const DISPINFO *d,
                    u8 *pal_by_pix, PROG *pg);

#endif
