/*
 * jobcore.c - where a job's results are allocated (shared memory for a
 * worker), and the step log. Linked into the programs (cooperative jobs
 * under TOS) and their background workers under MiNT. No GEM in here.
 * The work itself is job_exec: jobexec.c (FView, FConv) or arcjob.c
 * (FArchive).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <osbind.h>
#include <mintbind.h>
#include "job.h"

#ifndef SHMSETBLK
#define SHMGETBLK (('M' << 8) | 0)
#define SHMSETBLK (('M' << 8) | 1)
#endif

static int   shared;                 /* allocate in U:\SHM            */
static int   seq;
static char  b_name[RB_MAX][14];
static void *b_ptr[RB_MAX];
static long  b_size[RB_MAX];

void shm_path(char *buf, const char *name)
{
    strcpy(buf, "U:\\SHM\\");
    strcat(buf, name);
}

void res_set_shared(int on) { shared = on; }

void *res_alloc(int slot, long size)
{
    void *p;
    if (b_ptr[slot])
        res_free(slot);
    if (size < 2)
        size = 2;
    if (!shared) {
        p = malloc(size);
        b_ptr[slot] = p;
        b_size[slot] = size;
        return p;
    }
    /* global memory (TT-RAM preferred) handed to a shared memory file;
       the GUI process attaches it by name, no copying */
    p = (void *)Mxalloc(size, 0x28 | 3);
    if ((long)p <= 0)
        p = (void *)Malloc(size);
    if ((long)p <= 0)
        return 0;
    {
        char path[40];
        long fd;
        sprintf(b_name[slot], "FV%04X%02X", (unsigned)(Pgetpid() & 0xFFFF),
                (unsigned)(seq++ & 0xFF));
        shm_path(path, b_name[slot]);
        fd = Fcreate(path, 0);
        if (fd < 0 || Fcntl((short)fd, (long)p, SHMSETBLK) < 0) {
            if (fd >= 0) {
                Fclose((short)fd);
                Fdelete(path);
            }
            Mfree(p);
            b_name[slot][0] = 0;
            return 0;
        }
        Fclose((short)fd);
        dbg("shm block %d '%s' %ld bytes at %lx", slot, b_name[slot], size, (long)p);
    }
    b_ptr[slot] = p;
    b_size[slot] = size;
    return p;
}

void res_free(int slot)
{
    if (!b_ptr[slot])
        return;
    if (shared) {
        char path[40];
        shm_path(path, b_name[slot]);
        Fdelete(path);
        Mfree(b_ptr[slot]);
        b_name[slot][0] = 0;
    } else
        free(b_ptr[slot]);
    b_ptr[slot] = 0;
    b_size[slot] = 0;
}

void res_free_all(void)
{
    int i;
    for (i = 0; i < RB_MAX; i++)
        res_free(i);
}

/* move the allocated blocks into the result (and forget them here) */
void res_collect(JOBRES *r)
{
    int i;
    for (i = 0; i < RB_MAX; i++) {
        r->size[i] = b_size[i];
        r->attached[i] = 0;
        if (shared) {
            strcpy(r->shm[i], b_ptr[i] ? b_name[i] : "");
            r->ptr[i] = 0;
        } else {
            r->shm[i][0] = 0;
            r->ptr[i] = b_ptr[i];
        }
        b_ptr[i] = 0;
        b_size[i] = 0;
        b_name[i][0] = 0;
    }
}

void *shm_attach(const char *name, int del)
{
    char path[40];
    long fd, addr;
    shm_path(path, name);
    fd = Fopen(path, 2);
    if (fd < 0)
        return 0;
    addr = Fcntl((short)fd, 0L, SHMGETBLK);
    Fclose((short)fd);
    if (del)
        Fdelete(path);
    return addr > 0 ? (void *)addr : 0;
}

/* ----------------------------------------------------------- debug log */

/* Step log for tracking down problems on real machines. Each line is
   appended and the file closed again, so the log survives a hang.     */
static char logpath[256];
static const char *logwho = "";

void dbg_init(const char *path, const char *who)
{
    strncpy(logpath, path, sizeof logpath - 1);
    logwho = who;
}

void dbg(const char *fmt, ...)
{
    FILE *f;
    va_list ap;
    if (!logpath[0])
        return;
    f = fopen(logpath, "a");
    if (!f)
        return;
    fprintf(f, "%s %ld: ", logwho, (long)Pgetpid());
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* ------------------------------------------------------------ notes */

void (*job_note_hook)(int kind, const char *text);

void job_note(int kind, const char *text)
{
    if (job_note_hook)
        job_note_hook(kind, text);
}
