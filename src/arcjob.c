/*
 * arcjob.c - the work an FArchive job does: list, extract or add.
 * Linked into farchive.prg (cooperative jobs under TOS) and fawork.ttp
 * (the background worker under MiNT). No GEM calls in here.
 *
 * A listing comes back in RB_DOC:
 *   "FAL1", fmt (2), count (4), totals usize, csize (4 + 4), then per
 *   member: usize, csize, dostime (4 each), flags (1: 1 folder, 2 not
 *   supported, 4 merged group), method name (8), name length (1), name.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "job.h"
#include "archive.h"

static void put4(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}

static void jlog(void *ctx, const char *line)
{
    (void)ctx;
    job_note(NOTE_LOG, line);
}

static void jitem(void *ctx, int i, int n, const char *name)
{
    char b[300];
    (void)ctx;
    sprintf(b, "%d %d %.250s", i, n, name);
    job_note(NOTE_ITEM, b);
}

static int do_list(JOBREQ *q, JOBRES *r)
{
    ARCH a;
    long size = 18, i;
    u8 *b, *p;
    int e = arch_open(&a, q->path);
    if (e) {
        arch_close(&a);
        return e;
    }
    for (i = 0; i < a.n; i++)
        size += 22 + (long)strlen(a.e[i].name);
    b = res_alloc(RB_DOC, size);
    if (!b) {
        arch_close(&a);
        return E_NOMEM;
    }
    memcpy(b, "FAL1", 4);
    b[4] = (u8)(a.fmt >> 8);
    b[5] = (u8)a.fmt;
    put4(b + 6, (u32)a.n);
    put4(b + 10, (u32)a.usize);
    put4(b + 14, (u32)a.csize);
    p = b + 18;
    for (i = 0; i < a.n; i++) {
        AENT *e2 = &a.e[i];
        int nl = (int)strlen(e2->name);
        if (nl > 255) nl = 255;
        put4(p, (u32)e2->usize);
        put4(p + 4, (u32)e2->csize);
        put4(p + 8, e2->dostime);
        p[12] = (u8)((e2->dir ? 1 : 0) | (e2->method < 0 ? 2 : 0) | (e2->group ? 4 : 0));
        memcpy(p + 13, e2->mname, 8);
        p[21] = (u8)nl;
        memcpy(p + 22, e2->name, nl);
        p += 22 + nl;
    }
    r->kind = K_ARCLIST;
    r->doclen = (long)(p - b);
    arch_close(&a);
    return E_OK;
}

static void set_opts(AOPT *o, JOBREQ *q, PROG *pg)
{
    memset(o, 0, sizeof *o);
    o->log = jlog;
    o->item = jitem;
    o->replace = (q->aflags & AJ_REPLACE) != 0;
    o->rename83 = (q->aflags & AJ_RENAME83) != 0;
    o->subdirs = (q->aflags & AJ_SUBDIRS) != 0;
    o->paths = (q->aflags & AJ_PATHS) != 0;
    o->level = q->aflags & AJ_FAST ? 1 : q->aflags & AJ_BEST ? 3 : 2;
    o->pg = pg;
}

static int do_extract(JOBREQ *q, JOBRES *r, PROG *pg)
{
    ARCH a;
    AOPT o;
    u8 *sel = 0;
    int e = arch_open(&a, q->path);
    if (e) {
        arch_close(&a);
        return e;
    }
    if (q->list[0]) {                  /* one byte per member, 1 = take it */
        FILE *f = fopen(q->list, "rb");
        sel = calloc(1, a.n + 1);
        if (f && sel)
            fread(sel, 1, a.n, f);
        if (f)
            fclose(f);
    }
    set_opts(&o, q, pg);
    e = arch_extract(&a, q->out, sel, &o);
    r->acount[0] = o.nok;
    r->acount[1] = o.nfail;
    r->acount[2] = o.nskip;
    r->acount[3] = o.nren;
    free(sel);
    arch_close(&a);
    return e;
}

static int do_add(JOBREQ *q, JOBRES *r, PROG *pg)
{
    AOPT o;
    FILE *f = fopen(q->list, "r");
    char line[260], **files = 0;
    int n = 0, cap = 0, e, i;
    if (!f)
        return E_OPEN;
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
            line[--l] = 0;
        if (!l)
            continue;
        if (n == cap) {
            char **nf = realloc(files, (cap = cap ? cap * 2 : 16) * sizeof(char *));
            if (!nf)
                break;
            files = nf;
        }
        files[n] = malloc(l + 1);
        if (!files[n])
            break;
        strcpy(files[n++], line);
    }
    fclose(f);
    set_opts(&o, q, pg);
    e = arch_add(q->path, q->afmt, files, n, &o);
    r->acount[0] = o.nok;
    r->acount[1] = o.nfail;
    r->acount[2] = o.nskip;
    r->acount[3] = o.nren;
    for (i = 0; i < n; i++)
        free(files[i]);
    free(files);
    return e;
}

int job_exec(JOBREQ *q, JOBRES *r, PROG *pg)
{
    int e;
    fv_errdetail[0] = 0;
    r->kind = K_NONE;
    switch (q->op) {
    case OP_ARC_LIST:    e = do_list(q, r); break;
    case OP_ARC_EXTRACT: e = do_extract(q, r, pg); break;
    case OP_ARC_ADD:     e = do_add(q, r, pg); break;
    default:             e = E_UNSUP;
    }
    if (pg->cancelled)
        e = E_CANCEL;
    dbg("job_exec op %d -> err %d", q->op, e);
    if (e)
        res_free_all();
    r->err = e;
    strncpy(r->detail, fv_errdetail, sizeof r->detail - 1);
    r->detail[sizeof r->detail - 1] = 0;
    return e;
}
