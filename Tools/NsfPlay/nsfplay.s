; NSFPLAY.CMD - NSF music player for the BBK learning machine (BBGDOS, BBK 1.0 BIOS)
;
; Plays the NES sound files (.NSF) in the current directory, and .NSB files made from
; bank-switching or expansion-sound NSFs by nsf2bbk.py. The 2A03 sound channels of the
; machine's CPU play the music; there is no expansion sound hardware.
;
; How it works:
; - The file goes into free 16K DRAM pages (DosInquirePages), laid out as the NSF's 4K
;   banks one after another, so 8K page k holds banks 2k and 2k+1. A window ($8000,
;   $A000, $C000, $E000) showing an aligned pair maps that page directly; any other pair
;   gets a copied 8K page of its own.
; - For playback the program takes the machine over: interrupts off, the PPU's NMI off,
;   and the code below RESIDENT copied to $5000 (its variables at $4E00). The NSF gets
;   $0000-$07FF, $6000-$7FFF
;   and all of $8000-$FFFF, mapped with the 8K page registers $FF14/$FF1C/$FF24/$FF2C.
;   What it overwrites is saved in a DRAM page and put back before the BIOS runs again.
; - PLAY is timed by polling the APU frame-counter flag ($4015 bit 6, 4-step mode, every
;   29830 CPU cycles), or by the vblank flag for 50 Hz tunes and machines without it.
; - Keys and joypad are read directly ($4016/$4017); an action happens when its key is
;   released, so the BIOS never sees the key afterwards.

	.setcpu "6502"
	.import __RESIDENT_LOAD__, __RESIDENT_RUN__, __RESIDENT_SIZE__

BIOS	= $5800			; BIOS calls, A = function
FILEIO	= $5803			; file calls, X|Y = file channel
DOS	= $580C			; DOS calls, A = function

ARG_FE	= $FE			; BIOS argument bytes
ARG_FF	= $FF

Joystick1Key	= $5FFC		; BIOS variable: joypad 1 buttons held (R L D U St Se B A)
; Floppy motor (same in the BBK 1.0 and BBK-98 BIOS): the NMI counts MotorCountdown down
; and at 0 writes $0C to MotorState and the controller's $FF90 (motors off)
MotorCountdown	= $5FA0
MotorState	= $5FA3		; copy of $FF90: D5/D4 = motors

BiosSetPage		= $01
BiosGetPage		= $02
BiosWaitVSynCnt		= $0E
BiosSetCursor		= $1C
BiosGetPagedChar	= $20
BiosWaitDisplay		= $22

DosProgramReturn	= $00
DosDisplayChar		= $02
DosInquirePages		= $03
DosReadKeyNoEcho	= $07
DosInitialScreen	= $09
DosGetSetDispMode	= $0B
DosCheckKeyBoard	= $0E

FileRead		= $02
FileClose		= $05
FileNext		= $07
FilePathOpen		= $14
FilePathFirst		= $16

CH_CMD	= 0			; file channel (48 bytes)
CH_PAGE	= 2
CH_BUF	= 3
CH_LEN	= 5
CH_POS	= 7
FF_ATTR	= 0			; record found by FilePathFirst/FileNext
FF_NAME	= 9
RECSIZE	= 20

PPUCTRL		= $2000
PPUSTATUS	= $2002
PPUADDR		= $2006
PPUDATA		= $2007
PPUCTRL_SHADOW	= $6000		; the memory controller copies $2000 writes here
APUSTATUS	= $4015
APUFRAME	= $4017
PAD		= $4016
KEYS		= $4017

REG_PAGE16	= $FF04		; 16K page at $8000-$BFFF (the value BiosSetPage writes)
REG_W0		= $FF14		; 8K DRAM page at $8000
REG_W1		= $FF1C		; $A000
REG_W2		= $FF24		; $C000
REG_W3		= $FF2C		; $E000
DOS_W2		= $FE		; what the BIOS keeps in $FF24/$FF2C (page $FF)
DOS_W3		= $FF

LASTPAGE	= $FD		; $FE is the fixed $4000-$7FFF window, $FF the DOS's $C000-$FFFF
MAXFILES	= 100
MAXEXTRA	= 32		; copied 8K pages for unaligned bank pairs
SINK		= $5700		; expansion-sound writes patched by nsf2bbk.py land here

; Microseconds of NTSC time per tick: 29830 cycles of the frame counter and 35464 cycles
; per frame, both counted at the NTSC clock (the machine's slower clock lowers the pitch by
; the same amount, so a tune stays in step with itself)
US_FRAMEFLAG	= 16667
US_VBLANK	= 19815
; The same ticks in real time, at the machine's 1.7734475 MHz CPU clock
REAL_FRAMEFLAG	= 16820
REAL_VBLANK	= 19997

; Playback modes
MODE_FRAME	= 0		; one PLAY per frame-counter flag
MODE_FRAMEACC	= 1		; frame-counter flags, PLAY by elapsed time
MODE_VBLANK	= 2		; one PLAY per vblank (50 Hz tunes)
MODE_VBLANKACC	= 3		; vblanks, PLAY by elapsed time

; Actions
ACT_NEXT	= 1
ACT_PREV	= 2
ACT_NEXTFILE	= 3
ACT_PREVFILE	= 4
ACT_QUIT	= 5
ACT_PAUSE	= 6

; Parameter block, copied into the resident part for each track
P_SAVEPAGE	= 0
P_SONG		= 1
P_REGION	= 2
P_MODE		= 3
P_INIT		= 4
P_PLAY		= 6
P_SPEED		= 8
P_IMGPAIRS	= 10		; 8K pages of the bank image
P_IMGPHYS	= 11		; 8K DRAM page number of image page 0
P_NEXTRA	= 12
P_SHADOW	= 13		; 4K bank in each slot $8000-$F000 (8)
P_EXLO		= 21		; extra pages: first bank (32)
P_EXHI		= P_EXLO + MAXEXTRA	; second bank (32)
P_EXPHYS	= P_EXHI + MAXEXTRA	; 8K DRAM page (32)
P_SIZE		= P_EXPHYS + MAXEXTRA

; Save page layout ($8000-$BFFF while saving)
SAVE_RAM	= $80		; $0000-$07FF
SAVE_WRAM	= $88		; $6000-$7FFF
SAVE_LOW	= $A8		; $4E00-$57FF

	.segment "LOADADDR"
	.word Start

	.segment "CODE"

Start:
	ldx #0				; English text mode (30 rows), no banded screen
	lda #DosGetSetDispMode
	jsr DOS
	sta oldMode
	beq :+
	ldx #1
	ldy #0
	lda #DosGetSetDispMode
	jsr DOS
:	lda #BiosGetPage
	jsr BIOS
	sta dosPage
	ldx #0				; X = 0: the first free page
	lda #DosInquirePages
	jsr DOS
	sta firstFree

	jsr FindArgument		; before anything grows the stack over $0100
	jsr ScanFiles
	lda fileCount
	bne :+
	jsr ClearScreen
	ldx #3
	lda #<msgNoFiles
	ldy #>msgNoFiles
	jsr PrintLine
	jsr WaitKey
	jmp Quit
:	jsr MatchArgument

LoadLoop:
	jsr LoadFile
	bcc Loaded
	jsr ShowError
	jsr WaitKey
	cmp #27
	bne :+
	jmp Quit
:	jsr NextFile
	jmp LoadLoop

Loaded:
	jsr DrawScreen

PlayLoop:
	jsr DrawTrack
	jsr Session
	cmp #ACT_NEXT
	bne @notNext
	inc song
	lda song
	cmp songs
	bcc PlayLoop
	lda #0
	sta song
	beq PlayLoop
@notNext:
	cmp #ACT_PREV
	bne @notPrev
	lda song
	bne :+
	lda songs
	sta song
:	dec song
	jmp PlayLoop
@notPrev:
	cmp #ACT_NEXTFILE
	bne @notNextFile
	jsr NextFile
	jmp LoadLoop
@notNextFile:
	cmp #ACT_PREVFILE
	bne Quit
	lda index
	bne :+
	lda fileCount
	sta index
:	dec index
	jmp LoadLoop

Quit:
	ldx dosPage
	lda #BiosSetPage
	jsr BIOS
	lda oldMode
	beq @flush
	ldx #1
	ldy oldMode
	lda #DosGetSetDispMode
	jsr DOS
@flush:
	lda #DosCheckKeyBoard		; drop keys typed while loading
	jsr DOS
	beq @done
	lda #DosReadKeyNoEcho
	jsr DOS
	jmp @flush
@done:
	ldx #0				; exit code 0, reinitialise the screen
	ldy #1
	lda #DosProgramReturn
	jmp DOS

NextFile:
	inc index
	lda index
	cmp fileCount
	bcc :+
	lda #0
	sta index
:	rts

;----------------------------------------------------------------------------
; Playing one track

; Plays track `song` until an action; returns it in A
Session:
	lda song
	sta params+P_SONG
	lda MotorState			; the drive motor: the BIOS stops it from its NMI, which
	and #$30			; is about to stop running, so have it do that now
	beq :+
	lda #1
	sta MotorCountdown
	ldx #3
	lda #BiosWaitVSynCnt
	jsr BIOS
:	lda #BiosWaitDisplay		; the BIOS draws through a queue its NMI drains
	jsr BIOS
	lda #BiosGetPage
	jsr BIOS
	sta curPage
	sei
	lda PPUCTRL_SHADOW		; NMI off; ppuSave is saved and restored with the program
	sta ppuSave
	and #$7F
	sta PPUCTRL
	lda params+P_SAVEPAGE
	sta REG_PAGE16
	lda #$4E			; whatever lives at $4E00-$57FF
	ldx #SAVE_LOW
	ldy #10
	jsr CopyPages
	ldx #0				; the resident part
@res:
	lda __RESIDENT_LOAD__,x
	sta __RESIDENT_RUN__,x
	lda __RESIDENT_LOAD__+$100,x
	sta __RESIDENT_RUN__+$100,x
	lda __RESIDENT_LOAD__+$200,x
	sta __RESIDENT_RUN__+$200,x
	lda __RESIDENT_LOAD__+$300,x
	sta __RESIDENT_RUN__+$300,x
	lda __RESIDENT_LOAD__+$400,x
	sta __RESIDENT_RUN__+$400,x
	lda __RESIDENT_LOAD__+$500,x
	sta __RESIDENT_RUN__+$500,x
	lda __RESIDENT_LOAD__+$600,x
	sta __RESIDENT_RUN__+$600,x
	inx
	bne @res
	ldx #P_SIZE-1
@par:
	lda params,x
	sta r_params,x
	dex
	bpl @par
	jsr R_Enter
	sta action			; everything from $6000 up is as it was before the call
	lda params+P_SAVEPAGE
	sta REG_PAGE16
	lda #SAVE_LOW
	ldx #$4E
	ldy #10
	jsr CopyPages
	lda ppuSave
	sta PPUCTRL
	cli
	ldx curPage
	lda #BiosSetPage
	jsr BIOS
	lda action
	rts

	.assert __RESIDENT_SIZE__ <= SINK - $5000, error, "resident part runs into the sink"

; Copies Y pages of 256 bytes from page A to page X
CopyPages:
	sta @src+2
	stx @dst+2
	ldx #0
@loop:
@src:	lda $FF00,x
@dst:	sta $FF00,x
	inx
	bne @loop
	inc @src+2
	inc @dst+2
	dey
	bne @loop
	rts

;----------------------------------------------------------------------------
; Loading

; Loads list entry `index`. Carry set on failure, with errMsg set.
LoadFile:
	jsr BuildPath
	jsr ClearScreen
	ldx #3
	lda #<msgLoading
	ldy #>msgLoading
	jsr PrintLine
	ldx #3
	ldy #9
	jsr SetCursor
	lda #<path
	ldx #>path
	ldy #12
	jsr PrintText

	ldx #47
	lda #0
@clr:
	sta fchan,x
	dex
	bpl @clr
	lda #FilePathOpen
	sta fchan+CH_CMD
	lda #<path
	sta fchan+CH_BUF
	lda #>path
	sta fchan+CH_BUF+1
	jsr CallFile
	bcc :+
	lda #<errOpen
	ldx #>errOpen
	jmp Fail
:	lda #0
	sta fchan+CH_POS
	sta fchan+CH_POS+1
	sta fchan+CH_POS+2
	sta fchan+CH_POS+3

	lda #<header
	ldx #>header
	ldy #128
	jsr ReadSmall
	bcs @badFormat
	ldx #4
@magic:
	lda header,x
	cmp nsfMagic,x
	bne @badFormat
	dex
	bpl @magic

	lda #0
	sta isNsb
	ldx #10				; extension NSB?
@ext:
	lda entry,x
	cmp extNsb-8,x
	bne @notNsb
	dex
	cpx #8
	bcs @ext
	lda #<block
	ldx #>block
	ldy #128
	jsr ReadSmall
	bcs @badFormat
	ldx #4
@bmagic:
	lda block,x
	cmp nsbMagic,x
	bne @badFormat
	dex
	bpl @bmagic
	inc isNsb
@notNsb:
	jmp ParseHeader

@badFormat:
	jsr CloseFile
	lda #<errFormat
	ldx #>errFormat
	jmp Fail

; Sets errMsg = A/X, closes nothing, carry set
Fail:
	sta errMsg
	stx errMsg+1
	sec
	rts

FailClose:
	sta errMsg
	stx errMsg+1
	jsr CloseFile
	sec
	rts

ParseHeader:
	lda header+6			; songs
	bne :+
	lda #<errFormat
	ldx #>errFormat
	jmp FailClose
:	sta songs
	ldx header+7			; starting song, 1-based
	dex
	cpx songs
	bcc :+
	ldx #0
:	stx song
	lda header+9			; load address must be $8000 or above
	bmi :+
	lda #<errLoad
	ldx #>errLoad
	jmp FailClose
:	lda header+$0A
	sta params+P_INIT
	lda header+$0B
	sta params+P_INIT+1
	lda header+$0C
	sta params+P_PLAY
	lda header+$0D
	sta params+P_PLAY+1
	lda isNsb			; expansion sound needs the converter's patches
	bne :+
	lda header+$7B
	beq :+
	lda #<errExpansion
	ldx #>errExpansion
	jmp FailClose
:
	; banked when any initial bank is non-zero
	lda #0
	sta banked
	ldx #7
@banks:
	lda header+$70,x
	sta params+P_SHADOW,x
	beq :+
	inc banked
:	dex
	bpl @banks
	lda banked
	beq @plain
	lda #0				; pad = load & $0FFF
	sta pad+2
	lda header+8
	sta pad
	lda header+9
	and #$0F
	sta pad+1
	jmp @region
@plain:
	ldx #7				; banks 0-7 at $8000-$F000
:	txa
	sta params+P_SHADOW,x
	dex
	bpl :-
	lda #0				; pad = load - $8000
	sta pad+2
	lda header+8
	sta pad
	lda header+9
	and #$7F
	sta pad+1

@region:
	; NTSC unless the tune is PAL only
	lda header+$7A
	and #3
	cmp #1
	beq @pal
	lda #0
	sta params+P_REGION
	lda header+$6E
	ldx header+$6F
	bne :+
	cmp #0
	bne :+
	lda #<16639
	ldx #>16639
:	sta params+P_SPEED
	stx params+P_SPEED+1
	; 16300-17100 us: one PLAY per frame-counter flag
	ldy #MODE_FRAME
	cpx #>16300
	bcc @accF
	bne :+
	cmp #<16300
	bcc @accF
:	cpx #>17100
	bcc @setMode
	bne @accF
	cmp #<17100
	bcc @setMode
@accF:
	ldy #MODE_FRAMEACC
	jmp @setMode
@pal:
	lda #1
	sta params+P_REGION
	lda header+$78
	ldx header+$79
	bne :+
	cmp #0
	bne :+
	lda #<19997
	ldx #>19997
:	sta params+P_SPEED
	stx params+P_SPEED+1
	; 19500-20500 us: one PLAY per vblank
	ldy #MODE_VBLANK
	cpx #>19500
	bcc @accV
	bne :+
	cmp #<19500
	bcc @accV
:	cpx #>20500
	bcc @setMode
	bne @accV
	cmp #<20500
	bcc @setMode
@accV:
	ldy #MODE_VBLANKACC
@setMode:
	sty params+P_MODE

	; memory: the save page, then the image
	lda firstFree
	sta params+P_SAVEPAGE
	clc
	adc #1
	sta imgBase
	cmp #LASTPAGE+1
	bcc :+
	lda #<errMemory
	ldx #>errMemory
	jmp FailClose
:
	jsr ReadData
	bcc :+
	jmp FailClose
:	jsr CloseFile

	; banks in the image
	lda imgEnd			; nbanks = (end + $FFF) >> 12
	clc
	adc #$FF
	lda imgEnd+1
	adc #$0F
	sta work
	lda imgEnd+2
	adc #0
	sta work+1
	lda work			; >> 4
	lsr work+1
	ror a
	lsr work+1
	ror a
	lsr work+1
	ror a
	lsr work+1
	ror a
	ldx work+1
	beq :+
	lda #<errMemory
	ldx #>errMemory
	jmp Fail
:	sta nbanks
	cmp #0
	bne :+
	lda #<errFormat
	ldx #>errFormat
	jmp Fail
:	lda banked
	bne @sized
	lda nbanks			; a plain NSF fits in $8000-$FFFF
	cmp #9
	bcc :+
	lda #<errLoad
	ldx #>errLoad
	jmp Fail
:	lda #8
	sta nbanks
@sized:
	lda nbanks			; pairs = (nbanks + 1) / 2
	clc
	adc #1
	lsr a
	sta params+P_IMGPAIRS
	lda imgBase			; 8K page number of the image's first page
	asl a
	sta params+P_IMGPHYS
	lda nbanks			; image 16K pages = (nbanks + 3) / 4
	clc
	adc #3
	lsr a
	lsr a
	clc
	adc imgBase
	sta extraBase

	; zero what the file does not cover: [0, pad) and [end, pairs * 8K)
	lda #0
	sta zFrom
	sta zFrom+1
	sta zFrom+2
	lda pad
	sta zTo
	lda pad+1
	sta zTo+1
	lda pad+2
	sta zTo+2
	jsr ZeroRange
	lda imgEnd
	sta zFrom
	lda imgEnd+1
	sta zFrom+1
	lda imgEnd+2
	sta zFrom+2
	lda #0
	sta zTo
	lda params+P_IMGPAIRS		; * $2000
	asl a
	asl a
	asl a
	asl a
	asl a
	sta zTo+1
	lda params+P_IMGPAIRS
	lsr a
	lsr a
	lsr a
	sta zTo+2
	jsr ZeroRange

	lda isNsb
	bne @nsbPairs
	lda banked
	beq :+
	jsr ScanBankStores
	bcc :+
	lda #<errBanking
	ldx #>errBanking
	jmp Fail
:	jsr StaticPairs
	jmp @extra
@nsbPairs:
	jsr BlockPairs
	bcc @extra
	lda #<errFormat
	ldx #>errFormat
	jmp Fail
@extra:
	jsr BuildExtraPages
	bcc :+
	lda #<errMemory
	ldx #>errMemory
	jmp Fail
:	clc
	rts

; Reads the program data into the image from offset pad. Sets imgEnd. Carry set (with
; A/X = message) when it does not fit.
ReadData:
	lda pad
	sta imgEnd
	lda pad+1
	sta imgEnd+1
	lda pad+2
	sta imgEnd+2
@chunk:
	lda imgEnd			; page and address of imgEnd
	sta aOff
	lda imgEnd+1
	sta aOff+1
	lda imgEnd+2
	sta aOff+2
	jsr ImageAddr
	lda zPage
	cmp #LASTPAGE+1
	bcc :+
	lda #<errMemory
	ldx #>errMemory
	sec
	rts
:	sta fchan+CH_PAGE
	lda zPtr
	sta fchan+CH_BUF
	lda zPtr+1
	sta fchan+CH_BUF+1
	lda imgEnd			; up to the next 4K boundary
	eor #$FF
	clc
	adc #1
	sta want
	lda imgEnd+1
	and #$0F
	eor #$0F
	adc #0
	sta want+1
	lda want
	sta fchan+CH_LEN
	lda want+1
	sta fchan+CH_LEN+1
	lda #FileRead
	sta fchan+CH_CMD
	jsr CallFile
	bcs @eof
	lda fchan+CH_LEN
	clc
	adc imgEnd
	sta imgEnd
	lda fchan+CH_LEN+1
	adc imgEnd+1
	sta imgEnd+1
	bcc :+
	inc imgEnd+2
:	lda fchan+CH_LEN
	cmp want
	bne @eof
	lda fchan+CH_LEN+1
	cmp want+1
	bne @eof
	jmp @chunk
@eof:
	clc
	rts

; zPage/zPtr = the 16K page and $8000-$BFFF address of image offset aOff
ImageAddr:
	lda aOff+1			; page = imgBase + (offset >> 14)
	asl a
	lda aOff+2
	rol a
	sta zPage
	lda aOff+1
	asl a
	asl a
	lda zPage
	rol a
	clc
	adc imgBase
	sta zPage
	lda aOff
	sta zPtr
	lda aOff+1
	and #$3F
	ora #$80
	sta zPtr+1
	rts

; Zeroes image offsets [zFrom, zTo)
ZeroRange:
	lda zFrom
	sta aOff
	lda zFrom+1
	sta aOff+1
	lda zFrom+2
	sta aOff+2
	jsr ImageAddr
	jsr MapZPage
@loop:
	lda zFrom			; done when zFrom >= zTo
	cmp zTo
	lda zFrom+1
	sbc zTo+1
	lda zFrom+2
	sbc zTo+2
	bcs @done
	lda zPtr
	sta @st+1
	lda zPtr+1
	sta @st+2
	lda #0
@st:	sta $FFFF
	inc zFrom
	bne :+
	inc zFrom+1
	bne :+
	inc zFrom+2
:	inc zPtr
	bne @loop
	inc zPtr+1
	lda zPtr+1
	cmp #$C0
	bne @loop
	lda #$80
	sta zPtr+1
	inc zPage
	jsr MapZPage
	jmp @loop
@done:
	rts

MapZPage:
	ldx zPage
	lda #BiosSetPage
	jmp BIOS

; Carry set when the image holds a store to a bank register ($5FF8-$5FFF)
ScanBankStores:
	lda #0
	sta work
@page:
	lda work
	asl a
	asl a
	cmp nbanks
	bcs @none
	lda work
	clc
	adc imgBase
	tax
	lda #BiosSetPage
	jsr BIOS
	lda #$80
	sta @rd+2
	sta @lo+2
	sta @hi+2
@block:
	ldx #0
@rd:	lda $FF00,x
	cmp #$8D			; STA abs
	beq @cand
	cmp #$8E			; STX abs
	beq @cand
	cmp #$8C			; STY abs
	beq @cand
	cmp #$9D			; STA abs,X
	beq @cand
	cmp #$99			; STA abs,Y
	beq @cand
@next:
	inx
	bne @rd
	inc @rd+2
	inc @lo+2
	inc @hi+2
	lda @rd+2
	cmp #$C0
	bne @block
	inc work
	jmp @page
@cand:
@hi:	lda $FF02,x
	cmp #$5F
	bne @next
@lo:	lda $FF01,x
	cmp #$F8
	bcc @next
	sec
	rts
@none:
	clc
	rts

; Extra pages for the initial pairs a plain NSF needs
StaticPairs:
	lda #0
	sta params+P_NEXTRA
	ldx #0
@win:
	stx work
	lda params+P_SHADOW,x
	sta pairLo
	lda params+P_SHADOW+1,x
	sta pairHi
	jsr AddPair
	ldx work
	inx
	inx
	cpx #8
	bcc @win
	rts

; Adds pairLo/pairHi to the extra list unless the image already holds it
AddPair:
	lda pairLo
	lsr a
	bcs @need
	cmp params+P_IMGPAIRS
	bcs @need
	ldx pairLo
	inx
	cpx pairHi
	beq @have
@need:
	ldy params+P_NEXTRA
@find:
	dey
	bmi @add
	lda params+P_EXLO,y
	cmp pairLo
	bne @find
	lda params+P_EXHI,y
	cmp pairHi
	bne @find
@have:
	rts
@add:
	ldy params+P_NEXTRA
	cpy #MAXEXTRA
	bcs @have
	lda pairLo
	sta params+P_EXLO,y
	lda pairHi
	sta params+P_EXHI,y
	inc params+P_NEXTRA
	rts

; The pairs listed in an NSB's block. Carry set if the list is too long.
BlockPairs:
	lda #0
	sta params+P_NEXTRA
	lda block+5
	cmp #MAXEXTRA+1
	bcs @bad
	sta work
	ldx #0
@loop:
	cpx work
	bcs @done
	lda block+8,x
	sta params+P_EXLO,x
	lda block+8+MAXEXTRA,x
	sta params+P_EXHI,x
	inx
	stx params+P_NEXTRA
	jmp @loop
@done:
	clc
	rts
@bad:
	sec
	rts

; Copies the banks of each extra pair into its own 8K page. Carry set if out of memory.
BuildExtraPages:
	lda params+P_NEXTRA		; last page used = extraBase + (n - 1) / 2
	beq @ok
	sec
	sbc #1
	lsr a
	clc
	adc extraBase
	cmp #LASTPAGE+1
	bcc :+
	rts
:	lda #0
	sta exIndex
@loop:
	ldx exIndex
	cpx params+P_NEXTRA
	bcs @ok
	lda extraBase			; physical 8K page = extraBase * 2 + index
	asl a
	clc
	adc exIndex
	sta params+P_EXPHYS,x
	lda params+P_EXLO,x
	ldy #0
	jsr CopyBank
	ldx exIndex
	lda params+P_EXHI,x
	ldy #$10
	jsr CopyBank
	inc exIndex
	jmp @loop
@ok:
	clc
	rts

; Copies 4K bank A into extra page exIndex at offset Y*256 (0 or $1000), or zeroes it if
; the image has no such bank
CopyBank:
	sty cbDstOff
	sta cbBank
	lda exIndex			; destination page and address
	lsr a
	clc
	adc extraBase
	sta cbDstPage
	lda exIndex			; (index & 1) * $20 in the high byte
	and #1
	beq :+
	lda #$20
:	ora #$80
	ora cbDstOff
	sta cbDst
	lda cbBank			; source: imgBase + bank/4, $8000 + (bank & 3) * $1000
	lsr a
	lsr a
	clc
	adc imgBase
	sta cbSrcPage
	lda cbBank
	and #3
	asl a
	asl a
	asl a
	asl a
	ora #$80
	sta cbSrc
	lda #16
	sta cbCount
@block:
	lda cbBank
	cmp nbanks
	bcs @zero
	ldx cbSrcPage
	lda #BiosSetPage
	jsr BIOS
	lda cbSrc
	sta @rd+2
	ldx #0
@rd:	lda $FF00,x
	sta copyBuf,x
	inx
	bne @rd
	jmp @write
@zero:
	lda #0
	tax
:	sta copyBuf,x
	inx
	bne :-
@write:
	ldx cbDstPage
	lda #BiosSetPage
	jsr BIOS
	lda cbDst
	sta @wr+2
	ldx #0
:	lda copyBuf,x
@wr:	sta $FF00,x
	inx
	bne :-
	inc cbSrc
	inc cbDst
	dec cbCount
	bne @block
	rts

;----------------------------------------------------------------------------
; Files

; Builds the file list from *.NSF and *.NSB in the current directory
ScanFiles:
	lda #0
	sta fileCount
	lda #<patNsf
	ldx #>patNsf
	jsr ScanPattern
	lda #<patNsb
	ldx #>patNsb
	; fall through

ScanPattern:
	sta schan+CH_BUF
	stx schan+CH_BUF+1
	ldx #47
	lda #0
@clr:
	cpx #CH_BUF
	beq :+
	cpx #CH_BUF+1
	beq :+
	sta schan,x
:	dex
	bpl @clr
	lda #FilePathFirst
	sta schan+CH_CMD
@call:
	ldx #>schan
	ldy #<schan
	jsr FILEIO
	bcc :+
	rts
:	jsr CopyFound
	lda found+FF_ATTR
	and #$18			; not directories or labels
	bne @next
	lda fileCount
	cmp #MAXFILES
	bcs @full
	jsr EntryAddr
	lda entryPtr
	sta @st+1
	lda entryPtr+1
	sta @st+2
	ldy #0
@copy:
	lda found+FF_NAME,y
@st:	sta $FFFF,y
	iny
	cpy #11
	bcc @copy
	inc fileCount
@next:
	lda #FileNext
	sta schan+CH_CMD
	jmp @call
@full:
	rts

; Copies the record FilePathFirst/FileNext found (at schan's page:buffer) into found
CopyFound:
	lda schan+CH_BUF
	sta ARG_FE
	lda schan+CH_BUF+1
	sta ARG_FF
	ldy #0
@loop:
	sty copyY
	ldx schan+CH_PAGE
	lda #BiosGetPagedChar
	jsr BIOS
	ldy copyY
	sta found,y
	iny
	cpy #RECSIZE
	bcc @loop
	rts

; entryPtr = fileList + A * 11
EntryAddr:
	sta work
	lda #0
	sta work+1
	lda work			; A * 11 = A * 8 + A * 2 + A
	asl a
	rol work+1
	asl a
	rol work+1
	asl a
	rol work+1
	sta entryPtr
	lda work+1
	sta entryPtr+1
	lda work
	asl a
	clc
	adc entryPtr
	sta entryPtr
	bcc :+
	inc entryPtr+1
:	lda work
	asl a
	bcc :+
	inc entryPtr+1
:	lda work
	clc
	adc entryPtr
	sta entryPtr
	bcc :+
	inc entryPtr+1
:	lda entryPtr
	clc
	adc #<fileList
	sta entryPtr
	lda entryPtr+1
	adc #>fileList
	sta entryPtr+1
	rts

; Copies list entry `index` to entry and builds "NAME.EXT",0 in path
BuildPath:
	lda index
	jsr EntryAddr
	lda entryPtr
	sta @rd+1
	lda entryPtr+1
	sta @rd+2
	ldy #10
@rd:	lda $FFFF,y
	sta entry,y
	dey
	bpl @rd
	ldx #0
	ldy #0
@name:
	lda entry,y
	cmp #' '
	beq @nameEnd
	sta path,x
	inx
	iny
	cpy #8
	bcc @name
@nameEnd:
	ldy #8
	lda entry,y
	cmp #' '
	beq @end
	lda #'.'
	sta path,x
	inx
@ext:
	lda entry,y
	cmp #' '
	beq @end
	sta path,x
	inx
	iny
	cpy #11
	bcc @ext
@end:
	lda #0
	sta path,x
	rts

; The program's argument ($0100: length, then the text after the command name) as an
; 11-character directory name in argName; argLen = 0 when there is none
FindArgument:
	lda #0
	sta argLen
	ldx #10
	lda #' '
:	sta argName,x
	dex
	bpl :-
	ldx $0100
	beq @done
	cpx #40
	bcs @done
	inx
	stx work			; end index
	ldy #1
@skip:
	cpy work
	bcs @done
	lda $0101-1,y
	cmp #' '
	bne @word
	iny
	bne @skip
@word:
	ldx #0				; name part, up to 8
@name:
	cpy work
	bcs @fin
	lda $0101-1,y
	jsr ArgChar
	bcs @fin
	cmp #'.'
	beq @dot
	cpx #8
	bcs :+
	sta argName,x
	inx
:	iny
	bne @name
@dot:
	iny
	ldx #8
@ext:
	cpy work
	bcs @fin
	lda $0101-1,y
	jsr ArgChar
	bcs @fin
	cpx #11
	bcs :+
	sta argName,x
	inx
:	iny
	bne @ext
@fin:
	lda argName
	cmp #' '
	beq @done
	inc argLen
	lda argName+8			; no extension given: .NSF
	cmp #' '
	bne @done
	ldx #2
:	lda extNsf,x
	sta argName+8,x
	dex
	bpl :-
@done:
	rts

; Uppercases A; carry set at the end of a word
ArgChar:
	cmp #' '+1
	bcc @end
	cmp #'a'
	bcc @ok
	cmp #'z'+1
	bcs @ok
	and #$DF
@ok:
	clc
	rts
@end:
	sec
	rts

; index = the list entry named by the argument, else 0
MatchArgument:
	lda #0
	sta index
	lda argLen
	beq @done
@try:
	lda index
	jsr EntryAddr
	lda entryPtr
	sta @rd+1
	lda entryPtr+1
	sta @rd+2
	ldy #10
@rd:	lda $FFFF,y
	cmp argName,y
	bne @next
	dey
	bpl @rd
	rts
@next:
	inc index
	lda index
	cmp fileCount
	bcc @try
	lda #0
	sta index
@done:
	rts

; Reads Y bytes into A/X. Carry set on error or short read.
ReadSmall:
	sta fchan+CH_BUF
	stx fchan+CH_BUF+1
	sty fchan+CH_LEN
	sty want
	lda #0
	sta fchan+CH_LEN+1
	sta fchan+CH_PAGE
	lda #FileRead
	sta fchan+CH_CMD
	jsr CallFile
	bcs @err
	lda fchan+CH_LEN+1
	bne @err
	lda fchan+CH_LEN
	cmp want
	bne @err
	clc
	rts
@err:
	sec
	rts

CloseFile:
	lda #FileClose
	sta fchan+CH_CMD
	; fall through
CallFile:
	ldx #>fchan
	ldy #<fchan
	jmp FILEIO

;----------------------------------------------------------------------------
; Screen (English mode: 30 rows of 32 characters)

ClearScreen:
	lda #DosInitialScreen
	jmp DOS

; X = row, Y = column
SetCursor:
	lda #BiosSetCursor
	jmp BIOS

; Prints the 0-terminated string at A/Y from column 1 of row X
PrintLine:
	sta printPtr
	sty printPtr+1
	ldy #1
	jsr SetCursor
	lda printPtr
	ldx printPtr+1
	ldy #31
	; fall through

; Prints up to Y characters at A/X, stopping at 0; other control and non-ASCII bytes
; print as '?'
PrintText:
	sta @rd+1
	stx @rd+2
	sty printMax
	lda #0
	sta printPos
@loop:
	ldy printPos
	cpy printMax
	bcs @done
@rd:	lda $FFFF,y
	beq @done
	cmp #' '
	bcc @bad
	cmp #$7F
	bcc @ok
@bad:
	lda #'?'
@ok:
	tax
	lda #DosDisplayChar
	jsr DOS
	inc printPos
	jmp @loop
@done:
	rts

; Prints A in decimal
PrintNumber:
	ldx #0
@h:	cmp #100
	bcc @t
	sbc #100
	inx
	bne @h
@t:	stx numH
	ldx #0
@t2:	cmp #10
	bcc @u
	sbc #10
	inx
	bne @t2
@u:	stx numT
	sta numU
	lda numH
	beq :+
	jsr PrintDigit
	lda numT
	jsr PrintDigit
	jmp @units
:	lda numT
	beq @units
	jsr PrintDigit
@units:
	lda numU
	; fall through
PrintDigit:
	ora #'0'
	tax
	lda #DosDisplayChar
	jmp DOS

PrintChar:
	tax
	lda #DosDisplayChar
	jmp DOS

DrawScreen:
	jsr ClearScreen
	ldx #1
	lda #<msgTitle
	ldy #>msgTitle
	jsr PrintLine
	ldx #3				; File  NAME.EXT  (n/m)
	lda #<msgFile
	ldy #>msgFile
	jsr PrintLine
	ldx #3
	ldy #8
	jsr SetCursor
	lda #<path
	ldx #>path
	ldy #12
	jsr PrintText
	ldx #3
	ldy #22
	jsr SetCursor
	lda #'('
	jsr PrintChar
	lda index
	clc
	adc #1
	jsr PrintNumber
	lda #'/'
	jsr PrintChar
	lda fileCount
	jsr PrintNumber
	lda #')'
	jsr PrintChar
	ldx #5				; title, artist, copyright: 32 bytes each
	ldy #1
	jsr SetCursor
	lda #<(header+$0E)
	ldx #>(header+$0E)
	ldy #30
	jsr PrintText
	ldx #6
	ldy #1
	jsr SetCursor
	lda #<(header+$2E)
	ldx #>(header+$2E)
	ldy #30
	jsr PrintText
	ldx #7
	ldy #1
	jsr SetCursor
	lda #<(header+$4E)
	ldx #>(header+$4E)
	ldy #30
	jsr PrintText
	lda header+$7B			; expansion sound is not played
	beq :+
	ldx #11
	lda #<msgExpansion
	ldy #>msgExpansion
	jsr PrintLine
:	lda params+P_REGION
	beq :+
	ldx #12
	lda #<msgPal
	ldy #>msgPal
	jsr PrintLine
:	ldx #15
	lda #<msgKeys1
	ldy #>msgKeys1
	jsr PrintLine
	ldx #16
	lda #<msgKeys2
	ldy #>msgKeys2
	jsr PrintLine
	ldx #17
	lda #<msgKeys3
	ldy #>msgKeys3
	jsr PrintLine
	ldx #18
	lda #<msgKeys4
	ldy #>msgKeys4
	jmp PrintLine

DrawTrack:
	ldx #9
	lda #<msgBlank
	ldy #>msgBlank
	jsr PrintLine
	ldx #9
	lda #<msgTrack
	ldy #>msgTrack
	jsr PrintLine
	ldx #9
	ldy #15
	jsr SetCursor
	lda #<msgZeroTime
	ldx #>msgZeroTime
	ldy #5
	jsr PrintText
	ldx #9
	ldy #8
	jsr SetCursor
	lda song
	clc
	adc #1
	jsr PrintNumber
	lda #'/'
	jsr PrintChar
	lda songs
	jmp PrintNumber

ShowError:
	jsr ClearScreen
	ldx #3
	lda #<msgCannot
	ldy #>msgCannot
	jsr PrintLine
	ldx #3
	ldy #13
	jsr SetCursor
	lda #<path
	ldx #>path
	ldy #12
	jsr PrintText
	ldx #5
	lda errMsg
	ldy errMsg+1
	jsr PrintLine
	ldx #8
	lda #<msgErrKeys
	ldy #>msgErrKeys
	jmp PrintLine

; Waits for a key (A = its code; 27 for Esc) or a joypad press (Select = 27)
WaitKey:
	lda Joystick1Key
	sta padPrev
@loop:
	lda #DosCheckKeyBoard
	jsr DOS
	beq @pad
	lda #DosReadKeyNoEcho
	jsr DOS
	cmp #0				; extended keys: take the scan code too
	beq :+
	cmp #$E0
	bne @key
:	lda #DosReadKeyNoEcho
	jsr DOS
	ora #$80
@key:
	rts
@pad:
	lda Joystick1Key
	tax
	lda padPrev
	eor #$FF
	stx padPrev
	sta work
	txa
	and work
	beq @loop
	and #$04			; Select
	beq :+
	lda #27
	rts
:	lda #13
	rts

;----------------------------------------------------------------------------
	.segment "RODATA"

nsfMagic:	.byte "NESM", $1A
nsbMagic:	.byte "BBKN", 1
patNsf:		.byte "*.NSF", 0
patNsb:		.byte "*.NSB", 0
extNsf:		.byte "NSF"
extNsb:		.byte "NSB"
msgTitle:	.byte "NSF Player", 0
msgFile:	.byte "File", 0
msgTrack:	.byte "Track", 0
msgZeroTime:	.byte " 0:00", 0
msgBlank:	.byte "                              ", 0
msgLoading:	.byte "Loading", 0
msgCannot:	.byte "Cannot play", 0
msgNoFiles:	.byte "No .NSF files here.", 0
msgExpansion:	.byte "Expansion sound is not played.", 0
msgPal:		.byte "PAL tune: plays a little sharp.", 0
msgKeys1:	.byte "Left Right   track", 0
msgKeys2:	.byte "Up Down      file", 0
msgKeys3:	.byte "Space        pause", 0
msgKeys4:	.byte "Esc          quit", 0
msgErrKeys:	.byte "Any key: next file  Esc: quit", 0
errOpen:	.byte "The file does not open.", 0
errFormat:	.byte "Not an NSF file.", 0
errLoad:	.byte "Load address below $8000.", 0
errExpansion:	.byte "Expansion sound: run nsf2bbk.", 0
errBanking:	.byte "Bank switching: run nsf2bbk.", 0
errMemory:	.byte "Too big for the free memory.", 0

;----------------------------------------------------------------------------
; The resident part: runs at $5000 while a track plays. Nothing here may use the zero
; page or call the BIOS. The stub table must stay first: nsf2bbk.py patches NSF code to
; call these addresses.

	.segment "RESIDENT"

STUB_TABLE = $5000
	.assert * = STUB_TABLE, error, "stub table moved"

; $5000 + 3n: STA $5FF8+n   $5018 + 3n: STX   $5030 + 3n: STY   (n = 0-7)
; $5048 + 3k: STA $5FF8+k,X   $5060 + 3k: STA $5FF8+k,Y   (k = 0-7; slot = k + index)
	.repeat 8, n
	jmp .ident(.sprintf("SA%d", n))
	.endrepeat
	.repeat 8, n
	jmp .ident(.sprintf("SX%d", n))
	.endrepeat
	.repeat 8, n
	jmp .ident(.sprintf("SY%d", n))
	.endrepeat
	.repeat 8, b
	jmp .ident(.sprintf("SAX%d", b))
	.endrepeat
	.repeat 8, b
	jmp .ident(.sprintf("SAY%d", b))
	.endrepeat
	.assert * = STUB_TABLE + $78, error, "stub table size"

	.repeat 8, n
.ident(.sprintf("SA%d", n)):
	php
	pha
	lda #n
	jmp StoreA
	.endrepeat
	.repeat 8, n
.ident(.sprintf("SX%d", n)):
	php
	pha
	lda #n
	jmp StoreX
	.endrepeat
	.repeat 8, n
.ident(.sprintf("SY%d", n)):
	php
	pha
	lda #n
	jmp StoreY
	.endrepeat
	.repeat 8, b
.ident(.sprintf("SAX%d", b)):
	php
	pha
	txa
	clc
	adc #b
	jmp StoreAChecked
	.endrepeat
	.repeat 8, b
.ident(.sprintf("SAY%d", b)):
	php
	pha
	tya
	clc
	adc #b
	jmp StoreAChecked
	.endrepeat

; A = slot; on the stack: the caller's P, then A (the value for the STA forms)
StoreX:
	stx r_bankVal
	jmp BankSet
StoreY:
	sty r_bankVal
	jmp BankSet
StoreAChecked:
	cmp #8				; past $5FFF: not a bank register
	bcc StoreA
	pla
	plp
	rts
StoreA:
	sta r_slot
	pla
	pha
	sta r_bankVal
	lda r_slot
BankSet:
	stx r_saveX
	sty r_saveY
	tax
	lda r_bankVal
	sta r_params+P_SHADOW,x
	txa
	lsr a
	tax
	jsr MapWindow
	ldx r_saveX
	ldy r_saveY
	pla
	plp
	rts

; Maps window X (0-3: $8000, $A000, $C000, $E000) to the page holding its two banks.
; A pair the memory does not hold leaves the window as it was (a tune switching both
; halves of a window passes through such a pair between its two writes).
MapWindow:
	stx r_w
	txa
	asl a
	tax
	lda r_params+P_SHADOW,x
	sta r_lo
	lda r_params+P_SHADOW+1,x
	sta r_hi
	lda r_lo
	lsr a
	bcs @search
	cmp r_params+P_IMGPAIRS
	bcs @search
	tay
	ldx r_lo
	inx
	cpx r_hi
	bne @search
	tya
	clc
	adc r_params+P_IMGPHYS
	jmp @set
@search:
	ldy r_params+P_NEXTRA
@loop:
	dey
	bmi @none
	lda r_params+P_EXLO,y
	cmp r_lo
	bne @loop
	lda r_params+P_EXHI,y
	cmp r_hi
	bne @loop
	lda r_params+P_EXPHYS,y
@set:
	ldx r_w
	beq @w0
	dex
	beq @w1
	dex
	beq @w2
	sta REG_W3
	rts
@w2:	sta REG_W2
	rts
@w1:	sta REG_W1
	rts
@w0:	sta REG_W0
@none:	rts

; Called by Session with the save page at $8000, interrupts and NMI off. Returns the
; action in A with everything below $8000 except $4E00-$57FF put back.
R_Enter:
	tsx
	stx r_sp
	lda #$00			; RAM and WRAM to the save page
	ldx #SAVE_RAM
	ldy #8
	jsr R_Copy
	lda #$60
	ldx #SAVE_WRAM
	ldy #32
	jsr R_Copy
	lda PPUCTRL_SHADOW		; (before WRAM is cleared)
	sta r_ppuctrl
	ldx #$FF			; a fresh stack; the old one is saved
	txs
	lda #0
	tax
@ram:
	sta $0000,x
	sta $0100,x
	sta $0200,x
	sta $0300,x
	sta $0400,x
	sta $0500,x
	sta $0600,x
	sta $0700,x
	inx
	bne @ram
	lda #$60
	sta @wram+2
	lda #0
	ldy #32
@wram:	sta $6000,x
	inx
	bne @wram
	inc @wram+2
	dey
	bne @wram

	sta r_action
	sta r_pending
	sta r_paused
	sta r_acc
	sta r_acc+1
	sta r_acc+2
	sta r_vbNoFlag
	sta r_seenFlag
	sta r_vblanks
	sta r_fresh
	sta r_usec
	sta r_usec+1
	sta r_usec+2
	sta r_sec
	sta r_min
	jsr MakeStatus
	ldx #3
@map:
	stx r_mapIndex
	jsr MapWindow
	ldx r_mapIndex
	dex
	bpl @map

	lda #0				; sound registers as the NSF format promises them
	ldx #$13
@apu:	sta $4000,x
	dex
	bpl @apu
	lda #$0F
	sta APUSTATUS
	lda #$40
	sta APUFRAME
	lda r_params+P_SONG
	ldx r_params+P_REGION
	ldy #0
	jsr R_Init
	lda #$00			; 4-step sequence; its flag is the PLAY timer
	sta APUFRAME
	bit APUSTATUS

R_Loop:
	bit APUSTATUS			; V = the frame-counter flag (reading clears it)
	bvc @noFlag
	bit APUSTATUS			; the flag is raised on three cycles running
	jsr OnFrameFlag
	lda #0
	sta r_fresh
@noFlag:
	bit PPUSTATUS
	bmi @vblank
	lda #1				; the next vblank seen is a fresh one
	sta r_fresh
	bne R_Loop
@vblank:
	jsr OnVblank
	lda r_action
	beq R_Loop

R_Exit:
	lda #0
	sta APUSTATUS
	lda #$40
	sta APUFRAME
	lda #DOS_W2
	sta REG_W2
	lda #DOS_W3
	sta REG_W3
	lda r_params+P_SAVEPAGE
	sta REG_PAGE16
	lda #SAVE_WRAM
	ldx #$60
	ldy #32
	jsr R_Copy
	ldx #0				; RAM last, without the stack
@ram:
	lda SAVE_RAM*256,x
	sta $0000,x
	lda SAVE_RAM*256+$100,x
	sta $0100,x
	lda SAVE_RAM*256+$200,x
	sta $0200,x
	lda SAVE_RAM*256+$300,x
	sta $0300,x
	lda SAVE_RAM*256+$400,x
	sta $0400,x
	lda SAVE_RAM*256+$500,x
	sta $0500,x
	lda SAVE_RAM*256+$600,x
	sta $0600,x
	lda SAVE_RAM*256+$700,x
	sta $0700,x
	inx
	bne @ram
	ldx r_sp
	txs
	lda r_action
	rts

R_Init:
	jmp (r_params+P_INIT)
R_Play:
	jmp (r_params+P_PLAY)

; Copies Y pages from page A to page X
R_Copy:
	sta @src+2
	stx @dst+2
	ldx #0
@loop:
@src:	lda $FF00,x
@dst:	sta $FF00,x
	inx
	bne @loop
	inc @src+2
	inc @dst+2
	dey
	bne @loop
	rts

OnFrameFlag:
	lda #1
	sta r_seenFlag
	lda #0
	sta r_vbNoFlag
	lda r_params+P_MODE
	cmp #MODE_VBLANK
	bcs @rts
	ldx r_paused
	bne @rts
	lda #<REAL_FRAMEFLAG		; the clock counts what drives PLAY: a long PLAY can
	ldx #>REAL_FRAMEFLAG		; hide a whole vblank, never a frame-counter flag
	jsr Clock
	lda r_params+P_MODE
	cmp #MODE_FRAME
	bne :+
	jmp R_Play
:	lda #<US_FRAMEFLAG
	ldx #>US_FRAMEFLAG
	jmp Accumulate
@rts:	rts

OnVblank:
	lda r_fresh			; the status line, only at the start of a vblank
	beq @time
	lda r_dirty
	beq @time
	jsr DrawStatus
@time:
	inc r_vblanks
	bne :+
	dec r_vblanks
:	jsr ReadInput
	lda r_params+P_MODE
	cmp #MODE_VBLANK
	bcs @vblankMode
	inc r_vbNoFlag			; no flag for four vblanks: start it again
	lda r_vbNoFlag
	cmp #4
	bcc @rts
	lda #0
	sta r_vbNoFlag
	lda r_seenFlag
	bne @restart
	lda r_vblanks			; never one: the machine has no frame counter flag
	cmp #12
	bcc @restart
	lda #MODE_VBLANKACC
	sta r_params+P_MODE
	rts
@restart:
	lda #$00
	sta APUFRAME
@rts:	rts
@vblankMode:
	ldx r_paused
	bne @rts
	lda #<REAL_VBLANK
	ldx #>REAL_VBLANK
	jsr Clock
	lda r_params+P_MODE
	cmp #MODE_VBLANK
	bne :+
	jmp R_Play
:	lda #<US_VBLANK
	ldx #>US_VBLANK
	jmp Accumulate

; Adds A/X microseconds to the elapsed time
Clock:
	clc
	adc r_usec
	sta r_usec
	txa
	adc r_usec+1
	sta r_usec+1
	bcc :+
	inc r_usec+2
:	lda r_usec			; a second = 1000000 = $0F4240
	sec
	sbc #$40
	tax
	lda r_usec+1
	sbc #$42
	tay
	lda r_usec+2
	sbc #$0F
	bcc @rts
	sta r_usec+2
	sty r_usec+1
	stx r_usec
	inc r_sec
	lda r_sec
	cmp #60
	bcc :+
	lda #0
	sta r_sec
	lda r_min
	cmp #99
	bcs :+
	inc r_min
:	jmp MakeStatus
@rts:	rts

; Adds A/X microseconds; plays once for every SPEED microseconds gathered
Accumulate:
	clc
	adc r_acc
	sta r_acc
	txa
	adc r_acc+1
	sta r_acc+1
	bcc @loop
	inc r_acc+2
@loop:
	lda r_acc
	sec
	sbc r_params+P_SPEED
	tax
	lda r_acc+1
	sbc r_params+P_SPEED+1
	tay
	lda r_acc+2
	sbc #0
	bcc @done
	sta r_acc+2
	sty r_acc+1
	stx r_acc
	jsr R_Play
	jmp @loop
@done:
	rts

; Reads the keyboard and joypad 1. An action fires when its key is let go.
ReadInput:
	lda #$05			; keyboard: row 0, column 0
	sta PAD
	jsr KeyDelay
	ldx #0
@row:
	lda #$04
	sta PAD
	jsr KeyDelay
	lda KEYS
	lsr a
	and #$0F
	eor #$0F
	sta r_keys,x
	inx
	lda #$06
	sta PAD
	jsr KeyDelay
	lda KEYS
	lsr a
	and #$0F
	eor #$0F
	sta r_keys,x
	inx
	cpx #18
	bcc @row
	; Joypad: a DPCM sample fetch during a read clocks the pad's shift register once more,
	; so read until two reads agree
	lda #4
	sta r_tries
	jsr ReadPad
@again:
	lda r_pad
	sta r_padLast
	jsr ReadPad
	lda r_pad
	cmp r_padLast
	beq @padOk
	dec r_tries
	bne @again
	lda #0				; never steady: count nothing as held
	sta r_pad
@padOk:

	ldx #KEYCOUNT-1
@key:
	ldy keyIndex,x
	lda r_keys,y
	and keyMask,x
	bne @keyHeld
	dex
	bpl @key
	ldx #PADCOUNT-1
@button:
	lda r_pad
	and padMask,x
	bne @padHeld
	dex
	bpl @button
	lda r_pending			; nothing held
	beq @rts
	ldx #0
	stx r_pending
	cmp #ACT_PAUSE
	beq TogglePause
	sta r_action
@rts:	rts
@keyHeld:
	lda keyAction,x
	sta r_pending
	rts
@padHeld:
	lda padAction,x
	sta r_pending
	rts

ReadPad:
	lda #$01
	sta PAD
	lda #$00
	sta PAD
	ldx #8
@bit:
	lda PAD
	lsr a
	rol r_pad
	dex
	bne @bit
	rts

TogglePause:
	lda r_paused
	eor #1
	sta r_paused
	beq :+
	lda #$00
	sta APUSTATUS
	jmp MakeStatus
:	lda #$0F
	sta APUSTATUS
	; fall through

; Puts "mm:ss  Paused" (13 characters) in r_text for DrawStatus
MakeStatus:
	lda r_min
	jsr Div10
	cpx #0
	bne :+
	ldx #<(' ' - '0')
:	txa
	clc
	adc #'0'
	sta r_text
	tya
	ora #'0'
	sta r_text+1
	lda #':'
	sta r_text+2
	lda r_sec
	jsr Div10
	txa
	ora #'0'
	sta r_text+3
	tya
	ora #'0'
	sta r_text+4
	lda #' '
	sta r_text+5
	sta r_text+6
	ldx #7
	lda r_paused
	beq @blank
@paused:
	lda msgPaused-7,x
	sta r_text,x
	inx
	cpx #13
	bcc @paused
	bcs @done
@blank:
	lda #' '
	sta r_text,x
	inx
	cpx #13
	bcc @blank
@done:
	lda r_ppuctrl			; VRAM writes need the +1 increment
	and #$04
	bne :+
	lda #1
	sta r_dirty
:	rts

; A = X * 10 + Y
Div10:
	ldx #0
:	cmp #10
	bcc :+
	sbc #10
	inx
	bne :-
:	tay
	rts

; Writes r_text at row 9, column 15 of the BIOS's text screen (tile = character code;
; BIOS column c is nametable column c + 1). Takes ~180 cycles, so it runs only right
; after the vblank flag rose. $2006 = 0, 0 afterwards leaves the scroll at 0, 0 on
; nametable 0, where the BIOS keeps its text screen.
STATUS_VRAM = $2000 + 9 * 32 + 15 + 1
DrawStatus:
	lda #>STATUS_VRAM
	sta PPUADDR
	lda #<STATUS_VRAM
	sta PPUADDR
	ldx #0
:	lda r_text,x
	sta PPUDATA
	inx
	cpx #13
	bcc :-
	lda #0
	sta PPUADDR
	sta PPUADDR
	sta r_dirty
	rts

msgPaused:	.byte "Paused"

KeyDelay:
	ldy #8
:	dey
	bne :-
	rts

; Keys: index into r_keys (row * 2 + column), bit, action
keyIndex:	.byte 4,   8,   9,   4,   8,   5,   11,  10,  16,  8,   16,  15
keyMask:	.byte $08, $08, $08, $04, $04, $02, $01, $01, $08, $02, $04, $02
keyAction:	.byte ACT_NEXT, ACT_PREV, ACT_NEXTFILE, ACT_NEXTFILE, ACT_PREVFILE, ACT_PREVFILE
		.byte ACT_QUIT, ACT_QUIT, ACT_PAUSE, ACT_PAUSE, ACT_NEXT, ACT_PREV
KEYCOUNT = keyMask - keyIndex
; Right, Left, Down, PgDn, Up, PgUp, Esc, Q, Space, Enter, N, P

; Joypad: R L D U Start Select A
padMask:	.byte $01, $02, $04, $08, $10, $20, $80
padAction:	.byte ACT_NEXT, ACT_PREV, ACT_NEXTFILE, ACT_PREVFILE, ACT_PAUSE, ACT_QUIT, ACT_PAUSE
PADCOUNT = padAction - padMask

	.segment "RESBSS"
r_params:	.res P_SIZE
r_bankVal:	.res 1
r_slot:		.res 1
r_saveX:	.res 1
r_saveY:	.res 1
r_w:		.res 1
r_lo:		.res 1
r_hi:		.res 1
r_sp:		.res 1
r_mapIndex:	.res 1
r_action:	.res 1
r_pending:	.res 1
r_paused:	.res 1
r_acc:		.res 3
r_vbNoFlag:	.res 1
r_seenFlag:	.res 1
r_vblanks:	.res 1
r_keys:		.res 18
r_pad:		.res 1
r_fresh:	.res 1
r_dirty:	.res 1
r_usec:		.res 3
r_sec:		.res 1
r_min:		.res 1
r_ppuctrl:	.res 1
r_text:		.res 13
r_padLast:	.res 1
r_tries:	.res 1
	; JMP (abs) cannot take a vector that straddles a page
	.assert <(r_params+P_INIT) <> $FF, error, "INIT vector straddles a page"
	.assert <(r_params+P_PLAY) <> $FF, error, "PLAY vector straddles a page"

;----------------------------------------------------------------------------
	.segment "BSS"

oldMode:	.res 1
dosPage:	.res 1
curPage:	.res 1
firstFree:	.res 1
ppuSave:	.res 1
action:		.res 1
fileCount:	.res 1
index:		.res 1
song:		.res 1
songs:		.res 1
isNsb:		.res 1
banked:		.res 1
nbanks:		.res 1
imgBase:	.res 1
extraBase:	.res 1
exIndex:	.res 1
pairLo:		.res 1
pairHi:		.res 1
pad:		.res 3
imgEnd:		.res 3
zFrom:		.res 3
zTo:		.res 3
aOff:		.res 3
zPage:		.res 1
zPtr:		.res 2
want:		.res 2
work:		.res 2
cbBank:		.res 1
cbSrcPage:	.res 1
cbSrc:		.res 1
cbDstPage:	.res 1
cbDst:		.res 1
cbDstOff:	.res 1
cbCount:	.res 1
copyY:		.res 1
entryPtr:	.res 2
printPtr:	.res 2
printMax:	.res 1
printPos:	.res 1
numH:		.res 1
numT:		.res 1
numU:		.res 1
padPrev:	.res 1
errMsg:		.res 2
argLen:		.res 1
argName:	.res 11
entry:		.res 11
path:		.res 13
params:		.res P_SIZE
found:		.res RECSIZE
header:		.res 128
block:		.res 128
schan:		.res 48
fchan:		.res 48
copyBuf:	.res 256
fileList:	.res MAXFILES*11
