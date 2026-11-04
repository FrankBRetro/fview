/*
 * job.c - running decode/render jobs (GUI side).
 *
 * MiNT, with U:\SHM, FVWORK.TTP next to fview and "MiNT workers" on:
 *   each job runs in its own FVWORK.TTP process, started asynchronously
 *   with Pexec(100) and running at nice 19, so windows load side by side
 *   while fview stays responsive. The request goes down one pipe;
 *   progress and the result header come back up another. Big results sit
 *   in shared memory (U:\SHM) that fview attaches without copying.
 *   Cancel sends SIGTERM; the worker stops at its next progress step.
 *
 * Plain TOS (or no worker):
 *   jobs are queued and run one at a time inside fview. Their progress
 *   hook keeps the event loop going (gui_coop_poll), so windows redraw,
 *   move and the Cancel button works while a file loads.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <osbind.h>
#include <mintbind.h>
#include <mint/cookie.h>
#include "gemui.h"

#ifndef FV_LOG
#define FV_LOG 0                  /* the Makefile's log build sets 1 */
#endif
#ifndef F_SETFD
#define F_SETFD 2
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif

int have_mint, have_shm, have_worker;
int coop_busy;
/* which program this is, for the step log (FConv sets its own) */
const char *job_app_name = "fview " FV_VERSION " (" FV_CPU ")";
const char *job_log_name = "FVIEW.LOG";

#define MAXJOBS 32
static JOB *jobs[MAXJOBS];
static char worker_path[256];
char log_path[256];

/* ------------------------------------------------------------- results */

void job_release(JOBRES *r, int slot)
{
    if (r->ptr[slot]) {
        if (r->attached[slot])
            Mfree(r->ptr[slot]);          /* detaches shared memory     */
        else
            free(r->ptr[slot]);
    }
    if (r->shm[slot][0]) {                /* file kept for re-renders   */
        char path[40];
        shm_path(path, r->shm[slot]);
        Fdelete(path);
    }
    r->shm[slot][0] = 0;
    r->attached[slot] = 0;
    r->ptr[slot] = 0;
    r->size[slot] = 0;
}

/* attach every block the worker produced */
static void attach_results(JOB *j)
{
    int i;
    for (i = 0; i < RB_MAX; i++) {
        j->res.ptr[i] = 0;
        j->res.attached[i] = 0;
        if (!j->res.shm[i][0])
            continue;
        /* the original's file stays so later re-renders can attach it;
           the others go now (the memory lives while it is attached)    */
        j->res.ptr[i] = shm_attach(j->res.shm[i], i != RB_ORIG);
        if (j->res.ptr[i])
            j->res.attached[i] = 1;
        else if (!j->res.err) {
            j->res.err = E_NOMEM;
            strcpy(j->res.detail, "could not attach shared memory");
        }
        if (i != RB_ORIG)
            j->res.shm[i][0] = 0;
    }
    if (j->res.err)
        for (i = 0; i < RB_MAX; i++)
            job_release(&j->res, i);
}

/* ---------------------------------------------------------------- setup */

static int file_exists(const char *p)
{
    return Fattrib(p, 0, 0) >= 0;
}

/* the folder FVIEW.PRG was started from (ends in a backslash) */
static void app_dir(char *dir)
{
    char cmd[256], tail[128], *s;
    cmd[0] = 0;
    if (shel_read(cmd, tail) && cmd[0] &&
        ((s = strrchr(cmd, '\\')) != 0 || (s = strrchr(cmd, '/')) != 0)) {
        s[1] = 0;
        if (cmd[1] == ':' || cmd[0] == '\\' || cmd[0] == '/') {
            strcpy(dir, cmd);
            return;
        }
        /* relative: prefix the current directory */
        dir[0] = Dgetdrv() + 'A';
        dir[1] = ':';
        Dgetpath(dir + 2, 0);
        strcat(dir, "\\");
        strcat(dir, cmd);
        return;
    }
    dir[0] = Dgetdrv() + 'A';
    dir[1] = ':';
    Dgetpath(dir + 2, 0);
    strcat(dir, "\\");
}

/* FVWORK.TTP lives next to FVIEW.PRG */
static void find_worker(const char *dir)
{
    char p[256];
    worker_path[0] = 0;
    strcpy(p, dir);
    strcat(p, "FVWORK.TTP");
    if (file_exists(p)) {
        strcpy(worker_path, p);
        return;
    }
    /* the current directory, as a fallback */
    p[0] = Dgetdrv() + 'A';
    p[1] = ':';
    Dgetpath(p + 2, 0);
    strcat(p, "\\FVWORK.TTP");
    if (file_exists(p))
        strcpy(worker_path, p);
}

void job_init_system(void)
{
    long v;
    char dir[256];
    app_dir(dir);
#if FV_LOG
    /* step log next to the program; switch off with FV_LOG=0 */
    strcpy(log_path, dir);
    strcat(log_path, job_log_name);
    Fdelete(log_path);
    dbg_init(log_path, "gui ");
    dbg("%s started, folder %s", job_app_name, dir);
#endif
    have_mint = Getcookie(C_MiNT, &v) == C_FOUND;
    have_shm = 0;
    have_worker = 0;
    if (have_mint) {
        long r = Dopendir("U:\\SHM", 0);
        if ((r & 0xFF000000L) != 0xFF000000L) {
            Dclosedir(r);
            have_shm = 1;
        }
        find_worker(dir);
        have_worker = have_shm && worker_path[0];
    }
    dbg("mint %d shm %d worker %d '%s'", have_mint, have_shm, have_worker, worker_path);
}

/* -------------------------------------------------------- worker start */

extern char **environ;

/* fview's environment for the worker, without ARGV (which mintlib would
   take as the worker's arguments) and with FVWORK=<pipe name> added   */
static char *worker_env(const char *pipe)
{
    long n = 0;
    char **e, *buf, *o;
    for (e = environ; e && *e; e++) {
        if (!strncmp(*e, "ARGV=", 5))
            break;                      /* fview's own arguments follow */
        if (strncmp(*e, WORKER_ENV "=", sizeof WORKER_ENV))
            n += strlen(*e) + 1;
    }
    n += strlen(WORKER_ENV) + 1 + strlen(pipe) + 1 + 2;
    n += strlen(LOG_ENV) + 2 + strlen(log_path);
    buf = malloc(n);
    if (!buf)
        return 0;
    o = buf;
    for (e = environ; e && *e; e++) {
        if (!strncmp(*e, "ARGV=", 5))
            break;
        if (!strncmp(*e, WORKER_ENV "=", sizeof WORKER_ENV))
            continue;
        strcpy(o, *e);
        o += strlen(o) + 1;
    }
    strcpy(o, WORKER_ENV "=");
    strcat(o, pipe);
    o += strlen(o) + 1;
    if (log_path[0]) {
        strcpy(o, LOG_ENV "=");
        strcat(o, log_path);
        o += strlen(o) + 1;
    }
    *o = 0;
    return buf;
}

static int start_worker(JOB *j)
{
    short jp[2], rp[2];
    char tail[2], *env, val[48];
    long pid;

    if (Fpipe(jp) < 0)
        return 0;
    if (Fpipe(rp) < 0) {
        Fclose(jp[0]); Fclose(jp[1]);
        return 0;
    }
    /* fview's own ends must not leak into later workers */
    Fcntl(jp[1], 1L, F_SETFD);
    Fcntl(rp[0], 1L, F_SETFD);
    sprintf(val, "%d %d %d %d", jp[0], jp[1], rp[0], rp[1]);
    env = worker_env(val);
    if (!env) {
        Fclose(jp[0]); Fclose(jp[1]); Fclose(rp[0]); Fclose(rp[1]);
        return 0;
    }
    tail[0] = 0;
    tail[1] = 0;
    pid = Pexec(100, worker_path, tail, env);     /* load and go, async */
    free(env);
    dbg("Pexec %s -> %ld (pipes %d %d %d %d)", worker_path, pid, jp[0], jp[1], rp[0], rp[1]);
    Fclose(jp[0]);                                /* the worker's ends  */
    Fclose(rp[1]);
    if (pid <= 0) {
        Fclose(jp[1]);
        Fclose(rp[0]);
        return 0;
    }
    j->pid = (int)pid;
    j->fd = rp[0];
    j->wfd = jp[1];
    j->hello = 0;
    j->t0 = now_ms();
    j->worker = 1;
    j->running = 1;
    return 1;
}

static void finish(JOB *j)
{
    j->running = 0;
    j->done = 1;
    if (j->win)
        win_job_finished(j->win, j);
}

static void job_unlist(JOB *j)
{
    int i;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i] == j)
            jobs[i] = 0;
}

int job_start(JOB *j)
{
    int i;
    j->done = j->running = j->queued = 0;
    j->cancel = 0;
    j->worker = 0;
    j->stage = 0;
    j->pdone = 0;
    j->ptotal = 1;
    memset(&j->res, 0, sizeof j->res);
    for (i = 0; i < MAXJOBS; i++)
        if (!jobs[i]) {
            jobs[i] = j;
            break;
        }
    if (i == MAXJOBS)
        return 0;
    /* a re-render can only go to a worker when the original picture is
       in shared memory the worker can attach */
    if (have_worker && prefs.use_workers &&
        (j->q.op != OP_RENDER || j->q.src_shm[0]) && start_worker(j))
        return 1;
    j->queued = 1;
    return 1;
}

void job_cancel(JOB *j)
{
    j->cancel = 1;
    if (j->worker && j->running)
        Pkill(j->pid, j->hello ? SIGTERM : SIGKILL);
    else if (j->queued && !j->running) {
        j->queued = 0;
        j->res.err = E_CANCEL;
        job_unlist(j);
        finish(j);
    }
}

static int readable(int fd)
{
    long mask = 1L << fd;
    if (Finstat((short)fd) > 0)
        return 1;
    return Fselect(1, &mask, 0L, 0L) > 0 && (mask & (1L << fd));
}

static void close_worker(JOB *j, int wait)
{
    long ru[2];
    if (j->fd >= 0)
        Fclose((short)j->fd);
    if (j->wfd >= 0)
        Fclose((short)j->wfd);
    j->fd = j->wfd = -1;
    if (j->pid > 0)
        Pwaitpid(j->pid, wait ? 0 : 1, ru);
}

/* the worker has sent its result header: take it */
static void take_result(JOB *j)
{
    long got = 0;
    char ack = 1;
    while (got < (long)sizeof(JOBRES)) {
        long r = Fread((short)j->fd, sizeof(JOBRES) - got, (char *)&j->res + got);
        if (r <= 0)
            break;
        got += r;
    }
    dbg("result header %ld of %ld bytes", got, (long)sizeof(JOBRES));
    if (got == (long)sizeof(JOBRES))
        attach_results(j);
    else {
        memset(&j->res, 0, sizeof j->res);
        j->res.err = E_NOMEM;
        strcpy(j->res.detail, "worker sent an incomplete result");
    }
    dbg("attached: err %d ptr %lx %lx %lx", j->res.err, (long)j->res.ptr[0],
        (long)j->res.ptr[1], (long)j->res.ptr[2]);
    Fwrite((short)j->wfd, 1, &ack);      /* the worker may exit now */
    close_worker(j, 1);
    if (j->cancel && !j->res.err) {
        int i;
        for (i = 0; i < RB_MAX; i++)
            job_release(&j->res, i);
        j->res.err = E_CANCEL;
    }
}

static void worker_failed(JOB *j, int i, const char *why)
{
    dbg("worker failed: %s", why);
    close_worker(j, 0);
    memset(&j->res, 0, sizeof j->res);
    j->res.err = j->cancel ? E_CANCEL : E_UNSUP;
    strcpy(j->res.detail, why);
    jobs[i] = 0;
    finish(j);
}

void job_poll(void)
{
    int i, k;
    for (i = 0; i < MAXJOBS; i++) {
        JOB *j = jobs[i];
        if (!j || !j->worker || !j->running)
            continue;
        for (k = 0; k < 64 && jobs[i] == j && readable(j->fd); k++) {
            WMSG m;
            if (Fread((short)j->fd, sizeof m, &m) != (long)sizeof m) {
                worker_failed(j, i, j->hello ? "worker process ended unexpectedly"
                                             : "FVWORK.TTP could not start");
                break;
            }
            if (m.type == WM_HELLO) {
                dbg("hello from worker %ld", m.a);
                j->hello = 1;
                if (Fwrite((short)j->wfd, sizeof(JOBREQ), &j->q) != (long)sizeof(JOBREQ)) {
                    Pkill(j->pid, SIGKILL);
                    worker_failed(j, i, "could not send the job to FVWORK");
                }
            } else if (m.type == WM_STAGE) {
                if (j->win)
                    win_stage(j->win, (int)m.a);
            } else if (m.type == WM_PROG) {
                j->pdone = m.a;
                j->ptotal = m.b;
                if (j->win)
                    win_progress(j->win, m.a, m.b);
            } else if (m.type == WM_RESULT) {
                take_result(j);
                jobs[i] = 0;
                finish(j);
            }
        }
        if (jobs[i] != j)
            continue;
        /* gone without a word, or never started? */
        {
            long ru[2], r = Pwaitpid(j->pid, 1, ru);     /* WNOHANG */
            if (r > 0 && (r >> 16) == j->pid) {
                j->pid = 0;                          /* already reaped */
                worker_failed(j, i, j->hello ? "worker process ended unexpectedly"
                                             : "FVWORK.TTP could not start");
                continue;
            }
        }
        if (!j->hello && now_ms() - j->t0 > 30000L) {
            Pkill(j->pid, SIGKILL);
            worker_failed(j, i, "FVWORK.TTP did not answer");
        }
    }
}

int job_any_active(void)
{
    int i;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i])
            return 1;
    return 0;
}

/* -------------------------------------------------- cooperative jobs */

static JOB *coop_job;

static int coop_prog(PROG *p, long done, long total)
{
    static long lastpoll;
    long t;
    JOB *j = coop_job;
    if (p->phases > 1) {
        if (p->phase != j->stage) {
            j->stage = p->phase;
            if (j->win)
                win_stage(j->win, p->phase);
        }
        if (total <= 0) total = 1;
        if (done > total) done = total;
        j->pdone = (p->phase * 1000L + done * 1000L / total) / p->phases;
        j->ptotal = 1000;
    } else {
        if (p->halves && p->phase && !j->stage) {
            j->stage = 1;
            if (j->win)
                win_stage(j->win, 1);
        }
        j->pdone = p->halves ? done + (p->phase ? total : 0) : done;
        j->ptotal = p->halves ? total * 2 : total;
    }
    if (j->win)
        win_progress(j->win, j->pdone, j->ptotal);
    t = now_ms();
    if (t - lastpoll >= 50) {
        lastpoll = t;
        gui_coop_poll();
    }
    return j->cancel;
}

void job_run_pending(void)
{
    int i;
    JOB *j = 0;
    PROG pg;
    if (coop_busy)
        return;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i] && jobs[i]->queued && !jobs[i]->running) {
            j = jobs[i];
            break;
        }
    if (!j)
        return;
    coop_busy = 1;
    coop_job = j;
    j->queued = 0;
    j->running = 1;
    memset(&pg, 0, sizeof pg);
    pg.fn = coop_prog;
    res_set_shared(0);
    /* let the window show its progress panel before the work starts */
    gui_coop_poll();
    if (j->cancel)
        j->res.err = E_CANCEL;
    else
        job_exec(&j->q, &j->res, &pg);
    res_collect(&j->res);
    job_unlist(j);
    coop_busy = 0;
    coop_job = 0;
    finish(j);
}

/* at exit: stop every worker and collect it, so no shared memory is left */
void job_kill_all(void)
{
    int i, k;
    for (i = 0; i < MAXJOBS; i++) {
        JOB *j = jobs[i];
        if (!j)
            continue;
        j->cancel = 1;
        if (j->worker && j->running)
            Pkill(j->pid, j->hello ? SIGTERM : SIGKILL);
    }
    for (i = 0; i < MAXJOBS; i++) {
        JOB *j = jobs[i];
        WMSG m;
        if (!j || !j->worker || !j->running)
            continue;
        /* blocking reads: the worker answers once it sees the signal */
        while (j->hello && j->fd >= 0 &&
               Fread((short)j->fd, sizeof m, &m) == (long)sizeof m) {
            if (m.type == WM_RESULT) {
                take_result(j);
                for (k = 0; k < RB_MAX; k++)
                    job_release(&j->res, k);
                break;
            }
        }
        close_worker(j, 1);
        j->running = 0;
        jobs[i] = 0;
    }
}
