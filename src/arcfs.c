/*
 * arcfs.c - the few file system calls FArchive needs beyond stdio.
 * mintlib gives the POSIX calls on TOS and MiNT alike; Dpathconf tells
 * FAT (8.3 names) from file systems with long names.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <utime.h>
#include <unistd.h>
#include "archive.h"

#ifdef __MINT__
#include <mint/mintbind.h>
#include <fcntl.h>
#endif

int afs_long_names(const char *dir)
{
#ifdef __MINT__
    char d[260];
    long nmax, trunc;
    size_t n;
    strncpy(d, dir && *dir ? dir : ".", sizeof d - 2);
    d[sizeof d - 2] = 0;
    n = strlen(d);
    if (n && d[n - 1] != '\\' && d[n - 1] != '/' && d[n - 1] != ':')
        strcat(d, "\\");
    nmax = Dpathconf(d, 3);            /* DP_NAMEMAX */
    if (nmax < 0)
        return 0;                      /* no MiNT: plain GEMDOS, FAT */
    trunc = Dpathconf(d, 5);           /* DP_TRUNC */
    if (trunc == 2)                    /* DP_DOSTRUNC: 8.3 */
        return 0;
    return nmax > 12;
#else
#ifdef ARC_TEST
    extern int force_fat;
    if (force_fat)
        return 0;
#endif
    (void)dir;
    return 1;
#endif
}

/* path without a trailing separator ("C:\\" stays as it is) */
static const char *bare(const char *path, char *buf)
{
    size_t n = strlen(path);
    if (n < 2 || n > 255 || (path[n - 1] != '\\' && path[n - 1] != '/') ||
        (n == 3 && path[1] == ':') || n == 1)
        return path;
    memcpy(buf, path, n - 1);
    buf[n - 1] = 0;
    return buf;
}

int afs_isdir(const char *path)
{
    struct stat st;
    char b[256];
    if (stat(bare(path, b), &st) != 0)
        return -1;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

int afs_mkdir(const char *path)
{
    char b[256];
    const char *p = bare(path, b);
    if (afs_isdir(p) == 1)
        return E_OK;
    if (mkdir(p, 0755) == 0 || afs_isdir(p) == 1)
        return E_OK;
    {
        /* the folder above may be missing too */
        char up[256];
        char *s;
        strcpy(up, p);
        s = strrchr(up, '\\');
        if (!s || strrchr(up, '/') > s)
            s = strrchr(up, '/');
        if (s && s > up && s[-1] != ':') {
            *s = 0;
            if (afs_mkdir(up) == E_OK && mkdir(p, 0755) == 0)
                return E_OK;
        }
    }
    return E_WRITE;
}

int afs_info(const char *path, long *size, u32 *dostime, int *isdir)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return E_OPEN;
    if (size) *size = (long)st.st_size;
    if (dostime) *dostime = dos_from_unix((long)st.st_mtime);
    if (isdir) *isdir = S_ISDIR(st.st_mode) ? 1 : 0;
    return E_OK;
}

int afs_settime(const char *path, u32 dostime)
{
    struct utimbuf u;
    if (!dostime)
        return E_OK;
    u.actime = u.modtime = (time_t)unix_from_dos(dostime);
    return utime(path, &u) == 0 ? E_OK : E_WRITE;
}

int afs_truncate(FILE *f, long len)
{
    fflush(f);
#ifdef __MINT__
    {
        long l = len, r = Fcntl(fileno(f), (long)&l, (('F' << 8) | 4));  /* FTRUNCATE */
        return r == 0 ? E_OK : E_UNSUP;
    }
#else
    return ftruncate(fileno(f), len) == 0 ? E_OK : E_UNSUP;
#endif
}

int afs_rename(const char *from, const char *to)
{
    return rename(from, to) == 0 ? E_OK : E_WRITE;
}

int afs_scandir(const char *dir, int (*fn)(void *ctx, const char *name), void *ctx)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    if (!d)
        return E_OPEN;
    while ((de = readdir(d)) != 0) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (fn(ctx, de->d_name))
            break;
    }
    closedir(d);
    return E_OK;
}

/* ------------------------------------------------------------- times */

static long days_from_civil(int y, int m, int d)
{
    long era;
    unsigned yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);
    doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + (long)doe - 719468L;
}

/* DOS times are local and so are the unix times we hand out: no zone */
u32 dos_from_unix(long t)
{
    long days = t / 86400L, s = t % 86400L, z, era;
    unsigned doe, yoe, doy, mp;
    int y, m, d;
    if (t < 315532800L)                /* before 1980 */
        return (u32)((0 << 9 | 1 << 5 | 1) << 16);
    z = days + 719468L;
    era = z / 146097L;
    doe = (unsigned)(z - era * 146097L);
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (int)(yoe + era * 400);
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y += m <= 2;
    if (y > 2107)
        y = 2107;
    return ((u32)((y - 1980) << 9 | m << 5 | d) << 16) |
           (u32)((s / 3600) << 11 | ((s / 60) % 60) << 5 | (s % 60) / 2);
}

long unix_from_dos(u32 dt)
{
    unsigned date = (unsigned)(dt >> 16) & 0xFFFF, tm = (unsigned)dt & 0xFFFF;
    int y = 1980 + (date >> 9), m = (date >> 5) & 15, d = date & 31;
    if (m < 1) m = 1;
    if (m > 12) m = 12;
    if (d < 1) d = 1;
    return days_from_civil(y, m, d) * 86400L + (long)(tm >> 11) * 3600L +
           (long)((tm >> 5) & 63) * 60L + (long)(tm & 31) * 2L;
}
