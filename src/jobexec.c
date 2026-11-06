/*
 * jobexec.c - the work an FView or FConv job does: load, render, convert,
 * and FView's picture tools (copy, reduce, remap, resize, count, share).
 * Linked into fview.prg, fconv.prg and fvwork.ttp. No GEM calls in here.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "job.h"
#include "palette.h"

/* paletted pictures on a 256+ colour screen: map their colours straight
   (a dither pattern only adds noise there) unless the user wants it    */
static void pick_dither(const IMAGE *im, JOBREQ *q)
{
    if (im->type == IT_INDEXED && !q->disp.truecolor && q->disp.planes >= 8 &&
        !q->pf.dither_pal && q->op == OP_LOAD)
        q->pf.dither = DITH_NONE;
}

/* the display version of im, real size or fitted into q->tw x q->th    */
static int show(JOBREQ *q, JOBRES *r, IMAGE *im, PROG *pg)
{
    long tw, th, wdw;
    u8 *buf;
    int e;
    pick_dither(im, q);
    r->rdither = q->pf.dither;
    img_display_size(im, &q->disp, &tw, &th);
    if (q->tw > 0 && q->th > 0)              /* fit to window */
        img_fit(tw, th, q->tw, q->th, &tw, &th);
    e = render_display(im, tw, th, &q->disp, &q->pf, r->pal, &buf, &wdw, pg);
    if (!e) {
        r->dw = tw; r->dh = th; r->wdwidth = wdw;
    }
    return e;
}

/* OP_SHAREPAL: the pictures named in the list file                     */
static int share_pal(JOBREQ *q, JOBRES *r, PROG *pg)
{
    FILE *f = fopen(q->list, "rb");
    SPREC rec;
    const IMAGE *ims[32];
    IMAGE *own[32];
    int n = 0, e, i;
    if (!f)
        return E_OPEN;
    while (n < 32 && fread(&rec, sizeof rec, 1, f) == 1) {
        own[n] = malloc(sizeof(IMAGE));
        if (!own[n])
            break;
        *own[n] = rec.im;
        if (rec.shm[0] && res_is_shared())   /* in a worker: attach it */
            own[n]->px = shm_attach(rec.shm, 0);
        if (own[n]->px)
            ims[n] = own[n], n++;
        else
            free(own[n]);
    }
    fclose(f);
    e = n ? shared_palette(ims, n, &q->disp, r->pal, pg) : E_FORMAT;
    for (i = 0; i < n; i++)
        free(own[i]);
    return e;
}

/* -------------------------------------------------------------- the work */

int job_exec(JOBREQ *q, JOBRES *r, PROG *pg)
{
    int e = E_OK;

    fv_errdetail[0] = 0;
    if (q->op == OP_CONVERT) {
        r->kind = K_NONE;
        e = convert_picture(q->path, q->ftype, q->out, q->xfmt, q->outpdf,
                            q->maxw, q->maxh, &q->pf, &q->disp, pg);
    } else if (q->op == OP_RENDER) {
        long wdw;
        u8 *buf;
        r->kind = K_IMAGE;
        r->rdither = q->pf.dither;
        e = render_display(&q->src, q->tw, q->th, &q->disp, &q->pf,
                           r->pal, &buf, &wdw, pg);
        if (!e) {
            r->dw = q->tw; r->dh = q->th; r->wdwidth = wdw;
        }
    } else if (q->op == OP_COUNT) {
        int used = 0;
        r->kind = K_NONE;
        pg->halves = 1;
        r->acount[0] = img_count_real(&q->src, &used, pg);
        r->acount[2] = used;
        pg->phase = 1;
        if (r->acount[0] >= 0)
            r->acount[1] = img_count_seen(&q->src, q->tw, q->th, &q->disp, &q->pf, pg);
        e = r->acount[0] == -2 || r->acount[1] == -2 ? E_NOMEM :
            r->acount[0] < 0 || r->acount[1] < 0 ? E_CANCEL : E_OK;
    } else if (q->op == OP_SHAREPAL) {
        r->kind = K_NONE;
        e = share_pal(q, r, pg);
    } else if (q->op == OP_DUP || q->op == OP_REDUCE || q->op == OP_RESIZE) {
        IMAGE *im = &r->im;
        r->kind = K_IMAGE;
        pg->halves = 1;
        if (q->op == OP_DUP) {
            *im = q->src;
            e = img_alloc_px(im, RB_ORIG);
            if (!e) {
                long y;
                for (y = 0; y < im->h; y++) {
                    memcpy(im->px + y * im->stride, q->src.px + y * q->src.stride, im->stride);
                    if ((y & 31) == 0 && prog_step(pg, y, im->h)) {
                        e = E_CANCEL;
                        break;
                    }
                }
            }
        } else if (q->op == OP_RESIZE)
            e = img_resize(&q->src, im, q->nw, q->nh, q->smooth, pg);
        else if (q->xpal_n > 0)
            e = img_remap(&q->src, im, q->xpal, q->xpal_n, &q->pf, pg);
        else
            e = img_reduce_colours(&q->src, im, q->ncol, &q->pf, pg);
        if (!e) {
            pg->phase = 1;
            e = show(q, r, im, pg);
        }
    } else if (FT_IS_GRAPHIC(q->ftype)) {
        IMAGE *im = &r->im;
        img_init(im);
        r->kind = K_IMAGE;
        pg->halves = 1;
        e = decode_picture(q->path, q->ftype, im, &q->pf, &q->disp, pg);
        if (!e) {
            pg->phase = 1;
            e = show(q, r, im, pg);
        }
    } else if (q->ftype == FT_PDF) {
        u8 *b;
        r->kind = K_PDF;
        e = dec_pdf(q->path, &q->pf, pg, &b, &r->doclen);
    } else if (q->ftype == FT_GEM) {
        u8 *b;
        r->kind = K_PDF;                 /* drawn like a one-page PDF */
        e = dec_gem(q->path, &q->pf, pg, &b, &r->doclen);
    } else {
        u8 *b;
        r->kind = K_TEXT;
        e = dec_text(q->path, q->ftype, &q->pf, pg, &b, &r->doclen);
    }
    if (pg->cancelled)
        e = E_CANCEL;
    dbg("job_exec op %d ftype %d -> err %d kind %d dw %ld dh %ld wdw %ld",
        q->op, q->ftype, e, r->kind, r->dw, r->dh, r->wdwidth);
    if (e)
        res_free_all();
    r->err = e;
    strncpy(r->detail, fv_errdetail, sizeof r->detail - 1);
    r->detail[sizeof r->detail - 1] = 0;
    return e;
}
