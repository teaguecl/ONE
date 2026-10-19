; Sunsoft 5B sound test ROM (mapper 69): the noise generator and the envelope generator, which Gimmick! never uses. Nothing is drawn; the 2A03's channels are off;
; there is nothing to press. About 22 seconds of sound, each part followed by silence; record it with Mesen's sound recorder (and with ONE: --wav).
;   1  reference: channel A tone, period $FE (220 Hz), volume 15, 1 s
;   2  noise only on channel A, volume 15, noise period 1, 4, 16, 31, 0.75 s each
;   3  noise and tone together (channel A, tone period $40, noise period 8)
;   4  the envelope on channel A (a tone of period $40, amplitude register bit 4 set, envelope period $0800), shapes $00 $04 $08 $09 $0A $0B $0C $0D $0E $0F, 1.2 s each
;   5  three tones at once, volumes 15, 10 and 5
; (YM2149 registers: 0-5 tone periods, 6 noise period, 7 mixer (bit n = tone n off, bit n+3 = noise n off), 8-A amplitudes (bit 4 = envelope), B/C the envelope
; period, D the envelope shape.)
; Build: ca65 s5b.s -o s5b.o && ld65 -C s5b.cfg -o s5b.nes s5b.o
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
        ; PRG bank registers are not needed ($E000-$FFFF is the fixed last bank); command 8 / 9 left alone
        psg 7, $3F              ; everything off
        psg 8, 0
        psg 9, 0
        psg 10, 0
        bit $2002
:       bit $2002
        bpl :-
        frames 20

        ; 1: reference tone
        psg 0, $FE
        psg 1, 0
        psg 7, $3E
        psg 8, $0F
        frames 60
        silence 30

        ; 2: noise only, channel A
        psg 7, $37
        psg 6, 1
        psg 8, $0F
        frames 45
        silence 20
        psg 6, 4
        psg 8, $0F
        frames 45
        silence 20
        psg 6, 16
        psg 8, $0F
        frames 45
        silence 20
        psg 6, 31
        psg 8, $0F
        frames 45
        silence 30

        ; 3: noise and tone together
        psg 0, $40
        psg 6, 8
        psg 7, $36
        psg 8, $0F
        frames 60
        silence 30

        ; 4: envelope shapes on a tone
        psg 0, $40
        psg 7, $3E
        psg 11, $00
        psg 12, $08
        ldy #0
shape:  lda #13
        sta $C000
        lda shapes, y
        sta $E000               ; the shape (restarts the envelope)
        psg 8, $10              ; amplitude: envelope
        frames 72
        silence 20
        iny
        cpy #10
        bne shape

        ; 5: three tones
        psg 0, $FE
        psg 2, $A0
        psg 4, $70
        psg 7, $38
        psg 8, $0F
        psg 9, $0A
        psg 10, $05
        frames 60
        silence 30
done:   jmp done

shapes: .byte $00, $04, $08, $09, $0A, $0B, $0C, $0D, $0E, $0F

nmi:    rti
irq:    rti

.segment "VECTORS"
    .word nmi, reset, irq

.segment "PRGBANKS"
    .res $6000, 0

.segment "CHR"
    .res $2000, 0
