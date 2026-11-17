/* Copyright (c) 2026 Francis C Bullen. SPDX-License-Identifier: MIT (see LICENSE) */
/*
 * pgkit.h - shared by pgaes.c and pgvdi.c, which are compiled against
 *           John Elliott's DJGPP GEM bindings (djgppgem.h), not against
 *           pcgem/include/gem.h. The structures are laid out the same in
 *           both (32-bit pointers in place of far pointers, packed).
 */
#ifndef PGKIT_H
#define PGKIT_H
#include "djgppgem.h"

extern short pg_wchar, pg_hchar, pg_scr_w, pg_scr_h;
extern short aes_global[15];

/* a tree in our memory copied to GEM's for one call, and back */
typedef struct pg_low PG_LOW;
LPTREE pg_tree_down(OBJECT *tree, PG_LOW **keep);
void   pg_tree_up(OBJECT *tree, LPTREE tr, PG_LOW *keep, int copyback);

#endif
