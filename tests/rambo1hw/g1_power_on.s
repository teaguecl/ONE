; Group 1: power-on state of the RAMBO-1 (design: docs/test_rom_design.md, section 5).
; Valid only after a power cycle: nothing here writes a mapper register before the checks, and the reset button leaves
; the mapper as it was. Hardware expectations: docs/findings.md F-001 to F-004, F-034, F-035.
.include "common/vars.inc"

rom_title: .asciiz "RAMBO-1 G1: POWER-ON STATE"
rom_note:  .asciiz "POWER-CYCLE FIRST. A RESET GIVES FALSE FAILS."

test_table:
        TEST t_irq,   n_irq,   $FF
        TEST t_prg,   n_prg,   $FF
        TEST t_chr,   n_chr,   $FF
        TEST t_mir,   n_mir,   $FF
        TEST t_latch, n_latch, $FF
        .word 0

n_irq:   .asciiz "IRQ ASSERTED AT POWER-ON"
n_prg:   .asciiz "PRG BANK 1F AT POWER-ON"
n_chr:   .asciiz "CHR BANK 0 AT POWER-ON"
n_mir:   .asciiz "MIRRORING VERTICAL"
n_latch: .asciiz "IRQ LATCH FF (CPU MODE)"

.segment "CODE"

; The mapper's /IRQ is low from power-on until $E000 is written (F-001, F-034). Nothing may touch the PPU before this test:
; A12 activity could clock the counter and assert /IRQ for a different reason. The APU frame IRQ was inhibited at reset.
; code 2: no interrupt arrived; code 3: still interrupting after $E000.
t_irq:  lda #0
        sta irq_count
        cli
        nop
        nop
        nop
        nop
        sei
        sta MAP_IRQOFF          ; acknowledge and disable, so later tests start clean
        lda irq_count
        sta t0
        lda #0
        sta irq_count
        cli
        nop
        nop
        nop
        nop
        sei
        lda t0
        bne @asserted
        sta det0                ; A = 0: got no interrupt
        lda #1
        sta det1                ; expected at least one
        lda #2
        rts
@asserted:
        lda irq_count
        beq @ok
        sta det0
        lda #0
        sta det1
        lda #3
        rts
@ok:    lda #0
        rts

; Windows $8000, $A000, $C000 and $E000 all show bank $1F (F-002). code 2: byte 0 wrong; AT = window high byte.
t_prg:  lda $8000
        ldx #$80
        jsr @chk
        bne @bad
        lda $A000
        ldx #$A0
        jsr @chk
        bne @bad
        lda $C000
        ldx #$C0
        jsr @chk
        bne @bad
        lda $E000
        ldx #$E0
        jsr @chk
        bne @bad
        lda #0
        rts
@chk:   cmp #$1F
        rts
@bad:   sta det0
        lda #$1F
        sta det1
        stx det2
        lda #2
        rts

; Every CHR window shows bank 0 (F-003): identity byte 0 = 0, byte 1 = $FF. code 2: byte 0; code 3: byte 1.
t_chr:  lda #0
        sta t0
@l:     lda t0
        jsr chr_id
        cmp #0
        bne @b0
        cpx #$FF
        bne @b1
        inc t0
        lda t0
        cmp #8
        bcc @l
        lda #0
        rts
@b0:    sta det0
        lda #0
        sta det1
        lda t0
        sta det2
        lda #2
        rts
@b1:    stx det0
        lda #$FF
        sta det1
        lda t0
        sta det2
        lda #3
        rts

; Vertical at power-on whatever the iNES header says (F-004). The header here says horizontal. GOT 1 = horizontal.
t_mir:  jsr mirror_probe
        beq @ok
        sta det0
        lda #0
        sta det1
        lda #2
        rts
@ok:    lda #0
        rts

; Unwritten latch is $FF (F-035): in CPU-cycle mode the first interrupt comes 4*(255+1)+2 = 1026 cycles after $C001.
; The wait loop takes 13 cycles per pass, so about 79 passes; 72..86 accepted. GOT = passes (0 = no interrupt).
t_latch: lda #0
        sta irq_count
        sta t0
        lda #1
        sta MAP_IRQMODE         ; CPU-cycle mode, restart the counter
        sta MAP_IRQON
        cli
@loop:  lda irq_count
        bne @got
        inc t0
        bne @loop
@got:   sei
        sta MAP_IRQOFF
        lda t0
        cmp #72
        bcc @bad
        cmp #87
        bcs @bad
        lda #0
        rts
@bad:   sta det0
        lda #79
        sta det1
        lda #2
        rts

.include "common/lib.s"
