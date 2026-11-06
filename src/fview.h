/*
 * fview - GEM file viewer for Atari TOS / MultiTOS / MiNT
 *
 * fview.h - shared types. This header is GEM-free so that the decoders,
 * quantiser and ditherer can be built and tested on any host.
 */
#ifndef FVIEW_H
#define FVIEW_H

#include <stddef.h>

#define FV_VERSION "1.7.0"

/* the CPU this build is for, from the compiler's own macros */
#if defined(__mcoldfire__)
#define FV_CPU "ColdFire"
#elif defined(__mc68020__) && defined(__mc68060__)
#define FV_CPU "68020-68060"
#elif defined(__mc68060__)
#define FV_CPU "68060"
#elif defined(__mc68040__)
#define FV_CPU "68040"
#elif defined(__mc68030__) && defined(__HAVE_68881__)
#define FV_CPU "68030 + FPU"
#elif defined(__mc68030__)
#define FV_CPU "68030"
#elif defined(__mc68020__)
#define FV_CPU "68020"
#else
#define FV_CPU "68000"
#endif

/* ---------------------------------------------------------------- types */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;
typedef long           i32;

/* file types */
enum {
    FT_UNKNOWN = 0,
    FT_GIF, FT_PNG, FT_IFF, FT_IMG, FT_JPEG, FT_BMP, /* graphics */
    FT_DEGAS, FT_DEGASC, FT_NEO, FT_SPU, FT_SPC, FT_SPS, FT_PCS, FT_PBX,
    FT_TEXT, FT_UTEXT, FT_MARKDOWN, FT_RTF,      /* documents */
    FT_PDF, FT_GEM                               /* pages */
};
#define FT_IS_GRAPHIC(t) ((t) >= FT_GIF && (t) <= FT_PBX)
#define FT_IS_ST(t)      ((t) >= FT_DEGAS && (t) <= FT_PBX)
#define FT_IS_PAGE(t)    ((t) == FT_PDF || (t) == FT_GEM)
#define FT_IS_DOC(t)     ((t) >= FT_TEXT && (t) <= FT_RTF)

/* image kinds */
enum { IT_INDEXED = 0, IT_RGB = 1 };

typedef struct {
    long w, h;
    int  type;            /* IT_INDEXED or IT_RGB                          */
    int  orig_bits;       /* bits per pixel of the source file (1..24)     */
    int  ncolors;         /* palette entries in use (indexed)              */
    int  trans;           /* transparent palette index or -1               */
    u8   pal[256 * 3];    /* palette for indexed images                    */
    u8  *px;              /* pixels: 1 byte (indexed) or 3 bytes (RGB)     */
    long stride;          /* bytes per row                                 */
    char fmtinfo[48];     /* short human description e.g. "GIF 89a, 8 bit" */
    int  aspx, aspy;      /* pixel shape width:height, 0 = square/unknown  */
} IMAGE;

/* ---------------------------------------------------------- preferences */

enum { DITH_FS = 0, DITH_ATKINSON, DITH_ORDERED, DITH_NONE };
enum { ZOOM_REAL = 0, ZOOM_FIT };
enum { BG_WHITE = 0, BG_GREY, BG_BLACK };
enum { CS_ATARI = 0, CS_LATIN1, CS_CP1252 };
enum { PAL_OPTIMISE = 0, PAL_SYSTEM };
enum { PDFZ_FITW = 0, PDFZ_100 };
enum { PRS_FIT = 0, PRS_REAL };

typedef struct {
    /* general */
    char start_iconified;
    char use_workers;       /* MiNT: one niced worker per window         */
    /* graphics */
    char dither;            /* DITH_*                                    */
    char serpentine;
    char zoom;              /* default zoom for new windows              */
    char smooth;            /* box-filter when shrinking                 */
    char palmode;           /* PAL_OPTIMISE / PAL_SYSTEM                 */
    char keep_sys;          /* keep GEM UI colours when optimising       */
    char ui_pens;           /* never give pens 0/1 (and 3D greys) to a
                               picture; off: a picture may use them, its
                               lightest/darkest colours go there          */
    char bg;                /* transparency / alpha background           */
    /* gif */
    char gif_trans;         /* honour transparency                        */
    /* png */
    char png_alpha;         /* composite alpha onto background            */
    /* iff */
    char iff_mask;          /* honour mask plane / transparent colour     */
    char iff_ham;           /* decode HAM/EHB (else show raw planes)      */
    /* img */
    char img_syspal;        /* use system palette when no XIMG palette    */
    char img_vdiorder;      /* XIMG palette in VDI pen order (standard)   */
    /* jpeg */
    char jpg_grey_mono;     /* decode as greyscale on mono displays       */
    /* text */
    char tabw;              /* 4 or 8                                     */
    char wrap;              /* wrap long lines in plain text              */
    char charset;           /* 8-bit text charset (CS_*)                  */
    /* markdown */
    char md_markup;         /* show markup characters                     */
    /* rtf */
    char rtf_pict;          /* show [picture] placeholders                */
    /* pdf */
    char pdf_zoom;          /* PDFZ_*                                     */
    char pdf_gfx;           /* draw lines, rectangles and fills           */
    char pdf_imgbox;        /* draw placeholder boxes for images          */
    /* printing */
    char prt_scale;         /* PRS_*                                      */
    char prt_dither;        /* DITH_* for printer output                  */
    char prt_header;        /* print file name / page number header       */
    char dither_pal;        /* dither paletted pictures on 256+ colour
                               screens too (off: their colours are mapped
                               straight, no dither pattern)               */
} PREFS;

void prefs_default(PREFS *p);

/* ----------------------------------------------------- display description */

/* Everything the worker needs to know about the screen, so it can run
 * without touching the VDI. Palette displays get pixels written as hardware
 * values in VDI standard format; true colour displays get packed pixels.  */
typedef struct {
    int  planes;            /* 1,2,4,8,15,16,24,32                        */
    int  truecolor;         /* planes > 8                                 */
    int  palmode;           /* PAL_* (forced PAL_SYSTEM if no palette)    */
    int  fixed_palette;     /* hardware palette cannot be changed         */
    u8   sysrgb[256 * 3];   /* current palette, indexed by pixel value    */
    u8   reserved[256];     /* 1 = pixel value may not be redefined       */
    int  ui_n;              /* pixel values of the UI pens 0, 1, 8, 9 that */
    int  ui_pix[4];         /* a picture may take: its lightest and darkest
                               colours (and greys) go there so menus stay
                               readable (0 = none)                        */
    /* true colour packing: bit numbers in the pixel for each channel,
       most significant first, as reported by vq_scrninfo                 */
    int  bytespp;           /* 2, 3 or 4                                  */
    int  nbits[3];
    int  bitpos[3][16];
    int  intel;             /* little-endian pixels                       */
    int  pix_w, pix_h;      /* screen pixel size in micrometres            */
    int  chunky;            /* palette screen with one byte per pixel:
                               pictures are written as bytes, no vr_trnfm */
} DISPINFO;

void disp_make_tc_luts(const DISPINFO *d, u32 lut[3][256]);

/* ------------------------------------------------------------- progress */

/* Progress / cancel hook used by every long-running routine.
 * Returns non-zero when the user cancelled.                              */
typedef struct PROG {
    int  (*fn)(struct PROG *p, long done, long total);
    void *data;
    int  cancelled;
    int  phase;             /* 0 = first half, 1 = second half            */
    int  halves;            /* progress is reported as two halves         */
    int  phases;            /* > 1: progress in that many equal parts,
                               phase 0..phases-1 (overrides halves)       */
} PROG;

int prog_step(PROG *p, long done, long total);

/* ---------------------------------------------------- result allocation */

/* Large result buffers come from here so that a worker process can hand
 * them to the GUI process (shared memory under MiNT, plain Malloc else). */
void *res_alloc(int slot, long size);
void  res_free(int slot);
enum { RB_ORIG = 0, RB_DISP, RB_DOC, RB_MAX };

/* --------------------------------------------------------------- errors */

#define E_OK        0
#define E_OPEN     -1
#define E_FORMAT   -2
#define E_NOMEM    -3
#define E_CANCEL   -4
#define E_UNSUP    -5
#define E_WRITE    -6

const char *fv_errstr(int e);
extern char fv_errdetail[80];

/* ------------------------------------------------------------ detection */

int  ftype_detect(const char *path);
const char *ftype_name(int t);

/* ------------------------------------------------------------- decoders */

int dec_gif (const char *path, IMAGE *im, const PREFS *pf, PROG *pg);
int dec_iff (const char *path, IMAGE *im, const PREFS *pf, PROG *pg);
int dec_img (const char *path, IMAGE *im, const PREFS *pf, const DISPINFO *d, PROG *pg);
int dec_stb (const char *path, IMAGE *im, const PREFS *pf, const DISPINFO *d, PROG *pg);
/* Windows / OS/2 BMP: 1-32 bit, RLE4/RLE8, bit fields */
int dec_bmp (const char *path, IMAGE *im, const PREFS *pf, PROG *pg);
/* Atari ST picture formats: Degas, NEOchrome, Spectrum 512, Photochrome,
   QuantumPaint */
int dec_st  (const char *path, int ftype, IMAGE *im, const PREFS *pf, PROG *pg);

/* documents produce a "doc blob" - see doc.h */
int dec_text(const char *path, int ftype, const PREFS *pf, PROG *pg,
             u8 **blob, long *len);
int dec_pdf (const char *path, const PREFS *pf, PROG *pg, u8 **blob, long *len);
/* GEM metafile -> one page in the PDF display list format */
int dec_gem (const char *path, const PREFS *pf, PROG *pg, u8 **blob, long *len);

/* ---------------------------------------------------------- image utils */

void img_init(IMAGE *im);
int  img_alloc_px(IMAGE *im, int slot);        /* uses res_alloc         */
void img_reduce(IMAGE *im, int slot);           /* RGB -> indexed if <=256 */
void img_compact(IMAGE *im);                    /* drop unused colours     */
void img_get_rgb_row(const IMAGE *im, long y, u8 *out);   /* w*3 bytes   */
void img_fit(long sw, long sh, long bw, long bh, long *tw, long *th);
/* real size on this screen: stretched (never shrunk) so non-square source
   pixels (Amiga lores interlace, IMG pixel sizes) keep their proportions */
void img_display_size(const IMAGE *im, const DISPINFO *d, long *w, long *h);

/* ---------------------------------------------------- converting (FConv) */

/* export formats: all write GEM IMG / XIMG */
enum { XF_ORIGINAL = 0,  /* the picture's own depth and palette           */
       XF_TRUE,          /* 24-bit true colour                            */
       XF_SCREEN,        /* dithered to the screen's colours (true colour
                            screens: 24-bit)                              */
       XF_MONO };        /* dithered to black and white                   */

/* Decode one picture and write it as an IMG at out. maxw/maxh > 0 shrink
 * bigger pictures to fit (not for XF_ORIGINAL). d describes the screen
 * for XF_SCREEN. Progress: phase 0 reading, 1 converting, 2 writing.    */
int convert_picture(const char *path, int ftype, const char *out, int xfmt,
                    int pdf, long maxw, long maxh, const PREFS *pf,
                    const DISPINFO *d, PROG *pg);
/* one-page PDF writer (pdf_save.c): rows of packed pixels, top first */
typedef struct PDFW PDFW;
PDFW *pdfw_open(const char *path, long w, long h, int ncomp, int bpc,
                const u8 *pal, int npal, int aspx, int aspy, int *err);
long  pdfw_rowbytes(PDFW *p);
int   pdfw_row(PDFW *p, const u8 *row);
int   pdfw_close(PDFW *p, const char *path, int abort);
/* decode any supported picture into im (pixels in RB_ORIG)              */
int decode_picture(const char *path, int ftype, IMAGE *im, const PREFS *pf,
                   const DISPINFO *d, PROG *pg);
/* the pixel shape written into the next IMG header (0, 0 = square)      */
void img_set_pixel_shape(int aspx, int aspy);

/* ------------------------------------------------- quantise and dither */

typedef struct DITHER DITHER;

/* Prepare a ditherer that scales im to tw x th and maps it onto display d.
 * pal_out (pixel value indexed, 256*3) receives the palette the window
 * should set when it is topped (palette displays).                       */
DITHER *dith_new(const IMAGE *im, long tw, long th, const DISPINFO *d,
                 int method, int serpentine, int smooth, u8 *pal_out,
                 PROG *pg);
/* produce row y: palette displays -> 1 byte pixel value per pixel,
   true colour -> packed device pixels (bytespp each)                     */
void dith_row(DITHER *dt, long y, u8 *out);
void dith_free(DITHER *dt);
/* UI pens (DISPINFO ui_pix): white and black are added when the colours
   have less light/dark contrast than this; slot[] gets the pixel value
   of each colour, the UI pens getting the lightest, darkest and greys */
#define UI_CONTRAST 128
int  ui_needs_bw(const DISPINFO *d, const u8 *cols, int got);
void ui_arrange(const DISPINFO *d, int ncol, const u8 *cols, int got, int *slot);

/* render a whole image into VDI standard format (palette) or packed device
 * format (true colour). Buffer comes from res_alloc(RB_DISP).            */
int render_display(const IMAGE *im, long tw, long th, const DISPINFO *d,
                   const PREFS *pf, u8 *pal_out, u8 **buf, long *wdwidth,
                   PROG *pg);

/* Palette-display render buffers use "banded standard format": the image
 * is cut into bands of STD_BAND rows and each band is a complete VDI
 * standard-format raster (plane after plane). The GUI can then vr_trnfm()
 * one band at a time instead of needing two full-size copies.          */
#define STD_BAND 16
u16 *std_row(u16 *buf, long y, long h, long wdwidth, int planes, long *psize);

/* chunky row of pixel values -> standard-format plane words              */
void chunky_to_planes(const u8 *src, long w, int planes, u16 *dst,
                      long planesize_words, long wdwidth);

/* ----------------------------------------------------------- IMG output */

int  img_save(const char *path, const IMAGE *im, const PREFS *pf, PROG *pg);
/* save from a standard-format plane buffer (palette by pixel value) */
int  img_save_planes(const char *path, const u16 *std, long w, long h,
                     long wdwidth, int planes, const u8 *pal_by_pix,
                     const PREFS *pf, PROG *pg);
int  img_save_rgbrows(const char *path, long w, long h,
                      void (*getrow)(void *ctx, long y, u8 *rgb), void *ctx,
                      PROG *pg);

/* VDI pen <-> pixel value for the standard Atari palette layouts */
int  vdi2pix(int pen, int planes);
int  pix2vdi(int pix, int planes);

/* ---------------------------------------------------------- charset */

int  uni_to_atari(long cp);                    /* -1 if no mapping        */
int  byte_to_atari(int b, int charset);
void atari_translit(long cp, char *out);       /* best-effort fallback    */

#endif
