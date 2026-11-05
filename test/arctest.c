/* arctest.c - host test tool for FArchive's archive layer.
 *   arctest l ARCHIVE
 *   arctest x ARCHIVE DEST [-8] [-n]     (-8: act as if DEST were FAT, -n: no paths)
 *   arctest a ARCHIVE FMT FILE...        (FMT zip lha zoo arc lzx tar tgz gz)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "archive.h"

int prog_step(PROG *p, long d, long t) { (void)p; (void)d; (void)t; return 0; }
const char *fv_errstr(int e) { static char b[16]; sprintf(b, "error %d", e); return b; }
char fv_errdetail[80];
int force_fat;

static void lg(void *ctx, const char *s) { (void)ctx; printf("  log: %s\n", s); }

int main(int argc, char **argv)
{
    ARCH a;
    AOPT o;
    int e, i;
    memset(&o, 0, sizeof o);
    o.log = lg;
    o.replace = 1; o.rename83 = 1; o.subdirs = 1; o.paths = 1; o.level = getenv("LEVEL") ? atoi(getenv("LEVEL")) : 2;
    if (argc < 3) return 2;
    if (argv[1][0] == 'l') {
        e = arch_open(&a, argv[2]);
        printf("%s: %s, %d members, e=%d\n", argv[2], af_name(a.fmt), a.n, e);
        for (i = 0; i < a.n; i++)
            printf("%8ld %8ld %-8s %08lx %c%c %s\n", a.e[i].usize, a.e[i].csize, a.e[i].mname,
                   (unsigned long)a.e[i].dostime, a.e[i].dir ? 'd' : '-', a.e[i].group ? 'g' : '-', a.e[i].name);
        arch_close(&a);
        return e != 0;
    }
    if (argv[1][0] == 'x') {
        static u8 sel[4096];
        int any = 0;
        for (i = 4; i < argc; i++) {
            if (!strcmp(argv[i], "-8")) force_fat = 1;
            if (!strcmp(argv[i], "-n")) o.paths = 0;
            if (!strcmp(argv[i], "-k")) o.replace = 0;
            if (!strcmp(argv[i], "-s") && i + 1 < argc) { sel[atoi(argv[++i])] = 1; any = 1; }
        }
        e = arch_open(&a, argv[2]);
        if (!e) e = arch_extract(&a, argv[3], any ? sel : 0, &o);
        printf("extract e=%d ok=%d fail=%d skip=%d ren=%d\n", e, o.nok, o.nfail, o.nskip, o.nren);
        arch_close(&a);
        return e != 0 || o.nfail;
    }
    if (argv[1][0] == 'a') {
        static const char *f[] = { "", "zip", "lha", "zoo", "arc", "lzx", "tar", "tgz", "gz" };
        int fmt = 0;
        for (i = 1; i < 9; i++) if (!strcmp(argv[3], f[i])) fmt = i;
        e = arch_add(argv[2], fmt, argv + 4, argc - 4, &o);
        printf("add e=%d ok=%d fail=%d skip=%d\n", e, o.nok, o.nfail, o.nskip);
        return e != 0;
    }
    return 2;
}
