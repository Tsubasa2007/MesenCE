# EasyMake (recreated)

Tools for making titles for **XDOS**, the hypertext player BBK shipped on disk 004 V2.0 and a
BBK-98 edition of it. BBK's own authoring tool, EasyMake (catalogue disk 281), is lost; this
rebuilds what it did from the player's code. File formats: [FORMAT.md](FORMAT.md) (中文：[格式说明.md](格式说明.md)).
中文使用说明：[使用指南.md](使用指南.md).

The tool is **EasyMake.exe**, a .NET/Avalonia program (this folder); nothing else needs to be
installed to run it. Build it with the .NET 10 SDK: `dotnet publish -c Release -r win-x64` in
this folder gives one self-contained `bin/Release/net10.0/win-x64/publish/EasyMake.exe` (about
23 MB). Run it with no
arguments for the editor, or with the commands below.

## The editor

Start `EasyMake.exe`. It opens the last
project (File menu: new, open, extract a disk). On the left is the project: the script, the
backgrounds and every pack's resources. On the right is an editor for the selected item:

- **Script**: the text with tags highlighted, and a preview of the page under the cursor as
  XDOS lays it out (background, pictures, text boxes, captions, menu areas outlined). Moving
  over the preview shows the position in pixels and in text cells; dragging copies a ready
  `<menuitem ...>` tag, and *Insert menu item* puts it at the cursor.
- **Backgrounds**: the converted picture; replace, add or remove.
- **Resources**: a form per kind - picture, glyph picture (with the background whose colours it
  uses), animation and hover-effect frames (edit, reorder, play), speech phrases (add WAV
  recordings, listen to them as the chip will say them), music (edit the listing, check it,
  listen: the INSTALL.CMD driver runs on a model of the sound chip, so it sounds as the machine
  plays it).
- **Build disk** writes the title onto a copy of the template disk; **Run in MesenCE** (F5)
  builds it and starts MesenCE with it: after a Build disk it rebuilds that file and opens it
  in MesenCE (when the BIOS is a `.nes` in MesenCE's folder, which MesenCE boots disks with),
  otherwise it runs a temporary copy (`%TEMP%\easymake_run`). Set the paths to Mesen.exe, a BBK 1.0 BIOS (`.nes` or
  `.zip`) and the template disk under Settings → Paths (kept in `~/.easymake_gui.json`).
- Settings → Language switches between Chinese and English.
- **Unused** resources and backgrounds are greyed out in the tree. XDOS holds one pack at a time,
  so the editor follows the script's pages and jumps from the first page (with SDOS.RES) to see
  which pack each page can run with, and counts what each page uses (plus what those resources
  refer to). A speech resource lists each phrase's `word=` number and the page that speaks it,
  and flags table slots that point at stale data. `<answer>`/`<easy>` read the pack at fixed
  addresses, so with them in the script some "unused" items may still be drawn (the log says so).
- Double-clicking an item in the tree plays it (music, speech, animations).

## The command line

```
EasyMake new <project>                              start from a demo title
EasyMake extract <disk.img | folder> <project>      unpack an existing title
EasyMake build <project> <template.img> <out.img> [--repack]
EasyMake verify <disk.img | folder>                 unpack + rebuild + compare
EasyMake speech <in.wav> <out.lpc>                  encode a recording for the speech chip
```

`build` writes the title onto a copy of a boot disk that already carries BBGDOS, BBGCDOS,
`INSTALL.CMD`, `MOUSE.CMD`, `XDOS.CMD` and an `AUTOEXEC.BAT` running them — the 004 V2.0 image
works — after removing that disk's own title files.

## A project

```
project.json          {"script": ..., "res": {"SDOS.RES": "res/sdos"}, "backgrounds": {"SKY.GRA": "backgrounds/sky"}}
script.txt            the script, UTF-8 (written to the disk as GBK)
backgrounds/sky.png   a 256x240 picture; with a sky.json beside it, it is taken as exact
                      NES data (see below), without one it is converted
res/sdos/objects.json the resources
res/sdos/*            their files
```

`objects.json` lists objects by name. Hand-written entries can be as short as:

```json
{"objects": {
  "robot": {"kind": "img", "source": "robot.png", "first_sprite": 0},
  "wave":  {"kind": "animate", "first_sprite": 20,
            "frames": [[24, 150, "robot_up.png", 0, 0, 0, 0], [24, 150, "robot.png", 0, 0, 0, 0]]},
  "tune":  {"kind": "music"}
}}
```

- `img` with `source`: any RGB(A) picture up to 256x256, cut into 8x16 sprite cells
  (transparent = alpha, or the top-left pixel's colour). 64 sprites at most, and the NES shows
  only 8 per scanline - wide pictures flicker or lose cells. Give each picture on screen at once
  its own `first_sprite` range; sprite 61 is the mouse pointer.
- `img` with `source` and `"glyphs": true, "palette_from": "<background>"`: the picture as BG
  glyphs (no sprite limits) in that background's colours - a glyph img loads its palette over
  the screen's. Place it at an even `left` column; glyph cells are not transparent, and cells
  that are all black are left out.
- `animate`: frames are `[x, y, image, x origin, y origin, mode, kind]`; an image ending in
  `.png` is converted, and all of an animation's frames share one set of cells.
- `act` (a `menuitem`'s hover effect) with `frames` like an animation's: played at the
  pointer, frame position = pointer + x - x origin. It plays to the end before XDOS reads the
  mouse again, so keep it short. Leaving the item hides the page's sprites too (see below). (XDOS's music and speech options for acts misread their own
  fields, so only the animation is offered.)
- **One sprite palette**: the machine has one, and every img, animation and hover effect loads
  its own over it, so all converted sprite pictures of a pack share one palette, chosen from all
  of them together. `"own_palette": true` (or an explicit `"palette"`) opts an object out.
- `music`: the listing in `<name>.mus` (syntax and events in [FORMAT.md](FORMAT.md); notes like
  `C-4 10`, lengths in hex ticks).
- `speak`: `{"kind": "speak", "phrases": ["hello.wav", "taken.lpc", ...]}` - `word=n` in the
  script says phrase n. `.wav` recordings are encoded at build time (any rate, mono or
  stereo); `.lpc` files are phrases as stored (`extract` writes them, with `.wav` previews,
  named by their `word=` number - `word0/18.lpc` is `word=18`).
  `speech` encodes one file and writes a decoded preview, to judge the result.
- Without an `entries` list, every named object gets a directory entry.

An extracted project is exact: images are indexed PNGs (index = sub-palette × 4 + colour) with
their palettes and cell lists in the JSON, and every object keeps its original offset. Edit
pixels in a paint program that keeps the palette indexes; change colours in the JSON. If any
object changes size, the whole pack is laid out afresh.

## Rules the player imposes

- **Backgrounds**: sub-palette entries 0/4/8/12 are black, and entry 13 is the text colour
  (text is printed white-on-black). Printing switches the 16x16 areas under the text to
  sub-palette 3, so put text on black areas. The converter follows all of this.
- **Hover effects hide sprites**: when the pointer leaves a `menuitem` with `act=`, XDOS hides
  sprites 0-58 (everything but the pointer's), not only the effect's own. On a page whose menu
  has a hover effect, draw pictures as glyphs (or into the background) - a sprite `img` or the
  last frame of an animation vanishes. `build` and the editor warn about such pages.
- **Animations** with `count=` only run while XDOS waits on a full text box; inside
  `<menu>...</menu>` nothing moves (hover `act`s excepted). Use blocking animations (no
  `count`) and draw a still `img` afterwards if the figure should stay.
- **Coordinates**: `static`, `textbox` in 8x16 cells (32 x 15); `menuitem` in pixels. On the
  screen everything XDOS places shows 8 pixels right of its coordinates - text and glyph
  pictures (column c starts at pixel (c + 1) x 8), sprites, and the mouse pointer, so menu
  areas too - while the `.GRA` background does not move. Draw boxes on backgrounds for where
  text really lands; the editor's preview shows it that way and reports XDOS coordinates.
- **Sprite palette**: sub-palette 3 colours the mouse pointer (1 fill, 2 shading, 3 outline),
  and every picture's palette replaces it, so converted sprite pictures use sub-palettes 0-2
  and carry the pointer colours in 3.
- `answer`/`easy` only work with 004's own resource layout.
- A new `desktop` hides all sprites, and one without `backsound` stops the music.

## State (2026-10-06)

- `verify` on 004 V2.0: script and all 17 pictures identical; all three packs identical at
  every byte their objects cover (the rest is stale data the original tool left behind).
- `--repack` of 004 boots and plays in MesenCE; the speech chip receives the same bytes.
- The `new` demo (converted picture, sprite, two-frame animation, written tune, two pages and a
  menu) runs in MesenCE: navigation by mouse, music at the written pitches.
- `verify` on the BBK-98 edition of disk 004 (two dumps): passes the same way.
- Glyph pictures and hover effects render in MesenCE; with the shared sprite palette the
  other sprites keep their colours while an effect plays.
- Speech: re-encoding decoded BBK phrases recovers their voicing on 95-99% of frames, the same
  pitch, and unbiased energy; an encoded English sentence plays on MesenCE's speech chip with
  the level envelope of the decoder's preview (correlation 0.82), every stream byte sent.
- Not done: authoring for `wait str=` type-in checks is plain script, nothing to convert;
  mode/kind animation transforms are passed through as numbers.
