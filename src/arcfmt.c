/*
 * arcfmt.c - member headers of ZIP, LHA, ZOO, ARC and LZX archives:
 * reading the directory, writing new members, copying old ones.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arcint.h"

static void slashes(char *s)
{
    for (; *s; s++)
        if (*s == '\\' || (u8)*s == 0xFF)
            *s = '/';
}

static void out_ent(AW *w, AENT *e)
{
    ar_push(&w->out, e);
}

/* =============================================================== ZIP */

static const char *zip_mname(unsigned m)
{
    switch (m) {
    case 0: return "stored";
    case 1: return "shrunk";
    case 2: case 3: case 4: case 5: return "reduced";
    case 6: return "implode";
    case 8: return "deflate";
    case 9: return "defl64";
    case 12: return "bzip2";
    case 14: return "lzma";
    }
    return "?";
}

static int zip_scan_local(ARCH *a, FILE *f)
{
    /* no central directory: walk the local headers */
    long pos = 0;
    u8 h[30];
    for (;;) {
        AENT e;
        unsigned nl, xl;
        fseek(f, pos, SEEK_SET);
        if (fread(h, 1, 30, f) != 30 || g32(h) != 0x04034B50UL)
            break;
        memset(&e, 0, sizeof e);
        e.rflags = (u16)g16(h + 6);
        e.rmeth = (u16)g16(h + 8);
        e.dostime = (u32)g16(h + 12) << 16 | g16(h + 10);
        e.crc = g32(h + 14);
        e.csize = (long)g32(h + 18);
        e.usize = (long)g32(h + 22);
        nl = g16(h + 26);
        xl = g16(h + 28);
        if (nl > 255 || fread(e.name, 1, nl, f) != nl)
            return E_FORMAT;
        e.name[nl] = 0;
        e.hdr = pos;
        e.off = pos + 30 + nl + xl;
        if (e.rflags & 8)
            return a->n ? E_OK : E_UNSUP;  /* sizes after the data */
        pos = e.off + e.csize;
        slashes(e.name);
        if (e.name[0] && e.name[strlen(e.name) - 1] == '/') {
            e.dir = 1;
            e.name[strlen(e.name) - 1] = 0;
        }
        e.crckind = 2;
        e.method = e.rmeth == 0 ? CM_STORE : e.rmeth == 8 ? CM_DEFLATE : -1;
        if (e.rflags & 1) {
            e.method = -1;
            ar_mname(&e, "crypt");
        } else
            ar_mname(&e, zip_mname(e.rmeth));
        if (ar_push(a, &e))
            return E_NOMEM;
    }
    return E_OK;
}

int zip_list(ARCH *a, FILE *f)
{
    long size, back, i, cdoff;
    unsigned n, k;
    u8 *t, h[46];
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    back = size < 65557L ? size : 65557L;
    t = malloc(back);
    if (!t)
        return E_NOMEM;
    fseek(f, size - back, SEEK_SET);
    if ((long)fread(t, 1, back, f) != back) {
        free(t);
        return E_FORMAT;
    }
    for (i = back - 22; i >= 0; i--)
        if (t[i] == 'P' && t[i + 1] == 'K' && t[i + 2] == 5 && t[i + 3] == 6)
            break;
    if (i < 0) {
        free(t);
        return zip_scan_local(a, f);
    }
    n = g16(t + i + 10);
    cdoff = (long)g32(t + i + 16);
    free(t);
    for (k = 0; k < n; k++) {
        AENT e;
        unsigned nl, xl, cl, lnl, lxl;
        u8 lh[30];
        long lho;
        fseek(f, cdoff, SEEK_SET);
        if (fread(h, 1, 46, f) != 46 || g32(h) != 0x02014B50UL)
            return a->n ? E_OK : E_FORMAT;
        memset(&e, 0, sizeof e);
        e.rflags = (u16)g16(h + 8);
        e.rmeth = (u16)g16(h + 10);
        e.dostime = (u32)g16(h + 14) << 16 | g16(h + 12);
        e.crc = g32(h + 16);
        e.csize = (long)g32(h + 20);
        e.usize = (long)g32(h + 24);
        nl = g16(h + 28);
        xl = g16(h + 30);
        cl = g16(h + 32);
        e.attr = h[38];
        lho = (long)g32(h + 42);
        if (fread(e.name, 1, nl > 255 ? 255 : nl, f) != (nl > 255 ? 255 : nl))
            return E_FORMAT;
        e.name[nl > 255 ? 255 : nl] = 0;
        cdoff += 46 + nl + xl + cl;
        fseek(f, lho, SEEK_SET);
        if (fread(lh, 1, 30, f) != 30 || g32(lh) != 0x04034B50UL)
            continue;
        lnl = g16(lh + 26);
        lxl = g16(lh + 28);
        e.hdr = lho;
        e.off = lho + 30 + lnl + lxl;
        slashes(e.name);
        if ((e.name[0] && e.name[strlen(e.name) - 1] == '/') || (e.attr & 0x10)) {
            e.dir = 1;
            if (e.name[0] && e.name[strlen(e.name) - 1] == '/')
                e.name[strlen(e.name) - 1] = 0;
        }
        e.crckind = 2;
        e.method = e.rmeth == 0 ? CM_STORE : e.rmeth == 8 ? CM_DEFLATE : -1;
        if (e.rflags & 1) {
            e.method = -1;
            ar_mname(&e, "crypt");
        } else
            ar_mname(&e, zip_mname(e.rmeth));
        if (ar_push(a, &e))
            return E_NOMEM;
    }
    return E_OK;
}

int zip_begin(AW *w)
{
    (void)w;
    return E_OK;
}

int zip_add(AW *w, AITEM *it)
{
    u8 h[30];
    AENT e;
    char nm[260];
    size_t nl;
    int m = CM_DEFLATE, err;
    long csize = 0;
    u32 crc = 0;
    unsigned c16;
    memset(&e, 0, sizeof e);
    strcpy(nm, it->name);
    if (it->dir)
        strcat(nm, "/");
    nl = strlen(nm);
    memset(h, 0, sizeof h);
    p32(h, 0x04034B50UL);
    p16(h + 4, 20);
    p16(h + 10, (unsigned)(it->dostime & 0xFFFF));
    p16(h + 12, (unsigned)(it->dostime >> 16));
    p16(h + 26, (unsigned)nl);
    e.hdr = aw_tell(w);
    if ((err = aw_write(w, h, 30)) != E_OK || (err = aw_write(w, nm, (long)nl)) != E_OK)
        return err;
    if (!it->dir) {
        err = aw_pack(w, it, &m, w->trunc_ok, &csize, &crc, &c16);
        if (err)
            return err;
    } else
        m = CM_STORE;
    e.rmeth = (u16)(m == CM_DEFLATE ? 8 : 0);
    p16(h + 4, m == CM_DEFLATE ? 20 : 10);
    p16(h + 8, e.rmeth);
    p32(h + 14, crc);
    p32(h + 18, (u32)csize);
    p32(h + 22, (u32)(it->dir ? 0 : it->size));
    if ((err = aw_seekpatch(w, e.hdr, h, 30)) != E_OK)
        return err;
    strncpy(e.name, nm, sizeof e.name - 1);
    e.usize = it->dir ? 0 : it->size;
    e.csize = csize;
    e.crc = crc;
    e.dostime = it->dostime;
    e.attr = it->dir ? 0x10 : 0x20;
    e.dir = (u8)it->dir;
    ar_mname(&e, m == CM_DEFLATE ? "deflate" : "stored");
    out_ent(w, &e);
    return E_OK;
}

int zip_copy(AW *w, FILE *of, const AENT *e)
{
    AENT n = *e;
    long len = e->off - e->hdr + e->csize;
    int err;
    if (e->rflags & 8) {               /* a data descriptor follows */
        u8 s[4];
        fseek(of, e->off + e->csize, SEEK_SET);
        len += fread(s, 1, 4, of) == 4 && g32(s) == 0x08074B50UL ? 16 : 12;
    }
    n.hdr = aw_tell(w);
    if ((err = aw_copy(w, of, e->hdr, len)) != E_OK)
        return err;
    if (e->dir)
        strcat(n.name, "/");
    out_ent(w, &n);
    return E_OK;
}

int zip_end(AW *w)
{
    long cd = aw_tell(w), cdsize;
    int i, err;
    u8 h[46];
    for (i = 0; i < w->out.n; i++) {
        AENT *e = &w->out.e[i];
        size_t nl = strlen(e->name);
        memset(h, 0, sizeof h);
        p32(h, 0x02014B50UL);
        p16(h + 4, 20);
        p16(h + 6, e->rmeth == 8 ? 20 : 10);
        p16(h + 8, e->rflags);
        p16(h + 10, e->rmeth);
        p16(h + 12, (unsigned)(e->dostime & 0xFFFF));
        p16(h + 14, (unsigned)(e->dostime >> 16));
        p32(h + 16, e->crc);
        p32(h + 20, (u32)e->csize);
        p32(h + 24, (u32)e->usize);
        p16(h + 28, (unsigned)nl);
        h[38] = e->attr;
        p32(h + 42, (u32)e->hdr);
        if ((err = aw_write(w, h, 46)) != E_OK || (err = aw_write(w, e->name, (long)nl)) != E_OK)
            return err;
    }
    cdsize = aw_tell(w) - cd;
    memset(h, 0, 22);
    p32(h, 0x06054B50UL);
    p16(h + 8, (unsigned)w->out.n);
    p16(h + 10, (unsigned)w->out.n);
    p32(h + 12, (u32)cdsize);
    p32(h + 16, (u32)cd);
    return aw_write(w, h, 22);
}

/* =============================================================== LHA */

static int lha_method(const u8 *m, AENT *e)
{
    char s[6];
    memcpy(s, m, 5);
    s[5] = 0;
    ar_mname(e, s);
    if (s[0] != '-' || s[4] != '-')
        return -1;
    if (!memcmp(s, "-lh0-", 5) || !memcmp(s, "-lz4-", 5)) return CM_STORE;
    if (!memcmp(s, "-lh1-", 5)) return CM_LH1;
    if (!memcmp(s, "-lh4-", 5)) return CM_LH4;
    if (!memcmp(s, "-lh5-", 5)) return CM_LH5;
    if (!memcmp(s, "-lh6-", 5)) return CM_LH6;
    if (!memcmp(s, "-lh7-", 5)) return CM_LH7;
    if (!memcmp(s, "-lzs-", 5)) return CM_LZS;
    if (!memcmp(s, "-lz5-", 5)) return CM_LZ5;
    if (!memcmp(s, "-lhd-", 5)) {
        e->dir = 1;
        return CM_STORE;
    }
    return -1;
}

int lha_list(ARCH *a, FILE *f)
{
    long pos = 0;
    for (;;) {
        u8 h[256 + 32];
        AENT e;
        int level, hs;
        long ext = 0, extpos;
        unsigned nx;
        int w4 = 0;
        char dir[256];
        fseek(f, pos, SEEK_SET);
        if (fread(h, 1, 22, f) != 22 || h[0] == 0)
            break;
        level = h[20];
        memset(&e, 0, sizeof e);
        dir[0] = 0;
        e.method = lha_method(h + 2, &e);
        e.csize = (long)g32(h + 7);
        e.usize = (long)g32(h + 11);
        e.attr = h[19];
        e.hdr = pos;
        e.crckind = 1;
        if (level == 0 || level == 1) {
            int nl;
            hs = h[0] + 2;
            if (fread(h + 22, 1, hs - 22, f) != (size_t)(hs - 22))
                return a->n ? E_OK : E_FORMAT;
            e.dostime = (u32)g16(h + 17) << 16 | g16(h + 15);
            nl = h[21];
            if (22 + nl + 2 > hs)
                return a->n ? E_OK : E_FORMAT;
            memcpy(e.name, h + 22, nl);
            e.name[nl] = 0;
            e.crc = g16(h + 22 + nl);
            extpos = pos + hs;
            nx = level == 1 ? g16(h + hs - 2) : 0;
        } else if (level == 2) {
            u8 x[2];
            fseek(f, pos, SEEK_SET);
            if (fread(x, 1, 2, f) != 2)
                break;
            hs = (int)g16(x);
            if (hs < 26 || hs > (int)sizeof h)
                return a->n ? E_OK : E_FORMAT;
            fseek(f, pos, SEEK_SET);
            if (fread(h, 1, 26, f) != 26)
                return E_FORMAT;
            e.dostime = dos_from_unix((long)g32(h + 15));
            e.crc = g16(h + 21);
            extpos = pos + 26;
            nx = g16(h + 24);
        } else if (level == 3) {
            fseek(f, pos, SEEK_SET);
            if (fread(h, 1, 32, f) != 32 || g16(h) != 4)
                return a->n ? E_OK : E_UNSUP;
            hs = (int)g32(h + 24);
            e.dostime = dos_from_unix((long)g32(h + 15));
            e.crc = g16(h + 21);
            extpos = pos + 32;
            nx = (unsigned)g32(h + 28);
            w4 = 1;
        } else
            return a->n ? E_OK : E_UNSUP;
        /* extended headers: type, data, size of the next one */
        while (nx >= (unsigned)(w4 ? 5 : 3)) {
            u8 xh[300];
            unsigned n = nx > sizeof xh ? sizeof xh : nx;
            int tail = w4 ? 4 : 2;
            fseek(f, extpos, SEEK_SET);
            if (fread(xh, 1, n, f) != n)
                return E_FORMAT;
            if (n < nx) {                  /* too long to keep: skip it */
                u8 t4[4];
                fseek(f, extpos + nx - tail, SEEK_SET);
                if (fread(t4, 1, tail, f) != (size_t)tail)
                    return E_FORMAT;
                extpos += nx;
                ext += nx;
                nx = w4 ? (unsigned)g32(t4) : g16(t4);
                continue;
            }
            if (xh[0] == 1 || xh[0] == 2) {
                int k = (int)nx - 1 - tail;
                char *dst = xh[0] == 1 ? e.name : dir;
                if (k > 250) k = 250;
                memcpy(dst, xh + 1, k);
                dst[k] = 0;
            } else if (xh[0] == 0x54 && nx >= 7)
                e.dostime = dos_from_unix((long)g32(xh + 1));
            else if (xh[0] == 0x40 && nx >= 5)
                e.attr = xh[1];
            extpos += nx;
            ext += nx;
            nx = w4 ? (unsigned)g32(xh + nx - 4) : g16(xh + nx - 2);
        }
        if (level == 1)
            e.csize -= ext;
        /* level 2: the header size, or past the extended headers when a
           writer (OS-9 LHA) got the size wrong */
        e.off = level >= 2 ? (pos + hs > extpos ? pos + hs : extpos) : extpos;
        if (dir[0]) {
            char full[256];
            size_t dl = strlen(dir);
            strcpy(full, dir);
            if (full[dl - 1] != (char)0xFF && full[dl - 1] != '/')
                strcat(full, "/");
            if (strlen(full) + strlen(e.name) < sizeof full)
                strcat(full, e.name);
            strcpy(e.name, full);
        }
        slashes(e.name);
        if (strchr(e.name, '|')) {     /* unix LHA symlink: "name|target" */
            *strchr(e.name, '|') = 0;
            e.method = -1;
            e.dir = 0;
            ar_mname(&e, "link");
        }
        if (e.dir) {
            size_t n = strlen(e.name);
            if (n && e.name[n - 1] == '/')
                e.name[n - 1] = 0;
        }
        e.hlen = e.off - pos;
        if (e.csize < 0)
            return a->n ? E_OK : E_FORMAT;
        if (ar_push(a, &e))
            return E_NOMEM;
        pos = e.off + e.csize;
    }
    return E_OK;
}

int lha_begin(AW *w)
{
    (void)w;
    return E_OK;
}

int lha_add(AW *w, AITEM *it)
{
    u8 h[300];
    char nm[256];
    int nl, hs, i, err, m = CM_LH5;
    unsigned sum = 0, c16 = 0;
    long csize = 0;
    u32 c32;
    AENT e;
    strcpy(nm, it->name);
    for (i = 0; nm[i]; i++)
        if (nm[i] == '/')
            nm[i] = '\\';
    nl = (int)strlen(nm);
    if (nl > 230)
        nl = 230;
    hs = 27 + nl;                      /* level 1, no extended headers */
    memset(h, 0, sizeof h);
    h[0] = (u8)(hs - 2);
    memcpy(h + 2, "-lh5-", 5);
    p16(h + 15, (unsigned)(it->dostime & 0xFFFF));
    p16(h + 17, (unsigned)(it->dostime >> 16));
    h[19] = 0x20;
    h[20] = 1;
    h[21] = (u8)nl;
    memcpy(h + 22, nm, nl);
    h[24 + nl] = 'M';
    memset(&e, 0, sizeof e);
    e.hdr = aw_tell(w);
    if ((err = aw_write(w, h, hs)) != E_OK)
        return err;
    if ((err = aw_pack(w, it, &m, 1, &csize, &c32, &c16)) != E_OK)
        return err;
    memcpy(h + 2, m == CM_LH5 ? "-lh5-" : "-lh0-", 5);
    p32(h + 7, (u32)csize);
    p32(h + 11, (u32)it->size);
    p16(h + 22 + nl, c16);
    for (i = 2; i < hs; i++)
        sum += h[i];
    h[1] = (u8)sum;
    if ((err = aw_seekpatch(w, e.hdr, h, hs)) != E_OK)
        return err;
    strcpy(e.name, it->name);
    e.usize = it->size;
    e.csize = csize;
    ar_mname(&e, m == CM_LH5 ? "-lh5-" : "-lh0-");
    out_ent(w, &e);
    return E_OK;
}

int lha_copy(AW *w, FILE *of, const AENT *e)
{
    return aw_copy(w, of, e->hdr, e->hlen + e->csize);
}

int lha_end(AW *w)
{
    u8 z = 0;
    return aw_write(w, &z, 1);
}

/* =============================================================== ZOO */

#define ZOO_TAG 0xFDC4A7DCUL

int zoo_list(ARCH *a, FILE *f)
{
    u8 h[64];
    long pos;
    fseek(f, 0, SEEK_SET);
    if (fread(h, 1, 34, f) != 34)
        return E_FORMAT;
    pos = (long)g32(h + 24);
    while (pos > 0) {
        AENT e;
        u8 d[56 + 600];
        int type, var = 0;
        long next;
        fseek(f, pos, SEEK_SET);
        if (fread(d, 1, 51, f) != 51 || g32(d) != ZOO_TAG)
            return a->n ? E_OK : E_FORMAT;
        type = d[4];
        next = (long)g32(d + 6);
        if (!next)
            break;
        memset(&e, 0, sizeof e);
        if (type == 2) {
            if (fread(d + 51, 1, 5, f) != 5)
                return E_FORMAT;
            var = (int)g16(d + 51);
            if (var > 600) var = 600;
            if (var && fread(d + 56, 1, var, f) != (size_t)var)
                return E_FORMAT;
        }
        e.rmeth = d[5];
        e.off = (long)g32(d + 10);
        e.dostime = (u32)g16(d + 14) << 16 | g16(d + 16);
        e.crc = g16(d + 18);
        e.crckind = 1;
        e.usize = (long)g32(d + 20);
        e.csize = (long)g32(d + 24);
        e.hdr = pos;
        e.hlen = type == 2 ? 56 + var : 51;
        memcpy(e.name, d + 38, 13);
        e.name[13] = 0;
        if (type == 2 && var >= 2) {
            int nl = d[56], dl = d[57];
            char lf[256], dn[256];
            lf[0] = dn[0] = 0;
            if (nl && 58 + nl <= 56 + var) {
                memcpy(lf, d + 58, nl);
                lf[nl] = 0;
            }
            if (dl && 58 + nl + dl <= 56 + var) {
                memcpy(dn, d + 58 + nl, dl);
                dn[dl] = 0;
            }
            if (lf[0])
                strcpy(e.name, lf);
            if (dn[0]) {
                char full[300], *s = dn;
                while (*s == '/' || *s == '\\')
                    s++;
                if (*s && strlen(s) + strlen(e.name) + 2 < sizeof e.name) {
                    sprintf(full, "%s/%s", s, e.name);
                    strcpy(e.name, full);
                }
            }
        }
        slashes(e.name);
        e.method = e.rmeth == 0 ? CM_STORE : e.rmeth == 1 ? CM_ZOOLZW : e.rmeth == 2 ? CM_LH5 : -1;
        ar_mname(&e, e.rmeth == 0 ? "stored" : e.rmeth == 1 ? "lzw" : e.rmeth == 2 ? "lh5" : "?");
        if (!d[30] && ar_push(a, &e))  /* d[30]: deleted */
            return E_NOMEM;
        pos = next;
    }
    return E_OK;
}

int zoo_begin(AW *w)
{
    u8 h[42];
    memset(h, 0, sizeof h);
    memcpy(h, "ZOO 2.10 Archive.\x1a", 18);
    p32(h + 20, ZOO_TAG);
    p32(h + 24, 42);
    p32(h + 28, (u32)-42L);
    h[32] = 2;
    h[33] = 0;
    h[34] = 1;
    h[41] = 3;
    return aw_write(w, h, 42);
}

/* the CRC over a directory entry with its CRC field zero */
static void zoo_dcrc(u8 *d, int len)
{
    p16(d + 54, 0);
    p16(d + 54, crc16_update(0, d, len));
}

/* a type 2 entry for name at pos; returns its length */
static int zoo_dir(u8 *d, const char *name, const AENT *e, long next, long off)
{
    char fn[16], dn[256], lf[256];
    const char *b = ar_base(name);
    int nl, dl, var, k;
    memset(d, 0, 56);
    p32(d, ZOO_TAG);
    d[4] = 2;
    d[5] = (u8)e->rmeth;
    p32(d + 6, (u32)next);
    p32(d + 10, (u32)off);
    p16(d + 14, (unsigned)(e->dostime >> 16));
    p16(d + 16, (unsigned)(e->dostime & 0xFFFF));
    p16(d + 18, (unsigned)e->crc);
    p32(d + 20, (u32)e->usize);
    p32(d + 24, (u32)e->csize);
    d[28] = e->rmeth == 2 ? 2 : 1;
    d[29] = e->rmeth == 2 ? 1 : 0;
    name83(b, fn, 0);
    memcpy(d + 38, fn, strlen(fn));
    d[53] = 127;                       /* no time zone */
    strcpy(lf, strcmp(fn, b) ? b : "");
    k = (int)(b - name);
    memcpy(dn, name, k);
    dn[k] = 0;
    if (k && dn[k - 1] == '/')
        dn[--k] = 0;
    nl = lf[0] ? (int)strlen(lf) + 1 : 0;
    dl = dn[0] ? (int)strlen(dn) + 1 : 0;
    d[56] = (u8)nl;
    d[57] = (u8)dl;
    memcpy(d + 58, lf, nl);
    memcpy(d + 58 + nl, dn, dl);
    k = 58 + nl + dl;
    memset(d + k, 0, 8);               /* system id, attributes, version */
    var = nl + dl + 10;
    p16(d + 51, (unsigned)var);
    zoo_dcrc(d, 56 + var);
    return 56 + var;
}

int zoo_add(AW *w, AITEM *it)
{
    u8 d[600];
    AENT e;
    long pos = aw_tell(w), dpos;
    int len, err, m = CM_ZOOLZW;
    u32 c32;
    unsigned c16 = 0;
    memset(&e, 0, sizeof e);
    e.rmeth = 1;
    e.dostime = it->dostime;
    e.usize = it->size;
    len = zoo_dir(d, it->name, &e, 0, 0);
    if ((err = aw_write(w, d, len)) != E_OK)
        return err;
    dpos = pos + len;
    if ((err = aw_pack(w, it, &m, 1, &e.csize, &c32, &c16)) != E_OK)
        return err;
    e.rmeth = (u16)(m == CM_ZOOLZW ? 1 : 0);
    e.crc = c16;
    len = zoo_dir(d, it->name, &e, aw_tell(w), dpos);
    if ((err = aw_seekpatch(w, pos, d, len)) != E_OK)
        return err;
    strcpy(e.name, it->name);
    ar_mname(&e, m == CM_ZOOLZW ? "lzw" : "stored");
    out_ent(w, &e);
    return E_OK;
}

int zoo_copy(AW *w, FILE *of, const AENT *e)
{
    u8 d[56 + 600];
    long pos = aw_tell(w);
    int err, len = (int)e->hlen;
    if (len > (int)sizeof d)
        return E_FORMAT;
    fseek(of, e->hdr, SEEK_SET);
    if (fread(d, 1, len, of) != (size_t)len)
        return E_FORMAT;
    if (len < 56) {                    /* type 1: make it a type 2 entry */
        AENT n = *e;
        char nm[256];
        strcpy(nm, e->name);
        len = zoo_dir(d, nm, &n, pos + 0, 0);
        len = zoo_dir(d, nm, &n, pos + len + e->csize, pos + len);
    } else {
        p32(d + 6, (u32)(pos + len + e->csize));
        p32(d + 10, (u32)(pos + len));
        zoo_dcrc(d, len);
    }
    if ((err = aw_write(w, d, len)) != E_OK)
        return err;
    if ((err = aw_copy(w, of, e->off, e->csize)) != E_OK)
        return err;
    out_ent(w, (AENT *)e);
    return E_OK;
}

int zoo_end(AW *w)
{
    u8 d[56];
    memset(d, 0, sizeof d);
    p32(d, ZOO_TAG);
    d[4] = 2;
    zoo_dcrc(d, 56);
    return aw_write(w, d, 56);
}

/* =============================================================== ARC */

int arcf_list(ARCH *a, FILE *f)
{
    long pos = 0;
    for (;;) {
        u8 h[29];
        AENT e;
        int m, hl;
        fseek(f, pos, SEEK_SET);
        if (fread(h, 1, 2, f) != 2 || h[0] != 0x1A || h[1] == 0)
            break;
        m = h[1];
        hl = m == 1 ? 25 : 29;
        if (fread(h + 2, 1, hl - 2, f) != (size_t)(hl - 2))
            return a->n ? E_OK : E_FORMAT;
        memset(&e, 0, sizeof e);
        memcpy(e.name, h + 2, 13);
        e.name[12] = 0;
        e.csize = (long)g32(h + 15);
        e.dostime = (u32)g16(h + 19) << 16 | g16(h + 21);
        e.crc = g16(h + 23);
        e.crckind = 1;
        e.usize = m == 1 ? e.csize : (long)g32(h + 25);
        e.hdr = pos;
        e.off = pos + hl;
        e.hlen = hl;
        e.rmeth = (u16)m;
        switch (m) {
        case 1: case 2: e.method = CM_STORE; ar_mname(&e, "stored"); break;
        case 3: e.method = CM_ARCPACK; ar_mname(&e, "packed"); break;
        case 4: e.method = CM_ARCSQUEEZE; ar_mname(&e, "squeeze"); break;
        case 5: e.method = CM_ARCOLD5; ar_mname(&e, "crunch5"); break;
        case 6: e.method = CM_ARCOLD6; ar_mname(&e, "crunch6"); break;
        case 7: e.method = CM_ARCOLD7; ar_mname(&e, "crunch7"); break;
        case 8: e.method = CM_ARCCRUNCH; ar_mname(&e, "crunch"); break;
        case 9: e.method = CM_ARCSQUASH; ar_mname(&e, "squash"); break;
        default: e.method = -1; ar_mname(&e, "?"); break;
        }
        if (m < 20 && ar_push(a, &e))  /* 20+: PAK extras, skipped */
            return E_NOMEM;
        pos = e.off + e.csize;
    }
    return E_OK;
}

int arcf_begin(AW *w)
{
    (void)w;
    return E_OK;
}

int arcf_add(AW *w, AITEM *it)
{
    u8 h[29];
    AENT e;
    char nm[16];
    long pos = aw_tell(w);
    int err, m = CM_ARCCRUNCH, i;
    u32 c32;
    unsigned c16 = 0;
    memset(&e, 0, sizeof e);
    name83(it->name, nm, 1);
    /* the same name twice in an ARC: make it unique */
    for (i = 0; i < w->out.n; i++)
        if (!strcmp(w->out.e[i].name, nm)) {
            char base[16];
            int n = 1, k;
            do {
                char *dot;
                sprintf(base, "%s", nm);
                dot = strchr(base, '.');
                if (dot) *dot = 0;
                base[6] = 0;
                sprintf(base + strlen(base), "~%d", n++);
                dot = strchr(nm, '.');
                if (dot)
                    strcat(base, dot);
                for (k = 0; k < w->out.n && strcmp(w->out.e[k].name, base); k++)
                    ;
            } while (k < w->out.n && n < 10);
            ar_log(w->o, "%s stored as %s", it->name, base);
            strcpy(nm, base);
            break;
        }
    memset(h, 0, sizeof h);
    h[0] = 0x1A;
    h[1] = 8;
    memcpy(h + 2, nm, strlen(nm));
    p16(h + 19, (unsigned)(it->dostime >> 16));
    p16(h + 21, (unsigned)(it->dostime & 0xFFFF));
    if ((err = aw_write(w, h, 29)) != E_OK)
        return err;
    if ((err = aw_pack(w, it, &m, 1, &e.csize, &c32, &c16)) != E_OK)
        return err;
    h[1] = (u8)(m == CM_ARCCRUNCH ? 8 : 2);
    p32(h + 15, (u32)e.csize);
    p16(h + 23, c16);
    p32(h + 25, (u32)it->size);
    if ((err = aw_seekpatch(w, pos, h, 29)) != E_OK)
        return err;
    strcpy(e.name, nm);
    e.usize = it->size;
    ar_mname(&e, m == CM_ARCCRUNCH ? "crunch" : "stored");
    out_ent(w, &e);
    return E_OK;
}

int arcf_copy(AW *w, FILE *of, const AENT *e)
{
    int err = aw_copy(w, of, e->hdr, e->hlen + e->csize);
    if (!err)
        out_ent(w, (AENT *)e);
    return err;
}

int arcf_end(AW *w)
{
    u8 z[2];
    z[0] = 0x1A;
    z[1] = 0;
    return aw_write(w, z, 2);
}

/* =============================================================== LZX */

static u32 lzx_dos(const u8 *p)
{
    u32 t = b32(p);
    int yf = (int)((t >> 17) & 63), y;
    int day = (int)((t >> 27) & 31), mon = (int)((t >> 23) & 15) + 1;
    int hr = (int)((t >> 12) & 31), mi = (int)((t >> 6) & 63), se = (int)(t & 63);
    if (!t)
        return 0;
    y = yf <= 7 ? yf + 2034 : yf <= 29 ? yf + 1970 : yf <= 57 ? yf + 1976 : yf + 1942;
    if (y < 1980) y = 1980;
    if (!day) day = 1;
    return (u32)((y - 1980) << 9 | mon << 5 | day) << 16 | (u32)(hr << 11 | mi << 5 | se / 2);
}

static void lzx_amiga(u32 dt, u8 *p)
{
    unsigned date = (unsigned)(dt >> 16), tm = (unsigned)(dt & 0xFFFF);
    int y = 1980 + (int)(date >> 9), m = (date >> 5) & 15, d = date & 31, yf;
    u32 t;
    if (!dt) {
        memset(p, 0, 4);
        return;
    }
    yf = y <= 1999 ? y - 1970 : y <= 2005 ? y - 1942 : y <= 2033 ? y - 1976 : y <= 2041 ? y - 2034 : 0;
    t = (u32)d << 27 | (u32)((m ? m - 1 : 0) & 15) << 23 | (u32)yf << 17 |
        (u32)(tm >> 11) << 12 | (u32)((tm >> 5) & 63) << 6 | (u32)((tm & 31) * 2);
    p[0] = (u8)(t >> 24); p[1] = (u8)(t >> 16); p[2] = (u8)(t >> 8); p[3] = (u8)t;
}

int lzx_list(ARCH *a, FILE *f)
{
    long pos = 10;
    int ingroup = 0;
    for (;;) {
        u8 h[31];
        AENT e;
        int nl, cl;
        fseek(f, pos, SEEK_SET);
        if (fread(h, 1, 31, f) != 31)
            break;
        memset(&e, 0, sizeof e);
        e.attr = 0;
        e.usize = (long)g32(h + 2);
        e.csize = (long)g32(h + 6);
        e.rmeth = h[11];
        e.group = h[12] ? 1 : 0;
        cl = h[14];
        e.dostime = lzx_dos(h + 18);
        e.crc = g32(h + 22);
        e.crckind = 2;
        nl = h[30];
        if (fread(e.name, 1, nl, f) != (size_t)nl)
            return a->n ? E_OK : E_FORMAT;
        e.name[nl] = 0;
        e.hdr = pos;
        e.off = pos + 31 + nl + cl;
        e.hlen = 31 + nl + cl;
        e.method = e.rmeth == 0 ? CM_STORE : e.rmeth == 2 ? CM_LZX : -1;
        ar_mname(&e, e.rmeth == 0 ? "stored" : e.rmeth == 2 ? "lzx" : "?");
        if (e.group && !ingroup)
            e.gstart = a->n;
        ingroup = e.group && e.csize == 0;
        {
            char *s = strchr(e.name, ':');
            if (s)
                memmove(e.name, s + 1, strlen(s));
        }
        if (ar_push(a, &e))
            return E_NOMEM;
        pos = e.off + e.csize;
    }
    return E_OK;
}

int lzx_begin(AW *w)
{
    u8 h[10];
    int i;
    unsigned sum = 0;
    memset(h, 0, sizeof h);
    memcpy(h, "LZX", 3);
    h[6] = 0x0A;
    h[7] = 0x04;
    for (i = 0; i < 10; i++)
        sum += h[i];
    h[4] = (u8)sum;
    return aw_write(w, h, 10);
}

static void lzx_hcrc(u8 *h, int len)
{
    p32(h + 26, 0);
    p32(h + 26, crc32_update(0xFFFFFFFFUL, h, len) ^ 0xFFFFFFFFUL);
}

int lzx_add(AW *w, AITEM *it)
{
    u8 h[31 + 256];
    AENT e;
    long pos = aw_tell(w);
    int nl = (int)strlen(it->name), err, m = CM_LZX;
    u32 c32 = 0;
    unsigned c16;
    if (nl > 255)
        nl = 255;
    memset(h, 0, sizeof h);
    memset(&e, 0, sizeof e);
    h[10] = 0x0A;
    h[11] = 2;
    h[15] = 0x0A;
    lzx_amiga(it->dostime, h + 18);
    h[30] = (u8)nl;
    memcpy(h + 31, it->name, nl);
    if ((err = aw_write(w, h, 31 + nl)) != E_OK)
        return err;
    if ((err = aw_pack(w, it, &m, w->trunc_ok, &e.csize, &c32, &c16)) != E_OK)
        return err;
    h[11] = (u8)(m == CM_LZX ? 2 : 0);
    p32(h + 2, (u32)it->size);
    p32(h + 6, (u32)e.csize);
    p32(h + 22, c32);
    lzx_hcrc(h, 31 + nl);
    if ((err = aw_seekpatch(w, pos, h, 31 + nl)) != E_OK)
        return err;
    strcpy(e.name, it->name);
    e.usize = it->size;
    ar_mname(&e, m == CM_LZX ? "lzx" : "stored");
    out_ent(w, &e);
    return E_OK;
}

int lzx_copy(AW *w, FILE *of, const ARCH *a, int i, int n)
{
    const AENT *f = &a->e[i], *l = &a->e[i + n - 1];
    int k, err = aw_copy(w, of, f->hdr, l->off + l->csize - f->hdr);
    for (k = 0; !err && k < n; k++)
        out_ent(w, (AENT *)&a->e[i + k]);
    return err;
}

int lzx_end(AW *w)
{
    (void)w;
    return E_OK;
}
