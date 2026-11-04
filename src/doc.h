/*
 * doc.h - serialised document formats shared by worker and GUI.
 *
 * All multi-byte values are written byte by byte (big endian) so records
 * never need to be word aligned on the 68000.
 *
 * Text blob ("FVTX"):
 *   "FVTX" u32 nparas u32 flags
 *   [DOCF_RICH only: u16 nfonts, per font u8 family (FF_*), u8 n, n bytes
 *    name; u16 ncolors, ncolors * rgb; u32 text width in twips (0 = any)]
 *   para*: u16 len, u8 style (PS_* | align << 4), u8 indent, len bytes of
 *          Atari text with inline escapes:
 *          0x01 <attr>             attributes, TA_* bits
 *          0x02 <font> <halfpts>   font table index and size (rich only)
 *          0x03 <colour>           colour table index (rich only)
 *
 * PDF blob ("FVPD"):
 *   "FVPD" u32 npages
 *   page*: u32 bytes (of the rest of this page record),
 *          i32 x0 y0 x1 y1 (media box, 1/16 pt), u32 nitems, items
 *   item:  u8 kind, u8 r, u8 g, u8 b, ...
 *     PI_TEXT : i32 x, i32 y (1/16 pt, baseline), i32 width (1/16 pt),
 *               u16 size (1/16 pt), u8 attr (TA_* | family << 4), u8 pad,
 *               u16 n, n bytes Atari text
 *     PI_LINE / PI_FILL : u16 width (1/16 pt), u16 npts, npts*(i32 x,i32 y)
 *               (PI_FILL: width 0x8000 | interior << 8 | style = VDI pattern)
 *     PI_TEXT pad byte bit 0: natural width (draw unstretched)
 *     PI_IMAGE: i32 x y w h (page space bounding box),
 *               u8 fmt (PIF_*; PIF_NONE = placeholder box only),
 *               u8 flags (PIFL_*), u16 iw, u16 ih, u32 len, len bytes:
 *               PIF_MONO rows of (iw+7)/8 bytes, 1 = ink; PIF_GREY 1 byte
 *               per pixel; PIF_RGB 3 bytes per pixel. Row 0 is the top.
 */
#ifndef DOC_H
#define DOC_H

#include "fview.h"

#define DOCF_NOWRAP 1          /* plain text: keep lines as they are      */
#define DOCF_PDF    2
#define DOCF_RICH   4          /* fonts, sizes, colours, alignment (RTF) */

#define FM_FONT  0x02
#define FM_COLOR 0x03
enum { AL_LEFT = 0, AL_RIGHT, AL_CENTER, AL_JUST };
enum { FF_ANY = 0, FF_SERIF, FF_SANS, FF_MONO, FF_SCRIPT, FF_DECOR };

/* paragraph styles */
enum { PS_NORMAL = 0, PS_H1, PS_H2, PS_H3, PS_PRE, PS_QUOTE, PS_BULLET,
       PS_RULE, PS_BLANK };

/* inline attributes (match VDI vst_effects bits) */
#define TA_BOLD    1
#define TA_LIGHT   2
#define TA_ITALIC  4
#define TA_UNDER   8
#define TA_MARK    0x01        /* escape byte in paragraph text          */

enum { PI_TEXT = 1, PI_LINE, PI_FILL, PI_IMAGE };
enum { PIF_NONE = 0, PIF_MONO, PIF_GREY, PIF_RGB };
#define PIFL_MASK  1             /* stencil: only ink pixels are painted */
#define PIFL_FLIPX 2
#define PIFL_FLIPY 4

/* --------------------------------------------------- growable byte buffer */
typedef struct {
    u8  *p;
    long len, cap;
    int  err;
} BUF;

void buf_init(BUF *b);
void buf_free(BUF *b);
void buf_put(BUF *b, const void *d, long n);
void buf_u8 (BUF *b, int v);
void buf_u16(BUF *b, unsigned v);
void buf_u32(BUF *b, u32 v);
void buf_set_u32(BUF *b, long at, u32 v);
/* move the finished buffer into a result block */
int  buf_finish(BUF *b, int slot, u8 **out, long *len);

static inline u32 rd_u32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}
static inline unsigned rd_u16(const u8 *p) {
    return ((unsigned)p[0] << 8) | p[1];
}
static inline i32 rd_i32(const u8 *p) { return (i32)rd_u32(p); }

#endif
