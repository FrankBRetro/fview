# FView and FConv for PC GEM (FreeGEM / OpenGEM on DOS), 32-bit with DJGPP
# Copyright (c) 2026 Francis C Bullen. SPDX-License-Identifier: MIT (see LICENSE)
#
#   source ~/work/vibe/PC-GEM/scripts/djgppenv.sh
#   make -C pcgem              -> pcgem/FVIEW.APP and pcgem/FCONV.APP
#   make -C pcgem LOG=1        ... that write FVIEW.LOG / FCONV.LOG (clean first)
#
# FView's sources (../src) are compiled with -DPCGEM against GEMLib-style
# headers in pcgem/include (gem.h, vdi.h, osbind.h, mintbind.h ...),
# implemented here on John Elliott's DJGPP GEM bindings: pgaes.c and pgvdi.c
# (trees, strings and bitmaps copied to DOS memory for GEM), pgrsc.c (reads
# FView's own Atari .RSC files from ../rsc as they are) and pgtos.c (the TOS
# calls, on DOS). There is no MiNT on DOS, so jobs run inside FView, as on
# plain TOS (FVWORK.TTP isn't built).
#
# Run it with CWSDPMI.EXE beside it or on the PATH.

DJCC   ?= i586-pc-msdosdjgpp-gcc
GEMKIT ?= $(HOME)/work/vibe/PC-GEM/tools/djgppgem
GK      = $(GEMKIT)/CONTRIB/GEM
GKINC   = obj/kitinc
LOG    ?= 0

CFLAGS   = -O2 -Wall -Wno-pointer-sign -Wno-address-of-packed-member -DPCGEM \
           -DFV_LOG=$(LOG) $(if $(filter 1,$(LOG)),-DEDLOG) -Iinclude -I../src
KITFLAGS = -O2 -Wall -Wno-address-of-packed-member -fgnu89-inline $(if $(filter 1,$(LOG)),-DEDLOG) -I$(GKINC) -I.

CORE = jobcore jobexec util rd dither palette img_save convert pdf_save \
       arcio deflate dec_st dec_gem dec_gif dec_iff dec_bmp dec_img \
       dec_stb dec_text dec_pdf
GUI  = main win render fonts job print prefs tree picdlg wdlg
CONV = fconv job wdlg
LAYER   = pgrsc pgtos pglinear      # the PC GEM layer, GEMLib-style headers
KITPORT = pgaes pgvdi               # the PC GEM layer, the kit's headers

LIBOBJ  = $(addprefix obj/,$(addsuffix .o,$(CORE) $(LAYER) $(KITPORT) pclog))
HDRS    = $(wildcard ../src/*.h include/*.h include/mint/*.h)

all: FVIEW.APP FCONV.APP

FVIEW.APP: $(addprefix obj/,$(addsuffix .o,$(GUI))) $(LIBOBJ)
	$(DJCC) -o fview.exe $^ $(GK)/LIB/LIBGEM.A -lm
	cp fview.exe $@

FCONV.APP: $(addprefix obj/,$(addsuffix .o,$(CONV))) $(LIBOBJ)
	$(DJCC) -o fconv.exe $^ $(GK)/LIB/LIBGEM.A -lm
	cp fconv.exe $@

obj/%.o: ../src/%.c $(HDRS)
	@mkdir -p obj
	$(DJCC) $(CFLAGS) -c -o $@ $<
obj/pgrsc.o obj/pgtos.o obj/pglinear.o: obj/%.o: %.c
	@mkdir -p obj
	$(DJCC) $(CFLAGS) -c -o $@ $<
obj/pgaes.o obj/pgvdi.o: obj/%.o: %.c pgkit.h $(GKINC)/.fixed
	@mkdir -p obj
	$(DJCC) $(KITFLAGS) -c -o $@ $<
obj/pclog.o: pclog.c
	@mkdir -p obj
	$(DJCC) $(CFLAGS) -c -o $@ $<

# the kit's headers with their struct packing fixed for current gcc
$(GKINC)/.fixed: $(wildcard $(GK)/INCLUDE/*.H)
	@test -f $(GK)/INCLUDE/DJGPPGEM.H || { echo "DJGPP GEM kit not found: set GEMKIT"; exit 1; }
	rm -rf $(GKINC); mkdir -p $(GKINC)
	for f in $(GK)/INCLUDE/*.H; do \
	  lc=$$(basename "$$f" | tr A-Z a-z); \
	  sed -E 's/^\}[[:space:]]*([A-Za-z_0-9]+)[[:space:]]*(__attribute__\(\(packed\)\))?[[:space:]]*GPK/} __attribute__((packed)) \1;/' "$$f" > $(GKINC)/$$lc; \
	done
	touch $@

# into DOSBox-X's C: (C:\FVIEW), with the resource files and CWSDPMI
PCGEMC ?= $(HOME)/work/vibe/PC-GEM/dos/C
install: all
	mkdir -p $(PCGEMC)/FVIEW
	cp FVIEW.APP FCONV.APP $(PCGEMC)/FVIEW/
	for f in ../rsc/*.rsc; do cp $$f $(PCGEMC)/FVIEW/$$(basename $$f | tr a-z A-Z); done
	test -f $(PCGEMC)/FVIEW/CWSDPMI.EXE || cp $(HOME)/work/vibe/PC-GEM/tools/cwsdpmi/bin/CWSDPMI.EXE $(PCGEMC)/FVIEW/

clean:
	rm -rf obj fview.exe fconv.exe FVIEW.APP FCONV.APP

.PHONY: all install clean
