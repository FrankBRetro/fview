/* Copyright (c) 2026 Francis C Bullen. SPDX-License-Identifier: MIT (see LICENSE) */
/*
 * pglinear.c - the out-of-line LINEAR() and SEGOFF() the DJGPP GEM bindings
 *              call where gcc doesn't inline them. The kit's headers declare
 *              them GNU89 "extern inline" (no body of their own), and the
 *              kit's only real copies are in its MAIN.O, which would bring
 *              its own main() along. Not compiled against the kit's headers.
 */
long LINEAR(long x) { return (x & 0xFFFF) + ((x & 0xFFFF0000L) >> 12); }   /* seg:off -> linear */
long SEGOFF(long x) { return (x & 0x0F) | ((x & 0xFFFF0L) << 12); }        /* linear -> seg:off */
