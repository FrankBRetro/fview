/* deflate.h - incremental deflate compressor (raw RFC 1951 stream) */
#ifndef DEFLATE_H
#define DEFLATE_H
#include "arcio.h"

typedef struct DEFL DEFL;
/* level 1 fast .. 3 best; output goes to out */
DEFL *defl_new(BOUT *out, int level);
void  defl_put(DEFL *d, const u8 *p, long n);
void  defl_end(DEFL *d);          /* finish the stream (final block) */
void  defl_free(DEFL *d);
extern int deflate_level;         /* what deflate_stream uses (2)    */

#endif
