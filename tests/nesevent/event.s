; NES-EVENT (mapper 105, Nintendo World Championships 1990) timer test ROM. The cartridge's countdown timer counts CPU cycles while CHR register 0 bit 4 is
; 0, is held at 0 (and its IRQ off) while the bit is 1, and asks for an IRQ when the 30-bit counter reaches $20000000 (+ the DIP switches << 25, here 0):
; 2^29 = 536,870,912 CPU cycles, five minutes of emulated time.
;   1. CHR 0 = $10 (hold), wait two vblanks.
;   2. CHR 0 = $00: the counter runs ("start 1"); about 5000 cycles later CHR 0 = $10 resets it; about 5000 cycles later CHR 0 = $00 again ("start 2").
;   3. CLI and wait in "JMP *". The IRQ must come 536,870,912 cycles after start 2 (not start 1): the handler at irq writes CHR 0 = $10 (acknowledging it)
;      and counts in $0200; then it waits in "JMP *" again.
; The PRG register / control register are left as after a reset: the first 32KB of ROM at $8000 (CHR 0 bits 1-2 and bit 3 are 0).
; Build: ca65 event.s -o event.o && ld65 -C event.cfg -o event.nes event.o
.segment "HEADER"
    .byte "NES", $1A, 16, 0, $90, $60, 0,0,0,0,0,0,0,0 ; 256 KB PRG, CHR-RAM, mapper 105

.segment "BSS"
irq_count: .res 1

.segment "CODE"

; write A (5 bits) to the MMC1 CHR 0 register, bit 0 first
write_chr0:
        ldx #5
:       sta $A000
        lsr a
        dex
        bne :-
        rts

reset:  sei
        cld
        ldx #$FF
        txs
        lda #0
        sta $2000
        sta $2001
        sta irq_count
        lda #$40
        sta $4017               ; no APU frame IRQ
        lda #0
        sta $4010               ; no DMC IRQ
        lda #$10
        jsr write_chr0          ; hold: counter 0, no IRQ
        bit $2002
:       bit $2002
        bpl :-
:       bit $2002
        bpl :-

        lda #$00
        jsr write_chr0          ; start 1
        jsr delay
        lda #$10
        jsr write_chr0          ; hold: counter reset
        jsr delay
        lda #$00
        jsr write_chr0          ; start 2
        cli
loop:   jmp loop

; about 5000 cycles
delay:  ldy #8
d1:     ldx #0
d2:     dex
        bne d2
        dey
        bne d1
        rts

irq:    pha
        txa
        pha
        lda #$10
        jsr write_chr0          ; acknowledge (the counter is held again)
        inc irq_count
        pla
        tax
        pla
        rti
irq_wait:
        jmp irq_wait

nmi:    rti

.segment "VECTORS"
    .word nmi, reset, irq
