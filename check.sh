#!/bin/sh
EXT=/home/claude/ext
RES=$(clang -print-resource-dir)
cd "$(dirname "$0")/src"
for f in ${@:-*.c}; do
  clang --target=m68k-unknown-none -mcpu=M68000 ${OPT:--O2} -nostdinc \
    -isystem $EXT/inc -isystem $EXT/mintlib/include -isystem $RES/include \
    -D__MINT__ -D__atarist__ \
    -Wall -Wno-unused-function -Wno-unknown-pragmas -Wno-pointer-sign \
    ${MODE:--fsyntax-only} $f 2>&1
done
