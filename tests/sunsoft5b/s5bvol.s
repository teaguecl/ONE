; Sunsoft 5B volume ladder (mapper 69): channel A, a tone of period $FE (220 Hz), amplitude register 1, 2, ... 15, each for 20 frames with 8 frames of silence between,
; to measure the level of each of the 16 volume steps in a recording. About 8 seconds; the 2A03's channels are off; nothing to press.
; Build: ca65 s5bvol.s -o s5bvol.o && ld65 -C s5b.cfg -o s5bvol.nes s5bvol.o
.segment "HEADER"
    .byte "NES", $1A, 2, 1, $50, $40, 0,0,0,0,0,0,0,0 ; 32 KB PRG, 8 KB CHR, mapper 69

.segment "ZEROPAGE"
count: .res 1

.macro psg reg, value
        lda #reg
        sta $C000
        lda #value
        sta $E000
.endmacro

.segment "CODE"

; wait X frames (rendering is off: the vblank flag is polled)
wait_frames:
:       bit $2002
        bpl :-
:       bit $2002
        bmi :-
        dex
        bne wait_frames
        rts

.macro frames n
        ldx #n
        jsr wait_frames
.endmacro

.macro silence n
        psg 8, 0
        psg 9, 0
        psg 10, 0
        frames n
.endmacro

reset:  sei
        cld
        ldx #$FF
        txs
        lda #0
        sta $2000
        sta $2001
        sta $4010
        lda #$40
        sta $4017
        lda #$00
        sta $4015
        psg 7, $3F
        psg 8, 0
        bit $2002
:       bit $2002
        bpl :-
        frames 20
        psg 0, $FE
        psg 1, 0
        psg 7, $3E
        ldy #1
step:   lda #8
        sta $C000
        sty $E000
        frames 20
        psg 8, 0
        frames 8
        iny
        cpy #16
        bne step
done:   jmp done

nmi:    rti
irq:    rti

.segment "VECTORS"
    .word nmi, reset, irq

.segment "PRGBANKS"
    .res $6000, 0

.segment "CHR"
    .res $2000, 0
