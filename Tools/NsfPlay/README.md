# NsfPlay

**NSFPLAY.CMD** plays NES music files (`.NSF`) on the BBK learning machine (BBGDOS, BBK 1.0
or BBK-98 BIOS) through the 2A03 sound channels of its CPU. **nsf2bbk.py** prepares the NSFs
it cannot play directly. 中文说明：[使用说明.md](使用说明.md).

| Action | Keyboard | Joypad 1 |
| --- | --- | --- |
| Next / previous track | Right / Left (also N / P) | Right / Left |
| Next / previous file | Down, PgDn / Up, PgUp | Down / Up |
| Pause | Space, Enter | Start, A |
| Quit to DOS | Esc, Q | Select |

Actions happen when the key is let go. The screen shows the file, the title, artist and
copyright from the NSF header, the track, and the time played.

## Using it

Copy `NSFPLAY.CMD` and the music onto a BBGDOS disk and type `nsfplay` at the `A:\>`
prompt. It lists every `.NSF` and `.NSB` file in the current directory and starts with the
first, or with the file named on the command line (`nsfplay mm2`, `nsfplay cv3.nsb`). It
works in the English and the Chinese system (it switches to the English screen while it
runs and back on exit), with or without MOUSE.CMD and MUSIC.CMD loaded.

Which files play directly:

- **Plain NSFs** (no bank switching): yes.
- **NSFs whose banks never change** after the header sets them up (Mega Man 2, for one):
  yes. The player checks the code for bank-register stores and refuses the file if it
  finds any.
- **Bank switching or expansion sound** (VRC6, VRC7, MMC5, Namco 163, Sunsoft 5B): run
  `nsf2bbk.py` on a PC first and copy the `.NSB` it writes. Expansion channels do not
  play - the machine has no such chips - but the 2A03 part of the music does.
- **FDS** tunes and tunes loaded below $8000: no.

The music comes out about 1% slower and lower than on an NTSC NES: the machine's CPU clock
is the Dendy one (1.773 MHz), which drives both the pitch and the timer. PAL-only tunes
play at the right speed but about a semitone sharp (the machine has NTSC pitch tables).
Tunes that select the frame counter's 5-step mode get the 4-step mode, which the timer
needs; their envelopes and sweeps can run slightly faster.

## nsf2bbk.py

    python nsf2bbk.py "Castlevania 3 (VRC6).nsf"          -> CASTLEVA.NSB next to it
    python nsf2bbk.py cv3.nsf -o CV3.NSB --seconds 120

Python 3, no packages. It runs every song (INIT, then PLAY for `--seconds`, default 60)
in a 6502 emulator and:

- replaces each store to a bank register ($5FF8-$5FFF) with a `JSR` into NSFPLAY's bank
  routines, which map the right 8K DRAM pages; bank-register stores it did not see run
  are found in the code by pattern as well (`--no-scan` to skip those);
- points stores to expansion-sound registers (and anything else outside RAM and the 2A03)
  at a scratch area, so they cannot overwrite the tune or hit the machine's I/O page;
- lists the 8K pairs of banks the tune uses that are not consecutive in the file; NSFPLAY
  copies those into pages of their own when it loads the file;
- then runs every song again the way NSFPLAY will and checks that it writes exactly the
  same sound registers as the original. The `.NSB` is written only if all songs match.

A song that takes a path after `--seconds` that it never took within them could still meet
a store that was not patched; raise `--seconds` for long tunes. An `.NSB` is only for
NSFPLAY - other NSF players cannot play it.

## Building

`NSFPLAY.CMD` is checked in. To rebuild it you need [cc65](https://cc65.github.io/) (`ca65`
and `ld65`), on PATH or named by `CC65`:

    CC65="/d/Program Files/cc65/bin" sh build.sh

## How it works

- The file is read into free 16K DRAM pages (`DosInquirePages`) as its 4K banks one after
  the other, so 8K page *k* holds banks 2*k* and 2*k*+1. A window showing such a pair maps
  that page; any other pair the tune needs gets a copied page.
- For each track the program takes the machine over. It saves $0000-$07FF, $4E00-$57FF
  and $6000-$7FFF in a DRAM page, copies its resident part to $5000 (variables at $4E00),
  turns interrupts and the PPU's NMI off, and maps the tune at $8000-$FFFF with the 8K page
  registers $FF14, $FF1C, $FF24 and $FF2C. The tune gets the 2K of RAM and $6000-$7FFF as
  its work RAM. Everything is put back before the BIOS runs again, between tracks and at
  the end.
- PLAY is timed by polling the APU frame-counter flag ($4015 bit 6, 4-step mode, 59.45 Hz
  here); 50 Hz tunes use the vblank flag instead, and other rates add up the time. If the
  flag never comes - a famiclone APU without it - the player switches to vblank timing.
- Keys and the joypad are read directly ($4016/$4017). The joypad is read until two reads
  agree: a DPCM sample fetch during a read shifts the controller's bits.
- The time and "Paused" are written straight into the BIOS's English text screen (the
  nametable holds character codes) at the start of a vblank.
- Before taking over, the program lets the BIOS stop the floppy motor (the BIOS's motor
  countdown $5FA0 set to 1), which would otherwise run on while the BIOS is stopped.

`nsf2bbk.py` patches calls to fixed addresses: $5000 + 3*n* (`STA $5FF8+n`), $5018 (`STX`),
$5030 (`STY`), $5048 (`STA $5FF8+k,X`), $5060 (`STA $5FF8+k,Y`), and the scratch area
$5700-$57FF. `nsfplay.s` asserts that layout.

Tested in MesenCE with the BBK 1.0 and BBK-98 BIOS: for Journey to Silius, Mega Man 2 and
Castlevania III (VRC6, as `.NSB`) the sound-register writes match a reference run of the
original NSF write for write; track, file, pause and quit keys; the Chinese system with the
mouse and music drivers; DOS afterwards. With the BBK 1.0 BIOS also the bank-switching
Castlevania III (US), Final Fantasy III and Ninja Gaiden III as `.NSB`: the first and the
last two tracks of each match. Not tried on real hardware.
