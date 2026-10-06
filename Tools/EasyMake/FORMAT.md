# EasyMake / XDOS file formats

中文版：[格式说明.md](格式说明.md).

Worked out from `XDOS.CMD` (BBK disk 004 V2.0; 9118 bytes, loads at `$C300`, entry
`JMP $D1A9`), the music driver in `INSTALL.CMD`, and the BBK BIOS calls they make. Checked
against MesenCE: the `.GRA` decode and sprite cells render pixel-exact, and `verify` rebuilds
all of 004's files.

## Runtime

`AUTOEXEC.BAT`: `bbgcdos/z4`, `install`, `mouse`, `xdos`. XDOS needs BBGCDOS (it prints
`Not found BBGCDOS!` otherwise) and the music driver `INSTALL.CMD` copies to `$44C1-$4C25`
(`$4CFF=$FF` = present). File names are fixed in XDOS: `sdos.dsp`, and `sdos.res` until a
`resource=` attribute names another pack. The DSP loads into DRAM banks from `$D0`, the RES
into the bank after the DSP's end; XDOS adds that bank to every directory entry's bank byte.
Pointer and buttons come from the mouse driver (`$5FF2` x, `$5FF3` y, `$5FFC` bit 0 button).

## .DSP script

GBK text, CR LF. A line starting `<` is a tag; other lines are text for the open textbox.
Attributes are `name=value`, separated by spaces or commas; strings in `"`. Tag table at
`$E512` (16-byte entries: 14-byte name, handler address); unknown tags are skipped.

| tag | attributes | effect |
|---|---|---|
| `desktop` | `resource` `background` `backsound` `name` | new screen: hide sprites, load the RES if given, draw the `.GRA` (BIOS `$42`), start the music; `name` is a label |
| `img` | `src` `left` `top` | draw an image object (sprites at pixels; glyph type at text cells) |
| `animate` | `src` `time` `count` | play an animation, `time` ticks per frame. Without `count` it plays once and the script waits. With `count` it runs in the background — but only while XDOS waits on a full text box, not inside `<menu>…</menu>` |
| `static` | `left` `top` `caption` | print a string at column/row (32 × 15 cells of 8 × 16) through BBGCDOS (cursor `$1C`); column c appears at pixel (c + 1) × 8, like glyph images, sprites and the pointer, all 8 px right of the unmoved `.GRA` |
| `speak` | `src` `word` | say phrase `word` of a speech resource |
| `wait` | `time` / `key` / `str` `line` `col` | delay; wait for a key or click (`key=0`); or a type-in check of `str` (wrong → `错误！请重新输入！`) |
| `menu` … `/menu` | | collect `menuitem`s, then wait for a click |
| `menuitem` | `left` `top` `right` `bottom` `href` `act` | clickable rectangle in pixels; `act` = hover effect |
| `go` | `href` | jump to the tag with that `name=` (searched from the script start) |
| `textbox` | `left` `top` `right` `bottom` | text area in cells; the lines that follow go into it |
| `page` / `/textbox` | | page break / end of the text |
| `/desktop` | `name` | quit XDOS |
| `answer` `easy` | `result`, `right` | 004's quiz screens: fixed banks and addresses inside 004's own SDOS.RES — not usable elsewhere |

Attributes: src name background backsound left top right bottom href caption time word count
resource key str line col result act.

Text is drawn in BG palette entry 13 on entry 12, and printing switches the 16 × 16 attribute
areas it touches to sub-palette 3.

004's script holds a few bytes that are not valid GBK (extra BBK font symbols); the tool keeps
them as `{#XX}`.

## .RES container

- 100 directory entries × 16 bytes = `$640`: 12-byte name (zero padded), `0`, address low,
  address high (`$8000-$BFFF`), bank (16 KB units from the file start). Unused slots are `$FF`.
- No resource crosses a 16 KB bank; free space is `$FF`.
- Every pointer inside a resource is a CPU address in its own bank, so objects that point at
  each other must share a bank and move together. Resources share objects freely (an animation
  may use another resource's frames).
- The original tool left holes and stale data (unreferenced old objects, phrase tables with
  stale entries). Rebuilding keeps every object's offset, so the result is identical wherever
  an object is.

## Objects

**Image object** — `type, count, data` (XDOS `$DBB5`):

| type | data | drawn by |
|---|---|---|
| 0 | `count` GBK bytes of text | BIOS `$32` at the cursor; mode ≠ 0 swaps the colours |
| 1 | `count` × (`x, y, attribute, ?` + 32-byte 8 × 16 cell) — BG glyphs at text cell `(left + x/8, top + y/16)` | BIOS `$36` |
| 2 | a GR picture (as `.GRA`) | BIOS `$42` |
| 3 | `count` × (`dx, dy, attribute (palette), sprite number` + 32-byte 8 × 16 cell) | BIOS `$38` — the BIOS shows sprites at x + 8 |
| 4 | `count` × (`column, row, index, foreground, background, char lo, char hi`) — coloured characters | BIOS `$2F` |

A cell is two NES 2bpp tiles, top then bottom. An `img` resource has its 16-byte palette in
front of the directory pointer (sprite palette for types 0/3/4, BG palette for type 1).

**Animation**: count (14 bits; byte 1 bit 7 = load the BG palette too), then count × 8-byte
frames `x, y, image pointer, x origin, y origin, mode, kind`. Drawn at slot position + x − x
origin (the slot position is 0 for `animate`); kind 0 draws with `$DBB5`, 1 with `$DD8E`;
mode 0 normal, 1/6/7 transforms, `$10` erase. An `animate` resource is a 16-byte sprite
palette followed by the animation. 8 animation slots.

**Act** (hover effect), 25 bytes: flags (bit 0 music, 1 speech, 2 animation), music header
(4), speech pointer (2), animation pointer (2), sprite palette (16). Played when the pointer
enters the menu item (`$C6DD`). When the pointer leaves an item that has one, XDOS calls `$CC5B`,
which hides sprites 58 down to 0 one by one with BIOS `$38` - not just the act's, so sprite
pictures on the page vanish too.

**Speech**: a table of phrase pointers, then the phrases. A phrase = big-endian length, `$D6`,
then that many bytes of LPC-10 for the BBK speech chip (MesenCE `BbkLpcAudio`, "D6" tables;
bits read least significant first; energy 15 ends). Played by BIOS `$5809` X=`$11`, which
sends `length` bytes starting with the `$D6` - so the last stored byte never reaches the chip
(true of BBK's phrases as well); the encoder pads one byte.

Act music and speech: the branches exist (`$C709`, `$C76C`) but misread their fields — the
music branch pushes three bytes where the driver call takes four, and the speech branch loads
both pointer bytes into X — so only act animations are usable.

**Music** (for the INSTALL driver): song table pointer, track table pointer. Song = `tracks-1,
first track`. Track entry = `slot, flags (APU channel), data pointer`. Track data = 4-byte
instrument header (`tempo, duty, envelope, sweep`) then 2-byte events, `$FF` to end. Notes:
high nibble octave (0 = C2), low nibble semitone (≥ 12 = rest), then length. `$A0-$AF`
volume/sweep/duty/vibrato/segment/tempo, `$Bn t` loop to event t (n = 0: jump to the `$FD`
mark), `$Cn` decay, `$Dn/$En` volume slide, `$FD` mark. Loop targets are event numbers, so
track data moves without fix-ups.

The driver runs once per frame (50 Hz). A track slot (`$00 $15 $2A $3F`, and `$54 $69 $7E $93`,
the ones 004 uses) keeps a tempo accumulator: it starts at R = the tempo byte with its nibbles
swapped, plus `$0F`; each frame takes `$10` off, and an underflow is one tick (so 16 / (R + 1)
ticks per frame). Event numbers count from the start of the track data, the 4-byte header being
events 0 and 1. Events:

| Event | Meaning |
|---|---|
| note `on, len` | `o` octave (0 = C2), `n` semitone 0-11, for `len` ticks; `n` 12-15 is a rest. On the noise channel a byte below `$10` is the noise period, `$10` and up a rest |
| `$A0 v` | volume/envelope bits (the duty is kept) |
| `$A1 v` | sweep |
| `$A2 d` | duty 0-3 (triangle: the linear counter) |
| `$A3 v` | vibrato: bits 4-6 shape, low nibble the delay (x 2 ticks); bit 7 = off |
| `$A4 -` | vibrato off |
| `$AD n` | new segment: the data starts again here; n = 0: an instrument header follows |
| `$AE v` | bit 7 of the tempo reload |
| `$AF t` | tempo |
| `$Bn t` | n = 0: jump to event t (t = 0: the `$FD` mark). n > 0: loop, jumping back n times, so the section plays n + 1 times (t = 0 and t > 0 have separate counters) |
| `$Cn l` | decay shape n (1-9) over l frames from each note; only with constant volume (envelope bit 4) |
| `$Dn p` / `$En p` | volume up / down n steps, one every p ticks |
| `$FD -` | loop mark |
| `$FF` | end of track (one byte) |

**Music listings** (`.mus`, as `extract` writes and `build` reads): one item per line, `;` starts
a comment.

```
@label:                 define a label
header @songs @tracks   the two table pointers
song 3 0                a song entry
track $54 $00 @t0       a track entry
inst 31 02 1D 00        an instrument header (hex)
C-4 10  / C#4 10        a note (musical octave 2-11, so byte $29 is A-4, 440 Hz) and its length in ticks (hex)
rest $3C 10             a rest, with the byte as stored
ev A3 22                any other event (hex opcode, hex argument)
end                     $FF
db 12 34 ...            raw bytes (hex)
```

## .GRA

`"GR"`, 0, 0, width (LE16), height (LE16), 8 zeros; 16-byte BG palette; attribute table
`(w/32)·⌈h/32⌉` bytes in NES layout; `w/8 × h/8` NES 2bpp tiles, row-major. 256 × 240 = 15456
bytes. 004's pictures keep entries 0/4/8/12 black (`$0D`) and entry 13 white for text.
