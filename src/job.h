/* job.h - decode / render jobs, shared by fview.prg and fvwork.ttp.
 * No GEM in here: the worker program does not link gemlib. */
#ifndef JOB_H
#define JOB_H

#include "fview.h"

enum { OP_LOAD = 1, OP_RENDER, OP_CONVERT };
enum { K_NONE = 0, K_IMAGE, K_TEXT, K_PDF, K_MAIN };

typedef struct {
    int   err;
    char  detail[80];
    int   kind;                  /* K_IMAGE, K_TEXT, K_PDF               */
    IMAGE im;                    /* original image (px in RB_ORIG)       */
    long  dw, dh, wdwidth;       /* rendered display image (RB_DISP)     */
    u8    pal[256 * 3];          /* window palette by pixel value        */
    long  doclen;                /* document blob length (RB_DOC)        */
    char  shm[RB_MAX][14];       /* shared memory file still to delete   */
    char  attached[RB_MAX];      /* block is attached shared memory      */
    long  size[RB_MAX];
    u8   *ptr[RB_MAX];           /* addresses in the GUI process         */
} JOBRES;

/* The request part of a job: this is what is sent to the worker. */
typedef struct {
    int   op;
    char  path[256];
    int   ftype;
    PREFS pf;
    DISPINFO disp;
    long  tw, th;                /* render / fit size                    */
    IMAGE src;                   /* OP_RENDER: original image header     */
    char  src_shm[14];           /* ... and its shared memory name       */
    /* OP_CONVERT (FConv): write path as an IMG at out                    */
    char  out[256];
    int   xfmt;                  /* XF_*                                 */
    int   outpdf;                /* write a PDF instead of an IMG        */
    long  maxw, maxh;            /* shrink to fit, 0 = keep the size      */
} JOBREQ;

typedef struct JOB {
    JOBREQ q;
    /* state (GUI side only) */
    int   worker;                /* running in fvwork.ttp                */
    int   pid;                   /* worker process                       */
    int   fd, wfd;               /* read from / write to the worker      */
    int   hello;                 /* worker has connected                 */
    int   stage;                 /* 1 = converting for the screen        */
    long  t0;                    /* start time (ms) for the timeout      */
    int   queued, running, done;
    long  pdone, ptotal;
    int   cancel;
    JOBRES res;
    struct VWIN *win;
} JOB;

/* worker <-> GUI messages. The worker inherits two pipes; their handle
 * numbers come in the FVWORK environment variable ("jr jw rr rw": job pipe
 * read/write ends, result pipe read/write ends) rather than on the command
 * line, so a desktop's ARGV in the environment can't get in the way.
 *   worker: HELLO            GUI: JOBREQ
 *   worker: PROG... RESULT + JOBRES
 *   GUI: one acknowledge byte (shared memory attached), worker exits  */
typedef struct { short type; long a, b; } WMSG;
enum { WM_PROG = 1, WM_RESULT, WM_HELLO, WM_STAGE };
#define WORKER_ENV "FVWORK"

/* jobcore.c */
int   job_exec(JOBREQ *q, JOBRES *r, PROG *pg);
void  res_set_shared(int on);    /* worker: results in U:\SHM          */
void  res_collect(JOBRES *r);    /* hand allocated blocks to a result   */
void  res_free_all(void);
void  shm_path(char *buf, const char *name);
void *shm_attach(const char *name, int del);
void  dbg_init(const char *path, const char *who);
void  dbg(const char *fmt, ...);
#define LOG_ENV "FVLOG"

#endif
