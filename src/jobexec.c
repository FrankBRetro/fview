/*
 * jobexec.c - the work an FView or FConv job does: load, render, convert.
 * Linked into fview.prg, fconv.prg and fvwork.ttp. No GEM calls in here.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "job.h"

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
        e = render_display(&q->src, q->tw, q->th, &q->disp, &q->pf,
                           r->pal, &buf, &wdw, pg);
        if (!e) {
            r->dw = q->tw; r->dh = q->th; r->wdwidth = wdw;
        }
    } else if (FT_IS_GRAPHIC(q->ftype)) {
        IMAGE *im = &r->im;
        img_init(im);
        r->kind = K_IMAGE;
        pg->halves = 1;
        e = decode_picture(q->path, q->ftype, im, &q->pf, &q->disp, pg);
        if (!e) {
            long tw, th, wdw;
            u8 *buf;
            img_display_size(im, &q->disp, &tw, &th);
            if (q->tw > 0 && q->th > 0)          /* fit to window */
                img_fit(tw, th, q->tw, q->th, &tw, &th);
            pg->phase = 1;
            e = render_display(im, tw, th, &q->disp, &q->pf, r->pal,
                               &buf, &wdw, pg);
            if (!e) {
                r->dw = tw; r->dh = th; r->wdwidth = wdw;
            }
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
