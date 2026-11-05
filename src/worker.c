/*
 * worker.c - FVWORK.TTP (FView, FConv) and FAWORK.TTP (FArchive), the
 * background worker for MiNT; jobexec.c or arcjob.c is linked in.
 *
 * fview starts one of these per job with Pexec(100) (asynchronous), so it
 * runs side by side with the GUI in its own memory. (fork() can't be used
 * for this: on machines without an MMU MiNT suspends the parent until a
 * forked child exits, which froze fview's windows.)
 *
 * It inherits two pipes from fview; their handle numbers come in the
 * FVWORK environment variable (not on the command line, which a desktop's
 * ARGV could override). It says hello, reads its JOBREQ, drops to
 * nice 19, does the work with results in U:\SHM, reports progress and
 * the result header, then waits for fview's
 * acknowledgement (shared memory attached) before it exits.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <osbind.h>
#include <mintbind.h>
#include "job.h"

static short fd, rfd;
static volatile int cancel;
static JOBREQ req;
static JOBRES res;

static void on_term(int sig)
{
    (void)sig;
    cancel = 1;
}

static void send(short type, long a, long b)
{
    WMSG m;
    m.type = type;
    m.a = a;
    m.b = b;
    Fwrite(fd, sizeof m, &m);
}

static int prog(PROG *p, long done, long total)
{
    static long last = -1;
    static int stage;
    long pct = total > 0 ? done * 100 / total : 0;
    if (p->phases > 1) {
        if (p->phase != stage) {
            stage = p->phase;
            send(WM_STAGE, stage, 0);   /* reading / converting / writing */
        }
    } else if (p->halves && p->phase && !stage) {
        stage = 1;
        send(WM_STAGE, 1, 0);           /* now dithering for the screen */
    }
    if (pct > 100)
        pct = 100;
    if (p->phases > 1)
        pct = (p->phase * 100 + pct) / p->phases;
    else if (p->halves)
        pct = pct / 2 + (p->phase ? 50 : 0);
    if (pct != last) {
        send(WM_PROG, pct, 100);
        last = pct;
    }
    return cancel;
}

static void note(int kind, const char *text)
{
    long n = (long)strlen(text);
    WMSG m;
    m.type = WM_NOTE;
    m.a = n;
    m.b = kind;
    Fwrite(fd, sizeof m, &m);
    if (n)
        Fwrite(fd, n, text);
}

static long readall(void *buf, long n)
{
    long got = 0;
    while (got < n) {
        long r = Fread(rfd, n - got, (char *)buf + got);
        if (r <= 0)
            break;
        got += r;
    }
    return got;
}

int main(void)
{
    PROG pg;
    char ack, *val = getenv(WORKER_ENV);
    int jr, jw, rr, rw;

    if (getenv(LOG_ENV))
        dbg_init(getenv(LOG_ENV), "work");
    dbg("worker " FV_VERSION " (" FV_CPU ") started, FVWORK='%s'", val ? val : "(none)");
    if (!val || sscanf(val, "%d %d %d %d", &jr, &jw, &rr, &rw) != 4)
        return 2;
    rfd = (short)jr;                    /* job pipe: we read      */
    fd = (short)rw;                     /* result pipe: we write  */
    Fclose((short)jw);                  /* fview's ends, if inherited */
    Fclose((short)rr);

    signal(SIGTERM, on_term);
    signal(SIGINT, SIG_IGN);

    send(WM_HELLO, Pgetpid(), 0);       /* before niceing: answer quickly */
    nice(19);
    dbg("hello sent, reading %ld byte request", (long)sizeof req);
    if (readall(&req, sizeof req) != (long)sizeof req) {
        dbg("request read failed");
        Fclose(fd);
        Fclose(rfd);
        return 4;
    }

    memset(&res, 0, sizeof res);
    memset(&pg, 0, sizeof pg);
    pg.fn = prog;
    job_note_hook = note;
    res_set_shared(1);
    if (req.op == OP_RENDER) {
        req.src.px = req.src_shm[0] ? shm_attach(req.src_shm, 0) : 0;
        if (!req.src.px) {
            res.err = E_NOMEM;
            strcpy(res.detail, "worker could not attach the picture");
        }
    }
    if (!res.err)
        job_exec(&req, &res, &pg);
    res_collect(&res);

    send(WM_RESULT, sizeof(JOBRES), 0);
    Fwrite(fd, sizeof(JOBRES), &res);
    dbg("result sent: err %d, shm '%s' '%s' '%s'", res.err, res.shm[0], res.shm[1], res.shm[2]);

    /* wait until fview has attached the shared memory (or went away) */
    Fread(rfd, 1, &ack);
    dbg("ack received, exiting");
    Fclose(fd);
    Fclose(rfd);
    return 0;
}
