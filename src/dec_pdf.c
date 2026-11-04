/*
 * dec_pdf.c - lightweight PDF reader.
 *
 * Objects are located by scanning the file (robust against broken xref
 * tables and incremental updates); compressed object streams (PDF 1.5+)
 * are supported. Page content streams are interpreted for text, lines,
 * rectangles/fills and image positions. Text is decoded through ToUnicode
 * CMaps, /Differences and the standard encodings and converted to the
 * Atari character set. The result is a display list per page (doc.h).
 *
 * Not supported (by design, to stay light): fonts glyph shapes, images,
 * shadings, clipping, encryption.
 */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "rd.h"
#include "doc.h"

/* PDF real numbers ("-12.5", ".5", "3."), parsed here rather than with
   atof() so no C library floating point call is needed (FPU and non-FPU
   builds can then share one library)                                   */
static float pdf_real(const char *t)
{
    float v = 0, scale = 1;
    int neg = 0, frac = 0;
    if (*t == '-') { neg = 1; t++; } else if (*t == '+') t++;
    for (; *t; t++) {
        if (*t == '.') { if (frac) break; frac = 1; continue; }
        if (*t < '0' || *t > '9') break;
        if (frac) { scale /= 10; v += (*t - '0') * scale; }
        else v = v * 10 + (*t - '0');
    }
    return neg ? -v : v;
}

u8  *fv_inflate(const u8 *src, long len, long *outlen);
void fv_inflate_free(u8 *p);

/* ------------------------------------------------------------- values */

enum { PV_NULL, PV_BOOL, PV_INT, PV_REAL, PV_NAME, PV_STR, PV_ARRAY,
       PV_DICT, PV_REF, PV_OP };

typedef struct PV PV;
struct PV {
    char type;
    long  i;              /* int, bool, ref num, array/dict count, str len */
    float r;
    char *s;              /* name, string, operator                        */
    PV  **a;              /* array items / dict key,value pairs            */
    long  spos;           /* dict: offset of stream data or -1             */
    const u8 *sbase;      /* buffer the stream offset refers to            */
};

/* ------------------------------------------------------------- arena */

typedef struct ABLK { struct ABLK *next; long used, size; } ABLK;
typedef struct { ABLK *head; int err; } ARENA;

static void *aalloc(ARENA *ar, long n)
{
    ABLK *b = ar->head;
    n = (n + 7) & ~7L;            /* 8 keeps 64-bit hosts happy too */
    if (!b || b->used + n > b->size) {
        long sz = n > 16000 ? n + 64 : 16384;
        b = malloc(sizeof(ABLK) + sz);
        if (!b) {
            ar->err = 1;
            return 0;
        }
        b->size = sz;
        b->used = 0;
        b->next = ar->head;
        ar->head = b;
    }
    {
        void *p = (u8 *)(b + 1) + b->used;
        b->used += n;
        return p;
    }
}

static void afree(ARENA *ar)
{
    while (ar->head) {
        ABLK *n = ar->head->next;
        free(ar->head);
        ar->head = n;
    }
    ar->err = 0;
}

/* --------------------------------------------------------- document */

typedef struct { long num, off; int stm; int idx; } XENT;   /* stm>0: in objstm */

typedef struct {
    long  num;
    u8   *data;
    long  len;
    long  first;
    long *offs;            /* object numbers and offsets, pairs        */
    long  n;
} OBJSTM;

#define MAXSTM 8

typedef struct {
    u8   *f;
    long  flen;
    XENT *x;
    long  nx;
    ARENA ar;              /* per-page arena                             */
    ARENA dar;             /* document lifetime arena (page list)        */
    OBJSTM stm[MAXSTM];
    int   nstm;
    long *pages;           /* page object numbers                        */
    long  npages, capages;
    long  imgbytes;        /* image data stored so far              */
    PROG *pg;              /* progress: repeat pdone/ptotal while busy */
    long  pdone, ptotal;
    const PREFS *pf;
} PDF;

/* ------------------------------------------------------------- lexer */

typedef struct {
    const u8 *p;
    long n, i;
} LEX;

static int is_ws(int c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
static int is_delim(int c) { return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' ||
                                    c == ']' || c == '{' || c == '}' || c == '/' || c == '%'; }

static void lx_ws(LEX *l)
{
    while (l->i < l->n) {
        int c = l->p[l->i];
        if (is_ws(c))
            l->i++;
        else if (c == '%') {
            while (l->i < l->n && l->p[l->i] != '\n' && l->p[l->i] != '\r')
                l->i++;
        } else
            break;
    }
}

static PV *pv_new(PDF *d, int type)
{
    PV *v = aalloc(&d->ar, sizeof(PV));
    if (v) {
        memset(v, 0, sizeof *v);
        v->type = type;
        v->spos = -1;
    }
    return v;
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static PV *parse_obj(PDF *d, LEX *l, int depth);

static PV *parse_str(PDF *d, LEX *l)
{
    /* literal string, l->i points after '(' */
    long start = l->i, depth = 1, o = 0, cap;
    PV *v = pv_new(d, PV_STR);
    char *s;
    if (!v) return 0;
    /* upper bound on length */
    {
        long j = l->i, dd = 1;
        while (j < l->n && dd) {
            if (l->p[j] == '\\') j++;
            else if (l->p[j] == '(') dd++;
            else if (l->p[j] == ')') dd--;
            j++;
        }
        cap = j - start + 1;
    }
    s = aalloc(&d->ar, cap + 1);
    if (!s) return 0;
    while (l->i < l->n) {
        int c = l->p[l->i++];
        if (c == '(')
            depth++;
        else if (c == ')') {
            if (--depth == 0)
                break;
        } else if (c == '\\' && l->i < l->n) {
            c = l->p[l->i++];
            switch (c) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case '\r':
                if (l->i < l->n && l->p[l->i] == '\n') l->i++;
                continue;
            case '\n':
                continue;
            default:
                if (c >= '0' && c <= '7') {
                    int v8 = c - '0', k;
                    for (k = 0; k < 2 && l->i < l->n && l->p[l->i] >= '0' && l->p[l->i] <= '7'; k++)
                        v8 = v8 * 8 + (l->p[l->i++] - '0');
                    c = v8 & 0xFF;
                }
            }
        }
        if (o < cap)
            s[o++] = (char)c;
    }
    s[o] = 0;
    v->s = s;
    v->i = o;
    return v;
}

static PV *parse_hex(PDF *d, LEX *l)
{
    long j = l->i, o = 0;
    int hi = -1;
    PV *v = pv_new(d, PV_STR);
    char *s;
    while (j < l->n && l->p[j] != '>') j++;
    s = aalloc(&d->ar, (j - l->i) / 2 + 2);
    if (!v || !s) return 0;
    while (l->i < l->n && l->p[l->i] != '>') {
        int h = hexv(l->p[l->i++]);
        if (h < 0) continue;
        if (hi < 0) hi = h;
        else { s[o++] = (char)(hi * 16 + h); hi = -1; }
    }
    if (hi >= 0) s[o++] = (char)(hi * 16);
    l->i++;
    s[o] = 0;
    v->s = s;
    v->i = o;
    return v;
}

static PV *parse_name(PDF *d, LEX *l)
{
    long st = l->i, o = 0;
    PV *v = pv_new(d, PV_NAME);
    char *s;
    while (l->i < l->n && !is_ws(l->p[l->i]) && !is_delim(l->p[l->i]))
        l->i++;
    s = aalloc(&d->ar, l->i - st + 1);
    if (!v || !s) return 0;
    {
        long j;
        for (j = st; j < l->i; j++) {
            if (l->p[j] == '#' && j + 2 < l->i && hexv(l->p[j + 1]) >= 0 && hexv(l->p[j + 2]) >= 0) {
                s[o++] = (char)(hexv(l->p[j + 1]) * 16 + hexv(l->p[j + 2]));
                j += 2;
            } else
                s[o++] = l->p[j];
        }
    }
    s[o] = 0;
    v->s = s;
    return v;
}

/* collect items into an array */
typedef struct { PV **v; long n, cap; } PVLIST;
static void pl_add(PDF *d, PVLIST *pl, PV *v)
{
    if (pl->n == pl->cap) {
        long nc = pl->cap ? pl->cap * 2 : 8;
        PV **nv = aalloc(&d->ar, nc * sizeof(PV *));
        if (!nv) return;
        if (pl->n) memcpy(nv, pl->v, pl->n * sizeof(PV *));
        pl->v = nv;
        pl->cap = nc;
    }
    pl->v[pl->n++] = v;
}

/* parse a token or object. Operators come back as PV_OP. */
static PV *parse_obj(PDF *d, LEX *l, int depth)
{
    int c;
    lx_ws(l);
    if (l->i >= l->n || depth > 40)
        return 0;
    c = l->p[l->i];
    if (c == '(') { l->i++; return parse_str(d, l); }
    if (c == '/') { l->i++; return parse_name(d, l); }
    if (c == '<' && l->i + 1 < l->n && l->p[l->i + 1] == '<') {
        PVLIST pl = { 0, 0, 0 };
        PV *v = pv_new(d, PV_DICT);
        l->i += 2;
        for (;;) {
            PV *k;
            lx_ws(l);
            if (l->i >= l->n) break;
            if (l->p[l->i] == '>' && l->i + 1 < l->n && l->p[l->i + 1] == '>') {
                l->i += 2;
                break;
            }
            k = parse_obj(d, l, depth + 1);
            if (!k) break;
            if (k->type != PV_NAME) continue;
            pl_add(d, &pl, k);
            pl_add(d, &pl, parse_obj(d, l, depth + 1));
        }
        if (!v) return 0;
        v->a = pl.v;
        v->i = pl.n / 2;
        /* stream? */
        lx_ws(l);
        if (l->i + 6 <= l->n && !memcmp(l->p + l->i, "stream", 6)) {
            l->i += 6;
            if (l->i < l->n && l->p[l->i] == '\r') l->i++;
            if (l->i < l->n && l->p[l->i] == '\n') l->i++;
            v->spos = l->i;
            v->sbase = l->p;
        }
        return v;
    }
    if (c == '<') { l->i++; return parse_hex(d, l); }
    if (c == '[') {
        PVLIST pl = { 0, 0, 0 };
        PV *v = pv_new(d, PV_ARRAY);
        l->i++;
        for (;;) {
            PV *it;
            lx_ws(l);
            if (l->i >= l->n) break;
            if (l->p[l->i] == ']') { l->i++; break; }
            it = parse_obj(d, l, depth + 1);
            if (!it) break;
            /* "n g R" inside arrays */
            if (it->type == PV_OP && !strcmp(it->s, "R") && pl.n >= 2 &&
                pl.v[pl.n - 1]->type == PV_INT && pl.v[pl.n - 2]->type == PV_INT) {
                PV *r = pl.v[pl.n - 2];
                r->type = PV_REF;
                pl.n--;
                continue;
            }
            pl_add(d, &pl, it);
        }
        if (!v) return 0;
        v->a = pl.v;
        v->i = pl.n;
        return v;
    }
    if (c == ']' || c == '>' || c == ')' || c == '}' || c == '{') {
        l->i++;
        return parse_obj(d, l, depth + 1);
    }
    if (isdigit(c) || c == '-' || c == '+' || c == '.') {
        long st = l->i, iv;
        int isreal = 0;
        PV *v;
        l->i++;
        while (l->i < l->n && (isdigit(l->p[l->i]) || l->p[l->i] == '.')) {
            if (l->p[l->i] == '.') isreal = 1;
            l->i++;
        }
        if (c == '.') isreal = 1;
        {
            char tmp[40];
            long n = l->i - st;
            if (n > 39) n = 39;
            memcpy(tmp, l->p + st, n);
            tmp[n] = 0;
            v = pv_new(d, isreal ? PV_REAL : PV_INT);
            if (!v) return 0;
            if (isreal) {
                v->r = pdf_real(tmp);
                v->i = (long)v->r;
            } else {
                iv = atol(tmp);
                v->i = iv;
                v->r = (float)iv;
            }
        }
        /* indirect reference "n g R" outside arrays */
        if (!isreal && depth > 0) {
            long save = l->i, g;
            lx_ws(l);
            if (l->i < l->n && isdigit(l->p[l->i])) {
                g = 0;
                while (l->i < l->n && isdigit(l->p[l->i])) g = g * 10 + l->p[l->i++] - '0';
                lx_ws(l);
                if (l->i < l->n && l->p[l->i] == 'R' &&
                    (l->i + 1 >= l->n || is_ws(l->p[l->i + 1]) || is_delim(l->p[l->i + 1]))) {
                    l->i++;
                    v->type = PV_REF;
                    return v;
                }
            }
            (void)g;
            l->i = save;
        }
        return v;
    }
    /* keyword / operator */
    {
        long st = l->i;
        PV *v;
        char *s;
        while (l->i < l->n && !is_ws(l->p[l->i]) && !is_delim(l->p[l->i]))
            l->i++;
        if (l->i == st) { l->i++; return parse_obj(d, l, depth + 1); }
        v = pv_new(d, PV_OP);
        s = aalloc(&d->ar, l->i - st + 1);
        if (!v || !s) return 0;
        memcpy(s, l->p + st, l->i - st);
        s[l->i - st] = 0;
        v->s = s;
        if (!strcmp(s, "true")) { v->type = PV_BOOL; v->i = 1; }
        else if (!strcmp(s, "false")) { v->type = PV_BOOL; v->i = 0; }
        else if (!strcmp(s, "null")) v->type = PV_NULL;
        return v;
    }
}

/* ------------------------------------------------------------ objects */

static PV *dget(PV *dict, const char *key)
{
    long k;
    if (!dict || dict->type != PV_DICT)
        return 0;
    for (k = 0; k < dict->i; k++)
        if (dict->a[k * 2] && !strcmp(dict->a[k * 2]->s, key))
            return dict->a[k * 2 + 1];
    return 0;
}

static long find_x(PDF *d, long num)
{
    long lo = 0, hi = d->nx - 1;
    while (lo <= hi) {
        long m = (lo + hi) / 2;
        if (d->x[m].num == num) return m;
        if (d->x[m].num < num) lo = m + 1; else hi = m - 1;
    }
    return -1;
}

static PV *get_stream_dict(PDF *d, long num);
static u8 *stream_data(PDF *d, PV *dict, long *len);

static OBJSTM *load_objstm(PDF *d, long num)
{
    int k;
    OBJSTM *s;
    PV *dict;
    ARENA save;
    for (k = 0; k < d->nstm; k++)
        if (d->stm[k].num == num)
            return &d->stm[k];
    if (d->nstm == MAXSTM) {          /* evict the oldest */
        fv_inflate_free(d->stm[0].data);
        free(d->stm[0].offs);
        memmove(&d->stm[0], &d->stm[1], sizeof(OBJSTM) * (MAXSTM - 1));
        d->nstm--;
    }
    s = &d->stm[d->nstm];
    memset(s, 0, sizeof *s);
    /* parse the stream dict in the document arena so it survives */
    save = d->ar;
    d->ar = d->dar;
    dict = get_stream_dict(d, num);
    if (dict) {
        PV *n = dget(dict, "N"), *f = dget(dict, "First");
        s->data = stream_data(d, dict, &s->len);
        if (s->data && n && f && n->type == PV_INT && f->type == PV_INT) {
            LEX l;
            long j;
            s->n = n->i;
            s->first = f->i;
            s->offs = malloc(sizeof(long) * 2 * (s->n + 1));
            l.p = s->data; l.n = s->len; l.i = 0;
            for (j = 0; s->offs && j < s->n * 2; j++) {
                PV *v = parse_obj(d, &l, 1);
                s->offs[j] = v && (v->type == PV_INT || v->type == PV_REF) ? v->i : 0;
                if (v && v->type == PV_REF) {   /* "num off" mis-read as ref */
                    l.i -= 1;
                }
            }
        }
    }
    d->dar = d->ar;
    d->ar = save;
    if (!s->data || !s->offs) {
        if (s->data) fv_inflate_free(s->data);
        free(s->offs);
        return 0;
    }
    s->num = num;
    d->nstm++;
    return s;
}

static PV *get_obj(PDF *d, long num)
{
    long k = find_x(d, num);
    LEX l;
    if (k < 0)
        return 0;
    if (d->x[k].stm) {
        OBJSTM *s = load_objstm(d, d->x[k].stm);
        long j;
        if (!s) return 0;
        for (j = 0; j < s->n; j++)
            if (s->offs[j * 2] == num) {
                l.p = s->data; l.n = s->len;
                l.i = s->first + s->offs[j * 2 + 1];
                return parse_obj(d, &l, 1);
            }
        return 0;
    }
    l.p = d->f; l.n = d->flen; l.i = d->x[k].off;
    /* skip "num gen obj" */
    {
        PV *a = parse_obj(d, &l, 0), *b = parse_obj(d, &l, 0), *o = parse_obj(d, &l, 0);
        if (!a || !b || !o || o->type != PV_OP || strcmp(o->s, "obj"))
            return 0;
    }
    return parse_obj(d, &l, 1);
}

static PV *res(PDF *d, PV *v)
{
    int guard = 8;
    while (v && v->type == PV_REF && guard--)
        v = get_obj(d, v->i);
    return v;
}
static PV *dgetr(PDF *d, PV *dict, const char *key) { return res(d, dget(res(d, dict), key)); }

static PV *get_stream_dict(PDF *d, long num)
{
    PV *v = get_obj(d, num);
    return v && v->type == PV_DICT ? v : 0;
}

static float num(PV *v) { return !v ? 0 : v->type == PV_REAL ? v->r : (float)v->i; }

/* ------------------------------------------------------------ filters */

static u8 *f_ahx(const u8 *s, long n, long *on)
{
    u8 *o = malloc(n / 2 + 2);
    long i, k = 0;
    int hi = -1;
    if (!o) return 0;
    for (i = 0; i < n && s[i] != '>'; i++) {
        int h = hexv(s[i]);
        if (h < 0) continue;
        if (hi < 0) hi = h; else { o[k++] = hi * 16 + h; hi = -1; }
    }
    if (hi >= 0) o[k++] = hi * 16;
    *on = k;
    return o;
}

static u8 *f_a85(const u8 *s, long n, long *on)
{
    u8 *o = malloc(n * 4 / 5 + 8);
    long i, k = 0;
    int cnt = 0;
    u32 t = 0;
    if (!o) return 0;
    for (i = 0; i < n; i++) {
        int c = s[i];
        if (c == '~') break;
        if (is_ws(c)) continue;
        if (c == 'z' && cnt == 0) { o[k++] = 0; o[k++] = 0; o[k++] = 0; o[k++] = 0; continue; }
        if (c < '!' || c > 'u') continue;
        t = t * 85 + (c - '!');
        if (++cnt == 5) {
            o[k++] = t >> 24; o[k++] = t >> 16; o[k++] = t >> 8; o[k++] = t;
            t = 0; cnt = 0;
        }
    }
    if (cnt > 1) {
        int j;
        for (j = cnt; j < 5; j++) t = t * 85 + 84;
        for (j = 0; j < cnt - 1; j++) o[k++] = t >> (24 - 8 * j);
    }
    *on = k;
    return o;
}

static u8 *f_rl(const u8 *s, long n, long *on)
{
    long i = 0, k = 0, cap = n * 2 + 256;
    u8 *o = malloc(cap);
    if (!o) return 0;
    while (i < n) {
        int c = s[i++];
        if (c == 128)
            break;
        if (c < 128) {
            long cnt = c + 1;
            if (i + cnt > n) cnt = n - i;
            if (k + cnt > cap) {
                u8 *no = realloc(o, cap = cap * 2 + cnt);
                if (!no) { free(o); return 0; }
                o = no;
            }
            memcpy(o + k, s + i, cnt);
            k += cnt;
            i += cnt;
        } else {
            long cnt = 257 - c;
            if (i >= n) break;
            if (k + cnt > cap) {
                u8 *no = realloc(o, cap = cap * 2 + cnt);
                if (!no) { free(o); return 0; }
                o = no;
            }
            memset(o + k, s[i++], cnt);
            k += cnt;
        }
    }
    *on = k;
    return o;
}

static u8 *stream_data_ex(PDF *d, PV *dict, long *len, int *dct);

/* returns malloc'd data; caller frees with free() */
static u8 *stream_data(PDF *d, PV *dict, long *len)
{
    int dct = 0;
    u8 *p = stream_data_ex(d, dict, len, &dct);
    if (p && dct) {              /* JPEG data is no use as content */
        free(p);
        return 0;
    }
    return p;
}

/* as stream_data; when the last filter is DCTDecode the JPEG data is
   returned as it is and *dct is set */
static u8 *stream_data_ex(PDF *d, PV *dict, long *len, int *dct)
{
    PV *lv = dgetr(d, dict, "Length"), *flt = dgetr(d, dict, "Filter");
    long l, i, nf = 0;
    const u8 *src;
    u8 *cur;
    long curlen;
    PV *fl[4];
    if (!dict || dict->spos < 0)
        return 0;
    src = dict->sbase + dict->spos;
    l = lv && lv->type == PV_INT ? lv->i : -1;
    {
        long avail = dict->sbase == d->f ? d->flen - dict->spos : l;
        if (l < 0 || l > avail) {
            /* find "endstream" */
            const u8 *e = src;
            long rest = avail > 0 ? avail : 0;
            l = 0;
            while (l + 9 <= rest && memcmp(e + l, "endstream", 9)) l++;
        }
    }
    if (flt && flt->type == PV_NAME)
        fl[nf++] = flt;
    else if (flt && flt->type == PV_ARRAY)
        for (i = 0; i < flt->i && nf < 4; i++)
            fl[nf++] = res(d, flt->a[i]);

    cur = malloc(l + 1);
    if (!cur) return 0;
    memcpy(cur, src, l);
    curlen = l;
    for (i = 0; i < nf; i++) {
        const char *f = fl[i] && fl[i]->type == PV_NAME ? fl[i]->s : "";
        u8 *nx = 0;
        long nl = 0;
        if (!strcmp(f, "FlateDecode") || !strcmp(f, "Fl")) {
            u8 *t = fv_inflate(cur, curlen, &nl);
            if (t) {
                nx = malloc(nl + 1);
                if (nx) memcpy(nx, t, nl);
                fv_inflate_free(t);
            }
        } else if (!strcmp(f, "ASCIIHexDecode") || !strcmp(f, "AHx"))
            nx = f_ahx(cur, curlen, &nl);
        else if (!strcmp(f, "ASCII85Decode") || !strcmp(f, "A85"))
            nx = f_a85(cur, curlen, &nl);
        else if (!strcmp(f, "RunLengthDecode") || !strcmp(f, "RL"))
            nx = f_rl(cur, curlen, &nl);
        else if ((!strcmp(f, "DCTDecode") || !strcmp(f, "DCT")) && i == nf - 1) {
            *dct = 1;
            break;
        } else {
            free(cur);         /* DCT, LZW, CCITT ... not needed here */
            return 0;
        }
        free(cur);
        if (!nx) return 0;
        cur = nx;
        curlen = nl;
    }
    *len = curlen;
    return cur;
}

/* --------------------------------------------------------- object scan */

static int xcmp(const void *a, const void *b)
{
    const XENT *x = a, *y = b;
    if (x->num != y->num) return x->num < y->num ? -1 : 1;
    return x->off < y->off ? -1 : x->off > y->off;
}

static int scan_objects(PDF *d, PROG *pg)
{
    long i, cap = 256;
    d->x = malloc(cap * sizeof(XENT));
    d->nx = 0;
    if (!d->x) return E_NOMEM;
    for (i = 3; i + 3 <= d->flen; i++) {
        long j, g, n, mul;
        if (d->f[i] != 'o' || d->f[i + 1] != 'b' || d->f[i + 2] != 'j')
            continue;
        if (i + 3 < d->flen && !is_ws(d->f[i + 3]) && !is_delim(d->f[i + 3]))
            continue;
        j = i - 1;
        if (!is_ws(d->f[j])) continue;
        while (j > 0 && is_ws(d->f[j])) j--;
        if (!isdigit(d->f[j])) continue;
        g = 0; mul = 1;
        while (j >= 0 && isdigit(d->f[j])) { g += (d->f[j] - '0') * mul; mul *= 10; j--; }
        if (j < 0 || !is_ws(d->f[j])) continue;
        while (j > 0 && is_ws(d->f[j])) j--;
        if (!isdigit(d->f[j])) continue;
        n = 0; mul = 1;
        while (j >= 0 && isdigit(d->f[j]) && mul < 100000000L) { n += (d->f[j] - '0') * mul; mul *= 10; j--; }
        if (j >= 0 && !is_ws(d->f[j]) && !is_delim(d->f[j])) continue;
        (void)g;
        if (d->nx == cap) {
            XENT *nx = realloc(d->x, (cap *= 2) * sizeof(XENT));
            if (!nx) return E_NOMEM;
            d->x = nx;
        }
        d->x[d->nx].num = n;
        d->x[d->nx].off = j + 1;
        d->x[d->nx].stm = 0;
        d->nx++;
        if ((i & 0xFFFF) == 0 && prog_step(pg, i, d->flen * 2))
            return E_CANCEL;
    }
    qsort(d->x, d->nx, sizeof(XENT), xcmp);
    /* keep the last definition of each object (incremental updates) */
    {
        long o = 0;
        for (i = 0; i < d->nx; i++) {
            if (o && d->x[o - 1].num == d->x[i].num)
                d->x[o - 1] = d->x[i];
            else
                d->x[o++] = d->x[i];
        }
        d->nx = o;
    }
    /* objects in object streams */
    {
        long k, nreal = d->nx;
        for (k = 0; k < nreal; k++) {
            PV *v = get_obj(d, d->x[k].num), *t;
            t = v ? dget(v, "Type") : 0;
            if (t && t->type == PV_NAME && !strcmp(t->s, "ObjStm")) {
                OBJSTM *s = load_objstm(d, d->x[k].num);
                long j;
                for (j = 0; s && j < s->n; j++) {
                    if (find_x(d, s->offs[j * 2]) >= 0)
                        continue;      /* a real object wins */
                    if (d->nx == cap) {
                        XENT *nx = realloc(d->x, (cap *= 2) * sizeof(XENT));
                        if (!nx) return E_NOMEM;
                        d->x = nx;
                    }
                    d->x[d->nx].num = s->offs[j * 2];
                    d->x[d->nx].off = 0;
                    d->x[d->nx].stm = d->x[k].num;
                    d->nx++;
                }
            }
            afree(&d->ar);
        }
        qsort(d->x, d->nx, sizeof(XENT), xcmp);
    }
    return E_OK;
}

/* ------------------------------------------------------------- pages */

static void add_page(PDF *d, long num)
{
    if (d->npages == d->capages) {
        long *np = realloc(d->pages, (d->capages = d->capages ? d->capages * 2 : 64) * sizeof(long));
        if (!np) return;
        d->pages = np;
    }
    d->pages[d->npages++] = num;
}

static void walk_pages(PDF *d, PV *ref, int depth)
{
    PV *node, *type, *kids;
    long i;
    if (!ref || ref->type != PV_REF || depth > 32 || d->npages > 9999)
        return;
    node = res(d, ref);
    if (!node || node->type != PV_DICT)
        return;
    type = dget(node, "Type");
    kids = dgetr(d, node, "Kids");
    if (kids && kids->type == PV_ARRAY && (!type || strcmp(type->s ? type->s : "", "Page"))) {
        /* copy kid refs before recursing: the arena entries stay valid */
        for (i = 0; i < kids->i; i++)
            walk_pages(d, kids->a[i], depth + 1);
    } else
        add_page(d, ref->i);
}

static PV *find_root(PDF *d)
{
    long i;
    /* search backwards for /Root n g R (trailer or xref stream dict) */
    for (i = d->flen - 5; i >= 0; i--) {
        if (d->f[i] == '/' && !memcmp(d->f + i, "/Root", 5)) {
            LEX l;
            PV *v;
            l.p = d->f; l.n = d->flen; l.i = i + 5;
            v = parse_obj(d, &l, 1);
            if (v && v->type == PV_REF)
                return v;
        }
    }
    /* fall back: look for a catalog object */
    for (i = 0; i < d->nx; i++) {
        PV *o = get_obj(d, d->x[i].num), *t = o ? dget(o, "Type") : 0;
        if (t && t->type == PV_NAME && !strcmp(t->s, "Catalog")) {
            PV *r = pv_new(d, PV_REF);
            if (r) r->i = d->x[i].num;
            return r;
        }
    }
    return 0;
}

/* ------------------------------------------------------------ images */

u8 *fv_jpeg_mem(const u8 *src, long len, int *w, int *h, int *comp);

/* Image data kept in the display list. 1-bit images stay 1-bit (1 = ink)
 * and keep their resolution up to IMG_MAXBITS pixels; grey and colour
 * images are boxed down to IMG_MAXPIX pixels. A document stops getting
 * images once IMG_BUDGET bytes of them are stored.                    */
#define IMG_MAXBITS (8L * 1024 * 1024)
#define IMG_MAXPIX  (200L * 1024)
#define IMG_BUDGET  (3L * 1024 * 1024)

typedef struct {
    int  fmt;              /* IF_MONO, IF_GREY, IF_RGB                  */
    int  mask;             /* stencil mask: paint 1 bits only           */
    long w, h;
    u8  *data;
    long len;
} PIMG;

/* PNG predictor (Flate / LZW DecodeParms /Predictor >= 10) */
static int unpredict(u8 *data, long len, long cols, int colors, int bpc, long *outlen)
{
    long bpp = (colors * bpc + 7) / 8, row = (cols * colors * bpc + 7) / 8;
    long nrows = len / (row + 1), r, i;
    u8 *prev, *out = data;
    if (bpp < 1) bpp = 1;
    prev = calloc(row, 1);
    if (!prev) return 0;
    for (r = 0; r < nrows; r++) {
        const u8 *in = data + r * (row + 1);
        int ft = in[0];
        u8 *o = out + r * row;
        in++;
        for (i = 0; i < row; i++) {
            int a = i >= bpp ? o[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0, v = in[i];
            switch (ft) {
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: {
                int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                v += (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
                break;
            }
            }
            o[i] = (u8)v;
        }
        memcpy(prev, o, row);
    }
    free(prev);
    *outlen = nrows * row;
    return 1;
}

static int sample(const u8 *row, long i, int bpc)
{
    switch (bpc) {
    case 1:  return (row[i >> 3] >> (7 - (i & 7))) & 1;
    case 2:  return (row[i >> 2] >> (6 - 2 * (i & 3))) & 3;
    case 4:  return (row[i >> 1] >> (i & 1 ? 0 : 4)) & 15;
    case 16: return row[i * 2];
    default: return row[i];
    }
}

/* decode an image XObject (dict with stream) into a PIMG */
static int load_image(PDF *d, PV *xo, PIMG *pi, long budget_left)
{
    PV *wv = dgetr(d, xo, "Width"), *hv = dgetr(d, xo, "Height");
    PV *bv = dgetr(d, xo, "BitsPerComponent"), *cs = dgetr(d, xo, "ColorSpace");
    PV *dec = dgetr(d, xo, "Decode"), *im = dgetr(d, xo, "ImageMask");
    PV *dp = dgetr(d, xo, "DecodeParms");
    long w, h, len, rowb, x, y;
    int bpc, ncomp = 1, dct = 0, invert = 0, hival = 0;
    u8 *raw, pal[256 * 3], *lookup = 0;
    long lookuplen = 0;
    int base_ncomp = 0;

    memset(pi, 0, sizeof *pi);
    if (!wv || !hv)
        return 0;
    w = wv->i; h = hv->i;
    if (w <= 0 || h <= 0 || w > 20000 || h > 20000)
        return 0;
    bpc = bv ? (int)bv->i : 1;
    pi->mask = im && im->type == PV_BOOL && im->i;
    if (pi->mask)
        bpc = 1;

    /* colour space */
    if (!pi->mask && cs) {
        const char *n = cs->type == PV_NAME ? cs->s : 0;
        PV *arr = cs->type == PV_ARRAY ? cs : 0;
        if (arr && arr->i > 0) {
            PV *a0 = res(d, arr->a[0]);
            n = a0 && a0->type == PV_NAME ? a0->s : "";
        }
        if (!n) return 0;
        if (!strcmp(n, "DeviceGray") || !strcmp(n, "CalGray") || !strcmp(n, "G"))
            ncomp = 1;
        else if (!strcmp(n, "DeviceRGB") || !strcmp(n, "CalRGB") || !strcmp(n, "RGB"))
            ncomp = 3;
        else if (!strcmp(n, "DeviceCMYK") || !strcmp(n, "CMYK"))
            ncomp = 4;
        else if (!strcmp(n, "ICCBased") && arr && arr->i > 1) {
            PV *st = res(d, arr->a[1]), *nv = dgetr(d, st, "N");
            ncomp = nv ? (int)nv->i : 3;
        } else if ((!strcmp(n, "Indexed") || !strcmp(n, "I")) && arr && arr->i >= 4) {
            PV *bs = res(d, arr->a[1]), *hv2 = res(d, arr->a[2]), *lk = res(d, arr->a[3]);
            const char *bn = bs && bs->type == PV_NAME ? bs->s : "DeviceRGB";
            if (bs && bs->type == PV_ARRAY && bs->i > 1) {   /* [/ICCBased n 0 R] */
                PV *st = res(d, bs->a[1]), *nv = dgetr(d, st, "N");
                base_ncomp = nv ? (int)nv->i : 3;
            } else
                base_ncomp = strstr(bn, "Gray") ? 1 : strstr(bn, "CMYK") ? 4 : 3;
            hival = hv2 ? (int)hv2->i : 255;
            if (hival > 255) hival = 255;
            if (lk && lk->type == PV_STR) {
                lookup = (u8 *)lk->s;
                lookuplen = lk->i;
            } else if (lk && lk->type == PV_DICT) {
                lookup = stream_data(d, lk, &lookuplen);
                if (lookup) {
                    /* copy into the arena-independent palette below */
                }
            }
            if (!lookup)
                return 0;
            {
                int i;
                for (i = 0; i <= hival; i++) {
                    long o = (long)i * base_ncomp;
                    u8 c[4] = { 0, 0, 0, 0 };
                    int k;
                    for (k = 0; k < base_ncomp && o + k < lookuplen; k++)
                        c[k] = lookup[o + k];
                    if (base_ncomp == 1) c[1] = c[2] = c[0];
                    else if (base_ncomp == 4) {
                        int kk = c[3];
                        c[0] = (u8)(255 - (c[0] + kk > 255 ? 255 : c[0] + kk));
                        c[1] = (u8)(255 - (c[1] + kk > 255 ? 255 : c[1] + kk));
                        c[2] = (u8)(255 - (c[2] + kk > 255 ? 255 : c[2] + kk));
                    }
                    memcpy(pal + i * 3, c, 3);
                }
            }
            if (lk && lk->type == PV_DICT)
                free(lookup);
            ncomp = -1;                   /* indexed */
        } else
            return 0;                     /* Separation, DeviceN, Lab ... */
    }
    if (dec && dec->type == PV_ARRAY && dec->i >= 2 && (ncomp == 1 || pi->mask))
        invert = num(res(d, dec->a[0])) > num(res(d, dec->a[1]));

    raw = stream_data_ex(d, xo, &len, &dct);
    if (!raw)
        return 0;
    if (dct) {
        int jw, jh, jc;
        u8 *px = fv_jpeg_mem(raw, len, &jw, &jh, &jc);
        free(raw);
        if (!px)
            return 0;
        raw = px;
        w = jw; h = jh;
        ncomp = jc;
        bpc = 8;
        len = (long)w * h * jc;
        invert = 0;
    } else if (dp) {
        PV *pr = dgetr(d, dp, "Predictor");
        if (pr && pr->i >= 10) {
            PV *cv = dgetr(d, dp, "Columns"), *co = dgetr(d, dp, "Colors"),
               *bp = dgetr(d, dp, "BitsPerComponent");
            unpredict(raw, len, cv ? cv->i : w, co ? (int)co->i : (ncomp > 0 ? ncomp : 1),
                      bp ? (int)bp->i : bpc, &len);
        }
    }

    {
        int nc = ncomp < 0 ? 1 : ncomp;
        rowb = (w * nc * bpc + 7) / 8;
        if (len < rowb * h)
            h = len / rowb;               /* short data: show what is there */
        if (h <= 0) {
            free(raw);
            return 0;
        }

        if (pi->mask || (bpc == 1 && nc == 1 && ncomp != -1)) {
            /* 1-bit: keep as bits, 1 = ink */
            long step = 1, ow, oh, obpr;
            while ((w / step) * (h / step) > IMG_MAXBITS) step++;
            ow = w / step; oh = h / step;
            obpr = (ow + 7) / 8;
            if (obpr * oh > budget_left) { free(raw); return 0; }
            pi->data = malloc(obpr * oh);
            if (!pi->data) { free(raw); return 0; }
            memset(pi->data, 0, obpr * oh);
            for (y = 0; y < oh; y++) {
                if ((y & 63) == 0 && d->pg && prog_step(d->pg, d->pdone, d->ptotal))
                    break;
                const u8 *r = raw + (y * step) * rowb;
                u8 *o = pi->data + y * obpr;
                for (x = 0; x < ow; x++) {
                    int v = sample(r, x * step, 1);
                    /* DeviceGray: 0 is black (ink); mask: 0 paints */
                    int ink = invert ? v : !v;
                    if (ink)
                        o[x >> 3] |= 0x80 >> (x & 7);
                }
            }
            pi->fmt = PIF_MONO;
            pi->w = ow; pi->h = oh;
            pi->len = obpr * oh;
        } else {
            /* grey / colour: box filter down to IMG_MAXPIX */
            long ow = w, oh = h, oc = (nc == 1 && ncomp != -1) ? 1 : 3;
            int maxv = bpc == 16 ? 255 : (1 << bpc) - 1;
            while (ow * oh > IMG_MAXPIX) { ow = ow * 3 / 4; oh = oh * 3 / 4; }
            if (ow < 1) ow = 1;
            if (oh < 1) oh = 1;
            if (ow * oh * oc > budget_left) { free(raw); return 0; }
            pi->data = malloc(ow * oh * oc);
            if (!pi->data) { free(raw); return 0; }
            for (y = 0; y < oh; y++) {
                if ((y & 63) == 0 && d->pg && prog_step(d->pg, d->pdone, d->ptotal))
                    break;
                long sy0 = y * h / oh, sy1 = (y + 1) * h / oh, sy;
                if (sy1 <= sy0) sy1 = sy0 + 1;
                for (x = 0; x < ow; x++) {
                    long sx0 = x * w / ow, sx1 = (x + 1) * w / ow, sx, n = 0;
                    long acc[3] = { 0, 0, 0 };
                    if (sx1 <= sx0) sx1 = sx0 + 1;
                    for (sy = sy0; sy < sy1; sy++) {
                        const u8 *r = raw + sy * rowb;
                        for (sx = sx0; sx < sx1; sx++) {
                            int c[4], k;
                            if (ncomp == -1) {
                                int idx = sample(r, sx, bpc);
                                if (idx > hival) idx = hival;
                                c[0] = pal[idx * 3]; c[1] = pal[idx * 3 + 1]; c[2] = pal[idx * 3 + 2];
                            } else {
                                for (k = 0; k < nc && k < 4; k++)
                                    c[k] = sample(r, sx * nc + k, bpc) * 255 / maxv;
                                if (nc == 1) {
                                    if (invert) c[0] = 255 - c[0];
                                    c[1] = c[2] = c[0];
                                } else if (nc == 4) {
                                    int kk = c[3];
                                    if (dct) {      /* Adobe JPEGs store inverted CMYK */
                                        c[0] = 255 - c[0]; c[1] = 255 - c[1];
                                        c[2] = 255 - c[2]; kk = 255 - kk;
                                    }
                                    c[0] = 255 - (c[0] + kk > 255 ? 255 : c[0] + kk);
                                    c[1] = 255 - (c[1] + kk > 255 ? 255 : c[1] + kk);
                                    c[2] = 255 - (c[2] + kk > 255 ? 255 : c[2] + kk);
                                } else if (nc == 2) {
                                    c[1] = c[2] = c[0];
                                }
                            }
                            acc[0] += c[0]; acc[1] += c[1]; acc[2] += c[2];
                            n++;
                        }
                    }
                    if (!n) n = 1;
                    if (oc == 1)
                        pi->data[y * ow + x] = (u8)(acc[0] / n);
                    else {
                        u8 *o = pi->data + (y * ow + x) * 3;
                        o[0] = (u8)(acc[0] / n); o[1] = (u8)(acc[1] / n); o[2] = (u8)(acc[2] / n);
                    }
                }
            }
            pi->fmt = oc == 1 ? PIF_GREY : PIF_RGB;
            pi->w = ow; pi->h = oh;
            pi->len = ow * oh * oc;
        }
    }
    free(raw);
    return 1;
}

/* ------------------------------------------------------------- fonts */

typedef struct { u16 code, uni; } CMAPE;

typedef struct {
    char  name[32];
    int   twobyte;
    int   first, nw;
    short *w;              /* simple font widths (1/1000 em)           */
    int   dw;              /* CID default width                        */
    long *cw;              /* CID widths: triples first,last,width     */
    long  ncw;
    CMAPE *cm;             /* ToUnicode, sorted by code                */
    long  ncm;
    u16   enc[256];        /* simple font: code -> unicode             */
    int   attr;
} FONT;

#define MAXFONT 24

/* glyph names that are not single letters / uniXXXX */
static const struct { const char *n; u16 u; } gnames[] = {
    {"space",32},{"exclam",33},{"quotedbl",34},{"numbersign",35},{"dollar",36},
    {"percent",37},{"ampersand",38},{"quotesingle",39},{"quoteright",0x2019},
    {"parenleft",40},{"parenright",41},{"asterisk",42},{"plus",43},{"comma",44},
    {"hyphen",45},{"minus",0x2212},{"period",46},{"slash",47},{"zero",48},{"one",49},
    {"two",50},{"three",51},{"four",52},{"five",53},{"six",54},{"seven",55},
    {"eight",56},{"nine",57},{"colon",58},{"semicolon",59},{"less",60},{"equal",61},
    {"greater",62},{"question",63},{"at",64},{"bracketleft",91},{"backslash",92},
    {"bracketright",93},{"asciicircum",94},{"underscore",95},{"grave",96},
    {"quoteleft",0x2018},{"braceleft",123},{"bar",124},{"braceright",125},
    {"asciitilde",126},{"bullet",0x2022},{"endash",0x2013},{"emdash",0x2014},
    {"quotedblleft",0x201C},{"quotedblright",0x201D},{"quotesinglbase",0x201A},
    {"quotedblbase",0x201E},{"ellipsis",0x2026},{"dagger",0x2020},{"daggerdbl",0x2021},
    {"fi",0},{"fl",0},{"ff",0},{"ffi",0},{"ffl",0},{"trademark",0x2122},
    {"copyright",0xA9},{"registered",0xAE},{"degree",0xB0},{"section",0xA7},
    {"paragraph",0xB6},{"sterling",0xA3},{"yen",0xA5},{"Euro",0x20AC},{"cent",0xA2},
    {"germandbls",0xDF},{"plusminus",0xB1},{"multiply",0xD7},{"divide",0xF7},
    {"periodcentered",0xB7},{"guillemotleft",0xAB},{"guillemotright",0xBB},
    {"exclamdown",0xA1},{"questiondown",0xBF},{"nbspace",0xA0},{"mu",0xB5},
    {"onehalf",0xBD},{"onequarter",0xBC},{"threequarters",0xBE},{"dotlessi",0x131},
    {"Aring",0xC5},{"aring",0xE5},{"AE",0xC6},{"ae",0xE6},{"Oslash",0xD8},{"oslash",0xF8},
    {"OE",0x152},{"oe",0x153},{"Ccedilla",0xC7},{"ccedilla",0xE7},{"Ntilde",0xD1},
    {"ntilde",0xF1},{"florin",0x192},{"lozenge",0x25CA},{"arrowright",0x2192},
    {0,0}
};

static u16 glyph_uni(const char *n)
{
    int k;
    static const char acc[][12] = { "acute", "grave", "circumflex", "dieresis", "tilde" };
    static const u16 base[5][12] = {
        /* A E I O U Y a e i o u y */
        {0xC1,0xC9,0xCD,0xD3,0xDA,0xDD,0xE1,0xE9,0xED,0xF3,0xFA,0xFD},
        {0xC0,0xC8,0xCC,0xD2,0xD9,0,0xE0,0xE8,0xEC,0xF2,0xF9,0},
        {0xC2,0xCA,0xCE,0xD4,0xDB,0,0xE2,0xEA,0xEE,0xF4,0xFB,0},
        {0xC4,0xCB,0xCF,0xD6,0xDC,0x178,0xE4,0xEB,0xEF,0xF6,0xFC,0xFF},
        {0xC3,0,0,0xD5,0,0,0xE3,0,0,0xF5,0,0}
    };
    if (!n[0]) return 0;
    if (!n[1]) return (u8)n[0];
    if (!strncmp(n, "uni", 3) && strlen(n) >= 7)
        return (u16)strtol(n + 3, 0, 16);
    for (k = 0; gnames[k].n; k++)
        if (!strcmp(n, gnames[k].n))
            return gnames[k].u;
    for (k = 0; k < 5; k++)
        if (!strcmp(n + 1, acc[k])) {
            const char *L = strchr("AEIOUYaeiouy", n[0]);
            if (L)
                return base[k][L - "AEIOUYaeiouy"];
        }
    return 0;
}

static const u16 win1252_hi[32] = {
    0x20AC,0,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,0x02C6,0x2030,0x0160,0x2039,0x0152,0,0x017D,0,
    0,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x02DC,0x2122,0x0161,0x203A,0x0153,0,0x017E,0x0178
};

static int cmcmp(const void *a, const void *b)
{
    return (int)((const CMAPE *)a)->code - (int)((const CMAPE *)b)->code;
}

static void parse_tounicode(PDF *d, FONT *f, PV *stm)
{
    long len, i;
    u8 *data = stream_data(d, stm, &len);
    long cap = 256;
    LEX l;
    if (!data) return;
    f->cm = malloc(cap * sizeof(CMAPE));
    f->ncm = 0;
    l.p = data; l.n = len; l.i = 0;
    while (f->cm && l.i < l.n) {
        PV *t = parse_obj(d, &l, 1);
        int range;
        if (!t) break;
        if (t->type != PV_OP) continue;
        range = !strcmp(t->s, "beginbfrange");
        if (!range && strcmp(t->s, "beginbfchar")) continue;
        for (;;) {
            PV *a = parse_obj(d, &l, 1), *b, *c;
            long lo, hi, u;
            if (!a || a->type == PV_OP) break;
            b = parse_obj(d, &l, 1);
            if (!b) break;
            lo = 0;
            for (i = 0; i < a->i; i++) lo = (lo << 8) | (u8)a->s[i];
            if (!range) {
                hi = lo;
                c = b;
            } else {
                hi = 0;
                for (i = 0; i < b->i; i++) hi = (hi << 8) | (u8)b->s[i];
                c = parse_obj(d, &l, 1);
                if (!c) break;
            }
            if (hi - lo > 2048) hi = lo + 2048;
            for (u = lo; u <= hi; u++) {
                long uv = 0;
                if (c->type == PV_STR && c->i >= 2) {
                    uv = ((u8)c->s[0] << 8) | (u8)c->s[1];
                    if (uv >= 0xD800 && uv < 0xDC00) uv = '?';
                    uv += u - lo;
                } else if (c->type == PV_STR && c->i == 1)
                    uv = (u8)c->s[0] + (u - lo);
                else if (c->type == PV_ARRAY && u - lo < c->i) {
                    PV *e = c->a[u - lo];
                    if (e && e->type == PV_STR && e->i >= 2)
                        uv = ((u8)e->s[0] << 8) | (u8)e->s[1];
                }
                if (f->ncm == cap) {
                    CMAPE *n = realloc(f->cm, (cap *= 2) * sizeof(CMAPE));
                    if (!n) break;
                    f->cm = n;
                }
                f->cm[f->ncm].code = (u16)u;
                f->cm[f->ncm].uni = (u16)uv;
                f->ncm++;
            }
        }
    }
    free(data);
    if (f->cm)
        qsort(f->cm, f->ncm, sizeof(CMAPE), cmcmp);
}

static void free_font(FONT *f)
{
    free(f->w); free(f->cw); free(f->cm);
    memset(f, 0, sizeof *f);
}

static void load_font(PDF *d, FONT *f, PV *fd)
{
    PV *st = dgetr(d, fd, "Subtype"), *bf = dgetr(d, fd, "BaseFont");
    PV *tu = dgetr(d, fd, "ToUnicode"), *enc = dgetr(d, fd, "Encoding");
    int k;
    const char *base = bf && bf->type == PV_NAME ? bf->s : "";
    for (k = 0; k < 256; k++)
        f->enc[k] = (u16)k;
    for (k = 0x80; k < 0xA0; k++)
        f->enc[k] = win1252_hi[k - 0x80];
    f->enc[0x27] = 0x2019;                /* StandardEncoding quoteright */
    f->enc[0x60] = 0x2018;
    if (strstr(base, "Bold") || strstr(base, "Black") || strstr(base, "Heavy") || strstr(base, "Semibold"))
        f->attr |= TA_BOLD;
    if (strstr(base, "Italic") || strstr(base, "Oblique"))
        f->attr |= TA_ITALIC;
    /* family for choosing a GDOS font when drawing (attr bits 4-5) */
    {
        char lb[64];
        int k2, fam = 0;
        for (k2 = 0; base[k2] && k2 < 63; k2++)
            lb[k2] = (char)tolower((u8)base[k2]);
        lb[k2] = 0;
        if (strstr(lb, "courier") || strstr(lb, "mono") || strstr(lb, "consol") ||
            strstr(lb, "typewriter") || strstr(lb, "code"))
            fam = FF_MONO;
        else if (strstr(lb, "helvet") || strstr(lb, "arial") || strstr(lb, "sans") ||
                 strstr(lb, "swiss") || strstr(lb, "verdana") || strstr(lb, "calibri") ||
                 strstr(lb, "univers") || strstr(lb, "futura") || strstr(lb, "frutiger"))
            fam = FF_SANS;
        else if (strstr(lb, "times") || strstr(lb, "serif") || strstr(lb, "roman") ||
                 strstr(lb, "garamond") || strstr(lb, "georgia") || strstr(lb, "palatino") ||
                 strstr(lb, "book") || strstr(lb, "cambria") || strstr(lb, "dutch") ||
                 strstr(lb, "minion") || strstr(lb, "baskerville") || strstr(lb, "cmr") ||
                 strstr(lb, "lmroman") || strstr(lb, "texgyre"))
            fam = FF_SERIF;
        if (strstr(lb, "cmtt") || strstr(lb, "lmmono")) fam = FF_MONO;
        if (strstr(lb, "cmss") || strstr(lb, "heros")) fam = FF_SANS;
        f->attr |= fam << 4;
    }

    if (st && st->type == PV_NAME && !strcmp(st->s, "Type0")) {
        PV *df = dgetr(d, fd, "DescendantFonts"), *cid;
        f->twobyte = 1;
        f->dw = 1000;
        cid = df && df->type == PV_ARRAY && df->i ? res(d, df->a[0]) : 0;
        if (cid) {
            PV *dw = dgetr(d, cid, "DW"), *w = dgetr(d, cid, "W");
            if (dw) f->dw = (int)num(dw);
            if (w && w->type == PV_ARRAY) {
                long i = 0, cap = 64;
                f->cw = malloc(cap * 3 * sizeof(long));
                while (f->cw && i < w->i) {
                    PV *a = res(d, w->a[i]), *b = i + 1 < w->i ? res(d, w->a[i + 1]) : 0;
                    if (!a || !b) break;
                    if (b->type == PV_ARRAY) {
                        long j;
                        for (j = 0; j < b->i; j++) {
                            if (f->ncw == cap) {
                                long *n = realloc(f->cw, (cap *= 2) * 3 * sizeof(long));
                                if (!n) break;
                                f->cw = n;
                            }
                            f->cw[f->ncw * 3] = f->cw[f->ncw * 3 + 1] = a->i + j;
                            f->cw[f->ncw * 3 + 2] = (long)num(res(d, b->a[j]));
                            f->ncw++;
                        }
                        i += 2;
                    } else {
                        PV *c = i + 2 < w->i ? res(d, w->a[i + 2]) : 0;
                        if (!c) break;
                        if (f->ncw == cap) {
                            long *n = realloc(f->cw, (cap *= 2) * 3 * sizeof(long));
                            if (!n) break;
                            f->cw = n;
                        }
                        f->cw[f->ncw * 3] = a->i;
                        f->cw[f->ncw * 3 + 1] = b->i;
                        f->cw[f->ncw * 3 + 2] = (long)num(c);
                        f->ncw++;
                        i += 3;
                    }
                }
            }
        }
    } else {
        PV *fc = dgetr(d, fd, "FirstChar"), *ws = dgetr(d, fd, "Widths");
        if (ws && ws->type == PV_ARRAY) {
            long i;
            f->first = fc ? (int)fc->i : 0;
            f->nw = (int)ws->i;
            f->w = malloc(f->nw * sizeof(short));
            for (i = 0; f->w && i < f->nw; i++)
                f->w[i] = (short)num(res(d, ws->a[i]));
        }
        if (enc && enc->type == PV_NAME && !strcmp(enc->s, "MacRomanEncoding")) {
            static const u16 mac[] = { 0xC4,0xC5,0xC7,0xC9,0xD1,0xD6,0xDC,0xE1,0xE0,0xE2,0xE4,0xE3,0xE5,0xE7,0xE9,0xE8,
                                       0xEA,0xEB,0xED,0xEC,0xEE,0xEF,0xF1,0xF3,0xF2,0xF4,0xF6,0xF5,0xFA,0xF9,0xFB,0xFC,
                                       0x2020,0xB0,0xA2,0xA3,0xA7,0x2022,0xB6,0xDF,0xAE,0xA9,0x2122,0xB4,0xA8,0x2260,0xC6,0xD8 };
            for (k = 0; k < 48; k++) f->enc[0x80 + k] = mac[k];
            for (k = 0xB0; k < 256; k++) f->enc[k] = '?';
            f->enc[0xD0] = 0x2013; f->enc[0xD1] = 0x2014; f->enc[0xD2] = 0x201C;
            f->enc[0xD3] = 0x201D; f->enc[0xD4] = 0x2018; f->enc[0xD5] = 0x2019;
        } else if (enc && enc->type == PV_NAME && !strcmp(enc->s, "WinAnsiEncoding")) {
            f->enc[0x27] = 0x27;
            f->enc[0x60] = 0x60;
        }
        if (enc && enc->type == PV_DICT) {
            PV *diff = dgetr(d, enc, "Differences");
            if (diff && diff->type == PV_ARRAY) {
                long i, code = 0;
                for (i = 0; i < diff->i; i++) {
                    PV *e = diff->a[i];
                    if (!e) continue;
                    if (e->type == PV_INT) code = e->i;
                    else if (e->type == PV_NAME && code < 256) {
                        u16 u = glyph_uni(e->s);
                        f->enc[code] = u ? u : (u16)code;
                        code++;
                    }
                }
            }
        }
    }
    if (tu && tu->spos >= 0)
        parse_tounicode(d, f, tu);
}

static long font_uni(FONT *f, long code)
{
    if (f->cm && f->ncm) {
        long lo = 0, hi = f->ncm - 1;
        while (lo <= hi) {
            long m = (lo + hi) / 2;
            if (f->cm[m].code == code) return f->cm[m].uni;
            if (f->cm[m].code < code) lo = m + 1; else hi = m - 1;
        }
        if (f->twobyte) return 0;
    }
    if (f->twobyte)
        return code < 0x80 ? code : 0;   /* Identity-H without ToUnicode */
    return f->enc[code & 255];
}

static int font_width(FONT *f, long code)
{
    if (f->twobyte) {
        long i;
        for (i = 0; i < f->ncw; i++)
            if (code >= f->cw[i * 3] && code <= f->cw[i * 3 + 1])
                return (int)f->cw[i * 3 + 2];
        return f->dw;
    }
    if (f->w && code >= f->first && code < f->first + f->nw)
        return f->w[code - f->first] > 0 ? f->w[code - f->first] : 500;
    return code == 32 ? 250 : 500;
}

/* --------------------------------------------------------- interpreter */

/* small square root so no maths library is needed */
static float fsqrt(float v)
{
    float x = v > 1 ? v / 2 : 1;
    int i;
    if (v <= 0)
        return 0;
    for (i = 0; i < 12; i++)
        x = 0.5f * (x + v / x);
    return x;
}

typedef struct { float a, b, c, d, e, f; } MAT;

static MAT mmul(MAT x, MAT y)   /* x then y */
{
    MAT r;
    r.a = x.a * y.a + x.b * y.c;
    r.b = x.a * y.b + x.b * y.d;
    r.c = x.c * y.a + x.d * y.c;
    r.d = x.c * y.b + x.d * y.d;
    r.e = x.e * y.a + x.f * y.c + y.e;
    r.f = x.e * y.b + x.f * y.d + y.f;
    return r;
}
static MAT mident(void) { MAT m = { 1, 0, 0, 1, 0, 0 }; return m; }

typedef struct {
    MAT ctm;
    u8 fill[3], stroke[3];
    float lw;
} GSTATE;

typedef struct {
    PDF  *d;
    BUF  *out;
    long  nitems;
    GSTATE gs[32];
    int   gsp;
    /* text state */
    MAT   tm, tlm;
    float tc, tw, th, tl, tfs, trise;
    int   tr;
    FONT *font;
    FONT  fonts[MAXFONT];
    char  fnames[MAXFONT][32];
    int   nfonts;
    /* path */
    float *pts;            /* x,y pairs in page space */
    int   npts, cappts;
    int  *subs;            /* start index of each subpath */
    int   nsubs;
    float cx, cy;
    const PREFS *pf;
    int   depth;
} INTERP;

static void path_reset(INTERP *in) { in->npts = 0; in->nsubs = 0; }

static void path_pt(INTERP *in, float x, float y)
{
    MAT *m = &in->gs[in->gsp].ctm;
    if (in->npts == in->cappts) {
        int nc = in->cappts ? in->cappts * 2 : 256;
        float *np = realloc(in->pts, nc * 2 * sizeof(float));
        if (!np) return;
        in->pts = np;
        in->cappts = nc;
    }
    in->pts[in->npts * 2] = x * m->a + y * m->c + m->e;
    in->pts[in->npts * 2 + 1] = x * m->b + y * m->d + m->f;
    in->npts++;
    in->cx = x; in->cy = y;
}

static void path_move(INTERP *in, float x, float y)
{
    int *ns = realloc(in->subs, (in->nsubs + 1) * sizeof(int));
    if (!ns) return;
    in->subs = ns;
    in->subs[in->nsubs++] = in->npts;
    path_pt(in, x, y);
}

static void emit_poly(INTERP *in, int kind, const u8 *rgb, int from, int to)
{
    int n = to - from, i;
    if (n < 2 || n > 4000)
        return;
    buf_u8(in->out, kind);
    buf_put(in->out, rgb, 3);
    buf_u16(in->out, in->gs[in->gsp].lw * 16 > 32767 ? 32767u : (unsigned)(in->gs[in->gsp].lw * 16));
    buf_u16(in->out, n);
    for (i = from; i < to; i++) {
        buf_u32(in->out, (u32)(i32)(in->pts[i * 2] * 16));
        buf_u32(in->out, (u32)(i32)(in->pts[i * 2 + 1] * 16));
    }
    in->nitems++;
}

static void path_paint(INTERP *in, int fill, int stroke, int close)
{
    int s;
    if (!in->pf->pdf_gfx) {
        path_reset(in);
        return;
    }
    for (s = 0; s < in->nsubs; s++) {
        int from = in->subs[s], to = s + 1 < in->nsubs ? in->subs[s + 1] : in->npts;
        if (fill)
            emit_poly(in, PI_FILL, in->gs[in->gsp].fill, from, to);
        if (stroke) {
            if (close && to - from > 2) {     /* close by repeating first point */
                float x = in->pts[from * 2], y = in->pts[from * 2 + 1];
                if (in->npts == in->cappts) path_pt(in, 0, 0), in->npts--;
                if (to == in->npts) {
                    in->pts[in->npts * 2] = x;
                    in->pts[in->npts * 2 + 1] = y;
                    in->npts++;
                    to++;
                }
            }
            emit_poly(in, PI_LINE, in->gs[in->gsp].stroke, from, to);
        }
    }
    path_reset(in);
}

/* an image (or its placeholder box) covering the unit square under m */
static void emit_image(INTERP *in, MAT m, PV *xo)
{
    float xs[4], ys[4], x0, y0, x1, y1;
    int k, flags = 0;
    PIMG pi;
    xs[0] = m.e; ys[0] = m.f;
    xs[1] = m.a + m.e; ys[1] = m.b + m.f;
    xs[2] = m.c + m.e; ys[2] = m.d + m.f;
    xs[3] = m.a + m.c + m.e; ys[3] = m.b + m.d + m.f;
    x0 = x1 = xs[0]; y0 = y1 = ys[0];
    for (k = 1; k < 4; k++) {
        if (xs[k] < x0) x0 = xs[k];
        if (xs[k] > x1) x1 = xs[k];
        if (ys[k] < y0) y0 = ys[k];
        if (ys[k] > y1) y1 = ys[k];
    }
    memset(&pi, 0, sizeof pi);
    /* only unrotated images are drawn; rotated ones get the box */
    if (xo && in->pf->pdf_imgbox &&
        m.b * m.b < 0.0001f * m.a * m.a + 0.0001f && m.c * m.c < 0.0001f * m.d * m.d + 0.0001f &&
        load_image(in->d, xo, &pi, IMG_BUDGET - in->d->imgbytes)) {
        if (m.a < 0) flags |= PIFL_FLIPX;
        if (m.d < 0) flags |= PIFL_FLIPY;
        if (pi.mask) flags |= PIFL_MASK;
        in->d->imgbytes += pi.len;
    }
    buf_u8(in->out, PI_IMAGE);
    buf_put(in->out, in->gs[in->gsp].fill, 3);       /* mask colour */
    buf_u32(in->out, (u32)(i32)(x0 * 16));
    buf_u32(in->out, (u32)(i32)(y0 * 16));
    buf_u32(in->out, (u32)(i32)((x1 - x0) * 16));
    buf_u32(in->out, (u32)(i32)((y1 - y0) * 16));
    buf_u8(in->out, pi.data ? pi.fmt : PIF_NONE);
    buf_u8(in->out, flags);
    buf_u16(in->out, (unsigned)pi.w);
    buf_u16(in->out, (unsigned)pi.h);
    buf_u32(in->out, pi.data ? (u32)pi.len : 0);
    if (pi.data) {
        buf_put(in->out, pi.data, pi.len);
        free(pi.data);
    }
    in->nitems++;
}

static FONT *select_font(INTERP *in, PV *resd, const char *name)
{
    int k;
    PV *fdict, *fd;
    for (k = 0; k < in->nfonts; k++)
        if (!strcmp(in->fnames[k], name))
            return &in->fonts[k];
    if (in->nfonts == MAXFONT) {
        free_font(&in->fonts[0]);
        memmove(&in->fonts[0], &in->fonts[1], sizeof(FONT) * (MAXFONT - 1));
        memmove(in->fnames[0], in->fnames[1], sizeof(in->fnames[0]) * (MAXFONT - 1));
        in->nfonts--;
    }
    k = in->nfonts++;
    memset(&in->fonts[k], 0, sizeof(FONT));
    strncpy(in->fnames[k], name, 31);
    in->fnames[k][31] = 0;
    fdict = dgetr(in->d, resd, "Font");
    fd = dgetr(in->d, fdict, name);
    if (fd && fd->type == PV_DICT)
        load_font(in->d, &in->fonts[k], fd);
    else {
        int c;
        for (c = 0; c < 256; c++) in->fonts[k].enc[c] = (u16)c;
    }
    return &in->fonts[k];
}

/* show a string; emits one text item at the current text position */
static void show_text(INTERP *in, PV **items, int nitems)
{
    FONT *f = in->font;
    MAT trm;
    char txt[512];
    int n = 0, k;
    float startx, starty, size;
    MAT start;
    if (!f) return;

    start = mmul(in->tm, in->gs[in->gsp].ctm);
    startx = in->trise * start.c + start.e;
    starty = in->trise * start.d + start.f;
    size = in->tfs * fsqrt(start.c * start.c + start.d * start.d);

    for (k = 0; k < nitems; k++) {
        PV *s = items[k];
        long i;
        if (!s) continue;
        if (s->type == PV_INT || s->type == PV_REAL) {
            float adj = -num(s) / 1000.0f * in->tfs * in->th;
            in->tm.e += adj * in->tm.a;
            in->tm.f += adj * in->tm.b;
            if (num(s) < -180 && n && n < 500 && txt[n - 1] != ' ')
                txt[n++] = ' ';
            continue;
        }
        if (s->type != PV_STR) continue;
        for (i = 0; i < s->i; ) {
            long code;
            float wx;
            if (f->twobyte && i + 1 < s->i) {
                code = ((u8)s->s[i] << 8) | (u8)s->s[i + 1];
                i += 2;
            } else
                code = (u8)s->s[i++];
            wx = (font_width(f, code) / 1000.0f * in->tfs + in->tc +
                  (code == 32 && !f->twobyte ? in->tw : 0)) * in->th;
            in->tm.e += wx * in->tm.a;
            in->tm.f += wx * in->tm.b;
            if (n < 500) {
                long u = font_uni(f, code);
                int a;
                if (u == 0) continue;
                if (u < 0x20) u = ' ';
                a = uni_to_atari(u);
                if (a >= 0)
                    txt[n++] = (char)a;
                else {
                    char t[8], *p = t;
                    atari_translit(u, t);
                    if (u >= 0xFB00 && u <= 0xFB06) {  /* ligatures */
                        static const char *lig[] = { "ff", "fi", "fl", "ffi", "ffl", "st", "st" };
                        strcpy(t, lig[u - 0xFB00]);
                    }
                    while (*p && n < 500) txt[n++] = *p++;
                }
            }
        }
    }
    if (in->tr == 3 || n == 0)
        return;
    /* trim and emit */
    while (n && txt[n - 1] == ' ') n--;
    {
        int lead = 0;
        float endx;
        trm = mmul(in->tm, in->gs[in->gsp].ctm);
        endx = trm.e;
        while (lead < n && txt[lead] == ' ') lead++;
        if (lead == n) return;
        buf_u8(in->out, PI_TEXT);
        buf_put(in->out, in->gs[in->gsp].fill, 3);
        buf_u32(in->out, (u32)(i32)(startx * 16));
        buf_u32(in->out, (u32)(i32)(starty * 16));
        buf_u32(in->out, (u32)(i32)((endx - startx) * 16));
        buf_u16(in->out, (unsigned)(size * 16 > 65535 ? 65535 : size < 0 ? 0 : size * 16));
        buf_u8(in->out, f->attr);
        buf_u8(in->out, 0);
        buf_u16(in->out, n);
        buf_put(in->out, txt, n);
        in->nitems++;
    }
}

static void set_rgb(u8 *o, PV **op, int n)
{
    float v[4];
    int k;
    for (k = 0; k < n && k < 4; k++) v[k] = num(op[k]);
    if (n == 1) { o[0] = o[1] = o[2] = (u8)(v[0] * 255); return; }
    if (n == 3) { for (k = 0; k < 3; k++) o[k] = (u8)(v[k] < 0 ? 0 : v[k] > 1 ? 255 : v[k] * 255); return; }
    if (n == 4) {
        for (k = 0; k < 3; k++) {
            float c = 1 - (v[k] + v[3]);
            o[k] = (u8)(c < 0 ? 0 : c > 1 ? 255 : c * 255);
        }
    }
}

static void run_content(INTERP *in, const u8 *data, long len, PV *resd, PROG *pg);

static void do_xobject(INTERP *in, PV *resd, const char *name, PROG *pg)
{
    PV *xd = dgetr(in->d, resd, "XObject"), *xo = dgetr(in->d, xd, name), *st;
    if (!xo || xo->type != PV_DICT) return;
    st = dgetr(in->d, xo, "Subtype");
    if (st && st->type == PV_NAME && !strcmp(st->s, "Image"))
        emit_image(in, in->gs[in->gsp].ctm, xo);
    else if (st && st->type == PV_NAME && !strcmp(st->s, "Form") && in->depth < 4) {
        long len;
        u8 *data = stream_data(in->d, xo, &len);
        PV *m = dgetr(in->d, xo, "Matrix"), *r = dgetr(in->d, xo, "Resources");
        if (!data) return;
        if (in->gsp < 31) {
            in->gs[in->gsp + 1] = in->gs[in->gsp];
            in->gsp++;
            if (m && m->type == PV_ARRAY && m->i == 6) {
                MAT fm;
                fm.a = num(m->a[0]); fm.b = num(m->a[1]); fm.c = num(m->a[2]);
                fm.d = num(m->a[3]); fm.e = num(m->a[4]); fm.f = num(m->a[5]);
                in->gs[in->gsp].ctm = mmul(fm, in->gs[in->gsp].ctm);
            }
            in->depth++;
            run_content(in, data, len, r ? r : resd, pg);
            in->depth--;
            in->gsp--;
        }
        free(data);
    }
}

#define MAXOPS 48

static void run_content(INTERP *in, const u8 *data, long len, PV *resd, PROG *pg)
{
    LEX l;
    PV *ops[MAXOPS];
    int nop = 0;
    long count = 0;
    l.p = data; l.n = len; l.i = 0;
    while (l.i < l.n) {
        PV *t = parse_obj(in->d, &l, 0);
        const char *o;
        if (!t) break;
        if (t->type != PV_OP) {
            if (nop < MAXOPS) ops[nop++] = t;
            continue;
        }
        o = t->s;
        if ((++count & 1023) == 0 && pg && pg->cancelled)
            return;
#define OPN(n) (nop >= (n))
#define F(i) num(ops[nop - (n_) + (i)])
        switch (o[0]) {
        case 'q':
            if (!o[1] && in->gsp < 31) { in->gs[in->gsp + 1] = in->gs[in->gsp]; in->gsp++; }
            break;
        case 'Q':
            if (!o[1] && in->gsp > 0) in->gsp--;
            break;
        case 'c':
            if (!strcmp(o, "cm") && OPN(6)) {
                int n_ = 6;
                MAT m;
                m.a = F(0); m.b = F(1); m.c = F(2); m.d = F(3); m.e = F(4); m.f = F(5);
                in->gs[in->gsp].ctm = mmul(m, in->gs[in->gsp].ctm);
            } else if (!o[1] && OPN(6)) {          /* curveto: endpoint */
                int n_ = 6;
                MAT *m = &in->gs[in->gsp].ctm;
                float x0 = in->cx, y0 = in->cy, k;
                float x1 = F(0), y1 = F(1), x2 = F(2), y2 = F(3), x3 = F(4), y3 = F(5);
                (void)m;
                for (k = 0.25f; k <= 1.0f; k += 0.25f) {
                    float u = 1 - k;
                    path_pt(in, u*u*u*x0 + 3*u*u*k*x1 + 3*u*k*k*x2 + k*k*k*x3,
                                u*u*u*y0 + 3*u*u*k*y1 + 3*u*k*k*y2 + k*k*k*y3);
                }
            }
            break;
        case 'v': case 'y':
            if (!o[1] && OPN(4)) {
                int n_ = 4;
                path_pt(in, F(2), F(3));
            }
            break;
        case 'm':
            if (!o[1] && OPN(2)) { int n_ = 2; path_move(in, F(0), F(1)); }
            break;
        case 'l':
            if (!o[1] && OPN(2)) { int n_ = 2; if (!in->nsubs) path_move(in, F(0), F(1)); else path_pt(in, F(0), F(1)); }
            break;
        case 'h':
            if (!o[1] && in->nsubs) {
                int s = in->subs[in->nsubs - 1];
                float px = in->pts[s * 2], py = in->pts[s * 2 + 1];
                MAT *m = &in->gs[in->gsp].ctm;
                /* invert ctm cheaply is not needed: append the page point */
                (void)m;
                if (in->npts < in->cappts) {
                    in->pts[in->npts * 2] = px;
                    in->pts[in->npts * 2 + 1] = py;
                    in->npts++;
                }
            }
            break;
        case 'r':
            if (!strcmp(o, "re") && OPN(4)) {
                int n_ = 4;
                float x = F(0), y = F(1), w = F(2), h = F(3);
                path_move(in, x, y);
                path_pt(in, x + w, y);
                path_pt(in, x + w, y + h);
                path_pt(in, x, y + h);
                path_pt(in, x, y);
            } else if (!strcmp(o, "rg") && OPN(3))
                set_rgb(in->gs[in->gsp].fill, ops + nop - 3, 3);
            break;
        case 'R':
            if (!strcmp(o, "RG") && OPN(3)) set_rgb(in->gs[in->gsp].stroke, ops + nop - 3, 3);
            break;
        case 'g':
            if (!o[1] && OPN(1)) set_rgb(in->gs[in->gsp].fill, ops + nop - 1, 1);
            break;
        case 'G':
            if (!o[1] && OPN(1)) set_rgb(in->gs[in->gsp].stroke, ops + nop - 1, 1);
            break;
        case 'k':
            if (!o[1] && OPN(4)) set_rgb(in->gs[in->gsp].fill, ops + nop - 4, 4);
            break;
        case 'K':
            if (!o[1] && OPN(4)) set_rgb(in->gs[in->gsp].stroke, ops + nop - 4, 4);
            break;
        case 's':
            if (!strcmp(o, "sc") || !strcmp(o, "scn")) {
                int n = 0;
                while (n < nop && n < 4 && (ops[nop - 1 - n]->type == PV_INT || ops[nop - 1 - n]->type == PV_REAL)) n++;
                if (n == 1 || n == 3 || n == 4) set_rgb(in->gs[in->gsp].fill, ops + nop - n, n);
            } else if (!o[1])
                path_paint(in, 0, 1, 1);
            break;
        case 'S':
            if (!strcmp(o, "SC") || !strcmp(o, "SCN")) {
                int n = 0;
                while (n < nop && n < 4 && (ops[nop - 1 - n]->type == PV_INT || ops[nop - 1 - n]->type == PV_REAL)) n++;
                if (n == 1 || n == 3 || n == 4) set_rgb(in->gs[in->gsp].stroke, ops + nop - n, n);
            } else if (!o[1])
                path_paint(in, 0, 1, 0);
            break;
        case 'f': case 'F':
            path_paint(in, 1, 0, 0);
            break;
        case 'B':
            if (!strcmp(o, "BT")) {
                in->tm = in->tlm = mident();
            } else if (!strcmp(o, "BI")) {
                /* inline image: skip to EI */
                long j = l.i;
                while (j + 2 < l.n && !(l.p[j] == 'E' && l.p[j + 1] == 'I' &&
                        is_ws(l.p[j + 2]) && is_ws(l.p[j - 1]))) j++;
                l.i = j + 2;
                emit_image(in, in->gs[in->gsp].ctm, 0);
            } else if (!o[1] || !strcmp(o, "B*"))
                path_paint(in, 1, 1, 0);
            break;
        case 'b':
            path_paint(in, 1, 1, 1);
            break;
        case 'n':
            if (!o[1]) path_reset(in);
            break;
        case 'w':
            if (!o[1] && OPN(1)) in->gs[in->gsp].lw = num(ops[nop - 1]);
            break;
        case 'D':
            if (!strcmp(o, "Do") && OPN(1) && ops[nop - 1]->type == PV_NAME)
                do_xobject(in, resd, ops[nop - 1]->s, pg);
            break;
        case 'T':
            switch (o[1]) {
            case 'f':
                if (OPN(2) && ops[nop - 2]->type == PV_NAME) {
                    in->font = select_font(in, resd, ops[nop - 2]->s);
                    in->tfs = num(ops[nop - 1]);
                }
                break;
            case 'd': case 'D':
                if (OPN(2)) {
                    MAT t = mident();
                    t.e = num(ops[nop - 2]);
                    t.f = num(ops[nop - 1]);
                    if (o[1] == 'D') in->tl = -t.f;
                    in->tlm = in->tm = mmul(t, in->tlm);
                }
                break;
            case 'm':
                if (OPN(6)) {
                    int n_ = 6;
                    in->tm.a = F(0); in->tm.b = F(1); in->tm.c = F(2);
                    in->tm.d = F(3); in->tm.e = F(4); in->tm.f = F(5);
                    in->tlm = in->tm;
                }
                break;
            case '*': {
                MAT t = mident();
                t.f = -in->tl;
                in->tlm = in->tm = mmul(t, in->tlm);
                break;
            }
            case 'c': if (OPN(1)) in->tc = num(ops[nop - 1]); break;
            case 'w': if (OPN(1)) in->tw = num(ops[nop - 1]); break;
            case 'z': if (OPN(1)) in->th = num(ops[nop - 1]) / 100.0f; break;
            case 'L': if (OPN(1)) in->tl = num(ops[nop - 1]); break;
            case 's': if (OPN(1)) in->trise = num(ops[nop - 1]); break;
            case 'r': if (OPN(1)) in->tr = (int)num(ops[nop - 1]); break;
            case 'j':
                if (OPN(1)) show_text(in, ops + nop - 1, 1);
                break;
            case 'J':
                if (OPN(1) && ops[nop - 1]->type == PV_ARRAY)
                    show_text(in, ops[nop - 1]->a, (int)ops[nop - 1]->i);
                break;
            }
            break;
        case '\'':
        case '"': {
            MAT t = mident();
            if (o[0] == '"' && OPN(3)) {
                in->tw = num(ops[nop - 3]);
                in->tc = num(ops[nop - 2]);
            }
            t.f = -in->tl;
            in->tlm = in->tm = mmul(t, in->tlm);
            if (OPN(1)) show_text(in, ops + nop - 1, 1);
            break;
        }
        }
#undef F
#undef OPN
        nop = 0;
    }
}

/* -------------------------------------------------------------- page */

static PV *inherit(PDF *d, PV *page, const char *key)
{
    int guard = 32;
    while (page && guard--) {
        PV *v = dgetr(d, page, key);
        if (v) return v;
        page = dgetr(d, page, "Parent");
    }
    return 0;
}

static int do_page(PDF *d, long pnum, BUF *out, PROG *pg)
{
    PV *page = get_obj(d, pnum), *box, *resd, *cont;
    float bx[4] = { 0, 0, 612, 792 };
    long start, k;
    INTERP *in;

    if (!page || page->type != PV_DICT)
        return 0;
    box = inherit(d, page, "CropBox");
    if (!box) box = inherit(d, page, "MediaBox");
    if (box && box->type == PV_ARRAY && box->i == 4)
        for (k = 0; k < 4; k++)
            bx[k] = num(res(d, box->a[k]));
    if (bx[2] < bx[0]) { float t = bx[0]; bx[0] = bx[2]; bx[2] = t; }
    if (bx[3] < bx[1]) { float t = bx[1]; bx[1] = bx[3]; bx[3] = t; }

    start = out->len;
    buf_u32(out, 0);                         /* page record size */
    for (k = 0; k < 4; k++)
        buf_u32(out, (u32)(i32)(bx[k] * 16));
    buf_u32(out, 0);                         /* item count */

    in = calloc(1, sizeof(INTERP));
    if (!in) return E_NOMEM;
    in->d = d;
    in->out = out;
    in->pf = d->pf;
    in->gs[0].ctm = mident();
    in->gs[0].lw = 1;
    in->th = 1;
    resd = inherit(d, page, "Resources");
    cont = dgetr(d, page, "Contents");
    if (cont && cont->type == PV_DICT) {
        long len;
        u8 *data = stream_data(d, cont, &len);
        if (data) {
            run_content(in, data, len, resd, pg);
            free(data);
        }
    } else if (cont && cont->type == PV_ARRAY) {
        /* concatenate: operators may span stream boundaries */
        BUF all;
        buf_init(&all);
        for (k = 0; k < cont->i; k++) {
            PV *s = res(d, cont->a[k]);
            long len;
            u8 *data = s ? stream_data(d, s, &len) : 0;
            if (data) {
                buf_put(&all, data, len);
                buf_u8(&all, '\n');
                free(data);
            }
        }
        if (!all.err && all.len)
            run_content(in, all.p, all.len, resd, pg);
        buf_free(&all);
    }
    for (k = 0; k < in->nfonts; k++)
        free_font(&in->fonts[k]);
    free(in->pts);
    free(in->subs);
    buf_set_u32(out, start, out->len - start - 4);
    buf_set_u32(out, start + 4 + 16, in->nitems);
    free(in);
    return 0;
}

/* -------------------------------------------------------------- entry */

int dec_pdf(const char *path, const PREFS *pf, PROG *pg, u8 **blob, long *len)
{
    PDF d;
    int e;
    long i;
    BUF out;
    PV *root, *pages;

    memset(&d, 0, sizeof d);
    d.pf = pf;
    d.pg = pg;
    d.f = rd_slurp(path, &d.flen, pg, &e);
    if (!d.f)
        return e;
    e = scan_objects(&d, pg);
    if (e) goto done;

    /* encryption? */
    {
        long j;
        for (j = d.flen - 8; j > 0 && j > d.flen - 65536; j--)
            if (d.f[j] == '/' && !memcmp(d.f + j, "/Encrypt", 8) &&
                (j < 2 || d.f[j - 1] != '/')) {
                LEX l;
                PV *v;
                l.p = d.f; l.n = d.flen; l.i = j + 8;
                v = parse_obj(&d, &l, 1);
                if (v && (v->type == PV_REF || v->type == PV_DICT)) {
                    strcpy(fv_errdetail, "encrypted PDF");
                    e = E_UNSUP;
                    goto done;
                }
            }
    }
    root = find_root(&d);
    pages = root ? dget(res(&d, root), "Pages") : 0;
    if (pages)
        walk_pages(&d, pages, 0);
    afree(&d.ar);
    if (!d.npages) {
        e = E_FORMAT;
        goto done;
    }

    buf_init(&out);
    buf_put(&out, "FVPD", 4);
    buf_u32(&out, d.npages);
    for (i = 0; i < d.npages; i++) {
        d.pdone = d.npages + i;
        d.ptotal = d.npages * 2L;
        do_page(&d, d.pages[i], &out, pg);
        afree(&d.ar);
        if (prog_step(pg, d.npages + i + 1, d.npages * 2) || out.err) {
            e = out.err ? E_NOMEM : E_CANCEL;
            buf_free(&out);
            goto done;
        }
    }
    e = buf_finish(&out, RB_DOC, blob, len);

done:
    for (i = 0; i < d.nstm; i++) {
        fv_inflate_free(d.stm[i].data);
        free(d.stm[i].offs);
    }
    afree(&d.ar);
    afree(&d.dar);
    free(d.x);
    free(d.pages);
    free(d.f);
    return e;
}
