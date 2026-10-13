# PicView

**PICVIEW.CMD** is a BBK BBGDOS program (BBK 1.0) that shows the pictures saved by the paint
program on disk 122 (`BRUSH.EXC`, 神奇画笔): whole canvases (`.Grp`, and the extensionless
`BBG`, `BBK` and `YEAR` on that disk) and cut-outs (`.Pct`). It lists every picture in the
current directory and shows them one at a time, with the name and `n/total` on the bottom line.
The file format is in [../EasyMake/FORMAT.md](../EasyMake/FORMAT.md#grp--pct-the-paint-program).
中文说明：[使用说明.md](使用说明.md).

| Action | Keyboard | Joypad 1 |
| --- | --- | --- |
| Next picture | Right, Down, PgDn, Space, Enter, N | Right, Down, A |
| Previous picture | Left, Up, PgUp, Backspace, P | Left, Up, B |
| First / last | Home / End | |
| Quit to DOS | Esc, Q | Start |

The joypad only works when MOUSE.CMD is not loaded: the mouse driver takes over joypad
scanning and the BIOS joypad variable stops changing. The keyboard always works.

## Using it

Copy `PICVIEW.CMD` onto the disk with the pictures and type `picview` at the `A:\>` prompt
(or add a `picview` line to `AUTOEXEC.BAT`). The Chinese system has to be loaded first
(`BBGCDOS`; disk 122's `AUTOEXEC.BAT` already runs `bbgcdos/z4`). Esc returns to DOS.

`fat12add.py` copies files into the root directory of a FAT12 disk image, replacing an entry
with the same name. It writes the image in place, so use a copy:

    python fat12add.py "122 copy.img" PICVIEW.CMD
    python fat12add.py "122 copy.img" my_autoexec.bat --as AUTOEXEC.BAT

Tested in MesenCE on the BBK 1.0 BIOS (all 117 pictures on disk 122, keyboard and joypad,
with and without the mouse and music drivers) and the BBK-98 BIOS. Not tried on real hardware.

## Building

`PICVIEW.CMD` is checked in. To rebuild it you need [cc65](https://cc65.github.io/) (`ca65`
and `ld65`), on PATH or named by `CC65`:

    CC65="/d/Program Files/cc65/bin" sh build.sh

A BBGDOS `.CMD` is a 2-byte load address followed by the code, entered at the load address;
`picview.cfg` places it at `$6100`, as disk 122's own `SQHB.CMD` is.

## How it draws

A picture cell (a colour group and two 2bpp 8x8 tiles stacked into 8x16) is exactly what the
BIOS call `BiosSetBGFont` ($36) draws at the cursor in the Chinese display mode (14 rows of
31 cells), so every cell is one `BiosSetCursor` + `BiosSetBGFont`. The 12 palette bytes go to
BIOS palette entries 0-11 with `BiosSetPalette`, leaving out 4 and 8 (copies of entry 0).
Files are found with `FilePathFirst`/`FileNext` and read a row of cells at a time with
`FilePathOpen`/`FileRead`.

What it depends on, each found the hard way:

- Clear the screen with `DosInitialScreen`, not `BiosDispInitial`: with BBGCDOS loaded the
  ROM call leaves the banded screen and its line interrupt off, and the first queued draw
  then waits forever.
- Draw with the screen closed (`BiosDispOpenClose` 1) and call `BiosWaitDisplay` first. With
  the screen closed the BIOS stages each cell at `$5D6D`, inside its display queue (`$5D00`);
  a job still in the queue is overwritten and the interrupt handler later runs the garbage
  (it copied video memory over `$4000-$401F` and turned on the APU frame interrupt, which
  nothing acknowledges).
- `DosCheckKeyBoard` only looks; take the key with `DosReadKeyNoEcho`. Grey keys come as
  `$E0` (or 0) followed by the scan code.
- `BiosReadKey` in test mode reported nothing for the joypad; the viewer reads the BIOS
  variable `$5FFC` (joypad 1 buttons held: R L D U Start Select B A) and acts on new presses.
- `$FC-$FF` are the BIOS argument bytes; the viewer writes them only to pass arguments.
