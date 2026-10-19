; MMC5 test ROM for what no recording has shown yet: the PCM channel's IRQ, its read mode, the reads
; of $5010 and $5015. Nothing is heard; a Mesen CPU trace from power-on (tools/mesen/README.md) is the reference: every value a read
; returns is in A on the next line of the trace, an IRQ is the handler running at its exact cycle. No input.
; Build: ca65 mmc5pcm.s && ld65 -C mmc5pcm.cfg -o mmc5pcm.nes mmc5pcm.o
;
; The program is in the last 8 KB bank (ROM bank 1 of the 16 KB). T5 maps that bank at $A000 and $C000 too, so $E800 is also seen at $A800
; and $C800: the bytes there ($12 $00 $34 $00 $56) can be read in the range where read mode takes the PCM byte ($8000-$BFFF) and
; outside it.
;   T0  power-on: $5010 and $5015 read before anything was written
;   T1  write mode, IRQ off: a $5011 write of $55 and of $00; $5010 read twice (flag set? cleared by the read?)
;   T2  IRQ on: $5011 = $00 raises the IRQ; the handler acknowledges with a read of $5010 at once
;   T3  the same, the handler acknowledging at its 4th run (the IRQ stays low-active until then)
;   T4  with the I flag set: pending read before the IRQ is taken (a read clears it: no IRQ); pending then $5010 written 0 and $80
;       (does the flag survive?)
;   T5  read mode ($5010 = $81): reads at $A800 ($12), $A801 ($00: IRQ), $C801 ($00, outside $8000-$BFFF: none), $A802; a $5011
;       write in read mode
;   T6  $5015: both pulses disabled, a length written while disabled; both enabled with lengths 2 (pulse 1) and 10 (pulse 2) and
;       read every ~3000 cycles for 30 reads (the 240 Hz length clock shows in when the bits drop)
.segment "HEADER"
    .byte "NES", $1A, 1, 1, $50, $00, 0,0,0,0,0,0,0,0 ; 16 KB PRG, 8 KB CHR, mapper 5
.segment "ZEROPAGE"
irqcount: .res 1
ack_at:   .res 1
fcount:   .res 1
lcount:   .res 1
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
        stx irqcount
        ldx #1
        stx ack_at
        ldx #4                  ; the warm-up
        jsr frames

        ; T0
        lda $5010
        lda $5015

        ; T1: write mode, IRQ off
        lda #0
        sta $5010
        lda $5010
        lda #$55
        sta $5011
        lda #0
        sta $5011               ; (a $00 write)
        lda $5010
        lda $5010
        jsr gap

        ; T2: IRQ on, acknowledged at once
        lda #$80
        sta $5010
        lda $5010               ; nothing pending
        cli
        lda #0
        sta $5011
        nop
        nop
        nop
        nop
        lda irqcount
        lda $5010
        jsr gap

        ; T3: the handler acknowledges at its 4th run
        lda irqcount
        clc
        adc #4
        sta ack_at
        lda #0
        sta $5011
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        lda irqcount
        lda $5010
        lda #1
        sta ack_at
        jsr gap

        ; T4: with the I flag set
        sei
        lda #$80
        sta $5010
        lda #0
        sta $5011
        lda $5010               ; pending, and the read clears it
        lda $5010
        cli
        nop
        nop
        lda irqcount
        sei
        lda #0
        sta $5011               ; pending again ...
        lda #0
        sta $5010               ; ... IRQ switched off ...
        lda #$80
        sta $5010               ; ... and on
        lda $5010               ; (still pending?)
        cli
        nop
        nop
        lda irqcount
        jsr gap

        ; T5: read mode (ROM at $A000 and $C000 first: after power-on those are PRG-RAM)
        lda #3
        sta $5100
        lda #$81
        sta $5115
        sta $5116
        lda #$81
        sta $5010
        lda $A800               ; $12: the PCM output
        lda $A801               ; $00: the IRQ
        nop
        nop
        nop
        nop
        lda irqcount
        lda $C801               ; $00 outside $8000-$BFFF: nothing
        nop
        nop
        nop
        lda irqcount
        lda $A802
        lda #$7F
        sta $5011               ; (read mode: no effect)
        lda $5010
        lda #0
        sta $5010
        jsr gap

        ; T6: the pulses' length status
        lda #0
        sta $5015
        lda $5015
        lda #$9F                ; no halt, constant volume 15
        sta $5000
        sta $5004
        lda #$18                ; length index 3: 2
        sta $5003
        lda #0                  ; index 0: 10
        sta $5007
        lda $5015               ; (written while disabled: nothing loaded)
        lda #3
        sta $5015
        lda #$18
        sta $5003
        lda #0
        sta $5007
        lda #30
        sta lcount
len:    lda $5015
        jsr wait3000
        dec lcount
        bne len
        lda #0
        sta $5015

done:   jmp done

; the IRQ: counts, and acknowledges (a read of $5010) from its ack_at-th run on
irq:    inc irqcount
        pha
        lda irqcount
        cmp ack_at
        bcc out
        lda $5010
out:    pla
        rti

nmi:    rti

; about 2000 cycles of nothing between two parts
gap:    ldx #0
:       dex
        bne :-
        ldx #0
:       dex
        bne :-
        rts

; about 3000 cycles
wait3000:
        ldx #2
:       ldy #255
:       dey
        bne :-
        dex
        bne :--
        rts

; wait X frames of 29781 cycles (keeps Y)
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
        .byte $12, $00, $34, $00, $56

.segment "VECTORS"
    .word nmi, reset, irq
.segment "CHR"
    .res $2000, 0
