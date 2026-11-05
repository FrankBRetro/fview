/* codecs.c - pick the codec for a method (FArchive only; FView and
   FConv link arcio.c and deflate.c for their PDF writer without this). */
#include <string.h>
#include "arcio.h"

static int copy_stream(BIN *in, BOUT *out, long usize, PROG *pg)
{
    u8 tmp[2048];
    long done = 0;
    for (;;) {
        long want = sizeof tmp, got;
        if (usize >= 0 && want > usize - done)
            want = usize - done;
        if (want <= 0)
            break;
        got = bin_read(in, tmp, want);
        if (got <= 0)
            break;
        bout_put(out, tmp, got);
        done += got;
        if ((done & 0x7FFF) < (long)sizeof tmp && codec_progress(pg, done, usize))
            return E_CANCEL;
    }
    if (out->err)
        return out->err;
    return usize >= 0 && done < usize ? E_FORMAT : E_OK;
}

int unpack(int method, BIN *in, BOUT *out, long usize, PROG *pg)
{
    switch (method) {
    case CM_STORE:      return copy_stream(in, out, usize, pg);
    case CM_DEFLATE:    return inflate_stream(in, out, usize, pg);
    case CM_LH1: case CM_LH4: case CM_LH5: case CM_LH6: case CM_LH7:
    case CM_LZS: case CM_LZ5:
                        return lzh_unpack(method, in, out, usize, pg);
    case CM_ZOOLZW:     return zoolzw_unpack(in, out, usize, pg);
    case CM_ARCPACK: case CM_ARCSQUEEZE: case CM_ARCCRUNCH: case CM_ARCSQUASH:
    case CM_ARCOLD5: case CM_ARCOLD6: case CM_ARCOLD7:
                        return arc_unpack(method, in, out, usize, pg);
    case CM_LZX:        return lzx_unpack(in, out, usize, pg);
    }
    return E_UNSUP;
}

int pack(int method, BIN *in, BOUT *out, long usize, PROG *pg)
{
    switch (method) {
    case CM_STORE:      return copy_stream(in, out, usize, pg);
    case CM_DEFLATE:    return deflate_stream(in, out, usize, pg);
    case CM_LH5:        return lzh_pack(in, out, usize, pg);
    case CM_ZOOLZW:     return zoolzw_pack(in, out, usize, pg);
    case CM_ARCCRUNCH:  return arc_crunch(in, out, usize, pg);
    case CM_ARCSQUASH:  return arc_squash(in, out, usize, pg);
    case CM_LZX:        return lzx_pack(in, out, usize, pg);
    }
    return E_UNSUP;
}
