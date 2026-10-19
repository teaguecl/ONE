; MMC5 expansion audio test ROM: plays known sounds on the two MMC5 pulse channels and the PCM
; channel, about 28 seconds from power-on, the 2A03's own channels off except in the last two parts, so that a recording (Mesen's WAV, tools/mesen/README.md)
; and ONE's output can be compared. The picture is blank. Nothing needs pressing.
; Build: ca65 mmc5audio.s && ld65 -C mmc5audio.cfg -o mmc5audio.nes mmc5audio.o
;
; The program sits in the last 8 KB bank (the MMC5 maps the last bank at $E000 at power-on; the first 8 KB of the 16 KB are unused).
; All the timing is in CPU cycles (a frame is 29781 of them), not in vblanks: polling $2002 can miss a vblank, and the recording
; and ONE must line up. Schedule, in frames from power-on (plus the 4 of the warm-up):
;   1  silence                                           30
;   1b $5003 written while disabled, then enabled: silent    20, then 20 off
;   2  pulse 1 alone, A440, 50% duty, volume 15          90, then 20 off
;   3  pulse 2 alone, A440, 25% duty, volume 8           90, then 20 off
;   4  both: A440 and E5 (659 Hz), 50% duty, volume 12   90, then 20 off
;   5  pulse 1 duty 12.5% / 25% / 50% / 75%              4 x 25, then 20 off
;   6  pulse 1 volume 15 12 9 6 3 1                      6 x 15, then 20 off
;   7  pulse 1 pitch: timer 2047 253 63 15               4 x 45, then 20 off
;   8  pulse 1 envelope (decay period 10), retriggered with length index 3, 0, 2, 4   4 x 75, then 20 off
;   9  PCM: a square wave $40 / $C0 (600 periods, about 430 Hz), a sawtooth (250 waves, about 162 Hz),
;      steady levels $00 $40 $80 $C0 $FF (15 frames each)
;  10  the 2A03's pulse 1 alone, A440, 50% duty, volume 15    90, then 20 off
;  11  the 2A03's pulse 1 (A440) with the MMC5's pulse 1 (E5), volume 15 both   90
.segment "HEADER"
    .byte "NES", $1A, 1, 1, $50, $00, 0,0,0,0,0,0,0,0 ; 16 KB PRG, 8 KB CHR, mapper 5
.segment "ZEROPAGE"
count:  .res 2
level:  .res 1
fcount: .res 1
.segment "CODE"
reset:  sei
        cld
        ldx #$40
        stx $4017               ; no APU frame IRQ
        ldx #$FF
        txs
        inx
        stx $2000
        stx $2001
        stx $4010
        stx $4015               ; the 2A03's channels off
        ldx #4                  ; (a few frames: the PPU warm-up)
        jsr frames
        lda #3
        sta $5100               ; PRG mode 3, the last bank at $E000 ($5117 = $FF is the power-on state)
        lda #0
        sta $5010               ; PCM: write mode, no IRQ
        sta $5011
        sta $5015               ; MMC5 pulse channels off

        ; 1 silence
        ldx #30
        jsr frames

        ; 1b a length written while the channel is disabled is not loaded: enabling afterwards stays silent (Mesen: silent)
        lda #$BF
        sta $5000
        lda #253
        sta $5002
        lda #0
        sta $5003
        lda #1
        sta $5015
        ldx #20
        jsr frames
        jsr off

        ; 2 pulse 1 alone: A440 (timer 253), 50% duty, halt, constant volume 15 (enable first: the length counter loads)
        lda #1
        sta $5015
        lda #$BF
        sta $5000
        lda #253
        sta $5002
        lda #0
        sta $5003
        ldx #90
        jsr frames
        jsr off

        ; 3 pulse 2 alone: 25% duty, volume 8
        lda #2
        sta $5015
        lda #$78
        sta $5004
        lda #253
        sta $5006
        lda #0
        sta $5007
        ldx #90
        jsr frames
        jsr off

        ; 4 both: A440 and E5 (timer 169), 50% duty, volume 12
        lda #3
        sta $5015
        lda #$BC
        sta $5000
        sta $5004
        lda #253
        sta $5002
        lda #169
        sta $5006
        lda #0
        sta $5003
        sta $5007
        ldx #90
        jsr frames
        jsr off

        ; 5 pulse 1 duty cycles
        lda #1
        sta $5015
        lda #253
        sta $5002
        lda #0
        sta $5003
        ldy #0
duty:   lda duties,y            ; duty 12.5% 25% 50% 75%, halt, constant volume 12
        sta $5000
        ldx #25
        jsr frames
        iny
        cpy #4
        bne duty
        jsr off

        ; 6 pulse 1 volumes
        lda #1
        sta $5015
        lda #253
        sta $5002
        lda #0
        sta $5003
        ldy #0
vol:    lda volumes,y
        ora #$B0                ; 50% duty, halt, constant volume
        sta $5000
        ldx #15
        jsr frames
        iny
        cpy #6
        bne vol
        jsr off

        ; 7 pulse 1 pitches
        lda #1
        sta $5015
        lda #$BF
        sta $5000
        ldy #0
pitch:  lda timers_lo,y
        sta $5002
        lda timers_hi,y
        sta $5003
        ldx #45
        jsr frames
        iny
        cpy #4
        bne pitch
        jsr off

        ; 8 envelope: duty 50%, no halt (length counter on), envelope decay with period 10
        lda #1
        sta $5015
        lda #$8A
        sta $5000
        lda #253
        sta $5002
        ldy #0
env:    lda lengths,y           ; length index in bits 3-7, timer high 0
        sta $5003               ; (reloads the length counter and restarts the envelope)
        ldx #75
        jsr frames
        iny
        cpy #4
        bne env
        jsr off

        ; 9 PCM: a square wave, 600 periods of two halves of about 2090 cycles
        lda #<600
        sta count
        lda #>600
        sta count+1
square: lda #$40
        sta $5011
        jsr half
        lda #$C0
        sta $5011
        jsr half
        lda count
        bne :+
        dec count+1
:       dec count
        lda count
        ora count+1
        bne square
        lda #0
        sta $5011
        ldx #20
        jsr frames

        ; a sawtooth: level 0..255, 43 cycles a step, so 162 Hz; 250 waves
        lda #<250
        sta count
        lda #>250
        sta count+1
        lda #0
        sta level
saw:    lda level
        sta $5011
        inc level
        ldx #4
:       dex                     ; 19 cycles; 43 a step with the rest
        bne :-
        nop
        nop
        lda level
        bne saw
        lda count
        bne :+
        dec count+1
:       dec count
        lda count
        ora count+1
        bne saw
        lda #0
        sta $5011
        ldx #20
        jsr frames

        ; steady levels
        ldy #0
levels: lda pcm_levels,y
        sta $5011
        ldx #15
        jsr frames
        iny
        cpy #5
        bne levels
        lda #0
        sta $5011

        ; 10 the 2A03's pulse 1 alone (A440, 50% duty, volume 15): the reference for the MMC5's loudness
        lda #1
        sta $4015
        lda #$BF
        sta $4000
        lda #253
        sta $4002
        lda #0
        sta $4003
        ldx #90
        jsr frames
        lda #0
        sta $4015
        ldx #20
        jsr frames

        ; 11 the 2A03's pulse 1 (A440, volume 15) together with the MMC5's pulse 1 (E5, volume 15): how they add
        lda #1
        sta $4015
        sta $5015
        lda #$BF
        sta $4000
        sta $5000
        lda #253
        sta $4002
        lda #169
        sta $5002
        lda #0
        sta $4003
        sta $5003
        ldx #90
        jsr frames
        lda #0
        sta $4015
        sta $5015

done:   jmp done

; wait X frames (keeps Y)
frames: stx fcount
:       jsr frame
        dec fcount
        bne :-
        rts

; one frame of CPU cycles: 29781 with the loop around it (29780.5 on the NES)
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

; the pulse channels off, 20 frames of silence
off:    lda #0
        sta $5015
        ldx #20
        jmp frames

; about 2090 cycles
half:   ldx #8
:       ldy #51
:       dey
        bne :-
        dex
        bne :--
        rts

duties:    .byte $3C, $7C, $BC, $FC
volumes:   .byte 15, 12, 9, 6, 3, 1
timers_lo: .byte $FF, 253, 63, 15
timers_hi: .byte $07, 0, 0, 0
lengths:   .byte 3 << 3, 0 << 3, 2 << 3, 4 << 3
pcm_levels: .byte $00, $40, $80, $C0, $FF

nmi:    rti
irq:    rti

.segment "VECTORS"
    .word nmi, reset, irq
.segment "CHR"
    .res $2000, 0
