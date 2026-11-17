/* Copyright (c) 2026 Francis C Bullen. SPDX-License-Identifier: MIT (see LICENSE) */
/*
 * osbind.h - the TOS calls these programs make, for PC GEM on
 *            DOS (DJGPP). pcgem/pgtos.c implements them.
 *
 * Supexec() never runs its function on the PC: the ones programs pass it
 * read Atari hardware or line-A variables.
 */
#ifndef PCGEM_OSBIND_H
#define PCGEM_OSBIND_H

typedef struct {
    char  dta_buf[21];
    unsigned char dta_attribute;
    unsigned short dta_time, dta_date;
    long  dta_size;
    char  dta_name[14];
} _DTA;

typedef struct { void *unshift, *shift, *caps; } _KEYTAB;

#ifndef FA_RDONLY             /* DJGPP's <dir.h> has the same values */
#define FA_RDONLY   0x01
#define FA_HIDDEN   0x02
#define FA_SYSTEM   0x04
#define FA_LABEL    0x08
#define FA_DIREC    0x10
#define FA_ARCH     0x20
#endif
#ifndef FA_DIR
#define FA_DIR      0x10
#endif

long  Drvmap(void);
long  Dcreate(const char *path);
_KEYTAB *Keytbl(void *unshift, void *shift, void *caps);
long  Supexec(long (*fn)(void));
short Getrez(void);
void *Physbase(void);
long  Kbshift(short mode);
void *Malloc(long size);
short Dgetdrv(void);
long  Dgetpath(char *buf, short drive);
_DTA *Fgetdta(void);
void  Fsetdta(_DTA *dta);
long  Fsfirst(const char *spec, short attr);
long  Fsnext(void);
long  Fcreate(const char *name, short attr);
long  Fwrite(short fh, long n, const void *buf);
long  Fclose(short fh);
/* more GEMDOS (FView) */
long  Fopen(const char *name, short mode);
long  Fread(short fh, long n, void *buf);
long  Fseek(long off, short fh, short whence);
long  Fdelete(const char *name);
long  Fattrib(const char *name, short set, short attr);
void *Mxalloc(long size, short mode);
long  Mfree(void *block);
void *Logbase(void);
unsigned short Tgetdate(void);
unsigned short Tgettime(void);
short Setcolor(short n, short c);
void  VgetRGB(short first, short n, long *rgb);

#endif
