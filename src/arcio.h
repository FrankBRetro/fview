/*
 * arcio.h - shared pieces of FArchive's archive code: byte streams,
 * CRCs, bit readers/writers, canonical Huffman codes and the codecs.
 * No GEM or MiNT in here, so it builds and is tested on any host.
 */
#ifndef ARCIO_H
#define ARCIO_H

#include <stdio.h>
#include "fview.h"

/* ------------------------------------------------------------ streams */

/* byte input: a file (optionally limited to a number of bytes) or a
   function (another decoder's output, e.g. gzip under tar)          */
typedef struct BIN {
    FILE *f;
    long (*fn)(void *ctx, u8 *buf, long n);
    void *ctx;
    long  left;                  /* bytes still allowed, -1 = no limit */
    u8    buf[4096];
    int   pos, len;
    long  total;                 /* bytes consumed                    */
    int   eof;
} BIN;

void bin_file(BIN *b, FILE *f, long limit);
void bin_func(BIN *b, long (*fn)(void *, u8 *, long), void *ctx);
int  bin_byte(BIN *b);                         /* -1 at the end      */
long bin_read(BIN *b, void *d, long n);
long bin_skip(BIN *b, long n);

/* byte output: a file, a function, or nothing (counting only); keeps
   the CRCs of what passed through                                    */
typedef struct BOUT {
    FILE *f;
    int (*fn)(void *ctx, const u8 *buf, long n);
    void *ctx;
    long  count;
    u32   crc32;                 /* running, not yet inverted          */
    unsigned crc16;
    int   err;
    u8    buf[4096];
    int   pos;
} BOUT;

void bout_file(BOUT *o, FILE *f);
void bout_func(BOUT *o, int (*fn)(void *, const u8 *, long), void *ctx);
void bout_put(BOUT *o, const u8 *d, long n);
void bout_byte(BOUT *o, int c);
int  bout_flush(BOUT *o);                      /* E_OK or E_WRITE    */
u32  bout_crc32(const BOUT *o);                /* final CRC-32       */

/* ---------------------------------------------------------------- CRCs */

u32      crc32_update(u32 crc, const u8 *d, long n);   /* start 0xFFFFFFFF */
unsigned crc16_update(unsigned crc, const u8 *d, long n); /* ARC/LHA, start 0 */

/* ----------------------------------------------------------- bit input */

typedef struct {
    BIN *in;
    u32  buf;
    int  cnt;
    int  msb;                    /* 1: msb first; 2: LZX 16 bit words  */
    int  over;                   /* bits read past the end             */
} BITS;

void     bits_init(BITS *b, BIN *in, int msb);
unsigned bits_peek(BITS *b, int n);            /* n <= 16             */
void     bits_drop(BITS *b, int n);
unsigned bits_get(BITS *b, int n);             /* n <= 16             */
void     bits_align(BITS *b);                  /* to a byte boundary  */

/* ---------------------------------------------------------- bit output */

typedef struct {
    BOUT *out;
    u32   buf;
    int   cnt;
    int   msb;
} BITW;

void bitw_init(BITW *w, BOUT *out, int msb);
void bitw_put(BITW *w, unsigned v, int n);      /* n <= 16            */
void bitw_flush(BITW *w);                       /* pad to a byte      */

/* ------------------------------------------------- canonical Huffman */

#define HUFF_FAST 9
typedef struct {
    int  n;                      /* symbols                            */
    u16  fast[1 << HUFF_FAST];   /* (len << 11) | sym, 0 = slow path   */
    u16  count[17];
    u16  sym[1024];              /* symbols by code                    */
    int  ok;
} HUFF;

/* lengths 0..16; msb says how the codes come out of the bit reader
   (deflate: lsb, LHA/LZX: msb). Returns 0 for an over-full set.     */
int  huff_build(HUFF *h, const u8 *lens, int n, int msb);
int  huff_decode(HUFF *h, BITS *b);           /* -1 on a bad code    */
/* code lengths for freq[] with no length above maxlen */
void huff_lengths(const u32 *freq, int n, int maxlen, u8 *lens);
/* codes from lengths (canonical, most significant bit first)        */
void huff_codes(const u8 *lens, int n, u16 *codes);

/* -------------------------------------------------------------- codecs */

enum {
    CM_STORE = 0,
    CM_DEFLATE,                  /* raw deflate (ZIP, GZ)              */
    CM_LH1,                      /* LHA -lh1-: 4K, dynamic Huffman     */
    CM_LH4, CM_LH5, CM_LH6, CM_LH7,  /* static Huffman, 4K..64K       */
    CM_LZS, CM_LZ5,              /* LArc                               */
    CM_ZOOLZW,                   /* ZOO method 1                       */
    CM_ARCPACK,                  /* ARC 3: RLE                         */
    CM_ARCSQUEEZE,               /* ARC 4: RLE + Huffman tree          */
    CM_ARCCRUNCH,                /* ARC 8: RLE + LZW 9..12 bits        */
    CM_ARCSQUASH,                /* ARC 9: LZW 9..13 bits              */
    CM_LZX,                      /* Amiga LZX                          */
    CM_ARCOLD5, CM_ARCOLD6, CM_ARCOLD7  /* ARC 5..7: static LZW (+RLE)   */
};

/* decode exactly usize bytes (or until the input ends for usize < 0) */
int unpack(int method, BIN *in, BOUT *out, long usize, PROG *pg);
/* encode all of in */
int pack(int method, BIN *in, BOUT *out, long usize, PROG *pg);

/* the individual codecs (unpack/pack pick one) */
int inflate_stream(BIN *in, BOUT *out, long usize, PROG *pg);
int deflate_stream(BIN *in, BOUT *out, long usize, PROG *pg);
int lzh_unpack(int method, BIN *in, BOUT *out, long usize, PROG *pg);
int lzh_pack(BIN *in, BOUT *out, long usize, PROG *pg);           /* lh5 */
int zoolzw_unpack(BIN *in, BOUT *out, long usize, PROG *pg);
int zoolzw_pack(BIN *in, BOUT *out, long usize, PROG *pg);
int arc_unpack(int method, BIN *in, BOUT *out, long usize, PROG *pg);
int arc_crunch(BIN *in, BOUT *out, long usize, PROG *pg);         /* ARC 8 */
int arc_squash(BIN *in, BOUT *out, long usize, PROG *pg);         /* ARC 9 */
int lzx_unpack(BIN *in, BOUT *out, long usize, PROG *pg);
int lzx_pack(BIN *in, BOUT *out, long usize, PROG *pg);

/* report progress every so often while out grows; E_CANCEL if asked */
int codec_progress(PROG *pg, long done, long total);

#endif
