/* Copyright (c) 2026 Francis C Bullen. SPDX-License-Identifier: MIT (see LICENSE) */
/* mint/cookie.h - PC GEM: there is no cookie jar, so no cookie is ever found */
#ifndef PCGEM_COOKIE_H
#define PCGEM_COOKIE_H
#define C_FOUND    0
#define C_NOTFOUND (-1)
/* the cookies programs look for (never there on the PC) */
#ifndef C_MiNT
#define C_MiNT  0x4D694E54L     /* MiNT */
#define C_MagX  0x4D616758L     /* MagX */
#define C_EdDI  0x45644449L     /* EdDI */
#define C_NVDI  0x4E564449L     /* NVDI */
#define C__CPU  0x5F435055L     /* _CPU */
#define C__MCH  0x5F4D4348L     /* _MCH */
#define C__VDO  0x5F56444FL     /* _VDO */
#define C_FSMC  0x46534D43L     /* FSMC */
#endif
int Getcookie(long id, long *val);
#endif
