; A tiny "game" for the MCP end-to-end test (tests/health_check.py): find the
; RAM byte that holds the player's health, the instruction that decrements it, and a Game Genie code that stops it.
; MMC1 (mapper 1), 4 x 16 KB PRG-ROM (banks 0-2 switch at $8000, bank 3 fixed at $C000), 8 KB CHR-RAM.
;   health  $0347: 16 at the start; pressing A (a new press) is a hit: bank 1's take_hit decrements it (DEC $0347,
;           at the same address as other code in bank 2, so the code needs bank 1's compare value), down to 0.
;   decoys  $0346 frames (up every frame), $0350 timer (down every 8 frames, hit or not), $0351 hits (up on a hit).
;   screen  the health as a red bar of 16 cells on black (row 14).
; Build: ca65 health.s && ld65 -C health.cfg -o health.nes health.o
.segment "HEADER"
    .byte "NES", $1A, 4, 0, $10, $00, 0,0,0,0,0,0,0,0 ; 4 x 16 KB PRG, CHR-RAM, mapper 1, vertical

health = $0347
frames = $0346
timer  = $0350
hits   = $0351

.segment "ZEROPAGE"
pad:     .res 1
prev:    .res 1
pressed: .res 1
vblank:  .res 1
tmp:     .res 1

; --- bank 0 (mapped at $8000 most of the time): the timer -------------------------------------------------------
.segment "BANK0"
update_timer:
    lda frames
    and #7
    bne :+
    dec timer
:   rts

; --- bank 1: the hit ----------------------------------------------------------------------------------------------
.segment "BANK1"
    .res $0100, $00
take_hit:                   ; $8100
    inc hits
    lda health
    beq :+
    dec health              ; the instruction to find ($8108: CE 47 03)
:   rts

; --- bank 2: other code at the same addresses (never called) ------------------------------------------------------
.segment "BANK2"
    .res $0100, $00
bonus:                      ; $8100
    inc hits
    lda health
    nop
    nop
    lda health              ; $8108: AD 47 03 -- not CE: an 8-letter code with compare CE leaves this alone
    rts

; --- bank 3 (fixed at $C000) --------------------------------------------------------------------------------------
.segment "FIXED"
; MMC1: write A's low 5 bits to the register at the address X selects (0: control $8000, 1: PRG bank $E000)
mmc1_control:
    sta $8000
    lsr a
    sta $8000
    lsr a
    sta $8000
    lsr a
    sta $8000
    lsr a
    sta $8000
    rts
mmc1_prg:
    sta $E000
    lsr a
    sta $E000
    lsr a
    sta $E000
    lsr a
    sta $E000
    lsr a
    sta $E000
    rts

reset:
    sei
    cld
    ldx #$FF
    txs
    lda #$80
    sta $8000               ; MMC1 reset
    lda #$0E                ; vertical mirroring, PRG: 16 KB at $8000 switched, $C000 fixed; CHR 8 KB
    jsr mmc1_control
    lda #0
    jsr mmc1_prg
    lda #0
    sta $2000
    sta $2001
    bit $2002
:   bit $2002               ; two vblanks: the PPU is ready
    bpl :-
:   bit $2002
    bpl :-

    lda #0                  ; RAM
    tax
:   sta $0000,x
    sta $0300,x
    inx
    bne :-
    lda #16
    sta health
    lda #$80
    sta timer

    lda #$00                ; CHR-RAM: tile 0 blank, tile 1 solid (color 1)
    sta $2006
    sta $2006
    ldx #0
:   sta $2007
    inx
    cpx #16
    bne :-
    lda #$FF
    ldx #0
:   sta $2007
    inx
    cpx #8
    bne :-
    lda #$00
:   sta $2007
    inx
    cpx #16
    bne :-

    lda #$20                ; nametable $2000 and its attributes: tile 0
    sta $2006
    lda #$00
    sta $2006
    ldy #4
    ldx #0
:   sta $2007
    inx
    bne :-
    dey
    bne :-

    lda #$3F                ; palette: black, red
    sta $2006
    lda #$00
    sta $2006
    lda #$0F
    sta $2007
    lda #$16
    sta $2007

    lda #$80                ; NMI on
    sta $2000

main:
    lda #0
    sta vblank
:   lda vblank
    beq :-
    inc frames
    jsr update_timer        ; (bank 0)
    jsr read_pad
    lda pressed
    and #$80                ; A
    beq main
    lda #1
    jsr mmc1_prg
    jsr take_hit            ; (bank 1)
    lda #0
    jsr mmc1_prg
    jmp main

read_pad:
    lda #1
    sta $4016
    lda #0
    sta $4016
    ldx #8
:   lda $4016
    lsr a
    rol pad                 ; A ends up in bit 7
    dex
    bne :-
    lda prev
    eor #$FF
    and pad
    sta pressed
    lda pad
    sta prev
    rts

nmi:
    pha
    txa
    pha
    lda #$21                ; the health bar: row 14, columns 8-23
    sta $2006
    lda #$C8
    sta $2006
    ldx #0
:   lda #0
    cpx health
    bcs :+
    lda #1
:   sta $2007
    inx
    cpx #16
    bne :--
    lda #0
    sta $2005
    sta $2005
    lda #$80
    sta $2000
    lda #$0A                ; background on (left column too)
    sta $2001
    inc vblank
    pla
    tax
    pla
irq:
    rti

.segment "VECTORS"
    .word nmi, reset, irq
