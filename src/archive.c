/*
 * archive.c - FArchive's archive layer: detection, listing, extraction
 * (with 8.3 names on FAT drives), and adding files, for every format.
 * TAR, TAR.GZ and GZ live here; the formats with their own member
 * headers (ZIP, LHA, ZOO, ARC, LZX) are in arcfmt.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include "arcint.h"

/* ------------------------------------------------------------ helpers */

unsigned g16(const u8 *p) { return (unsigned)(p[0] | p[1] << 8); }
u32 g32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
unsigned b16(const u8 *p) { return (unsigned)(p[0] << 8 | p[1]); }
u32 b32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | (u32)p[3]; }
void p16(u8 *p, unsigned v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
void p32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

static const char *const fmt_names[AF_COUNT] = { "", "ZIP", "LHA", "ZOO", "ARC", "LZX", "TAR", "TAR.GZ", "GZ" };
static const char *const fmt_exts[AF_COUNT] = { "", "ZIP", "LZH", "ZOO", "ARC", "LZX", "TAR", "TGZ", "GZ" };

const char *af_name(int f) { return f > 0 && f < AF_COUNT ? fmt_names[f] : "?"; }
const char *af_ext(int f) { return f > 0 && f < AF_COUNT ? fmt_exts[f] : ""; }

int ar_push(ARCH *a, const AENT *e)
{
    if (a->n == a->cap) {
        int nc = a->cap ? a->cap * 2 : 32;
        AENT *ne = realloc(a->e, nc * sizeof(AENT));
        if (!ne)
            return E_NOMEM;
        a->e = ne;
        a->cap = nc;
    }
    a->e[a->n++] = *e;
    a->usize += e->usize;
    a->csize += e->csize;
    return E_OK;
}

void ar_log(AOPT *o, const char *fmt, ...)
{
    char b[300];
    va_list ap;
    if (!o || !o->log)
        return;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    o->log(o->ctx, b);
}

void ar_mname(AENT *e, const char *s)
{
    strncpy(e->mname, s, sizeof e->mname - 1);
    e->mname[sizeof e->mname - 1] = 0;
}

const char *ar_base(const char *name)
{
    const char *s = name, *p;
    for (p = name; *p; p++)
        if ((*p == '/' || *p == '\\' || *p == ':') && p[1])
            s = p + 1;
    return s;
}

static int lc_eq(const char *a, const char *b)
{
    while (*a && tolower((u8)*a) == tolower((u8)*b))
        a++, b++;
    return tolower((u8)*a) == tolower((u8)*b);
}

static int ext_is(const char *path, const char *ext)
{
    size_t n = strlen(path), m = strlen(ext);
    return n > m && path[n - m - 1] == '.' && lc_eq(path + n - m, ext);
}

/* ---------------------------------------------------------- 8.3 names */

static int ok83char(int c)
{
    if (c >= 128)
        return 1;
    if (isalnum(c))
        return 1;
    return c && strchr("!#$%&'()-@^_`{}~", c) != 0;
}

/* is s (one path part) a valid 8.3 name? */
static int valid83(const char *s)
{
    int base = 0, ext = -1;
    if (!*s || *s == '.')
        return 0;
    for (; *s; s++) {
        if (*s == '.') {
            if (ext >= 0)
                return 0;
            ext = 0;
            continue;
        }
        if (!ok83char((u8)*s))
            return 0;
        if (ext >= 0) ext++;
        else base++;
    }
    return base >= 1 && base <= 8 && ext != 0 && ext <= 3;
}

/* 8.3 form of a name with an optional ~n tail (n > 0) */
static void make83(const char *name, char *out, int upper, int n)
{
    char base[64], ext[4], tail[16];
    const char *dot = strrchr(name, '.'), *p;
    int nb = 0, ne = 0, k, room;
    while (*name == '.')
        name++;
    if (dot && dot < name)
        dot = 0;
    for (p = name; *p && p != dot && nb < 60; p++) {
        int c = (u8)*p;
        if (c == '.' || c == ' ')
            continue;
        base[nb++] = (char)(ok83char(c) ? c : '_');
    }
    if (dot)
        for (p = dot + 1; *p && ne < 3; p++) {
            int c = (u8)*p;
            if (c == ' ' || c == '.')
                continue;
            ext[ne++] = (char)(ok83char(c) ? c : '_');
        }
    if (!nb)
        base[nb++] = '_';
    base[nb] = 0;
    ext[ne] = 0;
    if (n == 0 && nb > 8)
        n = 1;
    if (n > 0) {
        sprintf(tail, "~%d", n);
        room = 8 - (int)strlen(tail);
        if (nb > room)
            nb = room;
        base[nb] = 0;
        strcat(base, tail);
    }
    strcpy(out, base);
    if (ne) {
        strcat(out, ".");
        strcat(out, ext);
    }
    for (k = 0; out[k]; k++)
        out[k] = (char)(upper ? toupper((u8)out[k]) : tolower((u8)out[k]));
}

void name83(const char *name, char *out, int upper)
{
    name = ar_base(name);
    if (valid83(name)) {
        int k;
        strcpy(out, name);
        for (k = 0; out[k]; k++)
            out[k] = (char)(upper ? toupper((u8)out[k]) : tolower((u8)out[k]));
    } else
        make83(name, out, upper, 0);
}

/* ------------------------------------------------------------ detect */

static int tar_header_ok(const u8 *h)
{
    unsigned long sum = 0, want = 0;
    int i, digits = 0;
    if (!memcmp(h + 257, "ustar", 5))
        return 1;
    for (i = 0; i < 512; i++)
        sum += (i >= 148 && i < 156) ? 32 : h[i];
    for (i = 148; i < 156; i++) {
        if (h[i] >= '0' && h[i] <= '7') {
            want = want * 8 + (h[i] - '0');
            digits++;
        } else if (digits && (h[i] == ' ' || !h[i]))
            break;
    }
    return digits && sum == want && h[0];
}

/* the first 512 bytes of a gzip stream */
typedef struct { u8 b[512]; int n; } PEEK;

static int peek_sink(void *ctx, const u8 *d, long n)
{
    PEEK *p = ctx;
    while (n-- > 0 && p->n < 512)
        p->b[p->n++] = *d++;
    return p->n >= 512 ? E_CANCEL : E_OK;
}

static long gz_header(FILE *f, char *name, u32 *mtime);

int af_detect(const char *path)
{
    FILE *f = fopen(path, "rb");
    u8 h[512];
    long n;
    int fmt = AF_NONE;
    if (!f)
        return AF_NONE;
    memset(h, 0, sizeof h);
    n = (long)fread(h, 1, sizeof h, f);
    if (n >= 4 && h[0] == 'P' && h[1] == 'K' && ((h[2] == 3 && h[3] == 4) || (h[2] == 5 && h[3] == 6)))
        fmt = AF_ZIP;
    else if (n >= 2 && h[0] == 0x1F && h[1] == 0x8B) {
        long off;
        fmt = AF_GZ;
        fseek(f, 0, SEEK_SET);
        off = gz_header(f, 0, 0);
        if (off > 0) {
            BIN in;
            BOUT out;
            PEEK pk;
            pk.n = 0;
            fseek(f, off, SEEK_SET);
            bin_file(&in, f, -1);
            bout_func(&out, peek_sink, &pk);
            inflate_stream(&in, &out, -1, 0);
            bout_flush(&out);
            if (pk.n >= 512 && tar_header_ok(pk.b))
                fmt = AF_TGZ;
            else if (pk.n == 0 && (ext_is(path, "tgz") || ext_is(path, "tar.gz")))
                fmt = AF_TGZ;
        }
    } else if (n >= 21 && h[2] == '-' && h[3] == 'l' && h[6] == '-' && h[20] <= 3)
        fmt = AF_LHA;
    else if (n >= 24 && !memcmp(h, "ZOO", 3) && g32(h + 20) == 0xFDC4A7DCUL)
        fmt = AF_ZOO;
    else if (n >= 3 && !memcmp(h, "LZX", 3))
        fmt = AF_LZX;
    else if (n >= 2 && h[0] == 0x1A && h[1] <= 11 && (h[1] == 0 || n >= 29))
        fmt = AF_ARC;
    else if (n >= 512 && tar_header_ok(h))
        fmt = AF_TAR;
    else if (n == 0 || (n >= 512 && !h[0] && !memcmp(h, h + 1, 511)))
        fmt = af_from_name(path) == AF_TAR ? AF_TAR : AF_NONE;
    fclose(f);
    return fmt;
}

int af_from_name(const char *path)
{
    if (ext_is(path, "zip")) return AF_ZIP;
    if (ext_is(path, "lzh") || ext_is(path, "lha")) return AF_LHA;
    if (ext_is(path, "zoo")) return AF_ZOO;
    if (ext_is(path, "arc")) return AF_ARC;
    if (ext_is(path, "lzx")) return AF_LZX;
    if (ext_is(path, "tgz") || ext_is(path, "tar.gz")) return AF_TGZ;
    if (ext_is(path, "tar")) return AF_TAR;
    if (ext_is(path, "gz") || ext_is(path, "z")) return AF_GZ;
    return AF_NONE;
}

/* ================================================================ TAR */

static long oct(const u8 *p, int n)
{
    long v = 0;
    if (*p & 0x80) {                   /* GNU base-256 */
        int i;
        for (i = 1; i < n; i++)
            v = v << 8 | p[i];
        return v;
    }
    while (n > 0 && (*p == ' ' || *p == 0)) p++, n--;
    while (n > 0 && *p >= '0' && *p <= '7') {
        v = v * 8 + (*p - '0');
        p++, n--;
    }
    return v;
}

/* push parser: tar bytes go in, members come out through callbacks */
typedef struct TARP {
    u8    blk[512];
    int   bpos;
    int   state;                       /* 0 header, 1 data, 2 padding, 3 meta, 9 end */
    long  left, pad;
    long  pos;                         /* tar stream offset of blk[0]        */
    char  lname[256];                  /* GNU long name / pax path for next  */
    u8    meta[512];                   /* collected 'L' / 'x' data           */
    int   metan, metatype;
    AENT  e;
    int   want;                        /* deliver this member's data         */
    int   err;
    void *ctx;
    int  (*begin)(void *ctx, AENT *e); /* 1: send data                       */
    int  (*data)(void *ctx, const u8 *d, long n);
    int  (*end)(void *ctx, AENT *e);
} TARP;

static void tar_meta_done(TARP *t)
{
    if (t->metatype == 'L') {
        int n = t->metan < 255 ? t->metan : 255;
        memcpy(t->lname, t->meta, n);
        t->lname[n] = 0;
    } else {                           /* pax: "len key=value\n" records */
        int i = 0;
        while (i < t->metan) {
            int len = 0, j = i;
            while (j < t->metan && t->meta[j] >= '0' && t->meta[j] <= '9')
                len = len * 10 + (t->meta[j++] - '0');
            if (len <= 0 || i + len > t->metan)
                break;
            if (j + 6 <= i + len && !memcmp(t->meta + j, " path=", 6)) {
                int n = i + len - (j + 6) - 1;
                if (n > 255) n = 255;
                if (n > 0) {
                    memcpy(t->lname, t->meta + j + 6, n);
                    t->lname[n] = 0;
                }
            }
            i += len;
        }
    }
}

static void tar_header(TARP *t)
{
    const u8 *h = t->blk;
    int type = h[156], i;
    long size;
    for (i = 0; i < 512 && !h[i]; i++)
        ;
    if (i == 512) {                    /* zero block: the end */
        t->state = 9;
        return;
    }
    if (!tar_header_ok(h)) {
        t->err = E_FORMAT;
        t->state = 9;
        return;
    }
    size = oct(h + 124, 12);
    if (type == 'L' || type == 'x') {  /* metadata for the next member */
        t->state = 3;
        t->metatype = type;
        t->metan = 0;
        t->left = size;
        t->pad = (512 - size % 512) % 512;
        if (!size) {
            tar_meta_done(t);
            t->state = 0;
        }
        return;
    }
    memset(&t->e, 0, sizeof t->e);
    if (t->lname[0]) {
        strcpy(t->e.name, t->lname);
        t->lname[0] = 0;
    } else {
        char nm[101], pre[156];
        memcpy(nm, h, 100);
        nm[100] = 0;
        pre[0] = 0;
        if (!memcmp(h + 257, "ustar", 5) && h[345]) {
            memcpy(pre, h + 345, 155);
            pre[155] = 0;
            strcat(pre, "/");
        }
        if (strlen(pre) + strlen(nm) < sizeof t->e.name) {
            strcpy(t->e.name, pre);
            strcat(t->e.name, nm);
        } else
            strcpy(t->e.name, nm);
    }
    t->e.mode = (u32)oct(h + 100, 8);
    t->e.dostime = dos_from_unix(oct(h + 136, 12));
    t->e.hdr = t->pos;
    t->e.off = t->pos + 512;
    t->e.rmeth = (u16)type;
    if (type == '5') {
        t->e.dir = 1;
        size = 0;
    } else if (type != '0' && type != 0 && type != '7')
        t->e.method = -1;              /* links, devices: not extracted */
    if (type == '0' || type == 0 || type == '7' || type == '5') {
        size_t n = strlen(t->e.name);
        if (n && t->e.name[n - 1] == '/') {
            t->e.dir = 1;
            t->e.name[n - 1] = 0;
        }
    }
    t->e.usize = t->e.csize = size;
    ar_mname(&t->e, t->e.method < 0 ? "link" : t->e.dir ? "dir" : "stored");
    if (t->e.method < 0) {
        char tc[2];
        tc[0] = (char)(type ? type : '?');
        tc[1] = 0;
        ar_mname(&t->e, tc);
    }
    t->want = t->begin ? t->begin(t->ctx, &t->e) : 0;
    if (t->want < 0) {
        t->err = t->want;
        t->state = 9;
        return;
    }
    t->left = size;
    t->pad = (512 - size % 512) % 512;
    t->state = size ? 1 : 0;
    if (!size && t->end && t->end(t->ctx, &t->e) < 0) {
        t->err = E_CANCEL;
        t->state = 9;
    }
}

static int tar_feed(void *ctx, const u8 *d, long n)
{
    TARP *t = ctx;
    while (n > 0 && t->state != 9) {
        if (t->state == 0) {
            long k = 512 - t->bpos;
            if (k > n) k = n;
            memcpy(t->blk + t->bpos, d, k);
            t->bpos += (int)k;
            d += k;
            n -= k;
            if (t->bpos == 512) {
                tar_header(t);
                t->bpos = 0;
                t->pos += 512;
            }
        } else if (t->state == 1 || t->state == 3) {
            long k = t->left < n ? t->left : n;
            if (t->state == 1 && t->want && t->data) {
                int e = t->data(t->ctx, d, k);
                if (e) {
                    t->err = e;
                    t->state = 9;
                    break;
                }
            }
            if (t->state == 3) {
                long c = k;
                if (t->metan + c > (long)sizeof t->meta - 1)
                    c = (long)sizeof t->meta - 1 - t->metan;
                if (c > 0) {
                    memcpy(t->meta + t->metan, d, c);
                    t->metan += (int)c;
                }
            }
            t->left -= k;
            t->pos += k;
            d += k;
            n -= k;
            if (!t->left) {
                if (t->state == 3)
                    tar_meta_done(t);
                else if (t->end && t->end(t->ctx, &t->e) < 0) {
                    t->err = E_CANCEL;
                    t->state = 9;
                    break;
                }
                t->state = t->pad ? 2 : 0;
            }
        } else if (t->state == 2) {
            long k = t->pad < n ? t->pad : n;
            t->pad -= k;
            t->pos += k;
            d += k;
            n -= k;
            if (!t->pad)
                t->state = 0;
        }
    }
    return E_OK;
}

/* run the whole tar (or tar.gz) through a parser */
static int tar_run(const char *path, int gz, TARP *t, PROG *pg)
{
    FILE *f = fopen(path, "rb");
    BIN in;
    BOUT out;
    int e;
    if (!f)
        return E_OPEN;
    bout_func(&out, tar_feed, t);
    if (gz) {
        long off = gz_header(f, 0, 0);
        if (off < 0) {
            fclose(f);
            return E_FORMAT;
        }
        fseek(f, off, SEEK_SET);
        bin_file(&in, f, -1);
        e = inflate_stream(&in, &out, -1, pg);
        if (e == E_FORMAT && t->state == 9)
            e = E_OK;                  /* stopped at the tar end */
    } else {
        u8 buf[2048];
        long n, done = 0, total;
        fseek(f, 0, SEEK_END);
        total = ftell(f);
        fseek(f, 0, SEEK_SET);
        e = E_OK;
        while (t->state != 9 && (n = (long)fread(buf, 1, sizeof buf, f)) > 0) {
            bout_put(&out, buf, n);
            done += n;
            if ((done & 0x7FFF) < (long)sizeof buf && prog_step(pg, done, total)) {
                e = E_CANCEL;
                break;
            }
        }
    }
    bout_flush(&out);
    fclose(f);
    if (!e && t->err)
        e = t->err;
    return e;
}

static int tar_list_begin(void *ctx, AENT *e)
{
    return ar_push(ctx, e) ? E_NOMEM : 0;
}

/* ================================================================= GZ */

/* parse a gzip member header at the file position: the data offset */
static long gz_header(FILE *f, char *name, u32 *mtime)
{
    u8 h[10];
    long start = ftell(f);
    int fl, c;
    if (fread(h, 1, 10, f) != 10 || h[0] != 0x1F || h[1] != 0x8B || h[2] != 8)
        return -1;
    fl = h[3];
    if (mtime)
        *mtime = g32(h + 4);
    if (fl & 4) {
        u8 x[2];
        if (fread(x, 1, 2, f) != 2)
            return -1;
        fseek(f, (long)g16(x), SEEK_CUR);
    }
    if (fl & 8) {
        int n = 0;
        while ((c = getc(f)) > 0)
            if (name && n < 255)
                name[n++] = (char)c;
        if (name)
            name[n] = 0;
        if (c < 0)
            return -1;
    }
    if (fl & 16)
        while ((c = getc(f)) > 0)
            ;
    if (fl & 2)
        fseek(f, 2, SEEK_CUR);
    (void)start;
    return ftell(f);
}

static int gz_list(ARCH *a, FILE *f)
{
    AENT e;
    char nm[256];
    u32 mt = 0;
    long off, end;
    u8 t[4];
    memset(&e, 0, sizeof e);
    nm[0] = 0;
    off = gz_header(f, nm, &mt);
    if (off < 0)
        return E_FORMAT;
    fseek(f, 0, SEEK_END);
    end = ftell(f);
    fseek(f, end - 4, SEEK_SET);
    if (fread(t, 1, 4, f) != 4)
        return E_FORMAT;
    if (!nm[0]) {                      /* the archive's name without .gz */
        char *dot;
        strcpy(nm, ar_base(a->path));
        dot = strrchr(nm, '.');
        if (dot && dot != nm)
            *dot = 0;
    }
    strcpy(e.name, ar_base(nm));
    e.usize = (long)g32(t);
    e.csize = end - off - 8;
    e.off = off;
    e.dostime = mt ? dos_from_unix((long)mt) : 0;
    e.method = CM_DEFLATE;
    e.crckind = 2;
    ar_mname(&e, "deflate");
    return ar_push(a, &e);
}

/* ============================================================== open */

int arch_open(ARCH *a, const char *path)
{
    FILE *f;
    int e;
    memset(a, 0, sizeof *a);
    strncpy(a->path, path, sizeof a->path - 1);
    a->fmt = af_detect(path);
    if (a->fmt == AF_NONE) {
        f = fopen(path, "rb");
        if (!f)
            return E_OPEN;
        fclose(f);
        return E_FORMAT;
    }
    if (a->fmt == AF_TAR || a->fmt == AF_TGZ) {
        TARP *t = calloc(1, sizeof(TARP));
        if (!t)
            return E_NOMEM;
        t->ctx = a;
        t->begin = tar_list_begin;
        e = tar_run(path, a->fmt == AF_TGZ, t, 0);
        free(t);
        return e;
    }
    f = fopen(path, "rb");
    if (!f)
        return E_OPEN;
    switch (a->fmt) {
    case AF_ZIP: e = zip_list(a, f); break;
    case AF_LHA: e = lha_list(a, f); break;
    case AF_ZOO: e = zoo_list(a, f); break;
    case AF_ARC: e = arcf_list(a, f); break;
    case AF_LZX: e = lzx_list(a, f); break;
    case AF_GZ:  e = gz_list(a, f); break;
    default:     e = E_UNSUP;
    }
    fclose(f);
    return e;
}

void arch_close(ARCH *a)
{
    free(a->e);
    a->e = 0;
    a->n = a->cap = 0;
}

/* ========================================================= extraction */

typedef struct DMAP {
    struct DMAP *next;
    char  from[256];                   /* folder as named in the archive     */
    char  to[256];                     /* where it went                      */
} DMAP;

typedef struct {
    ARCH  *a;
    AOPT  *o;
    char   dest[256];
    int    fat;                        /* make 8.3 names                     */
    DMAP  *dirs;
    char **made;                       /* files written in this run          */
    int    nmade, capmade;
    long   base, total;                /* bytes for the progress bar         */
    PROG   sub;
    /* the member being written */
    FILE  *of;
    char   opath[300];
    AENT  *cur;
    int    idx, nsel, isel;
    const u8 *sel;
    u32    crc32;
    unsigned crc16;
    long   got;
} XCTX;

static int sub_prog(PROG *p, long done, long total)
{
    XCTX *x = p->data;
    (void)total;
    return prog_step(x->o->pg, x->base + done, x->total > 0 ? x->total : 1);
}

static int made_has(XCTX *x, const char *path)
{
    int i;
    for (i = 0; i < x->nmade; i++)
        if (x->fat ? lc_eq(x->made[i], path) : !strcmp(x->made[i], path))
            return 1;
    return 0;
}

static void made_add(XCTX *x, const char *path)
{
    char *s;
    if (x->nmade == x->capmade) {
        int nc = x->capmade ? x->capmade * 2 : 64;
        char **nm = realloc(x->made, nc * sizeof(char *));
        if (!nm)
            return;
        x->made = nm;
        x->capmade = nc;
    }
    s = malloc(strlen(path) + 1);
    if (s) {
        strcpy(s, path);
        x->made[x->nmade++] = s;
    }
}

/* dir + name into out (256 bytes); -1 (and out = dir) if too long */
static int join(char *out, const char *dir, const char *name)
{
    size_t n = strlen(dir), m = strlen(name);
    int sep = n && dir[n - 1] != '/' && dir[n - 1] != '\\' && dir[n - 1] != ':';
    if (n + sep + m > 255) {
        if (n < 256)
            strcpy(out, dir);
        else
            out[0] = 0;
        return -1;
    }
    memcpy(out, dir, n);
    if (sep)
        out[n++] = ASEP;
    memcpy(out + n, name, m + 1);
    return 0;
}

/* name for one path part in folder dir; isdir picks the folder rules */
static void part_name(XCTX *x, const char *dir, const char *part, int isdir, char *out)
{
    char full[300];
    int n;
    if (!x->fat || !x->o->rename83 || valid83(part)) {
        strcpy(out, part);
        return;
    }
    for (n = 0; n < 1000; n++) {
        DMAP *m;
        int clash = 0;
        make83(part, out, 1, n);
        join(full, dir, out);
        if (isdir) {
            for (m = x->dirs; m; m = m->next)
                if (lc_eq(m->to, full))
                    clash = 1;
            if (!clash && afs_isdir(full) == 0)
                clash = 1;             /* a file of that name */
        } else
            clash = made_has(x, full) || afs_isdir(full) == 1;
        if (!clash)
            return;
    }
}

/* where member e goes; 0 OK, 1 skip it */
static int dest_path(XCTX *x, const AENT *e, char *out)
{
    char parts[256], dir[300], from[256];
    char *p, *q, *last = 0;
    int k, n = 0;
    char *list[64];
    strcpy(parts, e->name);
    for (p = parts; *p; p++)
        if (*p == '\\')
            *p = '/';
    /* drive letters and devices go */
    p = strrchr(parts, ':');
    p = p ? p + 1 : parts;
    for (q = strtok(p, "/"); q && n < 64; q = strtok(0, "/")) {
        if (!*q || !strcmp(q, ".") || !strcmp(q, ".."))
            continue;
        list[n++] = q;
    }
    if (!n)
        return 1;
    if (!x->o->paths && !e->dir) {
        list[0] = list[n - 1];
        n = 1;
    } else if (!x->o->paths)
        return 1;
    strcpy(dir, x->dest);
    from[0] = 0;
    for (k = 0; k < n - (e->dir ? 0 : 1); k++) {
        DMAP *m;
        if (from[0])
            strcat(from, "/");
        strcat(from, list[k]);
        for (m = x->dirs; m; m = m->next)
            if (!strcmp(m->from, from))
                break;
        if (!m) {
            char nm[256];
            m = calloc(1, sizeof(DMAP));
            if (!m)
                return -1;
            strcpy(m->from, from);
            part_name(x, dir, list[k], 1, nm);
            if (join(m->to, dir, nm)) {
                ar_log(x->o, "Path too long: %.200s", e->name);
                free(m);
                return -1;
            }
            if (strcmp(nm, list[k]) && x->o->rename83) {
                ar_log(x->o, "Folder %s renamed %s", from, nm);
                x->o->nren++;
            }
            m->next = x->dirs;
            x->dirs = m;
            if (afs_mkdir(m->to) != E_OK) {
                ar_log(x->o, "Cannot make folder %s", m->to);
                return -1;
            }
        }
        strcpy(dir, m->to);
    }
    if (e->dir) {
        strcpy(out, dir);
        return 0;
    }
    last = list[n - 1];
    {
        char nm[256];
        part_name(x, dir, last, 0, nm);
        if (join(out, dir, nm)) {
            ar_log(x->o, "Path too long: %.200s", e->name);
            return -1;
        }
        if (strcmp(nm, last)) {
            ar_log(x->o, "Renamed %s to %s", e->name, nm);
            x->o->nren++;
        }
    }
    return 0;
}

static int x_open(XCTX *x, AENT *e)
{
    int r;
    x->of = 0;
    if (e->method < 0) {
        ar_log(x->o, "Skipped %s (%s not supported)", e->name, e->mname);
        x->o->nskip++;
        return 0;
    }
    r = dest_path(x, e, x->opath);
    if (r < 0) {
        x->o->nfail++;
        return 0;
    }
    if (r || e->dir)
        return 0;
    if (afs_isdir(x->opath) >= 0 && !made_has(x, x->opath) && !x->o->replace) {
        ar_log(x->o, "Skipped %s (already there)", e->name);
        x->o->nskip++;
        return 0;
    }
    x->of = fopen(x->opath, "wb");
    if (!x->of) {
        ar_log(x->o, "Cannot write %s", x->opath);
        x->o->nfail++;
        return 0;
    }
    made_add(x, x->opath);
    x->crc32 = 0xFFFFFFFFUL;
    x->crc16 = 0;
    x->got = 0;
    return 1;
}

/* finish the file: err from the decoder, crc as computed */
static void x_close(XCTX *x, AENT *e, int err, u32 crc32, unsigned crc16)
{
    int bad = 0;
    if (!x->of)
        return;
    if (fclose(x->of) != 0 && !err)
        err = E_WRITE;
    x->of = 0;
    if (!err) {
        if (e->crckind == 2 && (crc32 & 0xFFFFFFFFUL) != (e->crc & 0xFFFFFFFFUL))
            bad = 1;
        if (e->crckind == 1 && (crc16 & 0xFFFF) != (e->crc & 0xFFFF))
            bad = 1;
    }
    if (err == E_CANCEL) {
        remove(x->opath);
        return;
    }
    if (err) {
        ar_log(x->o, "Failed %s: %s", e->name, fv_errstr(err));
        remove(x->opath);
        x->o->nfail++;
        return;
    }
    afs_settime(x->opath, e->dostime);
    if (bad) {
        ar_log(x->o, "CRC error in %s", e->name);
        x->o->nfail++;
    } else
        x->o->nok++;
}

static void x_item(XCTX *x, AENT *e)
{
    if (x->o->item)
        x->o->item(x->o->ctx, ++x->isel, x->nsel, e->name);
}

/* members with their own offset: ZIP, LHA, ZOO, ARC, LZX, GZ */
static int x_member(XCTX *x, FILE *af, AENT *e)
{
    BIN in;
    BOUT out;
    int err;
    x_item(x, e);
    if (!x_open(x, e))
        return E_OK;
    fseek(af, e->off, SEEK_SET);
    bin_file(&in, af, e->csize);
    bout_file(&out, x->of);
    err = unpack(e->method, &in, &out, e->usize, &x->sub);
    if (bout_flush(&out) && !err)
        err = E_WRITE;
    x_close(x, e, err, bout_crc32(&out), out.crc16);
    return err == E_CANCEL ? err : E_OK;
}

/* GZ: every member of the stream into one file */
static int x_gz(XCTX *x, FILE *af, AENT *e)
{
    BIN in;
    BOUT out;
    int err = E_OK;
    long pos = 0;
    x_item(x, e);
    if (!x_open(x, e))
        return E_OK;
    bout_file(&out, x->of);
    fseek(af, 0, SEEK_SET);
    for (;;) {
        long off = gz_header(af, 0, 0);
        u8 tr[8];
        u32 c0;
        long n0;
        if (off < 0) {
            if (pos == 0)
                err = E_FORMAT;
            break;
        }
        fseek(af, off, SEEK_SET);
        bin_file(&in, af, -1);
        bout_flush(&out);
        c0 = out.crc32;
        n0 = out.count;
        out.crc32 = 0xFFFFFFFFUL;
        err = inflate_stream(&in, &out, -1, &x->sub);
        if (err)
            break;
        bout_flush(&out);
        /* the trailer follows what inflate used */
        fseek(af, off + in.total, SEEK_SET);
        if (fread(tr, 1, 8, af) != 8) {
            err = E_FORMAT;
            break;
        }
        if ((g32(tr) & 0xFFFFFFFFUL) != (bout_crc32(&out) & 0xFFFFFFFFUL) ||
            ((g32(tr + 4) ^ (u32)(out.count - n0)) & 0xFFFFFFFFUL)) {
            ar_log(x->o, "CRC error in %s", e->name);
            x->o->nfail++;
        }
        (void)c0;
        pos = ftell(af);
        {
            int c = getc(af);
            if (c != 0x1F)
                break;
            ungetc(c, af);
        }
    }
    if (bout_flush(&out) && !err)
        err = E_WRITE;
    e->crckind = 0;                    /* checked per member above */
    x_close(x, e, err, 0, 0);
    return err == E_CANCEL ? err : E_OK;
}

/* LZX merged group: one stream, the members one after another */
typedef struct {
    XCTX *x;
    int   i, last;                     /* member now, last member            */
    long  left;                        /* bytes still due for member i       */
    int   err;
} LSPLIT;

static void ls_next(LSPLIT *s)
{
    XCTX *x = s->x;
    while (s->i <= s->last) {
        AENT *e = &x->a->e[s->i];
        if (!x->sel || x->sel[s->i]) {
            x_item(x, e);
            x->cur = e;
            if (!x_open(x, e))
                x->of = 0;
        } else
            x->of = 0;
        s->left = e->usize;
        if (s->left)
            return;
        /* empty member */
        if (x->of)
            x_close(x, e, E_OK, 0, 0);
        s->i++;
    }
}

static int ls_sink(void *ctx, const u8 *d, long n)
{
    LSPLIT *s = ctx;
    XCTX *x = s->x;
    while (n > 0 && s->i <= s->last) {
        long k = s->left < n ? s->left : n;
        if (x->of) {
            if ((long)fwrite(d, 1, k, x->of) != k)
                s->err = E_WRITE;
            x->crc32 = crc32_update(x->crc32, d, k);
        }
        s->left -= k;
        d += k;
        n -= k;
        if (!s->left) {
            AENT *e = &x->a->e[s->i];
            if (x->of)
                x_close(x, e, s->err, x->crc32 ^ 0xFFFFFFFFUL, 0);
            s->err = 0;
            s->i++;
            ls_next(s);
        }
    }
    return E_OK;
}

static int x_group(XCTX *x, FILE *af, int first, int last)
{
    AENT *t = &x->a->e[last];
    LSPLIT s;
    BIN in;
    BOUT out;
    long total = 0;
    int k, err;
    for (k = first; k <= last; k++)
        total += x->a->e[k].usize;
    memset(&s, 0, sizeof s);
    s.x = x;
    s.i = first;
    s.last = last;
    ls_next(&s);
    fseek(af, t->off, SEEK_SET);
    bin_file(&in, af, t->csize);
    bout_func(&out, ls_sink, &s);
    err = unpack(CM_LZX, &in, &out, total, &x->sub);
    bout_flush(&out);
    if (x->of) {                       /* stream ended inside a member */
        x_close(x, &x->a->e[s.i], err ? err : E_FORMAT, 0, 0);
    }
    if (err && err != E_CANCEL)
        ar_log(x->o, "LZX group damaged: %s", fv_errstr(err));
    return err == E_CANCEL ? err : E_OK;
}

/* TAR / TAR.GZ */
static int xt_begin(void *ctx, AENT *e)
{
    XCTX *x = ctx;
    int i = x->idx++;
    if (x->sel && (i >= x->a->n || !x->sel[i]))
        return 0;
    x->cur = e;
    x_item(x, e);
    if (e->dir) {
        x_open(x, e);
        return 0;
    }
    return x_open(x, e);
}

static int xt_data(void *ctx, const u8 *d, long n)
{
    XCTX *x = ctx;
    if (x->of && (long)fwrite(d, 1, n, x->of) != n) {
        x_close(x, x->cur, E_WRITE, 0, 0);
    }
    x->got += n;
    return x->o->pg && x->o->pg->cancelled ? E_CANCEL : E_OK;
}

static int xt_end(void *ctx, AENT *e)
{
    XCTX *x = ctx;
    if (x->of)
        x_close(x, e, E_OK, 0, 0);
    return E_OK;
}

static int sel_count(const ARCH *a, const u8 *sel, long *bytes)
{
    int i, n = 0;
    *bytes = 0;
    for (i = 0; i < a->n; i++)
        if (!sel || sel[i]) {
            n++;
            *bytes += a->e[i].usize;
        }
    return n;
}

int arch_extract(ARCH *a, const char *dest, const u8 *sel, AOPT *o)
{
    XCTX *x = calloc(1, sizeof(XCTX));
    FILE *af = 0;
    int i, err = E_OK;
    if (!x)
        return E_NOMEM;
    x->a = a;
    x->o = o;
    x->sel = sel;
    strncpy(x->dest, dest, sizeof x->dest - 1);
    if (afs_mkdir(dest) != E_OK) {
        ar_log(o, "Cannot make folder %s", dest);
        free(x);
        return E_WRITE;
    }
    x->fat = !afs_long_names(dest);
    x->nsel = sel_count(a, sel, &x->total);
    x->sub.fn = sub_prog;
    x->sub.data = x;
    if (a->fmt == AF_TAR || a->fmt == AF_TGZ) {
        TARP *t = calloc(1, sizeof(TARP));
        if (!t) {
            err = E_NOMEM;
            goto out;
        }
        t->ctx = x;
        t->begin = xt_begin;
        t->data = xt_data;
        t->end = xt_end;
        x->total = a->fmt == AF_TGZ ? a->usize + 1 : 0;
        if (a->fmt == AF_TAR) {
            FILE *f = fopen(a->path, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                x->total = ftell(f);
                fclose(f);
            }
        }
        err = tar_run(a->path, a->fmt == AF_TGZ, t, &x->sub);
        if (x->of)
            x_close(x, x->cur, err ? err : E_FORMAT, 0, 0);
        free(t);
        goto out;
    }
    af = fopen(a->path, "rb");
    if (!af) {
        err = E_OPEN;
        goto out;
    }
    for (i = 0; i < a->n && !err; i++) {
        AENT *e = &a->e[i];
        if (e->group) {
            /* a merged LZX group runs to the member with the data */
            int last = i, k, any = 0;
            while (last < a->n - 1 && a->e[last].csize == 0 && a->e[last + 1].group)
                last++;
            for (k = i; k <= last; k++)
                if (!sel || sel[k])
                    any = 1;
            if (any)
                err = x_group(x, af, i, last);
            for (k = i; k <= last; k++)
                if (!sel || sel[k])
                    x->base += a->e[k].usize;
            i = last;
            continue;
        }
        if (sel && !sel[i])
            continue;
        if (a->fmt == AF_GZ)
            err = x_gz(x, af, e);
        else if (e->dir) {
            x_item(x, e);
            x_open(x, e);
        } else
            err = x_member(x, af, e);
        x->base += e->usize;
    }
out:
    if (af)
        fclose(af);
    while (x->dirs) {
        DMAP *m = x->dirs->next;
        free(x->dirs);
        x->dirs = m;
    }
    for (i = 0; i < x->nmade; i++)
        free(x->made[i]);
    free(x->made);
    free(x);
    return err;
}

/* ========================================================== writing */

long aw_tell(AW *w)
{
    return ftell(w->f);
}

int aw_write(AW *w, const void *d, long n)
{
    long p;
    if ((long)fwrite(d, 1, n, w->f) != n)
        return E_WRITE;
    p = ftell(w->f);
    if (p > w->hiwater)
        w->hiwater = p;
    return E_OK;
}

int aw_seekpatch(AW *w, long at, const void *d, long n)
{
    long here = ftell(w->f);
    int e;
    fseek(w->f, at, SEEK_SET);
    e = aw_write(w, d, n);
    fseek(w->f, here, SEEK_SET);
    return e;
}

int aw_copy(AW *w, FILE *of, long from, long len)
{
    u8 buf[2048];
    fseek(of, from, SEEK_SET);
    while (len > 0) {
        long k = len < (long)sizeof buf ? len : (long)sizeof buf;
        if ((long)fread(buf, 1, k, of) != k)
            return E_FORMAT;
        if (aw_write(w, buf, k))
            return E_WRITE;
        len -= k;
    }
    return E_OK;
}

/* the source file, read with running CRCs */
typedef struct {
    FILE    *f;
    u32      crc32;
    unsigned crc16;
} SRC;

static long src_read(void *ctx, u8 *buf, long n)
{
    SRC *s = ctx;
    long got = (long)fread(buf, 1, n, s->f);
    if (got > 0) {
        s->crc32 = crc32_update(s->crc32, buf, got);
        s->crc16 = crc16_update(s->crc16, buf, got);
    }
    return got;
}

static int file_sink(void *ctx, const u8 *d, long n)
{
    AW *w = ctx;
    return aw_write(w, d, n);
}

int aw_pack(AW *w, AITEM *it, int *method, int may_store,
            long *csize, u32 *crc32, unsigned *crc16)
{
    SRC s;
    BIN in;
    BOUT out;
    long start = aw_tell(w);
    int e;
    s.f = fopen(it->path, "rb");
    if (!s.f)
        return E_OPEN;
    s.crc32 = 0xFFFFFFFFUL;
    s.crc16 = 0;
    bin_func(&in, src_read, &s);
    bout_func(&out, file_sink, w);
    e = it->size ? pack(*method, &in, &out, it->size, w->pg) : E_OK;
    if (!e && bout_flush(&out))
        e = E_WRITE;
    *csize = out.count;
    if (!e && may_store && *method != CM_STORE && *csize >= it->size) {
        /* did not shrink: store it instead */
        fseek(w->f, start, SEEK_SET);
        fseek(s.f, 0, SEEK_SET);
        s.crc32 = 0xFFFFFFFFUL;
        s.crc16 = 0;
        bin_func(&in, src_read, &s);
        bout_func(&out, file_sink, w);
        *method = CM_STORE;
        e = pack(CM_STORE, &in, &out, it->size, w->pg);
        if (!e && bout_flush(&out))
            e = E_WRITE;
        *csize = out.count;
    }
    if (!e && in.total != it->size)
        e = E_FORMAT;                  /* the file changed under us */
    fclose(s.f);
    if (!it->size)
        *method = CM_STORE;
    *crc32 = s.crc32 ^ 0xFFFFFFFFUL;
    *crc16 = s.crc16;
    return e;
}

/* ------------------------------------------------- TAR / TAR.GZ / GZ */

static int gz_sink(void *ctx, const u8 *d, long n)
{
    AW *w = ctx;
    w->gcrc = crc32_update(w->gcrc, d, n);
    w->gsize += n;
    defl_put(w->defl, d, n);
    return w->fo.err;
}

static int tar_put(AW *w, const void *d, long n)
{
    bout_put(&w->to, d, n);
    return w->to.err ? E_WRITE : E_OK;
}

static int gz_begin(AW *w, const char *name, u32 mtime)
{
    u8 h[10];
    memset(h, 0, sizeof h);
    h[0] = 0x1F; h[1] = 0x8B; h[2] = 8;
    h[3] = name ? 8 : 0;
    p32(h + 4, mtime);
    h[8] = 0;
    h[9] = 5;                          /* "Atari" */
    bout_file(&w->fo, w->f);
    bout_put(&w->fo, h, 10);
    if (name)
        bout_put(&w->fo, (const u8 *)name, (long)strlen(name) + 1);
    w->defl = defl_new(&w->fo, w->o->level ? w->o->level : 2);
    if (!w->defl)
        return E_NOMEM;
    w->gcrc = 0xFFFFFFFFUL;
    w->gsize = 0;
    bout_func(&w->to, gz_sink, w);
    return E_OK;
}

static int gz_end(AW *w)
{
    u8 t[8];
    bout_flush(&w->to);
    defl_end(w->defl);
    defl_free(w->defl);
    w->defl = 0;
    p32(t, w->gcrc ^ 0xFFFFFFFFUL);
    p32(t + 4, (u32)w->gsize);
    bout_put(&w->fo, t, 8);
    return bout_flush(&w->fo) ? E_WRITE : E_OK;
}

static void octal(u8 *p, int n, unsigned long v)
{
    /* n-1 digits and a NUL */
    int i;
    p[n - 1] = 0;
    for (i = n - 2; i >= 0; i--) {
        p[i] = (u8)('0' + (v & 7));
        v >>= 3;
    }
}

static int tar_hdr(AW *w, const char *name, int type, long size, u32 mode, u32 dostime)
{
    u8 h[512];
    unsigned long sum = 0;
    size_t n = strlen(name);
    int i;
    memset(h, 0, sizeof h);
    if (n > 100) {
        /* ustar prefix/name split, else a GNU long name block first */
        const char *cut = 0, *p;
        for (p = name; *p; p++)
            if (*p == '/' && p - name <= 155 && n - (p - name) - 1 <= 100 && n - (p - name) > 1)
                cut = p;
        if (cut) {
            memcpy(h + 345, name, cut - name);
            memcpy(h, cut + 1, n - (cut - name) - 1);
        } else {
            int e = tar_hdr(w, "././@LongLink", 'L', (long)n + 1, 0644, 0);
            u8 pad[512];
            long k = (long)n + 1;
            if (e)
                return e;
            if ((e = tar_put(w, name, k)) != E_OK)
                return e;
            memset(pad, 0, sizeof pad);
            if (k % 512 && (e = tar_put(w, pad, 512 - k % 512)) != E_OK)
                return e;
            memcpy(h, name, 100);
        }
    } else
        memcpy(h, name, n);
    octal(h + 100, 8, mode ? mode & 07777 : type == '5' ? 0755 : 0644);
    octal(h + 108, 8, 0);
    octal(h + 116, 8, 0);
    octal(h + 124, 12, (unsigned long)size);
    octal(h + 136, 12, dostime ? (unsigned long)unix_from_dos(dostime) : 0);
    h[156] = (u8)type;
    memcpy(h + 257, "ustar", 6);
    h[263] = '0';
    h[264] = '0';
    memset(h + 148, ' ', 8);
    for (i = 0; i < 512; i++)
        sum += h[i];
    octal(h + 148, 7, sum);
    h[155] = ' ';
    return tar_put(w, h, 512);
}

static int tar_data_pad(AW *w, long size)
{
    u8 pad[512];
    memset(pad, 0, sizeof pad);
    return size % 512 ? tar_put(w, pad, 512 - size % 512) : E_OK;
}

static int tar_add(AW *w, AITEM *it)
{
    char nm[260];
    int e;
    strcpy(nm, it->name);
    if (it->dir)
        strcat(nm, "/");
    if ((e = tar_hdr(w, nm, it->dir ? '5' : '0', it->dir ? 0 : it->size, 0, it->dostime)) != E_OK)
        return e;
    if (!it->dir) {
        FILE *f = fopen(it->path, "rb");
        u8 buf[2048];
        long left = it->size, done = 0;
        if (!f)
            return E_OPEN;
        while (left > 0) {
            long k = left < (long)sizeof buf ? left : (long)sizeof buf;
            if ((long)fread(buf, 1, k, f) != k) {
                fclose(f);
                return E_FORMAT;
            }
            if ((e = tar_put(w, buf, k)) != E_OK) {
                fclose(f);
                return e;
            }
            left -= k;
            done += k;
            if ((done & 0x7FFF) < (long)sizeof buf && prog_step(w->pg, done, it->size)) {
                fclose(f);
                return E_CANCEL;
            }
        }
        fclose(f);
        e = tar_data_pad(w, it->size);
    }
    return e;
}

/* copying the members of an old tar into the new one */
typedef struct {
    AW   *w;
    char **drop;                       /* names being replaced               */
    int   ndrop;
    int   keep;
    int   err;
} TCOPY;

static int tc_begin(void *ctx, AENT *e)
{
    TCOPY *c = ctx;
    int i;
    char nm[260];
    c->keep = 1;
    for (i = 0; i < c->ndrop; i++)
        if (lc_eq(c->drop[i], e->name))
            c->keep = 0;
    if (!c->keep)
        return 0;
    strcpy(nm, e->name);
    if (e->dir)
        strcat(nm, "/");
    c->err = tar_hdr(c->w, nm, e->dir ? '5' : e->rmeth ? e->rmeth : '0',
                     e->usize, e->mode, e->dostime);
    return c->err ? c->err : 1;
}

static int tc_data(void *ctx, const u8 *d, long n)
{
    TCOPY *c = ctx;
    return tar_put(c->w, d, n);
}

static int tc_end(void *ctx, AENT *e)
{
    TCOPY *c = ctx;
    if (c->keep && !e->dir)
        tar_data_pad(c->w, e->usize);
    return E_OK;
}

/* ---------------------------------------------------- gathering items */

typedef struct {
    AITEM *it;
    int    n, cap;
    AOPT  *o;
    int    err;
    const char *self;                  /* the archive: never into itself */
} ILIST;

static int il_push(ILIST *l, const char *path, const char *name, int dir, long size, u32 dt)
{
    AITEM *t;
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 64;
        AITEM *ni = realloc(l->it, nc * sizeof(AITEM));
        if (!ni)
            return E_NOMEM;
        l->it = ni;
        l->cap = nc;
    }
    t = &l->it[l->n++];
    memset(t, 0, sizeof *t);
    strncpy(t->path, path, sizeof t->path - 1);
    strncpy(t->name, name, sizeof t->name - 1);
    t->dir = dir;
    t->size = size;
    t->dostime = dt;
    return E_OK;
}

typedef struct {
    ILIST *l;
    char   dir[256], name[256];
    int    depth;
} WALK;

static int walk_dir(ILIST *l, const char *dir, const char *name, int depth);

static int walk_fn(void *ctx, const char *nm)
{
    WALK *w = ctx;
    char p[300], an[300];
    long size;
    u32 dt;
    int isdir;
    join(p, w->dir, nm);
    if (strlen(p) > 250 || strlen(w->name) + strlen(nm) > 250) {
        ar_log(w->l->o, "Path too long, skipped: %.200s", p);
        w->l->o->nskip++;
        return 0;
    }
    sprintf(an, "%s/%s", w->name, nm);
    if (afs_info(p, &size, &dt, &isdir) != E_OK)
        return 0;
    if (w->l->self && lc_eq(p, w->l->self))
        return 0;
    if (isdir) {
        if (w->l->o->subdirs && w->depth < 32)
            w->l->err = walk_dir(w->l, p, an, w->depth + 1);
    } else
        w->l->err = il_push(w->l, p, an, 0, size, dt);
    return w->l->err != E_OK;
}

static int walk_dir(ILIST *l, const char *dir, const char *name, int depth)
{
    WALK w;
    u32 dt = 0;
    int e;
    afs_info(dir, 0, &dt, 0);
    if ((e = il_push(l, dir, name, 1, 0, dt)) != E_OK)
        return e;
    w.l = l;
    strcpy(w.dir, dir);
    strcpy(w.name, name);
    w.depth = depth;
    afs_scandir(dir, walk_fn, &w);
    return l->err;
}

static int same_file(const char *a, const char *b)
{
    return lc_eq(a, b);
}

/* --------------------------------------------------------------- add */

typedef struct {
    PROG  p;
    PROG *parent;
    long  base, total;
} ADDP;

static int add_prog(PROG *p, long done, long total)
{
    ADDP *a = p->data;
    (void)total;
    return prog_step(a->parent, a->base + done, a->total);
}

static int fmt_has_dirs(int fmt)
{
    return fmt == AF_ZIP || fmt == AF_TAR || fmt == AF_TGZ;
}

int arch_add(const char *path, int fmt, char **files, int nfiles, AOPT *o)
{
    ARCH old;
    ILIST l;
    AW w;
    FILE *of = 0;
    char tmp[300];
    int have_old = 0, i, err = E_OK, k;
    long total = 0;
    char **drop = 0;
    int ndrop = 0;
    u8 *keep = 0;
    ADDP pc;

    memset(&old, 0, sizeof old);
    memset(&l, 0, sizeof l);
    memset(&w, 0, sizeof w);
    l.o = o;
    l.self = path;
    /* what is there already */
    if (afs_isdir(path) == 0) {
        err = arch_open(&old, path);
        if (err == E_OK) {
            have_old = 1;
            fmt = old.fmt;
        } else if (err == E_FORMAT) {
            ar_log(o, "%s is not an archive FArchive knows", ar_base(path));
            return err;
        } else
            return err;
    }
    if (fmt <= AF_NONE || fmt >= AF_COUNT)
        return E_UNSUP;
    /* gather the items */
    for (i = 0; i < nfiles && !err; i++) {
        long size;
        u32 dt;
        int isdir;
        const char *b;
        char nm[256], fp[256];
        size_t n;
        /* "C:\FOLDER\" as some desktops send it: without the backslash */
        strncpy(fp, files[i], sizeof fp - 1);
        fp[sizeof fp - 1] = 0;
        n = strlen(fp);
        while (n > 3 && (fp[n - 1] == '\\' || fp[n - 1] == '/'))
            fp[--n] = 0;
        b = ar_base(fp);
        strncpy(nm, b, sizeof nm - 1);
        nm[sizeof nm - 1] = 0;
        n = strlen(nm);
        while (n && (nm[n - 1] == '\\' || nm[n - 1] == '/'))
            nm[--n] = 0;
        if (same_file(fp, path))
            continue;                  /* not into itself */
        if (afs_info(fp, &size, &dt, &isdir) != E_OK) {
            ar_log(o, "Cannot find %s", fp);
            o->nfail++;
            continue;
        }
        if (isdir)
            err = walk_dir(&l, fp, nm, 0);
        else
            err = il_push(&l, fp, nm, 0, size, dt);
    }
    if (err)
        goto out;
    if (fmt == AF_GZ) {
        int nf = 0;
        for (i = 0; i < l.n; i++)
            nf += !l.it[i].dir;
        if (nf != 1 || have_old) {
            ar_log(o, "A GZ file holds one file; use TAR.GZ for more");
            err = E_UNSUP;
            goto out;
        }
    }
    /* the ARC format has no folders: flat 8.3 names */
    if (fmt == AF_ARC) {
        for (i = 0; i < l.n; i++)
            if (strchr(l.it[i].name, '/') && !l.it[i].dir) {
                ar_log(o, "ARC keeps no folders: %s stored as %s", l.it[i].name, ar_base(l.it[i].name));
                break;
            }
    }
    /* replacing old members */
    if (have_old) {
        keep = malloc(old.n + 1);
        drop = malloc((l.n + 1) * sizeof(char *));
        if (!keep || !drop) {
            err = E_NOMEM;
            goto out;
        }
        memset(keep, 1, old.n + 1);
        for (i = 0; i < l.n; i++) {
            int found = 0;
            for (k = 0; k < old.n; k++) {
                const char *on = old.e[k].name, *nn = l.it[i].name;
                if (fmt == AF_ARC) {
                    char a83[16];
                    name83(nn, a83, 1);
                    nn = a83;
                    if (!lc_eq(on, nn))
                        continue;
                } else if (!lc_eq(on, nn))
                    continue;
                if (old.e[k].dir || l.it[i].dir) {
                    if (old.e[k].dir && l.it[i].dir)
                        found = 2;      /* the folder is there: fine */
                    continue;
                }
                found = 1;
                if (o->replace) {
                    if (old.e[k].group) {
                        ar_log(o, "%s is in a merged LZX group: the old copy stays", on);
                        continue;
                    }
                    keep[k] = 0;
                } else
                    found = 3;
            }
            if (found == 3) {
                ar_log(o, "Skipped %s (already in the archive)", l.it[i].name);
                o->nskip++;
                l.it[i].path[0] = 0;   /* drop the item */
            } else if (found == 2)
                l.it[i].path[0] = 0;
            if (found == 1)
                drop[ndrop++] = l.it[i].name;
        }
    }
    for (i = 0; i < l.n; i++)
        if (l.it[i].path[0])
            total += l.it[i].size;
    if (have_old)
        total += old.csize;
    /* write a new archive next to the old one, then swap them */
    strcpy(tmp, path);
    {
        char *b = (char *)ar_base(tmp);
        strcpy(b, "FARC$$$$.TMP");
        if (b == tmp && strchr(path, ASEP) == 0 && strchr(path, '/') == 0)
            strcpy(tmp, "FARC$$$$.TMP");
    }
    w.f = fopen(tmp, "w+b");
    if (!w.f) {
        ar_log(o, "Cannot write %s", tmp);
        err = E_WRITE;
        goto out;
    }
    w.fmt = fmt;
    w.o = o;
    deflate_level = o->level >= 1 && o->level <= 3 ? o->level : 2;
    w.trunc_ok = afs_truncate(w.f, 0) == E_OK;
    memset(&pc, 0, sizeof pc);
    pc.total = total > 0 ? total : 1;
    pc.parent = o->pg;
    pc.p.fn = add_prog;
    pc.p.data = &pc;
    w.pg = &pc.p;
    switch (fmt) {
    case AF_ZIP: err = zip_begin(&w); break;
    case AF_LHA: err = lha_begin(&w); break;
    case AF_ZOO: err = zoo_begin(&w); break;
    case AF_ARC: err = arcf_begin(&w); break;
    case AF_LZX: err = lzx_begin(&w); break;
    case AF_TAR: bout_file(&w.to, w.f); err = E_OK; break;
    case AF_TGZ: err = gz_begin(&w, 0, 0); break;
    case AF_GZ: {
        AITEM *it = 0;
        for (i = 0; i < l.n; i++)
            if (!l.it[i].dir)
                it = &l.it[i];
        err = gz_begin(&w, ar_base(it->name), (u32)unix_from_dos(it->dostime));
        break;
    }
    }
    /* the old members */
    if (!err && have_old) {
        if (fmt == AF_TAR || fmt == AF_TGZ) {
            TARP *t = calloc(1, sizeof(TARP));
            TCOPY c;
            if (!t) {
                err = E_NOMEM;
                goto fail;
            }
            memset(&c, 0, sizeof c);
            c.w = &w;
            c.drop = drop;
            c.ndrop = ndrop;
            t->ctx = &c;
            t->begin = tc_begin;
            t->data = tc_data;
            t->end = tc_end;
            if (o->item)
                o->item(o->ctx, 0, 0, ar_base(path));
            err = tar_run(path, fmt == AF_TGZ, t, o->pg);
            if (!err)
                err = c.err;
            free(t);
        } else {
            of = fopen(path, "rb");
            if (!of)
                err = E_OPEN;
            for (k = 0; k < old.n && !err; k++) {
                AENT *e = &old.e[k];
                if (fmt == AF_LZX && e->group) {
                    int last = k;
                    while (last < old.n - 1 && old.e[last].csize == 0 && old.e[last + 1].group)
                        last++;
                    err = lzx_copy(&w, of, &old, k, last - k + 1);
                    k = last;
                    continue;
                }
                if (!keep[k])
                    continue;
                switch (fmt) {
                case AF_ZIP: err = zip_copy(&w, of, e); break;
                case AF_LHA: err = lha_copy(&w, of, e); break;
                case AF_ZOO: err = zoo_copy(&w, of, e); break;
                case AF_ARC: err = arcf_copy(&w, of, e); break;
                case AF_LZX: err = lzx_copy(&w, of, &old, k, 1); break;
                }
                pc.base += e->csize;
                if (prog_step(o->pg, pc.base, pc.total))
                    err = E_CANCEL;
            }
            if (of)
                fclose(of);
            of = 0;
        }
    }
    /* the new ones */
    for (i = 0, k = 0; i < l.n && !err; i++) {
        AITEM *it = &l.it[i];
        if (!it->path[0])
            continue;
        if (it->dir && !fmt_has_dirs(fmt))
            continue;
        if (fmt == AF_GZ && it->dir)
            continue;
        if (o->item)
            o->item(o->ctx, ++k, l.n, it->name);
        if (!it->dir) {
            /* make sure it opens before its header is written */
            FILE *t = fopen(it->path, "rb");
            if (!t) {
                ar_log(o, "Cannot read %s", it->path);
                o->nfail++;
                pc.base += it->size;
                continue;
            }
            fclose(t);
        }
        switch (fmt) {
        case AF_ZIP: err = zip_add(&w, it); break;
        case AF_LHA: err = lha_add(&w, it); break;
        case AF_ZOO: err = zoo_add(&w, it); break;
        case AF_ARC: err = arcf_add(&w, it); break;
        case AF_LZX: err = lzx_add(&w, it); break;
        case AF_TAR: case AF_TGZ: err = tar_add(&w, it); break;
        case AF_GZ: {
            FILE *f = fopen(it->path, "rb");
            u8 buf[2048];
            long n, done = 0;
            if (!f) {
                err = E_OPEN;
                break;
            }
            while ((n = (long)fread(buf, 1, sizeof buf, f)) > 0) {
                bout_put(&w.to, buf, n);
                done += n;
                if ((done & 0x7FFF) < (long)sizeof buf && prog_step(w.pg, done, it->size)) {
                    err = E_CANCEL;
                    break;
                }
            }
            fclose(f);
            break;
        }
        }
        pc.base += it->size;
        if (err == E_OPEN) {
            ar_log(o, "Cannot read %s", it->path);
            o->nfail++;
            err = E_OK;
            continue;
        }
        if (!err && !it->dir) {
            o->nok++;
            if (it->size > 0 && w.out.n > 0 && fmt != AF_TAR && fmt != AF_TGZ && fmt != AF_GZ) {
                AENT *e = &w.out.e[w.out.n - 1];
                long pct = e->usize ? 100 - (e->csize * 100 + e->usize / 2) / e->usize : 0;
                ar_log(o, "Added %s (%s, %ld%%)", it->name, e->mname, pct < 0 ? 0 : pct);
            } else
                ar_log(o, "Added %s", it->name);
        }
    }
    if (!err) {
        switch (fmt) {
        case AF_ZIP: err = zip_end(&w); break;
        case AF_LHA: err = lha_end(&w); break;
        case AF_ZOO: err = zoo_end(&w); break;
        case AF_ARC: err = arcf_end(&w); break;
        case AF_LZX: err = lzx_end(&w); break;
        case AF_TAR: {
            u8 z[1024];
            memset(z, 0, sizeof z);
            err = tar_put(&w, z, sizeof z);
            /* round to 10K records like tar does */
            {
                long sz = w.to.count, rec = 10240;
                while (!err && sz % rec) {
                    long k2 = rec - sz % rec;
                    if (k2 > (long)sizeof z) k2 = sizeof z;
                    err = tar_put(&w, z, k2);
                    sz += k2;
                }
            }
            if (!err && bout_flush(&w.to))
                err = E_WRITE;
            break;
        }
        case AF_TGZ: {
            u8 z[1024];
            memset(z, 0, sizeof z);
            err = tar_put(&w, z, sizeof z);
            if (!err)
                err = gz_end(&w);
            break;
        }
        case AF_GZ: err = gz_end(&w); break;
        }
    }
    if (!err && w.hiwater > aw_tell(&w))
        afs_truncate(w.f, aw_tell(&w));
fail:
    if (w.defl)
        defl_free(w.defl);
    if (fclose(w.f) != 0 && !err)
        err = E_WRITE;
    if (err) {
        remove(tmp);
        if (err != E_CANCEL)
            ar_log(o, "Could not write %s: %s", ar_base(path), fv_errstr(err));
    } else {
        /* the old archive steps aside first, and only goes once the new
           one has its name */
        char bak[300];
        strcpy(bak, tmp);
        strcpy(bak + strlen(bak) - 3, "OLD");
        remove(bak);
        if (have_old && afs_rename(path, bak) != E_OK) {
            ar_log(o, "Cannot replace %s", path);
            remove(tmp);
            err = E_WRITE;
        } else if (afs_rename(tmp, path) != E_OK) {
            ar_log(o, "Cannot rename %s to %s", tmp, path);
            if (have_old)
                afs_rename(bak, path);
            err = E_WRITE;
        } else if (have_old)
            remove(bak);
    }
out:
    arch_close(&old);
    arch_close(&w.out);
    free(l.it);
    free(drop);
    free(keep);
    return err;
}
