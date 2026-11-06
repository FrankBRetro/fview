/* job.h - decode / render jobs, shared by fview.prg and fvwork.ttp.
 * No GEM in here: the worker program does not link gemlib. */
#ifndef JOB_H
#define JOB_H

#include "fview.h"

enum { OP_LOAD = 1, OP_RENDER, OP_CONVERT, OP_ARC_LIST, OP_ARC_EXTRACT, OP_ARC_ADD,
       /* FView's picture tools: each works on q->src (the window's copy)
          and, but for OP_COUNT and OP_SHAREPAL, makes a new copy in
          RB_ORIG and renders it like OP_LOAD                          */
       OP_DUP,                   /* a copy of src                        */
       OP_REDUCE,                /* src in ncol colours, or mapped onto
                                    xpal when xpal_n > 0                 */
       OP_RESIZE,                /* src at nw x nh                       */
       OP_COUNT,                 /* acount[0] real colours, [1] seen on
                                    the screen, [2] palette entries used */
       OP_SHAREPAL };            /* one palette for the pictures listed
                                    in list (SPREC records) -> pal       */
/* the jobs that read q->src, so a worker must attach src_shm          */
#define OP_USES_SRC(op) ((op) == OP_RENDER || ((op) >= OP_DUP && (op) <= OP_COUNT))
enum { K_NONE = 0, K_IMAGE, K_TEXT, K_PDF, K_MAIN, K_ARCLIST };

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
    long  acount[4];             /* FArchive: ok, failed, skipped, renamed;
                                    OP_COUNT: real, seen, entries used     */
    int   rdither;               /* the dithering the render used         */
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
    /* OP_ARC_* (FArchive): archive in path, folder in out              */
    int   afmt;                  /* AF_* for a new archive               */
    int   aflags;                /* AJ_* below                           */
    char  list[256];             /* file of names (add) / selection (extract) */
    /* FView picture tools                                                */
    int   ncol;                  /* OP_REDUCE: colours                    */
    int   xpal_n;                /* OP_REDUCE: map onto these instead     */
    u8    xpal[256 * 3];
    long  nw, nh;                /* OP_RESIZE                             */
    int   smooth;                /* OP_RESIZE: average / blend            */
} JOBREQ;

/* OP_SHAREPAL: q->list names a file of these, one per picture          */
typedef struct {
    IMAGE im;                    /* header (px valid in the GUI process)  */
    char  shm[14];               /* its shared memory, "" if none         */
} SPREC;
enum { AJ_REPLACE = 1, AJ_RENAME83 = 2, AJ_SUBDIRS = 4, AJ_PATHS = 8, AJ_FAST = 16, AJ_BEST = 32 };

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
enum { WM_PROG = 1, WM_RESULT, WM_HELLO, WM_STAGE, WM_NOTE };
/* WM_NOTE: a = text length (the text follows), b = NOTE_* */
enum { NOTE_LOG = 0, NOTE_ITEM };
#define WORKER_ENV "FVWORK"

/* jobcore.c */
int   job_exec(JOBREQ *q, JOBRES *r, PROG *pg);
/* a line for the GUI (log, or "i n name" for NOTE_ITEM) from a job */
void  job_note(int kind, const char *text);
extern void (*job_note_hook)(int kind, const char *text);
void  res_set_shared(int on);    /* worker: results in U:\SHM          */
int   res_is_shared(void);        /* running in a worker                 */
void  res_collect(JOBRES *r);    /* hand allocated blocks to a result   */
void  res_free_all(void);
void  shm_path(char *buf, const char *name);
void *shm_attach(const char *name, int del);
void  dbg_init(const char *path, const char *who);
void  dbg(const char *fmt, ...);
#define LOG_ENV "FVLOG"

#endif
