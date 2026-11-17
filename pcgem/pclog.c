/* pclog.c - the PC GEM layer (pgaes.c, pgvdi.c) logs through edlog();
 * on a log build (LOG=1) that goes into FView's own step log */
#include <stdarg.h>
#include <stdio.h>

void dbg(const char *fmt, ...);

void edlog(const char *fmt, ...)
{
#if FV_LOG
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    dbg("%s", line);
#else
    (void)fmt;
#endif
}
