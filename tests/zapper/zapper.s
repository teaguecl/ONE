; Zapper test ROM (NROM). The picture is black with one white square, 16x16 pixels at X 128-143, Y 96-111 (tiles
; 16-17 of rows 12-13). The program syncs to the NMI at the start of vblank, then reads $4017 in a tight loop for 900
; iterations (about 25000 CPU cycles, up to scanline ~200 of the next frame) and records every change of bits 3 (light
; sense: 0 = light) and 4 (trigger: 1 = pulled), so one frame shows when in the frame the photodiode saw the square
; and for how long. 8 frames in all. With the Zapper aimed (tests/zapper/zapper_input.txt for `one --input`, same
; aim in Mesen) the values must match Mesen's, read for read.
;
; Results (RAM, read them after `done` = $A5):
;   frames[f * 48 + i * 3]: a change: counter low, counter high, ($4017 AND $18) -- 16 changes at most per frame, the
;                           first at counter 0 (the value of the first read). The counter is the poll iteration
;                           (about 28 CPU cycles: 4 iterations per scanline); it does not count the cycles of
;                           the change's own bookkeeping (a change costs about 25 more).
;   lens[f]:                bytes used in frames[f] (3 per change)
; Build: ca65 zapper.s -o zapper.o && ld65 -C zapper.cfg -o zapper.nes zapper.o
; Free-running variant for aiming by hand (zapper_free.nes): ca65 -D FREERUN zapper.s -o zapper_free.o &&
;   ld65 -C zapper.cfg -o zapper_free.nes zapper_free.o
; It never stops: every frame is measured the same way, and `frames` / `lens` hold the latest frame (block 0). Move the
; mouse over the square (and click) while Mesen logs; ONE replays the recorded aim (tools/mesen/zapper_record.lua).
.ifdef FREERUN
POLLS    = 1000                 ; (as recorded in Mesen: a frame with light can end after the next NMI, then it is skipped)
.else
POLLS    = 900                  ; (a frame with light, two more changes recorded, still ends before the next NMI)
.endif
FRAMES   = 8
BLOCK    = 48

.segment "HEADER"
    .byte "NES", $1A, 2, 1, $00, $00, 0,0,0,0,0,0,0,0 ; 32 KB PRG, 8 KB CHR, mapper 0

.segment "ZEROPAGE"
frame: .res 1
count: .res 2
last:  .res 1
ptr:   .res 2
nmis:  .res 1

.segment "BSS"
frames: .res FRAMES * BLOCK
lens:   .res FRAMES
done:   .res 1

.segment "CODE"
.macro wait_vblank
:       bit $2002
        bpl :-
.endmacro

reset:
        sei
        cld
        ldx #$FF
        txs
        inx
        stx $2000
        stx $2001
        bit $2002
        wait_vblank
        txa                     ; clear the results
        tay
clear:  sta $0200,y
        sta $0300,y
        sta $0400,y
        sta $0500,y
        iny
        bne clear
        wait_vblank

        lda #$3F                ; palette: black background, white for color 3
        sta $2006
        lda #$00
        sta $2006
        ldx #0
pal:    lda palette,x
        sta $2007
        inx
        cpx #4
        bne pal

        lda #$20                ; clear the name table and attributes
        sta $2006
        lda #$00
        sta $2006
        ldy #4
        tax
fill:   sta $2007
        inx
        bne fill
        dey
        bne fill

        lda #$21                ; the square: tiles (16,12) (17,12) (16,13) (17,13)
        sta $2006
        lda #$90
        sta $2006
        lda #1
        sta $2007
        sta $2007
        lda #$21
        sta $2006
        lda #$B0
        sta $2006
        lda #1
        sta $2007
        sta $2007

        lda #$00
        sta $2006               ; scroll 0, 0
        sta $2006
        sta $2005
        sta $2005
        lda #$0A                ; background on (and in the left 8 pixels)
        sta $2001
        lda #$80                ; NMI on: the frames are timed by it (polling $2002 can miss a vblank)
        sta $2000

        lda #<frames
        sta ptr
        lda #>frames
        sta ptr+1
        lda #0
        sta frame

next_frame:
        lda nmis                ; wait for the next vblank (the NMI)
:       cmp nmis
        beq :-
        lda #$FF                ; (never a read value: the first read is recorded)
        sta last
        lda #0
        sta count
        sta count+1
        tay                     ; Y: bytes used in this frame's block
poll:   lda $4017
        and #$18
        cmp last
        beq same
        tax
        stx last
        cpy #BLOCK              ; (block full: the change is not recorded)
        bcs same
        lda count
        sta (ptr),y
        iny
        lda count+1
        sta (ptr),y
        iny
        txa
        sta (ptr),y
        iny
same:   inc count
        bne :+
        inc count+1
:       lda count+1
        cmp #>POLLS
        bne poll
        lda count
        cmp #<POLLS
        bne poll

.ifdef FREERUN
        sty lens                ; the latest frame only
        inc frame
        jmp next_frame
.endif
        ldx frame
        tya
        sta lens,x
        clc
        lda ptr
        adc #BLOCK
        sta ptr
        bcc :+
        inc ptr+1
:       inc frame
        lda frame
        cmp #FRAMES
        bne next_frame
        lda #$A5
        sta done
halt:   jmp halt

nmi:    inc nmis
irq:    rti

palette: .byte $0F, $30, $30, $30

.segment "VECTORS"
    .word nmi, reset, irq

.segment "CHR"
    .res 16, $00                ; tile 0: color 0
    .res 16, $FF                ; tile 1: color 3
