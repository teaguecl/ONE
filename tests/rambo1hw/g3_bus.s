; Group 3: bus behaviour of the register interface (design: docs/test_rom_design.md, section 5).
; Expectations: no bus conflict (F-012), register decode on A0/A13/A14 only with /ROMSEL low, reads never act as writes.
; Meaningful when the PRG ROM output enable follows the RAMBO PRG /CE output, as on an original cart. (The development
; flash cart has a switch, SW2, for this; that is a property of that cart, not of the ROM, and is documented in
; docs/test_rom_design.md section 4.1 and rom/README.md.)
.include "common/vars.inc"

rom_title: .asciiz "RAMBO-1 G3: BUS BEHAVIOUR"
rom_note:  .asciiz "ANY START. PRG OE = RAMBO CE."

test_table:
        TEST t_noconf,   n_noconf,   $FF        ; 0
        TEST t_rmw,      n_rmw,      $FF        ; 1
        TEST t_alias,    n_alias,    $FF        ; 2
        TEST t_noromsel, n_noromsel, $FF        ; 3
        TEST t_reads,    n_reads,    $FF        ; 4
        TEST t_irqrd,    n_irqrd,    $FF        ; 5
        TEST t_openbus,  n_openbus,  $FF        ; 6
        .word 0

n_noconf:   .asciiz "NO BUS CONFLICT"
n_rmw:      .asciiz "READ-MODIFY-WRITE FINAL"
n_alias:    .asciiz "ALIASES A1-A12 DECODE"
n_noromsel: .asciiz "NO ROMSEL, NO REGISTER"
n_reads:    .asciiz "READS DO NOT WRITE"
n_irqrd:    .asciiz "READS LEAVE IRQ ALONE"
n_openbus:  .asciiz "OPEN BUS $4020/5000/6000"

.include "common/banking.s"

.segment "CODE"

; ---------------------------------------------------------------- 0: no bus conflict
; Registers must take the written value even when the ROM byte under the address differs (F-012). Writing R6 = m while
; the window shows bank n (byte 1 of the window is ~n), pairs chosen so that AND or OR with the ROM byte changes m.
; code 2: R6 data wrong; code 3: index write lost its value (ROM byte 0, AND); code 4: same with ROM byte $1F (OR).
cf_n: .byte $1F, $0A, $15, $07, $1B, $04
cf_m: .byte $0A, $15, $07, $1B, $04, $1F

t_noconf:
        lda #6
        sta MAP_BANKSEL
        lda #0
        sta t1
@l:     ldy t1
        lda cf_n,y
        sta MAP_BANKDATA
        lda cf_m,y
        sta MAP_BANKDATA
        lda $8000
        ldy t1
        cmp cf_m,y
        bne @bad
        inc t1
        lda t1
        cmp #6
        bcc @l
        ; index write while the byte at $8000 is 0 (a conflict that ANDs would select R0, not R6)
        lda #6
        sta MAP_BANKSEL
        lda #0
        sta MAP_BANKDATA        ; R6 = 0, window shows bank 0, byte 0 = $00
        lda #6
        sta MAP_BANKSEL
        lda #$0D
        sta MAP_BANKDATA
        lda $8000
        cmp #$0D
        bne @b3
        ; same with byte 0 = $1F (a conflict that ORs would select RF)
        lda #$1F
        sta MAP_BANKDATA
        lda #6
        sta MAP_BANKSEL
        lda #$0C
        sta MAP_BANKDATA
        lda $8000
        cmp #$0C
        bne @b4
        lda #0
        rts
@bad:   sta det0
        lda cf_m,y
        sta det1
        sty det2
        lda #2
        rts
@b3:    sta det0
        lda #$0D
        sta det1
        lda #3
        rts
@b4:    sta det0
        lda #$0C
        sta det1
        lda #4
        rts

; ---------------------------------------------------------------- 1: read-modify-write
; INC/DEC/ASL/LSR read the ROM byte under the register, write it back, then write the changed value. The register must
; end up with the changed value. At $8001 the byte read is ~n for a window showing bank n (n = 5: $FA); R6 keeps 5 bits.
; codes 2-5: INC, DEC, ASL, LSR on $8001; 6: INC $8000 (index 6 -> 7); 7: INC $A000 (mirroring 0 -> 1).
rmw_exp: .byte $1B, $19, $14, $1D

.macro RMW_SETUP
        lda #6
        sta MAP_BANKSEL
        lda #5
        sta MAP_BANKDATA        ; window shows bank 5, byte 1 = $FA
.endmacro

.macro RMW_CHECK idx, code
        lda $8000
        cmp rmw_exp+idx
        beq :+
        sta det0
        lda rmw_exp+idx
        sta det1
        lda #code
        rts
:
.endmacro

t_rmw:
        RMW_SETUP
        inc MAP_BANKDATA
        RMW_CHECK 0, 2
        RMW_SETUP
        dec MAP_BANKDATA
        RMW_CHECK 1, 3
        RMW_SETUP
        asl MAP_BANKDATA
        RMW_CHECK 2, 4
        RMW_SETUP
        lsr MAP_BANKDATA
        RMW_CHECK 3, 5
        ; INC $8000: R6 = 6 so the byte at $8000 is 6; the second write selects index 7
        lda #6
        sta MAP_BANKSEL
        lda #6
        sta MAP_BANKDATA
        lda #7
        sta MAP_BANKSEL
        lda #$0A
        sta MAP_BANKDATA        ; R7 = $0A
        lda #6
        sta MAP_BANKSEL
        inc MAP_BANKSEL         ; writes 6, then 7
        lda #$0F
        sta MAP_BANKDATA        ; must land in R7
        lda $A000
        cmp #$0F
        beq :+
        sta det0
        lda #$0F
        sta det1
        lda #6
        rts
:       ; INC $A000: byte = R7 = $0F? use R7 = $0A: writes $0A (vertical) then $0B (horizontal)
        lda #7
        sta MAP_BANKSEL
        lda #$0A
        sta MAP_BANKDATA
        lda #0
        sta MAP_MIRROR
        inc MAP_MIRROR
        jsr mirror_probe
        sta t5
        lda #0
        sta MAP_MIRROR
        lda t5
        cmp #1
        beq @ok
        sta det0
        lda #1
        sta det1
        lda #7
        rts
@ok:    lda #0
        rts

; ---------------------------------------------------------------- 2: aliases over A1-A12
; The chip has no A1-A12 pins, so writes at $8000|(1<<k) behave as $8000/$8001 and $A000|(1<<k) as $A000.
; code 2: bank register alias k failed (GOT = bank read, EXP = k, AT = k); code 3: mirroring alias k failed.
al_lo: .byte $02,$04,$08,$10,$20,$40,$80,$00,$00,$00,$00,$00
al_hi: .byte $80,$80,$80,$80,$80,$80,$80,$81,$82,$84,$88,$90

t_alias:
        lda #0
        sta t1                  ; alias index 0..11 (k = index + 1)
@bank:  lda #6
        sta MAP_BANKSEL
        lda #$1F
        sta MAP_BANKDATA
        ldy t1
        lda al_lo,y
        sta ptr
        ora #1
        sta ptr2
        lda al_hi,y
        sta ptr+1
        sta ptr2+1
        ldy #0
        lda #6
        sta (ptr),y             ; select R6 through the alias
        lda t1
        clc
        adc #1
        sta t2                  ; data = k
        sta (ptr2),y
        lda $8000
        cmp t2
        beq @bok
        sta det0
        lda t2
        sta det1
        sta det2
        lda #2
        rts
@bok:   inc t1
        lda t1
        cmp #12
        bcc @bank
        lda #6
        sta MAP_BANKSEL
        lda #$1F
        sta MAP_BANKDATA
        lda #0
        sta t1
@mir:   ldy t1
        lda al_lo,y
        sta ptr
        lda al_hi,y
        ora #$20                ; $A000 region
        sta ptr+1
        lda #0
        sta MAP_MIRROR
        ldy #0
        lda #1
        sta (ptr),y
        jsr mirror_probe
        sta t5
        lda #0
        sta MAP_MIRROR
        lda t5
        cmp #1
        beq @mok
        sta det0
        lda #1
        sta det1
        lda t1
        clc
        adc #1
        sta det2
        lda #3
        rts
@mok:   inc t1
        lda t1
        cmp #12
        bcc @mir
        lda #0
        rts

; ---------------------------------------------------------------- 3: writes without /ROMSEL are ignored
; Only A14 = 1 addresses exist below $8000 that look like registers ($4020-$7FFF ~ $C000-$FFFF), so the visible ones
; are the IRQ registers. CPU mode, L = 2. code 2: a write to $6000 or $7FFE (like $E000) released or disabled the IRQ;
; code 3: a write to $6001 (like $E001) enabled it; code 4: setup, no interrupt after the real $E001.
t_noromsel:
        sta MAP_IRQOFF
        lda #2
        sta MAP_IRQLATCH
        lda #1
        sta MAP_IRQMODE
        lda #0
        sta irq_count
        sta MAP_IRQON
        cli
        ldx #20
@w1:    dex
        bne @w1
        sei
        lda irq_count
        bne @a
        sta det0
        lda #1
        sta det1
        lda #4
        rts
@a:     lda #$00                ; writes that would acknowledge and disable if /ROMSEL were ignored
        sta $6000
        sta $7FFE
        sta $4020
        sta $5000
        lda #$01
        sta $4021
        lda #0
        sta irq_count
        cli
        nop
        nop
        nop
        nop
        sei
        lda irq_count
        bne @b
        sta det0
        lda #1
        sta det1
        lda #2
        rts
@b:     sta MAP_IRQOFF          ; real acknowledge and disable
        lda #0
        sta $6001               ; would enable if /ROMSEL were ignored
        sta $7FFF
        sta irq_count
        cli
        ldx #20
@w2:    dex
        bne @w2
        sei
        lda irq_count
        beq @ok
        sta det0
        lda #0
        sta det1
        lda #3
        rts
@ok:    lda #0
        rts

; ---------------------------------------------------------------- 4: reads never act as writes
; The data bus carries the ROM byte on a read. If the chip decoded reads, the index or a bank would change.
rd_lo: .byte $00,$01,$00,$01,$00,$01,$00,$01,$FE,$FF
rd_hi: .byte $80,$80,$A0,$A0,$C0,$C0,$E0,$E0,$9F,$9F

t_reads:
        lda #0
        jsr base_load           ; index 0, 2 KB mode
        jsr fp_save
        ldx #0
@l:     lda rd_lo,x
        sta ptr
        lda rd_hi,x
        sta ptr+1
        ldy #0
        lda (ptr),y
        inx
        cpx #10
        bcc @l
        jsr fp_check
        beq @fine
        rts
@fine:  lda #$22                ; goes to R0 only if the index is still 0
        sta MAP_BANKDATA
        lda #0
        jsr chr_id
        cmp #$22
        beq @ok
        sta det0
        lda #$22
        sta det1
        lda #3
        rts
@ok:    lda #0
        rts

; ---------------------------------------------------------------- 5: reads of the IRQ registers do nothing
; code 2: a read of $E001 enabled the IRQ; code 3: setup, no interrupt after a write to $E001;
; code 4: a read of $E000 released /IRQ. CPU mode with L = 2: expiry 14 cycles after $C001, then every 12.
t_irqrd:
        sta MAP_IRQOFF
        lda #2
        sta MAP_IRQLATCH
        lda #1
        sta MAP_IRQMODE
        lda MAP_IRQON           ; read of $E001
        lda MAP_IRQLATCH        ; read of $C000
        lda MAP_IRQMODE         ; read of $C001
        lda #0
        sta irq_count
        cli
        ldx #20
@w1:    dex
        bne @w1
        sei
        lda irq_count
        beq @ph2
        sta det0
        lda #0
        sta det1
        lda #2
        rts
@ph2:   sta irq_count
        sta MAP_IRQON           ; write of $E001
        cli
        ldx #20
@w2:    dex
        bne @w2
        sei
        lda irq_count
        bne @ph3
        sta det0
        lda #1
        sta det1
        lda #3
        rts
@ph3:   lda MAP_IRQOFF          ; read of $E000: must not acknowledge
        lda #0
        sta irq_count
        cli
        nop
        nop
        nop
        nop
        sei
        lda irq_count
        bne @ok
        sta det0
        lda #1
        sta det1
        lda #4
        rts
@ok:    sta MAP_IRQOFF          ; write: acknowledge and disable
        lda #0
        rts

; ---------------------------------------------------------------- 6: open bus (discovery)
; Reads from addresses nobody drives. The cart has 1 kohm pull-ups on D0-D7, so the value may be $FF, the address high
; byte (classic open bus), or something between. Never a failure; the three values are printed.
t_openbus:
        lda $4020
        sta det0
        lda $5000
        sta det1
        lda $6000
        sta det2
        lda #1
        sta disc
        lda #0
        rts

.include "common/lib.s"
