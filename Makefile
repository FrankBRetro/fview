# FView + FConv - GEM file viewer and picture converter for TOS / MultiTOS / MiNT
# (FArchive, the archiver, is its own project now: github.com/FrankBRetro/farchive)
#
#   make                 every build: release and log, for each CPU below
#   make release         release builds only
#   make log             log builds only
#   make VARIANTS=000    just the 68000 builds (any list of the variants)
#   make rsc             rebuild the resource files from rsc/lang/*.txt
#                        (python3; the .RSC files are in the repository)
#
# Needs the MiNT cross toolchain (m68k-atari-mint-gcc) with mintlib and
# gemlib. Do not use -mshort: stb_image needs 32-bit int.
#
# CPU variants, each with all three programs (FVIEW.PRG, FCONV.PRG and
# FVWORK.TTP) and the resource files (FVIEW.RSC, FCONV.RSC and their
# translations) - copy all of one build into the same folder:
#   000      68000, runs on every Atari          -> release/, log/
#   030      68030 code, no FPU (TT, Falcon)     -> release/030/, log/030/
#   030fpu   68030 + 68881/68882 FPU code        -> release/030fpu/, log/030fpu/
#            (the PDF and GEM metafile renderers use floating point;
#             needs an FPU: TT, or a Falcon/accelerator with a 68882)
#   040      68040 with its built-in FPU         -> release/040/, log/040/
#            (040 accelerators, Milan, Hades; not for a 68LC040)
#   cf       ColdFire V4e (FireBee)              -> release/cf/, log/cf/
# Log builds write FVIEW.LOG / FCONV.LOG next to the program.

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
CORE = jobcore.c jobexec.c util.c rd.c dither.c palette.c img_save.c convert.c pdf_save.c \
       arcio.c deflate.c dec_st.c dec_gem.c dec_gif.c dec_iff.c dec_bmp.c dec_img.c \
       dec_stb.c dec_text.c dec_pdf.c
GUI  = main.c win.c render.c fonts.c job.c print.c prefs.c tree.c picdlg.c wdlg.c
CONV = fconv.c job.c wdlg.c
HDRS = src/fview.h src/gemui.h src/job.h src/doc.h src/rd.h src/arcio.h src/deflate.h \
       src/palette.h src/wdlg.h src/fvrsc.h src/fcrsc.h

# resource files: English, German, French, Italian, Spanish, Czech
RSCS = fview.rsc fviewde.rsc fviewfr.rsc fviewit.rsc fviewes.rsc fviewcz.rsc \
       fconv.rsc fconvde.rsc fconvfr.rsc fconvit.rsc fconves.rsc fconvcz.rsc

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

build/$(2)$(1)/%.o: src/%.c $$(HDRS)
	@mkdir -p $$(@D)
	$$(CC) $$(CFLAGS) $(FLAGS_$(1)) -DFV_LOG=$(3) -c $$< -o $$@

$(4)$(DIR_$(1))/%.rsc: rsc/%.rsc
	@mkdir -p $$(@D)
	cp $$< $$@

$(4): $(4)$(DIR_$(1))/fview.prg $(4)$(DIR_$(1))/fconv.prg $(4)$(DIR_$(1))/fvwork.ttp \
      $(addprefix $(4)$(DIR_$(1))/,$(RSCS))
endef

all: release log

$(foreach v,$(VARIANTS),$(eval $(call BUILD,$(v),rel,0,release)))
$(foreach v,$(VARIANTS),$(eval $(call BUILD,$(v),log,1,log)))

# host tests of the decoders, ditherer, IMG writer and renderers
# (needs a native gcc; gemlib headers for rendertest: GEMINC=path)
HOSTSRC = src/util.c src/rd.c src/dither.c src/dec_gif.c src/dec_iff.c src/dec_bmp.c \
          src/dec_img.c src/dec_stb.c src/dec_text.c src/dec_pdf.c src/img_save.c \
          src/dec_st.c src/dec_gem.c src/convert.c src/pdf_save.c src/arcio.c src/deflate.c src/palette.c
hosttest: test/hosttest.c $(HOSTSRC)
	gcc -O1 -g -o test/hosttest test/hosttest.c $(HOSTSRC) -lm
convtest: test/convtest.c $(HOSTSRC)
	gcc -O1 -g -o test/convtest test/convtest.c $(HOSTSRC) -lm
rendertest: test/rendertest.c test/vdistub.c src/render.c $(HOSTSRC)
	gcc -O1 -g -I$(GEMINC) -o test/rendertest test/rendertest.c test/vdistub.c \
	    src/render.c $(HOSTSRC) -lm

# host test of the picture tools (palette.c)
paltest: test/paltest.c src/palette.c src/dither.c src/util.c
	gcc -O1 -g -Isrc -o test/paltest test/paltest.c src/palette.c src/dither.c src/util.c

rsc:
	python3 rsc/mkrsc.py

clean:
	rm -rf build release log test/hosttest test/rendertest test/convtest test/paltest

.PHONY: all release log clean hosttest rendertest convtest paltest rsc
