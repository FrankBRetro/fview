/* Copyright (c) 2026 Francis C Bullen. SPDX-License-Identifier: MIT (see LICENSE) */
/*
 * pgtos.c - the TOS calls the GEM front ends make, on DOS (DJGPP)
 *
 * GEMDOS's date and time words are DOS's own format, and the BIOS
 * shift-key bits are the Atari's (right shift, left shift, Ctrl, Alt).
 * Anything that reads Atari hardware does nothing here.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dir.h>
#include <dos.h>
#include <dpmi.h>
#include <bios.h>
#include <sys/stat.h>
#include <osbind.h>
#include <mint/cookie.h>

int Getcookie(long id, long *val) { (void)id; if (val) *val = 0; return C_NOTFOUND; }

long  Supexec(long (*fn)(void)) { (void)fn; return 0; }    /* Atari hardware and line-A: never on the PC */
short Getrez(void) { return -1; }                           /* work_in[0] = Getrez() + 2 = 1, the screen */
void *Physbase(void) { return NULL; }
short Setcolor(short n, short c) { (void)n; (void)c; return 0; }
void  VgetRGB(short first, short n, long *rgb) { (void)first; memset(rgb, 0, 4L * n); }

long Kbshift(short mode)
{
    (void)mode;
    return bioskey(2) & 0x0f;          /* BIOS: bit 0 right shift, 1 left shift, 2 Ctrl, 3 Alt */
}

void *Malloc(long size)
{
    if (size == -1L) return (void *)(long)_go32_dpmi_remaining_physical_memory();
    return NULL;                        /* the front end only asks how much is free */
}

short Dgetdrv(void) { return (short)getdisk(); }

/* the folder without the drive, with backslashes: "\FVIEW" ("" at the top) */
long Dgetpath(char *buf, short drive)
{
    char cwd[FILENAME_MAX], *p;
    (void)drive;
    if (!getcwd(cwd, sizeof cwd)) { buf[0] = 0; return -1; }
    p = cwd[1] == ':' ? cwd + 2 : cwd;
    strcpy(buf, strcmp(p, "/") ? p : "");
    for (p = buf; *p; p++) if (*p == '/') *p = '\\';
    return 0;
}

/* the drives DOS knows: INT 21h AX=4408h ("removable?") fails on the others
 * without touching the media */
long Drvmap(void)
{
    long map = 0;
    int d;
    for (d = 1; d <= 26; d++) {
        __dpmi_regs r;
        memset(&r, 0, sizeof r);
        r.x.ax = 0x4408; r.h.bl = (unsigned char)d;
        __dpmi_int(0x21, &r);
        if (!(r.x.flags & 1) || r.x.ax != 0x0f) map |= 1L << (d - 1);
    }
    return map;
}

long Dcreate(const char *path) { return mkdir(path, 0777) ? -36 : 0; }

static _DTA def_dta, *cur_dta = &def_dta;
static struct ffblk ff;

_DTA *Fgetdta(void) { return cur_dta; }
void  Fsetdta(_DTA *dta) { cur_dta = dta; }

static void ff_to_dta(void)
{
    cur_dta->dta_attribute = (unsigned char)ff.ff_attrib;
    cur_dta->dta_time = ff.ff_ftime; cur_dta->dta_date = ff.ff_fdate;
    cur_dta->dta_size = ff.ff_fsize;
    strncpy(cur_dta->dta_name, ff.ff_name, sizeof cur_dta->dta_name - 1);
    cur_dta->dta_name[sizeof cur_dta->dta_name - 1] = 0;
}

long Fsfirst(const char *spec, short attr)
{
    if (findfirst(spec, &ff, attr)) return -33;     /* EFILNF */
    ff_to_dta();
    return 0;
}

long Fsnext(void)
{
    if (findnext(&ff)) return -49;                  /* ENMFIL */
    ff_to_dta();
    return 0;
}

long Fcreate(const char *name, short attr)
{
    int fh;
    (void)attr;
    fh = open(name, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);
    return fh < 0 ? -36 : fh;
}

long Fwrite(short fh, long n, const void *buf) { return write(fh, buf, n); }
long Fclose(short fh) { return close(fh); }

/* GEMDOS errors as TOS gives them: EFILNF -33, EACCDN -36 */
long Fopen(const char *name, short mode)
{
    static const int how[3] = { O_RDONLY, O_WRONLY, O_RDWR };
    int fh = open(name, how[(mode & 3) < 3 ? (mode & 3) : 0] | O_BINARY);
    return fh < 0 ? -33 : fh;
}
long Fread(short fh, long n, void *buf) { long r = read(fh, buf, n); return r < 0 ? -1 : r; }
long Fseek(long off, short fh, short whence) { return lseek(fh, off, whence); }
long Fdelete(const char *name) { return unlink(name) ? -33 : 0; }

/* only asks (set 0): the DOS attributes, or EFILNF */
long Fattrib(const char *name, short set, short attr)
{
    struct ffblk f;
    (void)attr;
    if (set) return -36;
    if (findfirst(name, &f, FA_RDONLY | FA_HIDDEN | FA_SYSTEM | FA_DIREC | FA_ARCH)) return -33;
    return f.ff_attrib;
}

/* GEMDOS memory: Malloc/Mxalloc hand out nothing on the PC (programs use
 * malloc); Mxalloc(-1) says how much is free, as Malloc(-1) does */
void *Mxalloc(long size, short mode) { (void)mode; return size == -1L ? Malloc(-1L) : NULL; }
long  Mfree(void *block) { (void)block; return 0; }
void *Logbase(void) { return NULL; }      /* no screen memory a program can write to */

unsigned short Tgetdate(void)
{
    struct dosdate_t d;
    _dos_getdate(&d);
    return (unsigned short)(((d.year - 1980) << 9) | (d.month << 5) | d.day);
}

unsigned short Tgettime(void)
{
    struct dostime_t t;
    _dos_gettime(&t);
    return (unsigned short)((t.hour << 11) | (t.minute << 5) | (t.second / 2));
}

/* the letters on the keys, by scan code (US layout) - for Alt+letter */
static char us_unshift[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 8, 9,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 13, 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};
static _KEYTAB keytab = { us_unshift, us_unshift, us_unshift };

_KEYTAB *Keytbl(void *unshift, void *shift, void *caps)
{
    (void)unshift; (void)shift; (void)caps;
    return &keytab;
}

/* gemrend.c reads the Line-A font ring for the PDF's bitmap glyphs; there is
 * no Line-A on the PC, so the pointers stay NULL and it finds no font (its
 * ptr_ok() checks) */
void  linea0(void) { }
char *__aline = NULL;
void **__fonts = NULL;
