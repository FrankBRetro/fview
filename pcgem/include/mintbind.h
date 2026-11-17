/* mintbind.h for PC GEM: DOS has no MiNT. FView only makes these calls
 * after finding the MiNT cookie (job.c), which is never there on the PC,
 * so they just answer "not available" (EINVFN, -32). */
#ifndef PC_MINTBIND_H
#define PC_MINTBIND_H
#include <osbind.h>

#define PC_EINVFN (-32L)
static __inline__ long Dopendir(const char *n, short f) { (void)n; (void)f; return 0xFF000000L | (PC_EINVFN & 0xFFFFFF); }
static __inline__ long Dreaddir(short l, long h, char *b) { (void)l; (void)h; (void)b; return PC_EINVFN; }
static __inline__ long Dclosedir(long h) { (void)h; return PC_EINVFN; }
static __inline__ long Fpipe(short *fd) { (void)fd; return PC_EINVFN; }
static __inline__ long Fcntl(short fd, long a, short c) { (void)fd; (void)a; (void)c; return PC_EINVFN; }
static __inline__ long Pexec(short m, const char *p, const void *t, const void *e)
{ (void)m; (void)p; (void)t; (void)e; return PC_EINVFN; }
static __inline__ long Pkill(short pid, short sig) { (void)pid; (void)sig; return PC_EINVFN; }
static __inline__ long Finstat(short fd) { (void)fd; return PC_EINVFN; }
static __inline__ long Fselect(unsigned short t, long *r, long *w, long *x)
{ (void)t; (void)r; (void)w; (void)x; return PC_EINVFN; }
static __inline__ long Pwaitpid(short pid, short f, long *ru) { (void)pid; (void)f; (void)ru; return PC_EINVFN; }
static __inline__ long Pgetpid(void) { return 1; }
#endif
