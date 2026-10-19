; Zapper palette sweep ROM (NROM): 12 squares of 32x32 pixels, each in one NES color, 12 colors a page, 6 pages ($00-$3F in
; order; the last page is padded with $0F). Square (row r, column c) shows color r * 3 + c of the page: row r is background
; palette r, column c its entry c + 1. Squares at X 48 / 112 / 176 and Y 16 / 64 / 112 / 160 (make_sweep.py). Button A shows the
; next page, B the previous one (controller 1); sprites at the top left count the page (1-6 white squares).
; Like zapper_free.nes it never stops: every frame (NMI-synced) it reads $4017 in a loop of 1250 polls (about 22 CPU cycles
; each), covering the picture and the 21 lines of light after its last square. The frame's page is in the trace as
; "STA $4018" (A = the page; $4018 does nothing) just before the polls, so tests/zapper/sweep_analyze.py can tell, from a
; Mesen trace log plus the recorded aim (tools/mesen/zapper_record.lua), which colors the Zapper saw light in.
; Build: python3 make_sweep.py && ca65 sweep.s -o sweep.o && ld65 -C sweep.cfg -o sweep.nes sweep.o
PAGES = 6
POLLS = 1250

.segment "HEADER"
    .byte "NES", $1A, 2, 1, $00, $00, 0,0,0,0,0,0,0,0 ; 32 KB PRG, 8 KB CHR, mapper 0

.segment "ZEROPAGE"
page:     .res 1                ; (address $00: sweep_analyze.py)
dirty:    .res 1
pad_prev: .res 1
pad_new:  .res 1
nmis:     .res 1
count:    .res 2
ptr:      .res 2
xpos:     .res 1

.segment "OAM"
oam: .res 256

.segment "RODATA"
.include "sweep_data.inc"

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
        stx page
        stx dirty
        stx pad_prev
        bit $2002
        wait_vblank
        lda #$FF                ; no sprites yet
        ldx #0
:       sta oam,x
        inx
        bne :-
        wait_vblank

        lda #$20                ; the name table and attributes (make_sweep.py)
        sta $2006
        lda #$00
        sta $2006
        lda #<nametable_data
        sta ptr
        lda #>nametable_data
        sta ptr+1
        ldy #0
        ldx #4
:       lda (ptr),y
        sta $2007
        iny
        bne :-
        inc ptr+1
        dex
        bne :-
        jsr update_screen       ; palette and sprites of page 0
        lda #$1E                ; background and sprites on (also in the left 8 pixels)
        sta $2001
        lda #$80                ; NMI on: the frames are timed by it
        sta $2000

next_frame:
        lda nmis                ; wait for the next vblank (the NMI)
:       cmp nmis
        beq :-
        lda #1                  ; controller 1: A = next page, B = previous
        sta $4016
        lda #0
        sta $4016
        lda $4016
        and #1
        sta pad_new
        lda $4016
        and #1
        asl a
        ora pad_new
        sta pad_new
        eor pad_prev            ; the buttons that went down this frame
        and pad_new
        tay
        lda pad_new
        sta pad_prev
        tya
        and #1
        beq no_a
        ldx page
        inx
        cpx #PAGES
        bcc :+
        ldx #0
:       stx page
        lda #1
        sta dirty
no_a:   tya
        and #2
        beq no_b
        ldx page
        bne :+
        ldx #PAGES
:       dex
        stx page
        lda #1
        sta dirty
no_b:   lda dirty
        beq :+
        jsr update_screen
        lda #0
        sta dirty
:
        lda page                ; the marker for the trace log: A = the page
        sta $4018
        lda #0
        sta count
        sta count+1
poll:   lda $4017
        and #$18
        inc count
        bne :+
        inc count+1
:       lda count+1
        cmp #>POLLS
        bne poll
        lda count
        cmp #<POLLS
        bne poll
        jmp next_frame

; The page's palettes and the page number as sprites (in vblank, or with rendering off)
update_screen:
        ldx #31                 ; the first 8 sprites off
        lda #$FF
:       sta oam,x
        dex
        bpl :-
        lda #8
        sta xpos
        ldx #0                  ; page + 1 white squares
        ldy #0
:       lda #8
        sta oam,x
        lda #3
        sta oam+1,x
        lda #0
        sta oam+2,x
        lda xpos
        sta oam+3,x
        clc
        adc #10
        sta xpos
        inx
        inx
        inx
        inx
        iny
        cpy page
        beq :-                  ; (page + 1 squares: the loop runs for y = 0 .. page)
        bcc :-
        lda page
        asl a
        asl a
        asl a
        asl a
        tax
        lda #$3F
        sta $2006
        lda #$00
        sta $2006
        ldy #16
:       lda page_palettes,x
        sta $2007
        inx
        dey
        bne :-
        lda #$3F                ; sprite palette 0: white
        sta $2006
        lda #$10
        sta $2006
        lda #$0F
        sta $2007
        lda #$30
        sta $2007
        sta $2007
        sta $2007
        lda #7                  ; OAM DMA from $0700
        sta $4014
        lda #0                  ; back to scroll 0, 0
        sta $2006
        sta $2006
        sta $2005
        sta $2005
        rts

nmi:    inc nmis
irq:    rti

.segment "VECTORS"
    .word nmi, reset, irq

.segment "CHR"
    .res 16, $00                ; tile 0: color 0
    .res 8, $FF                 ; tile 1: color 1
    .res 8, $00
    .res 8, $00                 ; tile 2: color 2
    .res 8, $FF
    .res 16, $FF                ; tile 3: color 3
