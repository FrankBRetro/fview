/* rd.h - small buffered reader with progress reporting */
#ifndef RD_H
#define RD_H
#include <stdio.h>
#include "fview.h"

typedef struct {
    FILE *f;
    u8   buf[4096];
    long pos, len;       /* position/length inside buf                  */
    long done, total;    /* bytes consumed, file size                    */
    int  eof;
    PROG *pg;
    long lastrep;
} RD;

int  rd_open(RD *r, const char *path, PROG *pg);
void rd_close(RD *r);
int  rd_byte(RD *r);                 /* -1 at end of file                */
int  rd_peek(RD *r, int i);          /* look ahead without consuming     */
long rd_read(RD *r, void *d, long n);
int  rd_skip(RD *r, long n);
int  rd_u16be(RD *r);
long rd_u32be(RD *r);
int  rd_u16le(RD *r);
int  rd_cancelled(RD *r);            /* report progress, check cancel    */
/* read the entire file into memory (malloc); returns NULL on failure    */
u8  *rd_slurp(const char *path, long *len, PROG *pg, int *err);

static inline int bg_value(const PREFS *pf)
{
    return pf->bg == BG_BLACK ? 0 : pf->bg == BG_GREY ? 160 : 255;
}
#endif
