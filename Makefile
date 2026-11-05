# FView + FConv + FArchive - GEM file viewer, picture converter and archiver
# for TOS / MultiTOS / MiNT
#
#   make                 every build: release and log, for each CPU below
#   make release         release builds only
#   make log             log builds only
#   make VARIANTS=000    just the 68000 builds (any list of the variants)
#
# Needs the MiNT cross toolchain (m68k-atari-mint-gcc) with mintlib and
# gemlib. Do not use -mshort: stb_image needs 32-bit int.
#
# CPU variants, each with all five programs (FVIEW.PRG, FCONV.PRG and their
# worker FVWORK.TTP; FARCHIVE.PRG and its worker FAWORK.TTP - copy the
# programs of one build into the same folder):
#   000      68000, runs on every Atari          -> release/, log/
#   030      68030 code, no FPU (TT, Falcon)     -> release/030/, log/030/
#   030fpu   68030 + 68881/68882 FPU code        -> release/030fpu/, log/030fpu/
#            (the PDF and GEM metafile renderers use floating point;
#             needs an FPU: TT, or a Falcon/accelerator with a 68882)
#   040      68040 with its built-in FPU         -> release/040/, log/040/
#            (040 accelerators, Milan, Hades; not for a 68LC040)
#   cf       ColdFire V4e (FireBee)              -> release/cf/, log/cf/
# Log builds write FVIEW.LOG / FCONV.LOG / FARCHIVE.LOG next to the program.

CROSS   ?= m68k-atari-mint-
CC       = $(CROSS)gcc
STRIP    = $(CROSS)strip
CFLAGS  ?= -O2 -fomit-frame-pointer
CFLAGS  += -Wall -Wno-pointer-sign
LDLIBS   = -lgem

VARIANTS ?= 000 030 030fpu 040 cf
FLAGS_000    = -m68000
FLAGS_030    = -m68030 -msoft-float
FLAGS_030fpu = -m68030 -m68881
FLAGS_040    = -m68040
FLAGS_cf     = -mcpu=5475
DIR_000    =
DIR_030    = /030
DIR_030fpu = /030fpu
DIR_040    = /040
DIR_cf     = /cf

# shared by FView, FConv and their background worker
CORE = jobcore.c jobexec.c util.c rd.c dither.c img_save.c convert.c pdf_save.c \
       arcio.c deflate.c dec_st.c dec_gem.c dec_gif.c dec_iff.c dec_bmp.c dec_img.c \
       dec_stb.c dec_text.c dec_pdf.c
GUI  = main.c win.c render.c fonts.c job.c print.c prefs.c tree.c
CONV = fconv.c job.c
# FArchive and its worker: the archive formats and codecs
ACORE = jobcore.c util.c arcjob.c archive.c arcfmt.c arcfs.c arcio.c codecs.c \
        deflate.c inflate.c lzh.c lzw.c lzx.c
ARCH = farchive.c job.c
HDRS = src/fview.h src/gemui.h src/job.h src/doc.h src/rd.h src/arcio.h src/archive.h \
       src/arcint.h src/deflate.h

# one build: $(1) variant, $(2) rel|log, $(3) FV_LOG value, $(4) release|log
define BUILD
$(4)$(DIR_$(1))/fview.prg: $(addprefix build/$(2)$(1)/,$(GUI:.c=.o) $(CORE:.c=.o))
	@mkdir -p $$(@D)
	$$(CC) $(FLAGS_$(1)) -o $$@ $$^ $$(LDLIBS)
	$$(STRIP) $$@

$(4)$(DIR_$(1))/fconv.prg: $(addprefix build/$(2)$(1)/,$(CONV:.c=.o) $(CORE:.c=.o))
	@mkdir -p $$(@D)
	$$(CC) $(FLAGS_$(1)) -o $$@ $$^ $$(LDLIBS)
	$$(STRIP) $$@

$(4)$(DIR_$(1))/fvwork.ttp: $(addprefix build/$(2)$(1)/,worker.o $(CORE:.c=.o))
	@mkdir -p $$(@D)
	$$(CC) $(FLAGS_$(1)) -o $$@ $$^
	$$(STRIP) $$@

$(4)$(DIR_$(1))/farchive.prg: $(addprefix build/$(2)$(1)/,$(ARCH:.c=.o) $(ACORE:.c=.o))
	@mkdir -p $$(@D)
	$$(CC) $(FLAGS_$(1)) -o $$@ $$^ $$(LDLIBS)
	$$(STRIP) $$@

$(4)$(DIR_$(1))/fawork.ttp: $(addprefix build/$(2)$(1)/,worker.o $(ACORE:.c=.o))
	@mkdir -p $$(@D)
	$$(CC) $(FLAGS_$(1)) -o $$@ $$^
	$$(STRIP) $$@

build/$(2)$(1)/%.o: src/%.c $$(HDRS)
	@mkdir -p $$(@D)
	$$(CC) $$(CFLAGS) $(FLAGS_$(1)) -DFV_LOG=$(3) -c $$< -o $$@

$(4): $(4)$(DIR_$(1))/fview.prg $(4)$(DIR_$(1))/fconv.prg $(4)$(DIR_$(1))/fvwork.ttp \
     $(4)$(DIR_$(1))/farchive.prg $(4)$(DIR_$(1))/fawork.ttp
endef

all: release log

$(foreach v,$(VARIANTS),$(eval $(call BUILD,$(v),rel,0,release)))
$(foreach v,$(VARIANTS),$(eval $(call BUILD,$(v),log,1,log)))

# host tests of the decoders, ditherer, IMG writer and renderers
# (needs a native gcc; gemlib headers for rendertest: GEMINC=path)
HOSTSRC = src/util.c src/rd.c src/dither.c src/dec_gif.c src/dec_iff.c src/dec_bmp.c \
          src/dec_img.c src/dec_stb.c src/dec_text.c src/dec_pdf.c src/img_save.c \
          src/dec_st.c src/dec_gem.c src/convert.c src/pdf_save.c src/arcio.c src/deflate.c
hosttest: test/hosttest.c $(HOSTSRC)
	gcc -O1 -g -o test/hosttest test/hosttest.c $(HOSTSRC) -lm
convtest: test/convtest.c $(HOSTSRC)
	gcc -O1 -g -o test/convtest test/convtest.c $(HOSTSRC) -lm
rendertest: test/rendertest.c test/vdistub.c src/render.c $(HOSTSRC)
	gcc -O1 -g -I$(GEMINC) -o test/rendertest test/rendertest.c test/vdistub.c \
	    src/render.c $(HOSTSRC) -lm

# host test of FArchive's archive layer (test/arctest.c)
ARCSRC = src/archive.c src/arcfmt.c src/arcfs.c src/arcio.c src/codecs.c src/deflate.c \
         src/inflate.c src/lzh.c src/lzw.c src/lzx.c
arctest: test/arctest.c $(ARCSRC)
	gcc -O1 -g -DARC_TEST -Isrc -o test/arctest test/arctest.c $(ARCSRC)

clean:
	rm -rf build release log test/hosttest test/rendertest test/convtest test/arctest

.PHONY: all release log clean hosttest rendertest convtest arctest
