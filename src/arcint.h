/* arcint.h - shared between archive.c and arcfmt.c (not for the GUI) */
#ifndef ARCINT_H
#define ARCINT_H

#include "archive.h"
#include "deflate.h"

unsigned g16(const u8 *p);
u32      g32(const u8 *p);
unsigned b16(const u8 *p);
u32      b32(const u8 *p);
void     p16(u8 *p, unsigned v);
void     p32(u8 *p, u32 v);

int  ar_push(ARCH *a, const AENT *e);
void ar_log(AOPT *o, const char *fmt, ...);
void ar_mname(AENT *e, const char *s);
/* the 8.3 form of the last part of name (no uniqueness), upper or lower */
void name83(const char *name, char *out, int upper);
/* the last part of a path */
const char *ar_base(const char *name);

/* something being added */
typedef struct {
    char  path[256];                   /* where it is                        */
    char  name[256];                   /* name in the archive, '/' separated */
    long  size;
    u32   dostime;
    int   dir;
} AITEM;

/* an archive being written */
typedef struct AW {
    int    fmt;
    FILE  *f;
    AOPT  *o;
    ARCH   out;                        /* entries written so far             */
    long   hiwater;                    /* furthest byte written              */
    PROG  *pg;                         /* progress for the current item      */
    /* TAR, TAR.GZ, GZ */
    BOUT   fo;                         /* the file                           */
    BOUT   to;                         /* the tar stream (into defl or file) */
    DEFL  *defl;
    u32    gcrc;
    long   gsize;
    long   zoolink;                    /* ZOO: where the last 'next' field is */
    int    trunc_ok;                   /* the file can be cut short          */
} AW;

/* write item's data at the current file position.
   method: CM_*; may_store: fall back to storing when it does not shrink.
   On return *method is what was used, sizes and CRCs are filled in.  */
int  aw_pack(AW *w, AITEM *it, int *method, int may_store,
             long *csize, u32 *crc32, unsigned *crc16);
/* copy len bytes from of at from to the current file position */
int  aw_copy(AW *w, FILE *of, long from, long len);
long aw_tell(AW *w);
int  aw_write(AW *w, const void *d, long n);
int  aw_seekpatch(AW *w, long at, const void *d, long n);   /* and back */

/* per format (arcfmt.c) */
int zip_list(ARCH *a, FILE *f);
int lha_list(ARCH *a, FILE *f);
int zoo_list(ARCH *a, FILE *f);
int arcf_list(ARCH *a, FILE *f);
int lzx_list(ARCH *a, FILE *f);

int zip_begin(AW *w);  int zip_add(AW *w, AITEM *it);  int zip_end(AW *w);
int lha_begin(AW *w);  int lha_add(AW *w, AITEM *it);  int lha_end(AW *w);
int zoo_begin(AW *w);  int zoo_add(AW *w, AITEM *it);  int zoo_end(AW *w);
int arcf_begin(AW *w); int arcf_add(AW *w, AITEM *it); int arcf_end(AW *w);
int lzx_begin(AW *w);  int lzx_add(AW *w, AITEM *it);  int lzx_end(AW *w);
/* copy an old member unchanged */
int zip_copy(AW *w, FILE *of, const AENT *e);
int lha_copy(AW *w, FILE *of, const AENT *e);
int zoo_copy(AW *w, FILE *of, const AENT *e);
int arcf_copy(AW *w, FILE *of, const AENT *e);
int lzx_copy(AW *w, FILE *of, const ARCH *a, int i, int n);  /* n members */

#endif
