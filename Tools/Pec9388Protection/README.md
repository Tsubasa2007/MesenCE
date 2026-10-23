# PEC-9388 boot encryption and protection chip

The Pyramid (金字塔) PEC-9388 from Dongda (东达), 1990, doesn't keep its post-menu boot code in the
ROM as plain bytes. At power-on it rebuilds 1,280 bytes of code at `$6000-$64FF` by XORing
together windows of the ROM, with the window addresses encrypted by an 8-byte key. On the
protected main ROM, a loader computes that key at boot from three things:

- checksums of ROM and RAM;
- the answers of a protection chip on the main board;
- the result of an undocumented 6502 opcode, which differs between CPUs.

The key is stored nowhere: not in the ROM, and not in the chip.

This page describes the whole scheme as measured on a real machine. It is emulated in
`Core/NES/Mappers/Unlicensed/DongdaPec586.h` (mapper 257) and `Core/NES/NesCpu.h` (the LXA constant).

## The two main ROMs

The main ROM sits on the machine's 汉字卡 (Chinese character card); the machine doesn't boot without it.
Two revisions are known. Their circuit boards are the same, and they differ in 2,209 bytes, all in
the boot windows.

| Card | PRG CRC32 (256KB) | `$8009-$800F` | Key |
|---|---|---|---|
| CKSBASIC (mask ROM marked "CKSBASIC PEC8801") | `F1371ED2` | `PYRAMID` | computed by the protected loader |
| T1037 | `A385B194` | not `PYRAMID` | 0 |

The PEC-586's main ROM has the same header as T1037 and also boots with key 0.

```
            $8000 $8001 ...                  $8008 $8009 ...
CKSBASIC:   99    98 B7 E4 4F E9 31 44 72    50 59 52 41 4D 49 44   ("PYRAMID")
T1037:      FF    71 E0 11 E1 14 E2 88 B0    FF 4C 03 A0 6C FC FF
```

The protection chip is on the main board, not on the card. Reads with slot 1 empty gave the same
answers as with either card inserted.

## Layer 1: the boot code is XORed together

Reset goes through `$AC03` to `$AC1B`, which compares `$8009-$800F` with `PYRAMID`:

- **No match** (T1037, PEC-586): the key at `$08-$0F` is 0, and boot goes straight on to `$AC34`.
- **Match** (CKSBASIC): `JMP $8010` runs the protected loader (layer 2). It leaves the key at `$08-$0F`
  and returns through `$A003` to `$AC34`.

At `$AC34`:

1. `$00-$07` = `$08-$0F` XOR `$8001-$8008`. This gives four 16-bit pointers, p0-p3.
2. `$0030-$07FF` is cleared.
3. `$6000-$64FF` (5 pages) = W(p1) XOR W(p0) XOR W(p3) XOR W(p2), each pointer stepping a page at a time.
4. At `$AF30`, `$5000` = (p0 high + 5) XOR `$A5`. A fifth window is XORed in, at
   ((p0 high + 5) XOR `$76`) << 8 | (p0 low XOR `$F9`).
5. `JMP $6000`.

With key 0, T1037's pointers are its header bytes: `$E071, $E111, $E214, $B088`. The fifth window
is `$9388` in bank 0 of the 8KB mode; the address spells the model number.

CKSBASIC's key is exactly what turns its header into the same four pointers:

```
key = CKSBASIC header XOR T1037 header = E9 57 F5 AE FD D3 CC C2
```

All five windows fall in bytes the two ROMs share, so both decrypt to the same `$6000` code. The
manufacturer gave the same software an encrypted header plus a loader that computes the key.

## Layer 2: the protected loader at `$8010` (CKSBASIC only)

In order:

1. Write `$5000` once, clear `$08-$0F`, enable NMI, and wait until the NMI counter `$01` reaches `$AC`.
2. **Feed:** the 4,096 bytes at `$B000-$BFFF` are written to `$5400+Y`, 16 runs of 256.
3. Fill RAM with pseudo-random data: `$0010-$07FF` and the WRAM at `$6000-$7FFF`.
4. **Key bytes 0-6:** for each X = 0..6, checksum page after page at `$8150`. Each page mixes the key
   byte with a ROM page (`($04),Y`) and a RAM page (`($06),Y`) by EOR / ROR / ROL, and writes the
   intermediate result back over the RAM page. Even X uses the zero page, the stack page and
   `$0200-$07FF` (8 pages); odd X uses `$6000-$7FFF` (32 pages); 128 pages in all.
   After **every page**:
   ```
   STA $5400   ; key byte AND #$90 - only ever 00, 10, 80 or 90
   LDA $5500   ; read straight back
   AND #$07
   EOR key byte, store it back
   ```
   Every 4 pages it also writes `$93` and then `$88` to `$5400` without reading, and folds the NMI
   counter in with `SRE $01`, `SRE $01F9` and `RLA $01`.
5. **Key byte 7 (`$0F`):** `JSR $A018` (`$A354`) XORs together ROM bytes from banks of the 8KB mode,
   picked by the key so far.
6. **`LXA #$07`** (undocumented, opcode `$AB`). The result goes to X and sets the length of a
   `RRA $08,X` chain over the key; the key is then XORed with one more byte.
7. Check `PYRAMID` again, then `JMP $A003`, which goes on to `$AC34`.

While the loader runs, the NMI handler is in a special mode (`$00 = $FF`, `$03 = $AA`). It stores
the high byte of the interrupted address at `$01F9`. That byte must be `$80` or `$81`, otherwise it
jumps to `$ABD2` and aborts. It then increments `$01`. So the computation is tied to the frame, and
stepping through it with a debugger breaks it. In practice the result is stable: other frame lengths
and CPU/PPU phases give the same key.

Since every checksummed byte feeds the key, any patch to the ROM (for example, NOPing out a check)
changes the key, and layer 1 decrypts garbage.

## The protection chip (main board: write `$54xx`, read `$5500`)

Older emulators treat the chip as a fixed table: `$10 → 1`, `$70 → $20`, `$80 → 4`, `$F0 → 2`. That
table is only what the chip answers to the boot code's check order. The chip itself is a small
state machine that looks at the previous writes.

Only **bits 7, 6 and 4** of the written value matter; the low nibble is ignored. Bits 2-0 of a read
give the answer, valid when read right after the write, as the boot code does.

| State | Write | Answer | Next state |
|---|---|---|---|
| rest | `1x` / `3x` (bit 4 = 1, bits 7-6 = 0) | 1 | A |
| rest | anything else | 0 | rest |
| A | `1x` / `3x` | 1 | A |
| A | `Dx` / `Fx` (bits 7, 6, 4 = 1) | **2** | B |
| A | anything else | 0 | rest |
| B | `8x` / `Ax` (bit 7 = 1, bits 6 and 4 = 0) | **4** | rest |
| B | anything else, including `1x` / `3x` | 0 | rest |

Two more bits:

- **Bit 3** is a latch. A `$5400` write while `$5000` bit 4 is set sets it for `1x` / `3x` and clears
  it for anything else. It is 1 at power-on. The boot code's "`$5000 = $10`, `$5400 = $10`" and
  "`$5000 = $70`, `$5400 = $70`" steps exercise it.
- **Bit 5** is 1 at power-on. A `Cx` write right after the answer 4 clears it. The boot check wants
  it set each time it starts over, and that check begins with a `$5700` write, so the emulation
  takes `$5700` as what sets it again. This step is **not measured**.

Bits 7, 6 and 4 of a read are open bus.

The loader only writes `00`, `10`, `80`, `90`, `93` and `88`, never `Dx` or `Fx`. So while the key is
being computed, the chip only ever answers "1 after `1x`, else 0". The full three-step sequence
matters only to the checks in the decrypted `$6000` code.

## The CPU: `LXA #$07` gives 6

`LXA #imm` on an NMOS 6502 computes `A = X = (A OR magic) AND imm`. The magic constant differs from
chip to chip and is known to be unstable. The loader reaches `LXA` at `$80F8` with A = 0:

- an NES 2A03, or an emulator using magic `$FF`, gives X = 7;
- **the PEC-9388's CPU gives X = 6**. That fits the common `$EE` constant (low 3 bits `110`).

The loader produces `E9 57 F5 AE FD D3 CC C2` only when **both** the chip state machine and X = 6
hold.

## What the decrypted `$6000` code checks

The PEC-586 main ROM contains a copy of the same code with 337 bytes changed. Every difference
replaces a check or a decoding step with NOPs:

| Address | Original | PEC-586 copy |
|---|---|---|
| `$60D9`, `$60F2`, `$611F` | three chip checks; a failure goes to `$63B8` (error, back to the menu) | NOP |
| `$628E-$62B8` | slot card header pointer = `$FFFE ^ $D9 ^ $FFFC`, `$FFFF ^ $CA ^ $FFFD`; header bytes XOR `$FFFC ^ $FFFD` | NOP |
| `$62CB-$62FC` | card checksum over `$8000-$FFFF`: even bytes XORed, odd bytes added. It must equal the card's `$FFFE/$FFFF`, so the card's IRQ vector is its checksum | NOP |
| `$6303`, `$6371` | XOR with a partner block while copying; patterns XORed with `$AF` too | NOP |
| `$6400-$64FF` | unused data | the PEC-586's own start-up code |

The chip check starts at `$6090`. After some set-up writes, it writes `10 F0 40 70 10 F0 80 C0` to
`$5400`, checking each answer; the final `C0` is the write that clears bit 5.

So the PEC-586 copy needs no protection chip and cannot load encrypted slot cards.

## How it was measured

Small test programs were loaded into the WRAM at `$6000` and run on the PEC-9388's own CPU. Each
test does what the boot code does: a write to `$5400`, then a read of `$5500` in the very next
instruction, for sequences of values. The results were drawn on screen and photographed.

Four tests produced about 1,300 values. Run in MesenCE with this implementation, they match the
photos exactly.

## In MesenCE

- `DongdaPec586::ChipWrite()` is the state machine above, plus the bit 3 and bit 5 latches. They are
  saved in save states, reset at power-on, and kept on RESET.
- `NesCpu::SetLxaMagic()` makes the LXA constant per machine. The default `$FF` keeps the old
  behaviour for everything else; both PEC-9388 main ROMs (`F1371ED2`, `A385B194`) set `$EE`.
- The unmodified CKSBASIC ROM boots on its own. No key is inserted from outside and no ROM patch is
  needed. The PEC-586, PEC-686F, PEC-9588 and T1037 behave as they did before.

## How common is this?

Each ingredient appears elsewhere:

- XOR-scrambled boot code is common on Chinese and Taiwanese learning machines and cartridges.
- Cartridge protection chips that answer reads were used by several Taiwanese makers.
- Deriving a key from checksums is an old anti-tamper technique.

What is unusual here:

- **A chip with state.** Its answer depends on earlier writes, so no fixed table reproduces it.
- **A key that depends on LXA.** The key comes out right only on CPUs with the matching constant.
  This keeps NES-accurate emulators and clone boards out, whether that was intended or not.
- **The NMI position check,** a light anti-debugging measure.

Pyramid seems to have dropped the scheme: T1037 and the PEC-586 take the key-0 path.

## Open questions

- What sets bit 5 again after it is cleared. The emulation assumes the `$5700` write.
- The next state after `1x` / `3x` in state B. It is taken as rest; the loader never does this.
- Whether only writes to `$5400` itself count during the feed, or the whole `$5401-$54FF` range does.
  Both variants give the right key.
