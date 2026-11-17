/* Copyright (c) 2026 Francis C Bullen. SPDX-License-Identifier: MIT (see LICENSE) */
/*
 * vdi.h - the VDI part of the GEMLib API these programs use, for PC GEM (see gem.h)
 */
#ifndef PCGEM_VDI_H
#define PCGEM_VDI_H

/* writing modes */
#define MD_REPLACE 1
#define MD_TRANS 2
#define MD_XOR 3
#define MD_ERASE 4
/* fill */
#define FIS_HOLLOW 0
#define FIS_SOLID 1
#define FIS_PATTERN 2
#define FIS_HATCH 3
#define FIS_USER 4
/* lines */
#define SOLID 1
#define LONGDASH 2
#define DOT 3
#define DASHDOT 4
#define DASH 5
#define DASH2DOT 6
#define USERLINE 7
#define SQUARE 0
#define ARROWED 1
#define ROUND 2
/* colours */
#define G_WHITE 0
#define G_BLACK 1
#define G_RED 2
#define G_GREEN 3
#define G_BLUE 4
#define G_CYAN 5
#define G_YELLOW 6
#define G_MAGENTA 7
#define G_LWHITE 8
#define G_LBLACK 9
#define WHITE G_WHITE
#define BLACK G_BLACK
/* text */
#define TXT_NORMAL 0x0000
#define TXT_THICKENED 0x0001
#define TXT_LIGHT 0x0002
#define TXT_SKEWED 0x0004
#define TXT_UNDERLINED 0x0008
#define TXT_OUTLINED 0x0010
#define TXT_SHADOWED 0x0020
#define TA_LEFT 0
#define TA_CENTER 1
#define TA_RIGHT 2
#define TA_BASE 0
#define TA_HALF 1
#define TA_ASCENT 2
#define TA_BOTTOM 3
#define TA_DESCENT 4
#define TA_TOP 5
/* raster operations */
#define ALL_WHITE 0
#define S_AND_D 1
#define S_AND_NOTD 2
#define S_ONLY 3
#define NOTS_AND_D 4
#define D_ONLY 5
#define S_XOR_D 6
#define S_OR_D 7
#define NOT_SORD 8
#define NOT_SXORD 9
#define D_INVERT 10
#define NOT_D 10
#define S_OR_NOTD 11
#define NOTS 12
#define NOTS_OR_D 13
#define NOT_SANDD 14
#define ALL_BLACK 15
/* GDOS */
#define GDOS_NONE   (-2L)
#define GDOS_FSM    0x5F46534DL
#define GDOS_FNT    0x5F464E54L
#define GDOS_TTF    0x3E5F2E5FL
#define SCRATCH_BOTH 0
#define SCRATCH_BITMAP 1
#define SCRATCH_NONE 2
#define SCREEN_ERROR 1
#define APP_ERROR 0

/* a raw VDI call (gemrend.c makes vqt_fontinfo this way, with big reply
 * buffers for SpeedoGDOS); pg_vdi() passes the opcodes PC GEM has on */
typedef struct { short *control, *intin, *ptsin, *intout, *ptsout; } VDIPB;
#define vdi pg_vdi
void vdi(VDIPB *pb);

#define v_opnwk          pg_v_opnwk
#define v_opnvwk         pg_v_opnvwk
#define v_clswk          pg_v_clswk
#define v_clsvwk         pg_v_clsvwk
#define v_clrwk          pg_v_clrwk
#define v_updwk          pg_v_updwk
#define vs_clip          pg_vs_clip
#define vswr_mode        pg_vswr_mode
#define vs_color         pg_vs_color
#define vq_color         pg_vq_color
#define vq_extnd         pg_vq_extnd
#define vq_scrninfo      pg_vq_scrninfo
#define vq_gdos          pg_vq_gdos
#define vq_vgdos         pg_vq_vgdos
#define v_pline          pg_v_pline
#define v_pmarker        pg_v_pmarker
#define v_fillarea       pg_v_fillarea
#define v_circle         pg_v_circle
#define vr_recfl         pg_vr_recfl
#define v_gtext          pg_v_gtext
#define v_justified      pg_v_justified
#define vsl_color        pg_vsl_color
#define vsl_type         pg_vsl_type
#define vsl_width        pg_vsl_width
#define vsl_ends         pg_vsl_ends
#define vsl_udsty        pg_vsl_udsty
#define vsm_type         pg_vsm_type
#define vsm_color        pg_vsm_color
#define vsf_interior     pg_vsf_interior
#define vsf_color        pg_vsf_color
#define vsf_style        pg_vsf_style
#define vsf_perimeter    pg_vsf_perimeter
#define vst_color        pg_vst_color
#define vst_font         pg_vst_font
#define vst_point        pg_vst_point
#define vst_height       pg_vst_height
#define vst_arbpt        pg_vst_arbpt
#define vst_effects      pg_vst_effects
#define vst_alignment    pg_vst_alignment
#define vst_load_fonts   pg_vst_load_fonts
#define vst_unload_fonts pg_vst_unload_fonts

/* more of the VDI (Fbench) */
#define v_bar            pg_v_bar
#define v_arc            pg_v_arc
#define v_pieslice       pg_v_pieslice
#define v_ellipse        pg_v_ellipse
#define v_ellarc         pg_v_ellarc
#define v_ellpie         pg_v_ellpie
#define v_rbox           pg_v_rbox
#define v_rfbox          pg_v_rfbox
#define v_contourfill    pg_v_contourfill
#define v_get_pixel      pg_v_get_pixel
#define v_show_c         pg_v_show_c
#define v_hide_c         pg_v_hide_c
#define vsm_height       pg_vsm_height
#define vst_rotation     pg_vst_rotation
#define vsf_udpat        pg_vsf_udpat
#define vqt_attributes   pg_vqt_attributes
#define vqt_fontinfo     pg_vqt_fontinfo
#define vst_scratch      pg_vst_scratch
#define vst_error        pg_vst_error
#define vst_charmap      pg_vst_charmap
#define vqt_extent       pg_vqt_extent
#define vqt_width        pg_vqt_width
#define vqt_name         pg_vqt_name
#define vqt_advance      pg_vqt_advance
#define v_getoutline     pg_v_getoutline
#define vro_cpyfm        pg_vro_cpyfm
#define vrt_cpyfm        pg_vrt_cpyfm
#define vr_trnfm         pg_vr_trnfm

void  v_opnwk(short *work_in, short *handle, short *work_out);
void  v_opnvwk(short *work_in, short *handle, short *work_out);
void  v_clswk(short h);
void  v_clsvwk(short h);
void  v_clrwk(short h);
void  v_updwk(short h);
void  vs_clip(short h, short on, short *pxy);
short vswr_mode(short h, short mode);
void  vs_color(short h, short index, const short *rgb);
short vq_color(short h, short index, short flag, short *rgb);
void  vq_extnd(short h, short flag, short *work_out);
void  vq_scrninfo(short h, short *work_out);
short vq_gdos(void);
long  vq_vgdos(void);
void  v_pline(short h, short n, short *pxy);
void  v_pmarker(short h, short n, short *pxy);
void  v_fillarea(short h, short n, short *pxy);
void  v_circle(short h, short x, short y, short r);
void  vr_recfl(short h, short *pxy);
void  v_gtext(short h, short x, short y, const char *s);
void  v_justified(short h, short x, short y, const char *s, short len, short wsp, short csp);
short vsl_color(short h, short c);
short vsl_type(short h, short t);
short vsl_width(short h, short w);
void  vsl_ends(short h, short b, short e);
void  vsl_udsty(short h, short pat);
short vsm_type(short h, short t);
short vsm_color(short h, short c);
short vsf_interior(short h, short s);
short vsf_color(short h, short c);
short vsf_style(short h, short s);
short vsf_perimeter(short h, short on);
short vst_color(short h, short c);
short vst_font(short h, short id);
short vst_point(short h, short pt, short *cw, short *ch, short *bw, short *bh);
void  vst_height(short h, short height, short *cw, short *ch, short *bw, short *bh);
short vst_arbpt(short h, short pt, short *cw, short *ch, short *bw, short *bh);
short vst_effects(short h, short e);
void  vst_alignment(short h, short hin, short vin, short *hout, short *vout);
short vst_load_fonts(short h, short select);
void  vst_unload_fonts(short h, short select);
void  vst_scratch(short h, short mode);
void  vst_error(short h, short mode, short *code);
short vst_charmap(short h, short mode);
void  vqt_extent(short h, const char *s, short *pts);
short vqt_width(short h, short ch, short *cw, short *ld, short *rd);
short vqt_name(short h, short n, char *name);
void  vqt_advance(short h, short ch, short *ax, short *ay, short *rx, short *ry);
void  v_getoutline(short h, short ch, short *xy, char *bez, short max, short *count);
void  vro_cpyfm(short h, short mode, short *pxy, MFDB *src, MFDB *dst);
void  vrt_cpyfm(short h, short mode, short *pxy, MFDB *src, MFDB *dst, short *colors);
void  vr_trnfm(short h, MFDB *src, MFDB *dst);


/* more of the VDI (Fbench) */
void  v_bar(short h, short *pxy);
void  v_arc(short h, short x, short y, short r, short beg, short end);
void  v_pieslice(short h, short x, short y, short r, short beg, short end);
void  v_ellipse(short h, short x, short y, short xr, short yr);
void  v_ellarc(short h, short x, short y, short xr, short yr, short beg, short end);
void  v_ellpie(short h, short x, short y, short xr, short yr, short beg, short end);
void  v_rbox(short h, short *pxy);
void  v_rfbox(short h, short *pxy);
void  v_contourfill(short h, short x, short y, short color);
short v_get_pixel(short h, short x, short y, short *pel, short *index);
void  v_show_c(short h, short reset);
void  v_hide_c(short h);
short vsm_height(short h, short height);
short vst_rotation(short h, short angle);
void  vsf_udpat(short h, short *pat, short planes);
void  vqt_attributes(short h, short *attr);
short vqt_fontinfo(short h, short *minade, short *maxade, short *dist, short *maxw, short *effects);

#endif
