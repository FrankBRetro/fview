/*
 * archive.h - FArchive's archive layer: list, extract and create ZIP,
 * LHA, ZOO, ARC, LZX, TAR, TAR.GZ and GZ archives.
 *
 * No GEM in here; the file system calls go through arcfs.c, so the same
 * code runs in FARCHIVE.PRG, its worker and the host test tool.
 */
#ifndef ARCHIVE_H
#define ARCHIVE_H

#include <stdio.h>
#include "arcio.h"

enum { AF_NONE = 0, AF_ZIP, AF_LHA, AF_ZOO, AF_ARC, AF_LZX, AF_TAR, AF_TGZ, AF_GZ, AF_COUNT };

const char *af_name(int fmt);          /* "ZIP" ...                         */
const char *af_ext(int fmt);           /* "ZIP", "LZH", ... (no dot)        */
int  af_detect(const char *path);      /* by contents, AF_NONE if unknown   */
int  af_from_name(const char *path);   /* by extension                      */

/* one archive member */
typedef struct {
    char  name[256];                   /* path inside, '/' separated        */
    long  usize, csize;
    u32   dostime;                     /* DOS date << 16 | DOS time          */
    int   method;                      /* CM_*, -1 if not supported          */
    char  mname[8];                    /* as the format calls it ("lh5")     */
    u8    dir;                         /* a folder entry                     */
    u8    crckind;                     /* 0 none, 1 CRC-16, 2 CRC-32         */
    u8    attr;                        /* DOS attributes                      */
    u8    group;                       /* LZX: member of a merged group      */
    u32   crc;
    u16   rmeth, rflags;               /* the format's own method and flags */
    u32   mode;                        /* unix mode (TAR), 0 if unknown      */
    long  hdr;                         /* offset of the entry's header       */
    long  off;                         /* offset of its data                 */
    long  hlen;                        /* header bytes (copying entries)     */
    long  gstart;                      /* LZX merged group: first entry      */
} AENT;

typedef struct {
    int    fmt;
    char   path[256];
    int    n, cap;
    AENT  *e;
    long   usize, csize;               /* totals                             */
} ARCH;

/* messages back to whoever runs the job */
typedef struct {
    void (*log)(void *ctx, const char *line);   /* one line for the log      */
    void (*item)(void *ctx, int i, int n, const char *name);  /* now doing   */
    void  *ctx;
    int    replace;                    /* overwrite existing files           */
    int    rename83;                   /* make 8.3 names on FAT drives       */
    int    subdirs;                    /* recurse into folders (adding)      */
    int    paths;                      /* keep folders (extracting)          */
    int    level;                      /* 1 fast .. 3 best                   */
    PROG  *pg;
    /* results */
    int    nok, nfail, nskip, nren;
} AOPT;

int  arch_open(ARCH *a, const char *path);     /* list the members */
void arch_close(ARCH *a);
/* extract the members with sel[i] set (sel 0: all) into dest */
int  arch_extract(ARCH *a, const char *dest, const u8 *sel, AOPT *o);
/* add files and folders to the archive (made if missing) */
int  arch_add(const char *path, int fmt, char **files, int nfiles, AOPT *o);

/* ------------------------------------------------- file system (arcfs.c) */

#ifdef __MINT__
#define ASEP '\\'
#else
#define ASEP '/'
#endif

int  afs_long_names(const char *dir);  /* 1: names need not be 8.3        */
int  afs_mkdir(const char *path);      /* E_OK if it exists afterwards     */
int  afs_isdir(const char *path);      /* 1 dir, 0 file, -1 missing        */
int  afs_info(const char *path, long *size, u32 *dostime, int *isdir);
int  afs_settime(const char *path, u32 dostime);
int  afs_truncate(FILE *f, long len);  /* E_UNSUP where it cannot          */
int  afs_rename(const char *from, const char *to);
/* list a folder: calls fn for each name (not . or ..) until it returns 1 */
int  afs_scandir(const char *dir, int (*fn)(void *ctx, const char *name), void *ctx);

/* time helpers */
u32  dos_from_unix(long t);
long unix_from_dos(u32 d);

#endif
