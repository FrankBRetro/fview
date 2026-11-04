/*
 * fonts.c - GDOS fonts for rich text (RTF) and PDF text.
 *
 * Fonts are loaded per workstation (screen, printer) with vst_load_fonts
 * and matched by name: exact face first, then the same face in the wanted
 * weight/slant, then a font of the same family (serif, sans, mono). When
 * no GDOS is present everything falls back to the system font, with VDI
 * effects for bold and italic.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "gemui.h"

#define MAXSETS 3
static FONTSET *sets[MAXSETS];

static void norm(const char *s, char *o, int max)
{
    int n = 0;
    for (; *s && n < max - 1; s++)
        if (isalnum((u8)*s))
            o[n++] = (char)tolower((u8)*s);
    o[n] = 0;
}

static int has(const char *s, const char *w) { return strstr(s, w) != 0; }

/* remove style words, leaving the face name */
static void face_of(const char *nm, char *o)
{
    static const char *words[] = { "bolditalic", "boldoblique", "bold", "italic",
        "oblique", "regular", "roman", "medium", "book", "normal", "plain", 0 };
    char t[40], *p;
    int k;
    strcpy(t, nm);
    for (k = 0; words[k]; k++)
        while ((p = strstr(t, words[k])) != 0 && p != t)
            memmove(p, p + strlen(words[k]), strlen(p + strlen(words[k])) + 1);
    strcpy(o, t);
}

static int family_of(const char *n)
{
    if (has(n, "mono") || has(n, "courier") || has(n, "typewriter") || has(n, "fixed") ||
        has(n, "letter") || has(n, "code"))
        return FF_MONO;
    if (has(n, "swiss") || has(n, "helvet") || has(n, "arial") || has(n, "sans") ||
        has(n, "univers") || has(n, "futura") || has(n, "vag") || has(n, "verdana") ||
        has(n, "gothic") || has(n, "frutiger") || has(n, "geneva") || has(n, "dejavu"))
        return FF_SANS;
    if (has(n, "dutch") || has(n, "times") || has(n, "garamond") || has(n, "serif") ||
        has(n, "baskerville") || has(n, "bookman") || has(n, "palatino") ||
        has(n, "century") || has(n, "georgia") || has(n, "charter") || has(n, "roman") ||
        has(n, "cooper") || has(n, "schoolbook"))
        return FF_SERIF;
    if (has(n, "script") || has(n, "park") || has(n, "allegro") || has(n, "brush"))
        return FF_SCRIPT;
    return FF_ANY;
}

void fonts_load(FONTSET *fs, int handle, int sysfonts)
{
    int i, n, extra = 0;
    memset(fs, 0, sizeof *fs);
    fs->handle = handle;
    fs->gdos = vq_gdos() != 0;
    if (fs->gdos) {
        extra = vst_load_fonts(handle, 0);
        if (extra < 0) extra = 0;
        fs->loaded = 1;
        fs->arb = vq_vgdos() == GDOS_FSM;     /* SpeedoGDOS / FSM: any size */
    }
    n = sysfonts + extra;
    if (n < 1) n = 1;
    if (n > MAXGF) n = MAXGF;
    for (i = 0; i < n; i++) {
        char name[34];
        short id;
        memset(name, 0, sizeof name);
        id = vqt_name(handle, i + 1, name);
        name[32] = 0;
        fs->id[fs->n] = id;
        strcpy(fs->name[fs->n], name);
        norm(name, fs->norm[fs->n], sizeof fs->norm[0]);
        face_of(fs->norm[fs->n], fs->face[fs->n]);
        fs->bold[fs->n] = has(fs->norm[fs->n], "bold") || has(fs->norm[fs->n], "black") ||
                          has(fs->norm[fs->n], "heavy") || has(fs->norm[fs->n], "demi");
        fs->ital[fs->n] = has(fs->norm[fs->n], "italic") || has(fs->norm[fs->n], "oblique");
        fs->fam[fs->n] = family_of(fs->norm[fs->n]);
        fs->n++;
    }
    for (i = 0; i < MAXSETS; i++)
        if (!sets[i]) {
            sets[i] = fs;
            break;
        }
    dbg("fonts for handle %d: %d (gdos %d, arbitrary sizes %d)", handle, fs->n, fs->gdos, fs->arb);
}

void fonts_unload(FONTSET *fs)
{
    int i;
    if (fs->loaded)
        vst_unload_fonts(fs->handle, 0);
    for (i = 0; i < MAXSETS; i++)
        if (sets[i] == fs)
            sets[i] = 0;
    fs->n = 0;
    fs->loaded = 0;
}

FONTSET *fonts_for(int handle)
{
    int i;
    for (i = 0; i < MAXSETS; i++)
        if (sets[i] && sets[i]->handle == handle)
            return sets[i];
    return 0;
}

/* Best font id for a face name / family and TA_BOLD / TA_ITALIC.
   *fx receives the VDI effects still needed (bold/italic the font lacks). */
int font_pick(FONTSET *fs, const char *name, int family, int attr, int *fx)
{
    char nn[40], face[40];
    int i, best = -1, bs = 0;
    int wb = (attr & TA_BOLD) != 0, wi = (attr & TA_ITALIC) != 0;
    *fx = attr & (TA_BOLD | TA_ITALIC);
    if (!fs || fs->n <= 1)
        return 1;
    norm(name ? name : "", nn, sizeof nn);
    face_of(nn, face);
    if (has(nn, "bold")) wb = 1;
    if (has(nn, "italic") || has(nn, "oblique")) wi = 1;
    if (family == FF_ANY && nn[0])
        family = family_of(nn);
    for (i = 0; i < fs->n; i++) {
        int sc = 0;
        if (face[0] && !strcmp(fs->face[i], face))
            sc = 100;
        else if (face[0] && fs->face[i][0] &&
                 (!strncmp(fs->face[i], face, strlen(face)) ||
                  !strncmp(face, fs->face[i], strlen(fs->face[i]))))
            sc = 60;
        else if (family != FF_ANY && fs->fam[i] == family)
            sc = 20;
        if (!sc)
            continue;
        if (fs->bold[i] == wb) sc += 10;
        if (fs->ital[i] == wi) sc += 10;
        if (sc > bs) bs = sc, best = i;
    }
    if (best < 0)
        return 1;                          /* system font */
    *fx = (wb && !fs->bold[best] ? TA_BOLD : 0) | (wi && !fs->ital[best] ? TA_ITALIC : 0);
    return fs->id[best];
}

/* set a size in half points; returns the cell height in pixels */
int font_size(FONTSET *fs, int handle, int halfpts)
{
    short cw, ch, cellw, cellh;
    int pt = (halfpts + 1) / 2;
    if (pt < 4) pt = 4;
    if (fs && fs->arb)
        vst_arbpt(handle, pt, &cw, &ch, &cellw, &cellh);
    else
        vst_point(handle, pt, &cw, &ch, &cellw, &cellh);
    return cellh;
}
