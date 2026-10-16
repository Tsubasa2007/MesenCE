#!/bin/sh
# Builds NSFPLAY.CMD with cc65 (ca65 + ld65). Uses the tools on PATH, or the folder in $CC65.
set -e
cd "$(dirname "$0")"
BIN="${CC65:+$CC65/}"
"${BIN}ca65" -g -l nsfplay.lst nsfplay.s -o nsfplay.o
"${BIN}ld65" -C nsfplay.cfg -m nsfplay.map -Ln nsfplay.lbl nsfplay.o -o NSFPLAY.CMD
rm -f nsfplay.o
ls -l NSFPLAY.CMD
