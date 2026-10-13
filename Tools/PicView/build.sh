#!/bin/sh
# Builds PICVIEW.CMD with cc65 (ca65 + ld65). Uses the tools on PATH, or the folder in $CC65.
set -e
cd "$(dirname "$0")"
BIN="${CC65:+$CC65/}"
"${BIN}ca65" -g -l picview.lst picview.s -o picview.o
"${BIN}ld65" -C picview.cfg -m picview.map picview.o -o PICVIEW.CMD
rm -f picview.o
ls -l PICVIEW.CMD
