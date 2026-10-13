; PICVIEW.CMD - picture viewer for BBK BBGDOS (BBK 1.0 BIOS)
;
; Shows the .GRP canvases and .PCT stamps saved by 神奇画笔 (BRUSH.EXC, disk 122).
; File layout: 20-byte header, then rows*cols cells of 33 bytes.
;   header +0  ".Grp" or ".Pct"
;          +4  palette bytes in use (4/8/12)
;          +5  rows (16 px each)
;          +6  columns (8 px each)
;          +8  12 palette bytes = colour groups 0-2, 4 colours each
;   cell   +0  colour group, +1 two 2bpp 8x8 tiles stacked (an 8x16 cell)
; That cell is exactly what the BIOS call BiosSetBGFont draws, so the viewer
; only uses documented BIOS/DOS/file services (BBGDOS manual, chapters 2-5).
;
; Keys: Right/Down/PgDn/Space/Enter or pad Right/Down/A  = next picture
;       Left/Up/PgUp/Backspace or pad Left/Up/B           = previous picture
;       Home/End = first/last, Esc/Q or pad Start          = quit

	.setcpu "6502"

BIOS	= $5800			; BIOS calls, A = function
FILEIO	= $5803			; file calls, X|Y = file channel
DOS	= $580C			; DOS calls, A = function

ARG_FC	= $FC			; BIOS argument bytes
ARG_FD	= $FD
ARG_FE	= $FE
ARG_FF	= $FF

Joystick1Key	= $5FFC		; BIOS variable: main joypad buttons held (R L D U St Se B A)

; BIOS functions
BiosWaitVSynCnt		= $0E
BiosSetCursor		= $1C
BiosWaitDisplay		= $22
BiosGetPagedChar	= $20
BiosSetPalette		= $2B
BiosDispStringAt	= $32
BiosSetBGFont		= $36
BiosDispInitial		= $3C
BiosDispOpenClose	= $3D

; DOS functions
DosProgramReturn	= $00
DosReadKeyNoEcho	= $07
DosInitialScreen	= $09
DosGetSetDispMode	= $0B
DosCheckKeyBoard	= $0E

; File functions
FileRead		= $02
FileClose		= $05
FileNext		= $07
FilePathOpen		= $14
FilePathFirst		= $16

; File channel layout (48 bytes)
CH_CMD	= 0
CH_DRV	= 1
CH_PAGE	= 2
CH_BUF	= 3			; word
CH_LEN	= 5			; word
CH_POS	= 7			; dword

; Found-file record returned by FilePathFirst/FileNext
FF_ATTR	= 0
FF_SIZE	= 5			; dword
FF_NAME	= 9			; 11 chars, name padded to 8 + extension padded to 3

MAXFILES	= 200
SCREENROWS	= 13		; rows 0-12 hold the picture, row 13 the status line
SCREENCOLS	= 31
STATUSROW	= 13
CELLSIZE	= 33
HEADERSIZE	= 20

; Actions returned by GetAction
ACT_NEXT	= 1
ACT_PREV	= 2
ACT_FIRST	= 3
ACT_LAST	= 4
ACT_QUIT	= 5

	.segment "LOADADDR"
	.word Start

	.segment "CODE"

Start:
	ldx #0				; remember the display mode
	lda #DosGetSetDispMode
	jsr DOS
	sta oldMode
	ldx #1				; Chinese mode: 14 rows of 8x16 cells
	ldy #$80
	lda #DosGetSetDispMode
	jsr DOS

	lda Joystick1Key
	sta padPrev
	jsr ScanFiles
	lda fileCount
	bne @have
	jsr ClearScreen
	lda #<msgNone
	ldx #>msgNone
	ldy #msgNoneLen
	jsr PrintStatus
	jsr OpenScreen
	jsr GetAction
	jmp Quit

@have:
	lda #0
	sta index
	lda #ACT_NEXT
	sta direction

ShowLoop:
	lda #0
	sta tries
@try:
	jsr ShowPicture
	bcc WaitInput
	inc tries			; not a picture after all: move on in the same direction
	lda tries
	cmp fileCount
	bcs @none
	jsr Step
	jmp @try
@none:
	jsr ClearScreen
	lda #<msgNone
	ldx #>msgNone
	ldy #msgNoneLen
	jsr PrintStatus
	jsr OpenScreen
	jsr GetAction
	jmp Quit

WaitInput:
	jsr GetAction
	cmp #ACT_QUIT
	beq Quit
	cmp #ACT_FIRST
	bne @notFirst
	lda #0
	sta index
	lda #ACT_NEXT
	sta direction
	jmp ShowLoop
@notFirst:
	cmp #ACT_LAST
	bne @notLast
	ldx fileCount
	dex
	stx index
	lda #ACT_PREV
	sta direction
	jmp ShowLoop
@notLast:
	sta direction
	jsr Step
	jmp ShowLoop

Quit:
	ldx #1				; restore the display mode the DOS was in
	ldy oldMode
	lda #DosGetSetDispMode
	jsr DOS
	ldx #0				; return code 0, reinitialise the screen
	ldy #1
	lda #DosProgramReturn
	jmp DOS

; Moves index one file in the current direction, wrapping around
Step:
	lda direction
	cmp #ACT_PREV
	beq @prev
	inc index
	lda index
	cmp fileCount
	bcc @done
	lda #0
	sta index
@done:
	rts
@prev:
	lda index
	bne @dec
	lda fileCount
	sta index
@dec:
	dec index
	rts

;----------------------------------------------------------------------------
; Directory scan: keep plain files whose size is HEADERSIZE + n*CELLSIZE

ScanFiles:
	lda #0
	sta fileCount
	ldx #47
@clr:
	sta schan,x
	dex
	bpl @clr
	lda #FilePathFirst
	sta schan+CH_CMD
	lda #<patAll
	sta schan+CH_BUF
	lda #>patAll
	sta schan+CH_BUF+1
	lda #0				; attribute 0 = ordinary files only
	sta schan+CH_LEN
@call:
	ldx #>schan
	ldy #<schan
	jsr FILEIO
	bcc @found
	rts				; no (more) files
@found:
	jsr CopyFound
	jsr SizeFits
	bcs @next
	jsr ExtFits
	bcs @next
	lda found+FF_ATTR
	and #$18			; skip directories and volume labels
	bne @next
	jsr AddFile
@next:
	lda fileCount
	cmp #MAXFILES
	bcs @full
	lda #FileNext
	sta schan+CH_CMD
	jmp @call
@full:
	rts

; Copies the 20-byte found-file record (at schan's page:buffer) into found
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
	cpy #HEADERSIZE
	bcc @loop
	rts

; Carry clear when found's size is HEADERSIZE + n*CELLSIZE (n > 0, under 64K)
SizeFits:
	lda found+FF_SIZE+2
	ora found+FF_SIZE+3
	bne @no
	lda found+FF_SIZE
	sec
	sbc #HEADERSIZE
	sta work
	lda found+FF_SIZE+1
	sbc #0
	sta work+1
	bcc @no				; smaller than a header
	lda work
	ora work+1
	beq @no				; header only
@sub:
	lda work
	ora work+1
	beq @yes
	lda work
	sec
	sbc #CELLSIZE
	sta work
	lda work+1
	sbc #0
	sta work+1
	bcs @sub
@no:
	sec
	rts
@yes:
	clc
	rts

; Carry clear when found's extension is GRP, PCT or blank (BBG/BBK/YEAR on disk 122
; are extensionless canvases)
ExtFits:
	ldy #0
@grp:
	lda found+FF_NAME+8,y
	cmp extGrp,y
	bne @pct
	iny
	cpy #3
	bcc @grp
	clc
	rts
@pct:
	ldy #0
@pct2:
	lda found+FF_NAME+8,y
	cmp extPct,y
	bne @blank
	iny
	cpy #3
	bcc @pct2
	clc
	rts
@blank:
	ldy #0
@blank2:
	lda found+FF_NAME+8,y
	cmp #' '
	bne @no
	iny
	cpy #3
	bcc @blank2
	clc
	rts
@no:
	sec
	rts

; Appends found's 11-character name to the file list
AddFile:
	lda fileCount
	jsr EntryAddr
	lda entryPtr
	sta @store+1
	lda entryPtr+1
	sta @store+2
	ldy #0
@loop:
	lda found+FF_NAME,y
@store:
	sta $FFFF,y			; operand = entryPtr
	iny
	cpy #11
	bcc @loop
	inc fileCount
	rts

; Sets entryPtr to list entry A (fileList + A*11). $FC-$FF are left alone outside BIOS
; calls: the BIOS display queue works through them from its interrupt handler.
EntryAddr:
	sta work
	lda #0
	sta work+1
	sta entryPtr+1
	lda work			; A*8
	asl a
	rol entryPtr+1
	asl a
	rol entryPtr+1
	asl a
	rol entryPtr+1
	sta entryPtr
	lda work			; + A*2
	asl a
	rol work+1
	clc
	adc entryPtr
	sta entryPtr
	lda work+1
	adc entryPtr+1
	sta entryPtr+1
	lda work			; + A
	clc
	adc entryPtr
	sta entryPtr
	bcc :+
	inc entryPtr+1
:	lda entryPtr			; + fileList
	clc
	adc #<fileList
	sta entryPtr
	lda entryPtr+1
	adc #>fileList
	sta entryPtr+1
	rts

;----------------------------------------------------------------------------
; Shows list entry `index`. Carry set if it is not a picture.

ShowPicture:
	jsr BuildPath
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
	bcc @opened
	rts
@opened:
	lda #0				; read from the start
	sta fchan+CH_POS
	sta fchan+CH_POS+1
	sta fchan+CH_POS+2
	sta fchan+CH_POS+3
	lda #<header
	ldx #>header
	ldy #HEADERSIZE
	jsr ReadBytes
	bcs @bad
	jsr CheckHeader
	bcc @good
@bad:
	jsr CloseFile
	sec
	rts

@good:
	jsr ClearScreen
	jsr SetPalettes
	jsr CloseScreen

	; bytes per row = cols * 33
	lda #0
	sta rowBytes+1
	lda header+6
	sta rowBytes
	ldx #5				; *32
@mul:
	asl rowBytes
	rol rowBytes+1
	dex
	bne @mul
	lda rowBytes			; + cols
	clc
	adc header+6
	sta rowBytes
	bcc :+
	inc rowBytes+1
:
	; clip to the screen and centre
	lda header+5
	cmp #SCREENROWS+1
	bcc :+
	lda #SCREENROWS
:	sta drawRows
	lda header+6
	cmp #SCREENCOLS+1
	bcc :+
	lda #SCREENCOLS
:	sta drawCols
	lda #SCREENROWS
	sec
	sbc drawRows
	lsr a
	sta row0
	lda #SCREENCOLS
	sec
	sbc drawCols
	lsr a
	sta col0

	lda #0
	sta row
@rowLoop:
	lda row
	cmp drawRows
	bcc :+
	jmp @rowsDone
:	lda #<rowBuf			; one row of cells
	sta fchan+CH_BUF
	lda #>rowBuf
	sta fchan+CH_BUF+1
	lda rowBytes
	sta fchan+CH_LEN
	lda rowBytes+1
	sta fchan+CH_LEN+1
	jsr ReadChannel
	bcc :+
	jmp @rowsDone			; short file: show what was read
:

	lda #<(rowBuf+1)		; first cell's pattern
	sta cellPtr
	lda #>(rowBuf+1)
	sta cellPtr+1
	lda #0
	sta col
@colLoop:
	lda col
	cmp drawCols
	bcs @colsDone
	lda row0
	clc
	adc row
	tax
	lda col0
	clc
	adc col
	tay
	lda #BiosSetCursor
	jsr BIOS
	lda cellPtr			; colour group = byte before the pattern
	sec
	sbc #1
	sta @group+1
	lda cellPtr+1
	sbc #0
	sta @group+2
@group:
	lda $FFFF			; operand = cellPtr-1
	and #3
	sta ARG_FF
	ldx cellPtr+1
	ldy cellPtr
	lda #BiosSetBGFont
	jsr BIOS
	lda cellPtr
	clc
	adc #CELLSIZE
	sta cellPtr
	bcc :+
	inc cellPtr+1
:	inc col
	jmp @colLoop
@colsDone:
	inc row
	jmp @rowLoop
@rowsDone:
	jsr CloseFile
	jsr OpenScreen
	jsr ShowStatus
	clc
	rts

; Carry clear for a ".Grp"/".Pct" header with a sane size
CheckHeader:
	lda header
	cmp #'.'
	bne @no
	ldx #2
@grp:
	lda header+1,x
	cmp magicGrp,x
	bne @pct
	dex
	bpl @grp
	bmi @size
@pct:
	ldx #2
@pct2:
	lda header+1,x
	cmp magicPct,x
	bne @no
	dex
	bpl @pct2
@size:
	lda header+5
	beq @no
	lda header+6
	beq @no
	cmp #SCREENCOLS+1		; a row has to fit the row buffer
	bcs @no
	clc
	rts
@no:
	sec
	rts

; Palette bytes in use -> BIOS palette entries 0-11 (4 and 8 mirror 0 and are left alone)
SetPalettes:
	lda header+4
	cmp #13
	bcc :+
	lda #12
:	sta palCount
	lda #0
	sta palIndex
@loop:
	lda palIndex
	cmp palCount
	bcs @done
	cmp #4
	beq @skip
	cmp #8
	beq @skip
	tay
	ldx header+8,y
	lda #BiosSetPalette
	jsr BIOS
@skip:
	inc palIndex
	jmp @loop
@done:
	rts

; Builds "NAME.EXT",0 in path from list entry `index`
BuildPath:
	lda index
	jsr EntryAddr
	lda entryPtr
	sta ReadEntry+1
	lda entryPtr+1
	sta ReadEntry+2
	ldx #0
	ldy #0
@name:
	jsr ReadEntry
	cmp #' '
	beq @nameEnd
	sta path,x
	inx
	iny
	cpy #8
	bcc @name
@nameEnd:
	ldy #8
	jsr ReadEntry
	cmp #' '
	beq @end
	lda #'.'
	sta path,x
	inx
@ext:
	jsr ReadEntry
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
	stx pathLen
	rts

; A = byte Y of the list entry BuildPath selected
ReadEntry:
	lda $FFFF,y			; operand = entryPtr
	rts

;----------------------------------------------------------------------------
; File helpers

; Reads Y bytes into A(lo)/X(hi) from fchan. Carry set on error or short read.
ReadBytes:
	sta fchan+CH_BUF
	stx fchan+CH_BUF+1
	sty fchan+CH_LEN
	lda #0
	sta fchan+CH_LEN+1
	; fall through

; Reads CH_LEN bytes into CH_BUF. Carry set on error or short read.
ReadChannel:
	lda fchan+CH_LEN
	sta want
	lda fchan+CH_LEN+1
	sta want+1
	lda #FileRead
	sta fchan+CH_CMD
	lda #0
	sta fchan+CH_PAGE
	jsr CallFile
	bcs @err
	lda fchan+CH_LEN
	cmp want
	bne @err
	lda fchan+CH_LEN+1
	cmp want+1
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
; Screen helpers

; Clears through the DOS, which the Chinese system (BBGCDOS) hooks - the ROM's own
; BiosDispInitial leaves its banded screen and line interrupt off, and the BIOS
; display queue then never drains
ClearScreen:
	lda #DosInitialScreen
	jmp DOS

; Screen off for drawing: the BIOS then writes video memory directly instead of through
; its display queue. Its direct path stages each cell at $5D6D, inside the queue's own
; buffer, so the queue has to be empty first - jobs left in it get overwritten and the
; interrupt handler later runs the garbage (seen writing into $4000-$401F).
CloseScreen:
	lda #BiosWaitDisplay
	jsr BIOS
	ldx #1
	lda #BiosDispOpenClose
	jmp BIOS

OpenScreen:
	ldx #0
	lda #BiosDispOpenClose
	jmp BIOS

; Status line: "NAME.EXT     nnn/nnn  < >  ESC"
ShowStatus:
	ldx #SCREENCOLS-1
	lda #' '
@clr:
	sta status,x
	dex
	bpl @clr
	ldx #0
@name:
	cpx pathLen
	bcs @num
	lda path,x
	sta status,x
	inx
	bne @name
@num:
	ldx index
	inx
	txa
	ldx #13
	jsr PutNumber
	lda #'/'
	sta status+16
	lda fileCount
	ldx #17
	jsr PutNumber
	ldx #0
@tail:
	lda statusTail,x
	beq @print
	sta status+21,x
	inx
	bne @tail
@print:
	lda #<status
	ldx #>status
	ldy #SCREENCOLS
	; fall through

; Prints Y characters at A(lo)/X(hi) on the status row
PrintStatus:
	sta strPtr
	stx strPtr+1
	sty strLen
	ldx #STATUSROW
	ldy #0
	lda #BiosSetCursor
	jsr BIOS
	lda #0				; plain string, cursor back to the start
	sta ARG_FC
	lda strLen
	sta ARG_FD
	lda #$0E			; prompt-line colours: white on blue (group 3)
	sta ARG_FE
	lda #$0F
	sta ARG_FF
	ldx strPtr+1
	ldy strPtr
	lda #BiosDispStringAt
	jmp BIOS

; Writes A as 3 right-aligned digits at status+X (status is pre-filled with spaces)
PutNumber:
	ldy #0
@h:
	cmp #100
	bcc @hd
	sbc #100
	iny
	bne @h
@hd:
	sty digits
	ldy #0
@t:
	cmp #10
	bcc @td
	sbc #10
	iny
	bne @t
@td:
	ora #'0'
	sta status+2,x
	lda digits
	beq @noHundreds
	ora #'0'
	sta status,x
	tya
	ora #'0'
	sta status+1,x
	rts
@noHundreds:
	tya
	beq @done
	ora #'0'
	sta status+1,x
@done:
	rts

;----------------------------------------------------------------------------
; Input: waits for a key or joypad press, returns ACT_* in A

GetAction:
	lda #DosCheckKeyBoard		; Z clear = a key is waiting (it stays queued)
	jsr DOS
	beq @pad
	lda #DosReadKeyNoEcho
	jsr DOS
	cmp #0				; 0 or $E0 = extended key, the scan code follows
	beq @extKey
	cmp #$E0
	bne @ascii
@extKey:
	lda #DosReadKeyNoEcho
	jsr DOS
	ldx #extKeysEnd-extKeys-1
@ext:
	cmp extKeys,x
	beq @extHit
	dex
	bpl @ext
	bmi GetAction
@extHit:
	lda extActs,x
	rts
@ascii:
	ldx #asciiKeysEnd-asciiKeys-1
@asc:
	cmp asciiKeys,x
	beq @ascHit
	dex
	bpl @asc
	bmi GetAction
@ascHit:
	lda asciiActs,x
	rts

@pad:
	lda Joystick1Key		; buttons held now; act on the ones newly pressed
	tax
	lda padPrev
	eor #$FF
	stx padPrev
	sta padKeys
	txa
	and padKeys
	beq @wait
	sta padKeys
	ldx #padBitsEnd-padBits-1
@pb:
	lda padKeys
	and padBits,x
	bne @padHit
	dex
	bpl @pb
	bmi @wait
@padHit:
	lda padActs,x
	rts
@wait:
	ldx #0				; short pause between polls (~1.3 ms)
:	dex
	bne :-
	jmp GetAction

;----------------------------------------------------------------------------
	.segment "RODATA"

patAll:		.byte "*.*", 0
magicGrp:	.byte "Grp"
extGrp:		.byte "GRP"
extPct:		.byte "PCT"
magicPct:	.byte "Pct"
msgNone:	.byte "No pictures found.  Press a key."
msgNoneLen	= * - msgNone
statusTail:	.byte "< >  ESC", 0

asciiKeys:	.byte ' ', 13, 'n', 'N', 8, 'p', 'P', 27, 'q', 'Q'
asciiKeysEnd:
asciiActs:	.byte ACT_NEXT, ACT_NEXT, ACT_NEXT, ACT_NEXT, ACT_PREV, ACT_PREV, ACT_PREV, ACT_QUIT, ACT_QUIT, ACT_QUIT

extKeys:	.byte $4D, $50, $51, $4B, $48, $49, $47, $4F
extKeysEnd:
extActs:	.byte ACT_NEXT, ACT_NEXT, ACT_NEXT, ACT_PREV, ACT_PREV, ACT_PREV, ACT_FIRST, ACT_LAST

padBits:	.byte $80, $20, $01, $40, $10, $02, $08
padBitsEnd:
padActs:	.byte ACT_NEXT, ACT_NEXT, ACT_NEXT, ACT_PREV, ACT_PREV, ACT_PREV, ACT_QUIT

;----------------------------------------------------------------------------
	.segment "BSS"

oldMode:	.res 1
fileCount:	.res 1
index:		.res 1
direction:	.res 1
tries:		.res 1
copyY:		.res 1
work:		.res 2
want:		.res 2
rowBytes:	.res 2
drawRows:	.res 1
drawCols:	.res 1
row0:		.res 1
col0:		.res 1
row:		.res 1
col:		.res 1
cellPtr:	.res 2
entryPtr:	.res 2
palCount:	.res 1
palIndex:	.res 1
strPtr:		.res 2
strLen:		.res 1
pathLen:	.res 1
padKeys:	.res 1
padPrev:	.res 1
digits:		.res 1
path:		.res 16
status:		.res SCREENCOLS
found:		.res HEADERSIZE
header:		.res HEADERSIZE
schan:		.res 48
fchan:		.res 48
fileList:	.res MAXFILES*11
rowBuf:		.res SCREENCOLS*CELLSIZE
