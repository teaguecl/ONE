; MMC5 PCM read mode test ROM: does a nonzero byte read at $8000-$BFFF become the PCM output? Mesen's
; CPU trace could not show it (tests/exrom/mmc5pcm.nes), a recording of the sound can. Six parts of about 0.9 s, 0.3 s of silence after
; each, a square wave at about 430 Hz whenever the output should alternate between $40 and $C0 (the level of tests/exrom/mmc5audio.nes's PCM square):
;   1  read mode, reads at $A800 ($40) and $A801 ($C0)                   square
;   2  read mode, $5011 written $40 / $C0 (ignored in read mode)          silent
;   3  read mode, reads at $C800 ($40) and $C801 ($C0): outside $8000-$BFFF   silent
;   4  write mode, $5011 written $40 / $C0 (the reference)                square
;   5  read mode, reads at $A800 ($40) and $A802 ($00): a $00 loads nothing   silent (the output stays at $40)
;   6  read mode, reads at $8800 ($40) and $8801 ($C0): the other end of the range   square
; The 2A03's channels are off. Nothing needs pressing. About 8 seconds from power-on.
; Build: ca65 mmc5pcmread.s && ld65 -C mmc5pcmread.cfg -o mmc5pcmread.nes mmc5pcmread.o
.segment "HEADER"
    .byte "NES", $1A, 1, 1, $50, $00, 0,0,0,0,0,0,0,0 ; 16 KB PRG, 8 KB CHR, mapper 5
.segment "ZEROPAGE"
count:  .res 2
fcount: .res 1

; 400 periods of two halves of about 2090 cycles, each half after a read of ADDR1 / ADDR2
.macro read_square addr1, addr2
        .local loop, nodec
        lda #<400
        sta count
        lda #>400
        sta count+1
loop:   lda addr1
        jsr half
        lda addr2
        jsr half
        lda count
        bne nodec
        dec count+1
nodec:  dec count
        lda count
        ora count+1
        bne loop
.endmacro

.macro write_square
        .local loop, nodec
        lda #<400
        sta count
        lda #>400
        sta count+1
loop:   lda #$40
        sta $5011
        jsr half
        lda #$C0
        sta $5011
        jsr half
        lda count
        bne nodec
        dec count+1
nodec:  dec count
        lda count
        ora count+1
        bne loop
.endmacro

.segment "CODE"
reset:  sei
        cld
        ldx #$40
        stx $4017
        ldx #$FF
        txs
        inx
        stx $2000
        stx $2001
        stx $4010
        stx $4015
        ldx #4                  ; the warm-up
        jsr frames
        lda #3
        sta $5100
        lda #$81                ; this ROM bank (the code's) at $8000, $A000 and $C000 too: the data at $E800 is also at $8800 ...
        sta $5114
        sta $5115
        sta $5116
        ldx #30
        jsr frames

        lda #$01                ; read mode
        sta $5010
        read_square $A800, $A801                 ; 1
        jsr quiet
        write_square                            ; 2 (read mode: ignored)
        jsr quiet
        read_square $C800, $C801                ; 3
        jsr quiet
        lda #$00                ; write mode
        sta $5010
        write_square                            ; 4
        jsr quiet
        lda #$01
        sta $5010
        read_square $A800, $A802                ; 5
        jsr quiet
        read_square $8800, $8801                ; 6
        lda #$00
        sta $5010
        jsr quiet

done:   jmp done

quiet:  ldx #18
        jmp frames

; about 2090 cycles
half:   ldx #8
:       ldy #51
:       dey
        bne :-
        dex
        bne :--
        rts

; wait X frames of 29781 cycles
frames: stx fcount
:       jsr frame
        dec fcount
        bne :-
        rts

frame:  tya
        pha
        ldy #23
:       ldx #0
:       dex
        bne :-
        dey
        bne :--
        ldx #34
:       dex
        bne :-
        pla
        tay
        rts

.segment "RODATA"
        .byte $40, $C0, $00

nmi:    rti
irq:    rti

.segment "VECTORS"
    .word nmi, reset, irq
.segment "CHR"
    .res $2000, 0
