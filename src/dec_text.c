/* dec_text.c - plain text (8-bit and Unicode), Markdown and RTF readers.
 * Everything is converted to the Atari character set and stored as a
 * text blob of paragraphs (see doc.h); the GUI wraps and draws them.   */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "rd.h"
#include "doc.h"

/* ------------------------------------------------------- paragraph out */

typedef struct {
    BUF  out;            /* the blob                                  */
    BUF  para;           /* paragraph being built                     */
    long npara;
    int  attr;           /* attribute currently in effect in para     */
    int  want;           /* attribute requested for the next char     */
    int  style, indent;
    long col;            /* column, for tab expansion                 */
    int  tabw;
    /* rich text (RTF): fonts, sizes, colours, alignment */
    int  rich;
    int  font, size, color;          /* in effect in the paragraph (-1 none) */
    int  wfont, wsize, wcolor;       /* wanted for the next character        */
    int  align;
} PW;

static void pw_init(PW *w, int tabw, u32 flags)
{
    memset(w, 0, sizeof *w);
    buf_init(&w->out);
    buf_init(&w->para);
    buf_put(&w->out, "FVTX", 4);
    buf_u32(&w->out, 0);
    buf_u32(&w->out, flags);
    w->tabw = tabw > 0 ? tabw : 8;
}

static void pw_state(PW *w)
{
    if (!w->rich)
        return;
    if (w->font != w->wfont || w->size != w->wsize) {
        buf_u8(&w->para, FM_FONT);
        buf_u8(&w->para, w->wfont);
        buf_u8(&w->para, w->wsize > 255 ? 255 : w->wsize);
        w->font = w->wfont;
        w->size = w->wsize;
    }
    if (w->color != w->wcolor) {
        buf_u8(&w->para, FM_COLOR);
        buf_u8(&w->para, w->wcolor);
        w->color = w->wcolor;
    }
}

static void pw_char(PW *w, int c)
{
    pw_state(w);
    if (w->attr != w->want) {
        buf_u8(&w->para, TA_MARK);
        buf_u8(&w->para, w->want);
        w->attr = w->want;
    }
    if (c == '\t') {
        do {
            buf_u8(&w->para, ' ');
            w->col++;
        } while (w->col % w->tabw);
        return;
    }
    if (c < 0x20 || c == 0x7F)
        c = ' ';
    buf_u8(&w->para, c);
    w->col++;
}

static void pw_str(PW *w, const char *s)
{
    while (*s)
        pw_char(w, (u8)*s++);
}

/* Unicode code point -> Atari text */
static void pw_uni(PW *w, long cp)
{
    int a;
    if (cp == '\t' || (cp >= 0x20 && cp < 0x7F)) {
        pw_char(w, (int)cp);
        return;
    }
    a = uni_to_atari(cp);
    if (a >= 0x80) {
        pw_char(w, a);
        return;
    }
    {
        char t[8];
        atari_translit(cp, t);
        pw_str(w, t);
    }
}

static void pw_end(PW *w)
{
    long n;
    if (w->rich && w->para.len == 0)
        pw_state(w);                 /* empty line: its height needs the font */
    n = w->para.len;
    if (n > 65535)
        n = 65535;
    buf_u16(&w->out, (unsigned)n);
    buf_u8(&w->out, w->style | (w->rich ? w->align << 4 : 0));
    buf_u8(&w->out, w->indent);
    buf_put(&w->out, w->para.p, n);
    w->para.len = 0;
    w->npara++;
    w->attr = 0;
    w->font = w->size = w->color = -1;
    w->col = 0;
    w->style = PS_NORMAL;
    w->indent = 0;
}

static int pw_finish(PW *w, u8 **blob, long *len)
{
    if (w->para.err)
        w->out.err = 1;
    buf_free(&w->para);
    buf_set_u32(&w->out, 4, w->npara);
    return buf_finish(&w->out, RB_DOC, blob, len);
}

/* ----------------------------------------------------------- decoding */

enum { ENC_8BIT, ENC_UTF8, ENC_UTF16LE, ENC_UTF16BE };

static int looks_utf8(const u8 *p, long n)
{
    long i = 0;
    int multi = 0;
    while (i < n) {
        u8 c = p[i];
        int k;
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) k = 1;
        else if (c >= 0xE0 && c <= 0xEF) k = 2;
        else if (c >= 0xF0 && c <= 0xF4) k = 3;
        else return 0;
        if (i + k >= n)
            return multi || k == 0;
        while (k--) {
            i++;
            if ((p[i] & 0xC0) != 0x80)
                return 0;
        }
        i++;
        multi = 1;
    }
    return multi;
}

typedef struct {
    const u8 *p;
    long n, i;
    int enc, cs;
} DEC;

static void dec_init(DEC *d, const u8 *p, long n, int cs, int ftype)
{
    d->p = p; d->n = n; d->i = 0; d->cs = cs;
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { d->enc = ENC_UTF16LE; d->i = 2; }
    else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) { d->enc = ENC_UTF16BE; d->i = 2; }
    else if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { d->enc = ENC_UTF8; d->i = 3; }
    else if (n >= 4 && ftype == FT_UTEXT && p[0] && !p[1] && p[2] && !p[3]) d->enc = ENC_UTF16LE;
    else if (n >= 4 && ftype == FT_UTEXT && !p[0] && p[1] && !p[2] && p[3]) d->enc = ENC_UTF16BE;
    else d->enc = looks_utf8(p, n > 65536 ? 65536 : n) ? ENC_UTF8 : ENC_8BIT;
}

/* next code point, -1 at end. 8-bit text returns an Atari char | 0x40000000 */
#define ATARI_FLAG 0x40000000L
static long dec_next(DEC *d)
{
    long c;
    if (d->i >= d->n)
        return -1;
    switch (d->enc) {
    case ENC_UTF16LE:
    case ENC_UTF16BE:
        if (d->i + 1 >= d->n) { d->i = d->n; return -1; }
        c = d->enc == ENC_UTF16LE ? d->p[d->i] | (d->p[d->i + 1] << 8)
                                  : (d->p[d->i] << 8) | d->p[d->i + 1];
        d->i += 2;
        if (c >= 0xD800 && c <= 0xDBFF && d->i + 1 < d->n) {
            long lo = d->enc == ENC_UTF16LE ? d->p[d->i] | (d->p[d->i + 1] << 8)
                                            : (d->p[d->i] << 8) | d->p[d->i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                d->i += 2;
                c = 0x10000L + ((c - 0xD800) << 10) + (lo - 0xDC00);
            }
        }
        return c;
    case ENC_UTF8: {
        u8 b = d->p[d->i++];
        int k;
        if (b < 0x80) return b;
        if (b >= 0xF0) { c = b & 7; k = 3; }
        else if (b >= 0xE0) { c = b & 15; k = 2; }
        else if (b >= 0xC0) { c = b & 31; k = 1; }
        else return 0xFFFD;
        while (k-- && d->i < d->n && (d->p[d->i] & 0xC0) == 0x80)
            c = (c << 6) | (d->p[d->i++] & 0x3F);
        return c;
    }
    default:
        c = d->p[d->i++];
        if (c < 0x80)
            return c;
        return ATARI_FLAG | byte_to_atari((int)c, d->cs);
    }
}

static void put_cp(PW *w, long c)
{
    if (c & ATARI_FLAG)
        pw_char(w, (int)(c & 0xFF));
    else
        pw_uni(w, c);
}

/* Decode the whole file into Atari bytes with line breaks normalised to
 * '\n'. Used by plain text and markdown.                               */
static u8 *to_atari(const u8 *src, long n, int cs, int ftype, long *outlen, PROG *pg)
{
    DEC d;
    PW w;
    long c, prev = 0;
    memset(&w, 0, sizeof w);
    buf_init(&w.para);
    w.tabw = 1;                       /* no tab expansion here */
    dec_init(&d, src, n, cs, ftype);
    while ((c = dec_next(&d)) >= 0) {
        if (c == '\r') {
            buf_u8(&w.para, '\n');
        } else if (c == '\n') {
            if (prev != '\r')
                buf_u8(&w.para, '\n');
        } else if (c == '\t')
            buf_u8(&w.para, '\t');
        else if (c == 0xFEFF)
            ;
        else if ((c & ATARI_FLAG) || c >= 0x20)
            put_cp(&w, c);
        else
            buf_u8(&w.para, ' ');
        prev = c;
        if ((d.i & 0x3FFF) == 0 && prog_step(pg, n + d.i, n * 3)) {
            buf_free(&w.para);
            return 0;
        }
    }
    buf_u8(&w.para, 0);
    if (w.para.err) {
        buf_free(&w.para);
        return 0;
    }
    *outlen = w.para.len - 1;
    return w.para.p;
}

/* ---------------------------------------------------------- plain text */

static int do_plain(const u8 *t, long n, const PREFS *pf, PROG *pg, u8 **blob, long *len)
{
    PW w;
    long i;
    pw_init(&w, pf->tabw, pf->wrap ? 0 : DOCF_NOWRAP);
    for (i = 0; i < n; i++) {
        if (t[i] == '\n')
            pw_end(&w);
        else
            pw_char(&w, t[i]);
        if ((i & 0x3FFF) == 0 && prog_step(pg, 2 * n + i, n * 3)) {
            buf_free(&w.out);
            buf_free(&w.para);
            return E_CANCEL;
        }
    }
    if (w.para.len)
        pw_end(&w);
    return pw_finish(&w, blob, len);
}

/* ------------------------------------------------------------ markdown */

static int is_rule(const char *s)
{
    int n = 0;
    char c = 0;
    while (*s == ' ') s++;
    for (; *s; s++) {
        if (*s == ' ')
            continue;
        if (*s != '-' && *s != '*' && *s != '_')
            return 0;
        if (c && *s != c)
            return 0;
        c = *s;
        n++;
    }
    return n >= 3;
}

/* inline markdown: emphasis, code, links, images, escapes */
static void md_inline(PW *w, const char *s, int markup)
{
    int bold = 0, ital = 0, code = 0, strike = 0;
    while (*s) {
        if (*s == '\\' && s[1] && strchr("\\`*_{}[]()#+-.!|~>", s[1]) && !code) {
            pw_char(w, (u8)s[1]);
            s += 2;
            continue;
        }
        if (*s == '`') {
            code = !code;
            w->want = code ? TA_LIGHT : (bold ? TA_BOLD : 0) | (ital ? TA_ITALIC : 0);
            if (markup) pw_char(w, '`');
            s++;
            continue;
        }
        if (code) {
            pw_char(w, (u8)*s++);
            continue;
        }
        if ((s[0] == '*' && s[1] == '*') || (s[0] == '_' && s[1] == '_')) {
            bold = !bold;
            if (markup) { pw_char(w, (u8)s[0]); pw_char(w, (u8)s[1]); }
            s += 2;
        } else if (s[0] == '~' && s[1] == '~') {
            strike = !strike;
            if (markup) { pw_char(w, '~'); pw_char(w, '~'); }
            s += 2;
        } else if ((*s == '*' || *s == '_') &&
                   (ital || (s[1] && s[1] != ' '))) {
            /* '_' inside words (snake_case) is literal */
            if (*s == '_' && !ital && w->para.len &&
                isalnum(w->para.p[w->para.len - 1])) {
                pw_char(w, '_');
                s++;
                continue;
            }
            ital = !ital;
            if (markup) pw_char(w, (u8)*s);
            s++;
        } else if (s[0] == '!' && s[1] == '[') {
            const char *e = strchr(s + 2, ']');
            if (e && e[1] == '(' && strchr(e, ')')) {
                pw_str(w, "[image: ");
                while (s + 2 < e) { pw_char(w, (u8)s[2]); s++; }
                pw_char(w, ']');
                s = strchr(e, ')') + 1;
            } else
                pw_char(w, (u8)*s++);
            continue;
        } else if (s[0] == '[') {
            const char *e = strchr(s + 1, ']');
            if (e && e[1] == '(' && strchr(e, ')')) {
                int keep = w->want;
                w->want |= TA_UNDER;
                for (s++; s < e; s++)
                    pw_char(w, (u8)*s);
                w->want = keep;
                if (markup) {
                    const char *c = e + 1;
                    pw_char(w, ' ');
                    while (*c && *c != ')') pw_char(w, (u8)*c++);
                    pw_char(w, ')');
                }
                s = strchr(e, ')') + 1;
            } else
                pw_char(w, (u8)*s++);
            continue;
        } else {
            pw_char(w, (u8)*s++);
            continue;
        }
        w->want = (bold ? TA_BOLD : 0) | (ital ? TA_ITALIC : 0) | (strike ? TA_LIGHT : 0);
    }
    w->want = 0;
}

static int do_markdown(char *t, const PREFS *pf, PROG *pg, u8 **blob, long *len)
{
    PW w;
    char *line = t, *next;
    int fence = 0, inpara = 0;
    long total = (long)strlen(t);
    pw_init(&w, pf->tabw, 0);

    for (; line; line = next) {
        char *s;
        int lead = 0;
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        for (s = line; *s == ' ' || *s == '\t'; s++)
            lead += *s == '\t' ? 4 : 1;

        if (prog_step(pg, 2 * total + (line - t), total * 3)) {
            buf_free(&w.out); buf_free(&w.para);
            return E_CANCEL;
        }

        /* fenced code */
        if (!strncmp(s, "```", 3) || !strncmp(s, "~~~", 3)) {
            if (inpara) { pw_end(&w); inpara = 0; }
            fence = !fence;
            if (pf->md_markup) {
                w.style = PS_PRE;
                pw_str(&w, line);
                pw_end(&w);
            }
            continue;
        }
        if (fence) {
            w.style = PS_PRE;
            pw_str(&w, line);
            pw_end(&w);
            continue;
        }
        if (!*s) {                              /* blank line */
            if (inpara) { pw_end(&w); inpara = 0; }
            w.style = PS_BLANK;
            pw_end(&w);
            continue;
        }
        /* setext headings: underline applies to the paragraph above */
        if (inpara && (s[0] == '=' || s[0] == '-')) {
            char *q = s;
            while (*q == s[0]) q++;
            while (*q == ' ') q++;
            if (!*q && q - s >= 2) {
                w.style = s[0] == '=' ? PS_H1 : PS_H2;
                w.want = w.attr = 0;
                pw_end(&w);
                inpara = 0;
                continue;
            }
        }
        if (lead >= 4 && !inpara) {             /* indented code */
            w.style = PS_PRE;
            pw_str(&w, line + (lead >= 4 ? 4 : 0));
            pw_end(&w);
            continue;
        }
        if (is_rule(s)) {
            if (inpara) { pw_end(&w); inpara = 0; }
            w.style = PS_RULE;
            pw_end(&w);
            continue;
        }
        if (*s == '#') {
            int lv = 0;
            char *e;
            while (s[lv] == '#') lv++;
            if (s[lv] == ' ' || !s[lv]) {
                if (inpara) { pw_end(&w); inpara = 0; }
                s += lv;
                while (*s == ' ') s++;
                e = s + strlen(s);                 /* closing #s */
                while (e > s && (e[-1] == '#' || e[-1] == ' ')) e--;
                *e = 0;
                w.style = lv == 1 ? PS_H1 : lv == 2 ? PS_H2 : PS_H3;
                if (pf->md_markup) { while (lv--) pw_char(&w, '#'); pw_char(&w, ' '); }
                md_inline(&w, s, pf->md_markup);
                pw_end(&w);
                continue;
            }
        }
        if (*s == '>') {
            if (inpara && w.style != PS_QUOTE) { pw_end(&w); inpara = 0; }
            while (*s == '>' || *s == ' ') s++;
            if (!inpara) {
                w.style = PS_QUOTE;
                if (pf->md_markup) pw_str(&w, "> ");
            } else
                pw_char(&w, ' ');
            md_inline(&w, s, pf->md_markup);
            inpara = 1;
            continue;
        }
        if ((*s == '-' || *s == '*' || *s == '+') && s[1] == ' ') {
            if (inpara) pw_end(&w);
            w.style = PS_BULLET;
            w.indent = lead / 2 + 1;
            s += 2;
            if (!strncmp(s, "[ ] ", 4)) { pw_str(&w, "[ ] "); s += 4; }
            else if (!strncmp(s, "[x] ", 4) || !strncmp(s, "[X] ", 4)) { pw_str(&w, "[\xFB] "); s += 4; }
            md_inline(&w, s, pf->md_markup);
            inpara = 1;
            continue;
        }
        if (isdigit((u8)*s)) {
            char *q = s;
            while (isdigit((u8)*q)) q++;
            if ((*q == '.' || *q == ')') && q[1] == ' ') {
                if (inpara) pw_end(&w);
                w.style = PS_NORMAL;
                w.indent = lead / 2 + 1;
                while (s <= q) pw_char(&w, (u8)*s++);
                pw_char(&w, ' ');
                md_inline(&w, q + 2, pf->md_markup);
                inpara = 1;
                continue;
            }
        }
        if (*s == '|') {                        /* tables: one row per line */
            char *q;
            if (inpara) { pw_end(&w); inpara = 0; }
            for (q = s; *q; q++)
                if (*q != '|' && *q != '-' && *q != ':' && *q != ' ')
                    break;
            if (!*q)
                continue;                       /* separator row */
            w.style = PS_PRE;
            md_inline(&w, s, 1);
            pw_end(&w);
            continue;
        }
        /* ordinary text: join lines of one paragraph */
        if (inpara) {
            int hard = strlen(line) >= 2 && !strcmp(line + strlen(line) - 2, "  ");
            pw_char(&w, ' ');
            md_inline(&w, s, pf->md_markup);
            if (hard) { int st = w.style, ind = w.indent; pw_end(&w); w.style = st; w.indent = ind; }
        } else {
            md_inline(&w, s, pf->md_markup);
            inpara = 1;
        }
    }
    if (w.para.len || inpara)
        pw_end(&w);
    return pw_finish(&w, blob, len);
}

/* ----------------------------------------------------------------- RTF */

/* RTF keeps fonts (from the font table), sizes, colours, alignment and the
 * page width, so the GUI can lay it out with GDOS fonts.               */

#define RTF_MAXFONT 64
#define RTF_MAXCOL  64

enum { DEST_TEXT = 0, DEST_SKIP, DEST_FONTTBL, DEST_COLORTBL };

typedef struct {
    int attr, dest, uc;
    int font, size, color;       /* character formatting (group scoped) */
} RTFST;

typedef struct {
    int  num;                    /* \fN number                          */
    int  family;                 /* FF_*                                */
    char name[40];
} RFONT;

static const char *rtf_skipdest[] = {
    "stylesheet", "info", "header", "footer",
    "headerl", "headerr", "headerf", "footerl", "footerr", "footerf",
    "object", "themedata", "colorschememapping", "datastore",
    "latentstyles", "listtable", "listoverridetable", "rsidtbl",
    "generator", "xmlnstbl", "mmathPr", "fldinst", "filetbl",
    "revtbl", "pgdsctbl", "bkmkstart", "bkmkend", "nonshppict",
    "listtext", "pntext", "pntxtb", "pntxta", "fchars", "lchars",
    "wgrffmtfilter", "xe", "tc", "objdata", "blipuid", "panose",
    "falt", "fname", "fontemb", "fontfile", 0
};

static int do_rtf(const u8 *t, long n, const PREFS *pf, PROG *pg, u8 **blob, long *len)
{
    PW w;
    RTFST st[64];
    RFONT fonts[RTF_MAXFONT];
    u8 cols[RTF_MAXCOL * 3];
    int nfonts = 0, ncols = 0, cr = 0, cg = 0, cb = 0, curfont = -1;
    int sp = 0, ucskip = 0, cp = 1252, ignorable = 0, deff = 0;
    long i = 0, paperw = 12240, margl = 1800, margr = 1800;
    BUF body;

    pw_init(&w, 8, 0);
    buf_free(&w.out);             /* paragraphs go to 'out'; header later */
    buf_init(&w.out);
    w.rich = 1;
    w.font = w.size = w.color = -1;
    w.wfont = 0; w.wsize = 24; w.wcolor = 0;
    memset(&st[0], 0, sizeof st[0]);
    st[0].uc = 1;
    st[0].size = 24;
    st[0].font = 0;

    while (i < n) {
        int c = t[i];
        if ((i & 0x1FFF) == 0 && prog_step(pg, n + i, 2 * n)) {
            buf_free(&w.out); buf_free(&w.para);
            return E_CANCEL;
        }
        if (c == '{') {
            if (sp < 63) { st[sp + 1] = st[sp]; sp++; }
            ignorable = 0;
            i++;
            continue;
        }
        if (c == '}') {
            if (st[sp].dest == DEST_FONTTBL && sp > 0 && st[sp - 1].dest != DEST_FONTTBL)
                curfont = -1;
            if (sp > 0) sp--;
            w.want = st[sp].attr;
            i++;
            continue;
        }
        if (c == '\r' || c == '\n') { i++; continue; }
        if (c == '\\') {
            char word[32];
            int wl = 0, hasnum = 0, neg = 0;
            long num = 0;
            i++;
            if (i >= n) break;
            c = t[i];
            if (!isalpha(c)) {          /* control symbol */
                i++;
                if (st[sp].dest != DEST_TEXT) {
                    if (c == '\'') i += 2;
                    if (c == '*') ignorable = 1;
                    continue;
                }
                if (c == '*') { ignorable = 1; continue; }
                if (c == '\'' && i + 1 < n) {
                    char hx[3];
                    hx[0] = t[i]; hx[1] = t[i + 1]; hx[2] = 0;
                    i += 2;
                    if (ucskip) { ucskip--; continue; }
                    c = (int)strtol(hx, 0, 16);
                    pw_char(&w, byte_to_atari(c, cp == 1252 ? CS_CP1252 :
                                                 cp == 437 ? CS_ATARI : CS_LATIN1));
                    continue;
                }
                if (c == '\n' || c == '\r') { pw_end(&w); continue; }
                if (c == '~') { pw_char(&w, ' '); continue; }
                if (c == '_') { pw_char(&w, '-'); continue; }
                if (c == '-') continue;
                if (c == '\\' || c == '{' || c == '}') pw_char(&w, c);
                continue;
            }
            while (i < n && isalpha(t[i]) && wl < 31)
                word[wl++] = t[i++];
            word[wl] = 0;
            if (i < n && (t[i] == '-' || isdigit(t[i]))) {
                hasnum = 1;
                if (t[i] == '-') { neg = 1; i++; }
                while (i < n && isdigit(t[i]))
                    num = num * 10 + (t[i++] - '0');
                if (neg) num = -num;
            }
            if (i < n && t[i] == ' ')
                i++;

            /* tables we keep */
            if (!strcmp(word, "fonttbl")) { st[sp].dest = DEST_FONTTBL; ignorable = 0; continue; }
            if (!strcmp(word, "colortbl")) {
                st[sp].dest = DEST_COLORTBL;
                cr = cg = cb = 0;
                ignorable = 0;
                continue;
            }
            if (st[sp].dest == DEST_FONTTBL) {
                int k, skipit = ignorable;
                for (k = 0; rtf_skipdest[k]; k++)
                    if (!strcmp(word, rtf_skipdest[k])) skipit = 1;
                ignorable = 0;
                if (skipit) {             /* e.g. {\*\panose ...} inside */
                    st[sp].dest = DEST_SKIP;
                    continue;
                }
                if (!strcmp(word, "f") && hasnum && nfonts < RTF_MAXFONT) {
                    curfont = nfonts++;
                    fonts[curfont].num = (int)num;
                    fonts[curfont].family = FF_ANY;
                    fonts[curfont].name[0] = 0;
                } else if (curfont >= 0) {
                    if (!strcmp(word, "froman")) fonts[curfont].family = FF_SERIF;
                    else if (!strcmp(word, "fswiss")) fonts[curfont].family = FF_SANS;
                    else if (!strcmp(word, "fmodern")) fonts[curfont].family = FF_MONO;
                    else if (!strcmp(word, "fscript")) fonts[curfont].family = FF_SCRIPT;
                    else if (!strcmp(word, "fdecor") || !strcmp(word, "ftech")) fonts[curfont].family = FF_DECOR;
                }
                continue;
            }
            if (st[sp].dest == DEST_COLORTBL) {
                if (!strcmp(word, "red")) cr = (int)num;
                else if (!strcmp(word, "green")) cg = (int)num;
                else if (!strcmp(word, "blue")) cb = (int)num;
                continue;
            }
            /* destinations we skip */
            {
                int k, d = 0;
                for (k = 0; rtf_skipdest[k]; k++)
                    if (!strcmp(word, rtf_skipdest[k])) { d = 1; break; }
                if (d || (ignorable && st[sp].dest == DEST_TEXT)) {
                    st[sp].dest = DEST_SKIP;
                    ignorable = 0;
                    continue;
                }
                ignorable = 0;
                if (!strcmp(word, "pict")) {
                    if (st[sp].dest == DEST_TEXT && pf->rtf_pict)
                        pw_str(&w, "[picture]");
                    st[sp].dest = DEST_SKIP;
                    continue;
                }
            }
            if (st[sp].dest != DEST_TEXT)
                continue;
            if (!strcmp(word, "par") || !strcmp(word, "sect") || !strcmp(word, "page")) {
                pw_end(&w);
                if (!strcmp(word, "page")) { w.style = PS_RULE; pw_end(&w); }
            } else if (!strcmp(word, "line"))
                pw_end(&w);
            else if (!strcmp(word, "tab"))
                pw_char(&w, '\t');
            else if (!strcmp(word, "cell"))
                pw_str(&w, "  | ");
            else if (!strcmp(word, "row"))
                pw_end(&w);
            else if (!strcmp(word, "b"))
                st[sp].attr = (hasnum && !num) ? st[sp].attr & ~TA_BOLD : st[sp].attr | TA_BOLD;
            else if (!strcmp(word, "i"))
                st[sp].attr = (hasnum && !num) ? st[sp].attr & ~TA_ITALIC : st[sp].attr | TA_ITALIC;
            else if (!strcmp(word, "ul"))
                st[sp].attr = (hasnum && !num) ? st[sp].attr & ~TA_UNDER : st[sp].attr | TA_UNDER;
            else if (!strcmp(word, "ulnone"))
                st[sp].attr &= ~TA_UNDER;
            else if (!strcmp(word, "plain")) {
                st[sp].attr = 0;
                st[sp].size = 24;
                st[sp].font = deff;
                st[sp].color = 0;
            } else if (!strcmp(word, "f") && hasnum)
                st[sp].font = (int)num;
            else if (!strcmp(word, "fs") && hasnum)
                st[sp].size = num > 2 && num < 512 ? (int)num : 24;
            else if (!strcmp(word, "cf") && hasnum)
                st[sp].color = (int)num;
            else if (!strcmp(word, "deff") && hasnum)
                deff = st[sp].font = (int)num;
            else if (!strcmp(word, "pard")) {
                w.indent = 0;
                w.align = AL_LEFT;
            } else if (!strcmp(word, "ql"))
                w.align = AL_LEFT;
            else if (!strcmp(word, "qr"))
                w.align = AL_RIGHT;
            else if (!strcmp(word, "qc"))
                w.align = AL_CENTER;
            else if (!strcmp(word, "qj"))
                w.align = AL_JUST;
            else if (!strcmp(word, "li") && hasnum)
                w.indent = num > 0 ? (int)(num / 360 > 20 ? 20 : num / 360) : 0;
            else if (!strcmp(word, "paperw") && hasnum)
                paperw = num;
            else if (!strcmp(word, "margl") && hasnum)
                margl = num;
            else if (!strcmp(word, "margr") && hasnum)
                margr = num;
            else if (!strcmp(word, "ansicpg") && hasnum)
                cp = (int)num;
            else if (!strcmp(word, "uc") && hasnum)
                st[sp].uc = (int)num;
            else if (!strcmp(word, "u") && hasnum) {
                if (num < 0) num += 65536L;
                w.want = st[sp].attr;
                pw_uni(&w, num);
                ucskip = st[sp].uc;
            } else if (!strcmp(word, "bullet"))
                pw_char(&w, 0xF9);
            else if (!strcmp(word, "emdash"))
                pw_str(&w, "--");
            else if (!strcmp(word, "endash"))
                pw_char(&w, '-');
            else if (!strcmp(word, "lquote") || !strcmp(word, "rquote"))
                pw_char(&w, '\'');
            else if (!strcmp(word, "ldblquote") || !strcmp(word, "rdblquote"))
                pw_char(&w, '"');
            else if (!strcmp(word, "emspace") || !strcmp(word, "enspace"))
                pw_char(&w, ' ');
            /* character formatting now in effect */
            w.want = st[sp].attr;
            {
                int k, fi = 0;
                for (k = 0; k < nfonts; k++)
                    if (fonts[k].num == st[sp].font) { fi = k; break; }
                w.wfont = fi;
                w.wsize = st[sp].size;
                w.wcolor = st[sp].color < ncols ? st[sp].color : 0;
            }
            continue;
        }
        /* plain text */
        i++;
        if (st[sp].dest == DEST_FONTTBL) {
            if (curfont >= 0) {
                char *nm = fonts[curfont].name;
                size_t l = strlen(nm);
                if (c == ';')
                    curfont = -1;
                else if (l < sizeof fonts[0].name - 1 && (c >= ' ' || l))
                    nm[l] = (char)c, nm[l + 1] = 0;
            }
            continue;
        }
        if (st[sp].dest == DEST_COLORTBL) {
            if (c == ';' && ncols < RTF_MAXCOL) {
                cols[ncols * 3] = (u8)cr; cols[ncols * 3 + 1] = (u8)cg; cols[ncols * 3 + 2] = (u8)cb;
                ncols++;
                cr = cg = cb = 0;
            }
            continue;
        }
        if (st[sp].dest != DEST_TEXT)
            continue;
        if (ucskip) { ucskip--; continue; }
        w.want = st[sp].attr;
        {
            int k, fi = 0;
            for (k = 0; k < nfonts; k++)
                if (fonts[k].num == st[sp].font) { fi = k; break; }
            w.wfont = fi;
            w.wsize = st[sp].size;
            w.wcolor = st[sp].color < ncols ? st[sp].color : 0;
        }
        if (c >= 0x80)
            c = byte_to_atari(c, cp == 437 ? CS_ATARI : cp == 1252 ? CS_CP1252 : CS_LATIN1);
        pw_char(&w, c);
    }
    if (w.para.len)
        pw_end(&w);

    /* header + tables + paragraphs */
    buf_init(&body);
    buf_put(&body, "FVTX", 4);
    buf_u32(&body, w.npara);
    buf_u32(&body, DOCF_RICH);
    if (!nfonts) {                    /* always at least one font entry */
        fonts[0].family = FF_ANY;
        strcpy(fonts[0].name, "System");
        nfonts = 1;
    }
    buf_u16(&body, nfonts);
    for (i = 0; i < nfonts; i++) {
        char *nm = fonts[i].name;
        size_t l = strlen(nm);
        while (l && nm[l - 1] == ' ') nm[--l] = 0;
        buf_u8(&body, fonts[i].family);
        buf_u8(&body, (int)l);
        buf_put(&body, nm, (long)l);
    }
    if (!ncols) {                     /* colour 0 = automatic (black) */
        cols[0] = cols[1] = cols[2] = 0;
        ncols = 1;
    }
    buf_u16(&body, ncols);
    buf_put(&body, cols, ncols * 3L);
    buf_u32(&body, (u32)(paperw - margl - margr > 0 ? paperw - margl - margr : 0));
    buf_put(&body, w.out.p, w.out.len);
    if (w.out.err || w.para.err)
        body.err = 1;
    buf_free(&w.out);
    buf_free(&w.para);
    return buf_finish(&body, RB_DOC, blob, len);
}

/* --------------------------------------------------------------- entry */

int dec_text(const char *path, int ftype, const PREFS *pf, PROG *pg,
             u8 **blob, long *len)
{
    long n, an;
    int e;
    u8 *raw = rd_slurp(path, &n, pg, &e), *at;
    if (!raw)
        return e;
    if (ftype == FT_RTF) {
        e = do_rtf(raw, n, pf, pg, blob, len);
        free(raw);
        return e;
    }
    at = to_atari(raw, n, pf->charset, ftype, &an, pg);
    free(raw);
    if (!at)
        return pg && pg->cancelled ? E_CANCEL : E_NOMEM;
    if (ftype == FT_MARKDOWN)
        e = do_markdown((char *)at, pf, pg, blob, len);
    else
        e = do_plain(at, an, pf, pg, blob, len);
    free(at);
    return e;
}
