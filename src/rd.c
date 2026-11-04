/* rd.c - buffered reader with progress */
#include <stdlib.h>
#include <string.h>
#include "rd.h"

int rd_open(RD *r, const char *path, PROG *pg)
{
    memset(r, 0, sizeof(*r));
    r->pg = pg;
    r->f = fopen(path, "rb");
    if (!r->f)
        return E_OPEN;
    fseek(r->f, 0, SEEK_END);
    r->total = ftell(r->f);
    fseek(r->f, 0, SEEK_SET);
    if (r->total <= 0)
        r->total = 1;
    return E_OK;
}

void rd_close(RD *r)
{
    if (r->f)
        fclose(r->f);
    r->f = 0;
}

int rd_cancelled(RD *r)
{
    long d = r->done - r->len + r->pos;
    if (d - r->lastrep >= 4096 || d >= r->total) {
        r->lastrep = d;
        return prog_step(r->pg, d, r->total);
    }
    return r->pg && r->pg->cancelled;
}

static int fill(RD *r)
{
    if (r->eof)
        return 0;
    r->len = (long)fread(r->buf, 1, sizeof r->buf, r->f);
    r->pos = 0;
    r->done += r->len;
    if (r->len <= 0) {
        r->eof = 1;
        r->len = 0;
        return 0;
    }
    rd_cancelled(r);
    return 1;
}

/* look ahead i bytes without consuming (i < 16); -1 at end of file */
int rd_peek(RD *r, int i)
{
    if (r->pos + i >= r->len && !r->eof) {
        long keep = r->len - r->pos, n;
        memmove(r->buf, r->buf + r->pos, keep);
        r->pos = 0;
        r->len = keep;
        n = (long)fread(r->buf + keep, 1, sizeof r->buf - keep, r->f);
        if (n <= 0)
            r->eof = 1;
        else {
            r->len += n;
            r->done += n;
        }
    }
    if (r->pos + i >= r->len)
        return -1;
    return r->buf[r->pos + i];
}

int rd_byte(RD *r)
{
    if (r->pos >= r->len && !fill(r))
        return -1;
    return r->buf[r->pos++];
}

long rd_read(RD *r, void *d, long n)
{
    u8 *o = d;
    long got = 0;
    while (got < n) {
        long a;
        if (r->pos >= r->len && !fill(r))
            break;
        a = r->len - r->pos;
        if (a > n - got)
            a = n - got;
        memcpy(o + got, r->buf + r->pos, a);
        r->pos += a;
        got += a;
    }
    return got;
}

int rd_skip(RD *r, long n)
{
    while (n > 0) {
        long a;
        if (r->pos >= r->len && !fill(r))
            return 0;
        a = r->len - r->pos;
        if (a > n)
            a = n;
        r->pos += a;
        n -= a;
    }
    return 1;
}

int rd_u16be(RD *r) { int a = rd_byte(r), b = rd_byte(r); return (a < 0 || b < 0) ? -1 : (a << 8) | b; }
int rd_u16le(RD *r) { int a = rd_byte(r), b = rd_byte(r); return (a < 0 || b < 0) ? -1 : (b << 8) | a; }
long rd_u32be(RD *r)
{
    long a = rd_u16be(r), b = rd_u16be(r);
    if (a < 0 || b < 0)
        return -1;
    return (a << 16) | b;
}

u8 *rd_slurp(const char *path, long *len, PROG *pg, int *err)
{
    RD r;
    u8 *m;
    long got = 0;
    *err = rd_open(&r, path, pg);
    if (*err)
        return 0;
    m = malloc(r.total + 1);
    if (!m) {
        rd_close(&r);
        *err = E_NOMEM;
        return 0;
    }
    while (got < r.total) {
        long n = (long)fread(m + got, 1, r.total - got > 8192 ? 8192 : r.total - got, r.f);
        if (n <= 0)
            break;
        got += n;
        if (prog_step(pg, got, r.total)) {
            free(m);
            rd_close(&r);
            *err = E_CANCEL;
            return 0;
        }
    }
    rd_close(&r);
    m[got] = 0;
    *len = got;
    *err = E_OK;
    return m;
}
