; Group 4: CPU-cycle IRQ (design: docs/test_rom_design.md, sections 5 and 5.1).
; Hardware expectations: F-018 to F-020, F-024, F-025, F-028 to F-031, F-054, F-057.
;
; Timing method (interrupt-PC capture): after the `$C001` write a sled of NOPs runs with interrupts enabled. The handler
; reads the interrupted PC off the stack, so the expiry time is known in units of one NOP (2 cycles). The same trial is
; run a second time with the `$E001` store one cycle longer (STA abs,Y instead of STA abs, so the sled starts one cycle
; later); the two PCs together give the time to one cycle:
;       tau = a + 2*kA - 2 + par     (kA = NOPs run in trial A, par = 1 if kA = kB, 0 if kA - kB = 1, else invalid)
; `a` is the exact cycle at which the sled starts, counted from the end of the `$C001` write. Only differences between
; taus are asserted, so the fixed offset between the chip's /IRQ edge and the CPU's poll point cancels.
.include "common/vars.inc"

CUSTOM_IRQ = 1

rom_title: .asciiz "RAMBO-1 G4: CPU-CYCLE IRQ"
rom_note:  .asciiz "POWER-ON OR RESET. ANY START."

.zeropage
mvA:    .res 2                  ; trial routine A / B (B has the extra 3-cycle BIT)
mvB:    .res 2
sbA:    .res 2                  ; first NOP of each sled
sbB:    .res 2
a16:    .res 2                  ; cycle at which sled A starts
kA:     .res 1
kB:     .res 1
par:    .res 1
tau:    .res 2
irq_vec: .res 2
lidx:   .res 1
.bss
pcl_buf:  .res 4
pch_buf:  .res 4
tau_lo:   .res 9
tau_hi:   .res 9
tau2_lo:  .res 3
tau2_hi:  .res 3
scratch:  .res 1
.code

; test indices (0-based) are used as dependencies
test_table:
        TEST t_release,  n_release,  $FF        ; 0
        TEST t_first,    n_first,    $FF        ; 1
        TEST t_stable,   n_stable,   1          ; 2
        TEST t_period,   n_period,   1          ; 3
        TEST t_late,     n_late,     1          ; 4
        TEST t_ackenb,   n_ackenb,   1          ; 5
        TEST t_c001,     n_c001,     1          ; 6
        TEST t_window,   n_window,   1          ; 7
        TEST t_switch,   n_switch,   $FF        ; 8
        TEST t_abs,      n_abs,      1          ; 9
        TEST t_e1cyc,    n_e1cyc,    0          ; 10
        TEST t_e0cyc,    n_e0cyc,    0          ; 11
        TEST t_f87,      n_f87,      0          ; 12
        .word 0

n_release: .asciiz "IRQ RELEASE AND DISABLE"
n_first:   .asciiz "FIRST EXPIRY SLOPE 4/L"
n_stable:  .asciiz "THRESHOLD REPEATABLE"
n_period:  .asciiz "REPEAT PERIOD 4(L+1)"
n_late:    .asciiz "EXPIRY WHILE DISABLED"
n_ackenb:  .asciiz "E000+E001 KEEP COUNT"
n_c001:    .asciiz "C001 RESTARTS, KEEPS IRQ"
n_window:  .asciiz "LATCH TAKEN BY CYCLE 5"
n_switch:  .asciiz "C001 MODE SWITCH CANCELS"
n_abs:     .asciiz "FIRST EXPIRY ABSOLUTE"
n_e1cyc:   .asciiz "E001 IN EXPIRY CYCLE"
n_e0cyc:   .asciiz "E000 IN EXPIRY CYCLE"
n_f87:     .asciiz "A12 TO CPU, FILTER LOW"

L_tab:   .byte 0, 1, 2, 3, 4, 8, 16, 40, 255
expd_lo: .byte 4, 4, 4, 4, 16, 32, 96, $5C      ; tau(L[i+1]) - tau(L[i]) = 4 x (L[i+1] - L[i])
expd_hi: .byte 0, 0, 0, 0, 0, 0, 0, 3

.macro SETIRQ handler
        lda #<handler
        sta irq_vec
        lda #>handler
        sta irq_vec+1
.endmacro

.macro SETPAIR na, nb, sa, sb
        lda #<na
        sta mvA
        lda #>na
        sta mvA+1
        lda #<nb
        sta mvB
        lda #>nb
        sta mvB+1
        lda #<sa
        sta sbA
        lda #>sa
        sta sbA+1
        lda #<sb
        sta sbB
        lda #>sb
        sta sbB+1
.endmacro

; ---------------------------------------------------------------- interrupt handlers
irq:    jmp (irq_vec)

; records the interrupted PC, then returns with I set (no storm)
h_plain:
        pha
        txa
        pha
        tya
        pha
        tsx
        ldy irq_count
        cpy #4
        bcs @skip
        lda $0105,x
        sta pcl_buf,y
        lda $0106,x
        sta pch_buf,y
@skip:  inc irq_count
        lda $0104,x
        ora #$04
        sta $0104,x
        pla
        tay
        pla
        tax
        pla
        rti

; first interrupt: acknowledge and re-enable at once and return (about 31 cycles, shorter than the period for L >= 8, so the
; second expiry is caught while the CPU is running again; clobbers A, which the trial routines do not use). Second interrupt: recorded.
h_reack:
        sta MAP_IRQOFF
        sta MAP_IRQON
        lda irq_count
        bne @second
        inc irq_count
        rti
@second:
        jmp h_plain

; ---------------------------------------------------------------- trial routines
; MEAS name, sled label, B variant (first $E001 store 1 cycle longer)?, coarse delay?, mode, M, X1, X2, aval
; mode 0: `$E001` right after `$C001`, then (coarse) delay, sled
;      1: no early enable; M NOPs, `$E001`, sled   (late enable)
;      2: `$E001`, M NOPs, `lda #aval`, `sta X1`, `sta X2`, sled   (a = 14 + 2M)
;      6: M NOPs, `stx $C000` (X = t6), `$E001`, sled   (latch written 4 + 2M cycles after `$C001`; a = 8 + 2M)
.macro ENB bv
.if bv
        sta MAP_IRQON,y
.else
        sta MAP_IRQON
.endif
.endmacro

.macro STB bv, addr
.if bv
        sta addr,y
.else
        sta addr
.endif
.endmacro

.macro MEAS name, sled, bvar, coarse, mode, M, X1, X2, aval
name:
        sei
        sta MAP_IRQOFF
        lda t0
        sta MAP_IRQLATCH
        lda #0
        sta irq_count
        ldx t6
        ldy #0
        lda #1
        cli
        sta MAP_IRQMODE         ; the write that starts the counter
.if mode = 1
        .repeat M
        nop
        .endrepeat
        ENB bvar
.elseif mode = 6
        .repeat M
        nop
        .endrepeat
        stx MAP_IRQLATCH
        ENB bvar
.else
  .if mode = 0
        ENB bvar
  .else
        sta MAP_IRQON
  .endif
  .if coarse
        ldx t1
        jsr delay_q
  .endif
  .if mode = 2
        .repeat M
        nop
        .endrepeat
        lda #aval
        sta X1
        STB bvar, X2
  .endif
.endif
sled:
        .repeat 100
        nop
        .endrepeat
        sei
        rts
.endmacro

MEAS mb_sa, sl_sa, 0, 0, 0, 0, $0200, $0200, 0        ; base, small
MEAS mb_sb, sl_sb, 1, 0, 0, 0, $0200, $0200, 0
MEAS mb_la, sl_la, 0, 1, 0, 0, $0200, $0200, 0        ; base, coarse delay first
MEAS mb_lb, sl_lb, 1, 1, 0, 0, $0200, $0200, 0
MEAS ml_a,  sl_ma, 0, 0, 1, 11, $0200, $0200, 0       ; late enable (L = 4: enable at 26 > expiry at 22)
MEAS ml_b,  sl_mb, 1, 0, 1, 11, $0200, $0200, 0
MEAS mc_a,  sl_ca, 0, 0, 2, 2, $0200, $0200, 1        ; control: stores to RAM
MEAS mc_b,  sl_cb, 1, 0, 2, 2, $0200, $0200, 1
MEAS mk_a,  sl_ka, 0, 0, 2, 2, MAP_IRQOFF, MAP_IRQON, 1   ; $E000 then $E001 before the expiry
MEAS mk_b,  sl_kb, 1, 0, 2, 2, MAP_IRQOFF, MAP_IRQON, 1
MEAS mr_a,  sl_ra, 0, 0, 2, 2, MAP_IRQMODE, $0200, 1      ; second $C001 (CPU mode) before the expiry
MEAS mr_b,  sl_rb, 1, 0, 2, 2, MAP_IRQMODE, $0200, 1
MEAS mw0_a, sl_w0a, 0, 0, 6, 0, $0200, $0200, 0       ; latch written 4 cycles after $C001
MEAS mw0_b, sl_w0b, 1, 0, 6, 0, $0200, $0200, 0
MEAS mw1_a, sl_w1a, 0, 0, 6, 1, $0200, $0200, 0       ; 6 cycles after
MEAS mw1_b, sl_w1b, 1, 0, 6, 1, $0200, $0200, 0
MEAS ms2,   sl_s2,  0, 0, 2, 2, MAP_IRQMODE, $0200, 0     ; $C001 := 0 (switch to A12 mode) at 14, before the expiry (22)
MEAS ms4,   sl_s4,  0, 0, 2, 4, MAP_IRQMODE, $0200, 0     ; at 18
MEAS ms8,   sl_s8,  0, 0, 2, 8, MAP_IRQMODE, $0200, 0     ; at 26, after the expiry

; ---------------------------------------------------------------- measurement harness
call_a: jmp (mvA)
call_b: jmp (mvB)

calc_k:                         ; ptr = sled base, t5 = which interrupt -> A = NOPs executed, C set = invalid
        ldx t5
        cpx irq_count
        bcs @err
        lda pcl_buf,x
        sec
        sbc ptr
        sta t2
        lda pch_buf,x
        sbc ptr+1
        bne @err
        lda t2
        cmp #100
        bcs @err
        clc
        rts
@err:   sec
        rts

; runs trial A and trial B; A = 0 and tau set, or 2 (no usable interrupt) / 4 (bad parity)
meas_run:
        jsr call_a
        lda sbA
        sta ptr
        lda sbA+1
        sta ptr+1
        jsr calc_k
        bcs @e2
        sta kA
        jsr call_b
        lda sbB
        sta ptr
        lda sbB+1
        sta ptr+1
        jsr calc_k
        bcs @e2
        sta kB
        lda kA
        sec
        sbc kB
        cmp #1
        bne @n1
        lda #0
        sta par
        beq @go
@n1:    cmp #0
        bne @e4
        lda #1
        sta par
@go:    lda kA
        asl a
        clc
        adc par
        sta t2
        lda a16
        clc
        adc t2
        sta tau
        lda a16+1
        adc #0
        sta tau+1
        lda tau
        sec
        sbc #2
        sta tau
        lda tau+1
        sbc #0
        sta tau+1
        lda #0
        rts
@e2:    lda irq_count
        sta det0
        lda pch_buf
        sta det1
        lda pcl_buf
        sta det2
        lda #2
        rts
@e4:    lda kA
        sta det0
        lda kB
        sta det1
        lda #4
        rts

; t0 = L: picks the small path (L <= 36: sled right after `$E001`, a = 4) or the coarse path (q delay blocks of 32 cycles
; first, a = 32q + 18) so that the expiry falls within the first ~50 cycles of the sled
meas_L:
        lda t0
        cmp #37
        bcs @large
        SETPAIR mb_sa, mb_sb, sl_sa, sl_sb
        lda #4
        sta a16
        lda #0
        sta a16+1
        jmp meas_run
@large: asl a
        sta t2
        lda #0
        rol a
        sta t3
        asl t2
        rol t3                  ; 4L
        lda t2
        sec
        sbc #34
        sta t2
        lda t3
        sbc #0
        sta t3
        lda t2
        lsr a
        lsr a
        lsr a
        lsr a
        lsr a
        sta t1
        lda t3
        asl a
        asl a
        asl a
        ora t1
        sta t1                  ; q = (4L - 34) / 32
        sta t2
        lda #0
        sta t3
        asl t2
        rol t3
        asl t2
        rol t3
        asl t2
        rol t3
        asl t2
        rol t3
        asl t2
        rol t3                  ; 32q
        lda t2
        clc
        adc #18
        sta a16
        lda t3
        adc #0
        sta a16+1
        SETPAIR mb_la, mb_lb, sl_la, sl_lb
        jmp meas_run

; difference helpers: delta = tau - (tau_lo,tau_hi)[X] into t2:t3
delta_from:
        lda tau
        sec
        sbc tau_lo,x
        sta t2
        lda tau+1
        sbc tau_hi,x
        sta t3
        rts

.include "common/cycles.s"

; ---------------------------------------------------------------- 0: release and disable
; CPU mode, L = 2, disabled: expiries are lost (no interrupt); enabled: interrupt; `$E000` releases /IRQ and disables it.
; code 2: interrupt while disabled; 3: none after `$E001`; 4: still interrupting after `$E000`.
t_release:
        SETIRQ h_plain
        sei
        sta MAP_IRQOFF
        lda #2
        sta MAP_IRQLATCH
        lda #0
        sta irq_count
        lda #1
        sta MAP_IRQMODE
        cli
        ldx #25
@w1:    dex
        bne @w1
        sei
        lda irq_count
        beq @e1
        sta det0
        lda #0
        sta det1
        lda #2
        rts
@e1:    sta MAP_IRQON
        lda #0
        sta irq_count
        cli
        ldx #25
@w2:    dex
        bne @w2
        sei
        lda irq_count
        bne @e2
        sta det0
        lda #1
        sta det1
        lda #3
        rts
@e2:    sta MAP_IRQOFF
        lda #0
        sta irq_count
        cli
        ldx #25
@w3:    dex
        bne @w3
        sei
        lda irq_count
        beq @ok
        sta det0
        lda #0
        sta det1
        lda #4
        rts
@ok:    lda #0
        rts

; ---------------------------------------------------------------- 1: first expiry, slope 4 cycles per latch step (F-018, F-054)
; tau(L) measured for L = 0 1 2 3 4 8 16 40 255 and consecutive differences compared with 4 x (L step).
; code 2: slope wrong (GOT/EXP = low bytes, AT = index of the first L); code 3: measurement failed (GOT = meas code, AT = index)
t_first:
        SETIRQ h_plain
        lda #0
        sta t5
        sta lidx
@m:     ldy lidx
        lda L_tab,y
        sta t0
        jsr meas_L
        beq @ok
        sta det0
        lda lidx
        sta det2
        lda #3
        rts
@ok:    ldy lidx
        lda tau
        sta tau_lo,y
        lda tau+1
        sta tau_hi,y
        tya
        beq @next               ; first point: nothing to compare with
        tay
        dey                     ; previous index
        lda tau_lo+1,y
        sec
        sbc tau_lo,y
        sta t2
        lda tau_hi+1,y
        sbc tau_hi,y
        sta t3
        lda t2
        cmp expd_lo,y
        bne @bad
        lda t3
        cmp expd_hi,y
        bne @bad
@next:  inc lidx
        lda lidx
        cmp #9
        bcc @m
        lda #0
        rts
@bad:   lda t2
        sta det0
        lda expd_lo,y
        sta det1
        sty det2
        lda #2
        rts

; ---------------------------------------------------------------- 2: threshold repeatable
; L = 4 three more times and L = 255 twice must give exactly the first result (power-up phase does not matter for M2 counting).
t_stable:
        SETIRQ h_plain
        lda #0
        sta t5
        lda #3
        sta lidx
@r:     lda #4
        sta t0
        jsr meas_L
        bne @m
        ldx #4
        jsr delta_from
        lda t2
        ora t3
        bne @bad
        dec lidx
        bne @r
        lda #2
        sta lidx
@r2:    lda #255
        sta t0
        jsr meas_L
        bne @m
        ldx #8
        jsr delta_from
        lda t2
        ora t3
        bne @bad
        dec lidx
        bne @r2
        lda #0
        rts
@m:     sta det0
        lda #3
        rts
@bad:   lda t2
        sta det0
        lda #0
        sta det1
        lda #2
        rts

; ---------------------------------------------------------------- 3: repeat period 4(L+1) (F-019)
; tau2 = time of the second interrupt, from the NOPs run (the first handler's time is the same for every L, so it cancels).
; tau2 = 8(L+1) + 2 + const, so tau2(16) - tau2(8) = 64 and tau2(24) - tau2(16) = 64 (trial A only: 2-cycle bins are
; enough because both differences are even). code 2: first difference wrong, 3: second wrong, 4: no second interrupt.
t_period:
        SETIRQ h_reack
        lda #1
        sta t5
        lda #0
        sta lidx
@m:     ldy lidx
        lda @ls,y
        sta t0
        SETPAIR mb_sa, mb_sb, sl_sa, sl_sb
        jsr call_a
        lda sbA
        sta ptr
        lda sbA+1
        sta ptr+1
        jsr calc_k
        bcc @ok
        lda #0
        sta t5
        SETIRQ h_plain
        lda irq_count
        sta det0
        lda #4
        rts
@ok:    ldy lidx
        asl a
        sta tau2_lo,y
        inc lidx
        lda lidx
        cmp #3
        bcc @m
        lda #0
        sta t5
        SETIRQ h_plain
        lda tau2_lo+1
        sec
        sbc tau2_lo
        sta t2
        lda #64
        cmp t2
        beq @d2
        sta det1
        lda t2
        sta det0
        lda #2
        rts
@d2:    lda tau2_lo+2
        sec
        sbc tau2_lo+1
        sta t2
        lda #64
        cmp t2
        beq @ok2
        sta det1
        lda t2
        sta det0
        lda #3
        rts
@ok2:   lda #0
        rts
@ls:    .byte 8, 16, 24

; ---------------------------------------------------------------- 4: expiry while disabled is lost (F-031)
; L = 4, `$E001` written 26 cycles after `$C001` (expiry 22 is lost); the interrupt must come at the next expiry, 20 cycles
; later than the normal first expiry (not at once). code 2: GOT = tau - tau(4); 3: measurement failed.
t_late:
        SETIRQ h_plain
        lda #0
        sta t5
        lda #4
        sta t0
        SETPAIR ml_a, ml_b, sl_ma, sl_mb
        lda #26
        sta a16
        lda #0
        sta a16+1
        jsr meas_run
        beq @ok
        sta det0
        lda #3
        rts
@ok:    ldx #4
        jsr delta_from
        lda t3
        bne @bad
        lda t2
        cmp #20
        beq @fine
@bad:   lda t2
        sta det0
        lda #20
        sta det1
        lda #2
        rts
@fine:  lda #0
        rts

; ---------------------------------------------------------------- 5: $E000 and $E001 do not disturb the counter (F-029, F-030)
; L = 4: after `$C001` and `$E001`, two stores (control: to RAM; test: `$E000` then `$E001`) 14 cycles in, before the
; expiry at 22. tau must equal tau(4) in both. code 2: control differs (harness problem); 3: `$E000`/`$E001` moved the expiry.
t_ackenb:
        SETIRQ h_plain
        lda #0
        sta t5
        lda #4
        sta t0
        SETPAIR mc_a, mc_b, sl_ca, sl_cb
        lda #18
        sta a16
        lda #0
        sta a16+1
        jsr meas_run
        bne @mf
        ldx #4
        jsr delta_from
        lda t2
        ora t3
        beq @ctl
        lda t2
        sta det0
        lda #0
        sta det1
        lda #2
        rts
@ctl:   SETPAIR mk_a, mk_b, sl_ka, sl_kb
        lda #18
        sta a16
        lda #0
        sta a16+1
        jsr meas_run
        bne @mf
        ldx #4
        jsr delta_from
        lda t2
        ora t3
        beq @ok
        lda t2
        sta det0
        lda #0
        sta det1
        lda #3
        rts
@ok:    lda #0
        rts
@mf:    sta det0
        lda #4
        rts

; ---------------------------------------------------------------- 6: $C001 restarts the counter and does not release /IRQ (F-024, F-025)
; Part 1: a second `$C001` written 14 cycles after the first moves the expiry later by exactly 14 (code 2).
; Part 2: after an interrupt, `$C001` leaves /IRQ low: the interrupt is taken again at once (code 3 = released).
t_c001:
        SETIRQ h_plain
        lda #0
        sta t5
        lda #4
        sta t0
        SETPAIR mr_a, mr_b, sl_ra, sl_rb
        lda #18
        sta a16
        lda #0
        sta a16+1
        jsr meas_run
        beq @ok
        sta det0
        lda #4
        rts
@ok:    ldx #4
        jsr delta_from
        lda t3
        bne @bad
        lda t2
        cmp #14
        beq @p2
@bad:   lda t2
        sta det0
        lda #14
        sta det1
        lda #2
        rts
@p2:    jsr mb_sa               ; interrupt taken, handler leaves I set
        lda irq_count
        beq @none
        lda #1
        sta MAP_IRQMODE
        lda #0
        sta irq_count
        cli
        nop
        nop
        nop
        sei
        lda irq_count
        bne @pass
        sta det0
        lda #1
        sta det1
        lda #3
        rts
@none:  sta det0
        lda #4
        rts
@pass:  lda #0
        rts

; ---------------------------------------------------------------- 7: the latch is taken about 5 cycles after $C001 (F-028)
; L = 2 written normally; `$C000` := 8 written 4 cycles after `$C001` is used for the first load (tau = tau(8)); written 6
; cycles after it is not (tau = tau(2)). code 2: late-by-4 write missed; 3: write after 6 cycles still used; 4: measurement failed.
t_window:
        SETIRQ h_plain
        lda #0
        sta t5
        lda #2
        sta t0
        lda #8
        sta t6
        SETPAIR mw0_a, mw0_b, sl_w0a, sl_w0b
        lda #8
        sta a16
        lda #0
        sta a16+1
        jsr meas_run
        bne @mf
        ldx #5                  ; tau(8)
        jsr delta_from
        lda t2
        ora t3
        beq @s2
        lda t2
        sta det0
        lda #0
        sta det1
        lda #2
        rts
@s2:    SETPAIR mw1_a, mw1_b, sl_w1a, sl_w1b
        lda #10
        sta a16
        lda #0
        sta a16+1
        jsr meas_run
        bne @mf
        ldx #2                  ; tau(2)
        jsr delta_from
        lda t2
        ora t3
        beq @ok
        lda t2
        sta det0
        lda #0
        sta det1
        lda #3
        rts
@ok:    lda #0
        rts
@mf:    sta det0
        lda #4
        rts

; ---------------------------------------------------------------- 8: $C001 with bit 0 = 0 cancels a pending CPU expiry (F-057)
; L = 4, first expiry at 22. Switch to A12 mode written at 14 or 18 cycles: no interrupt in the next 200 cycles. At 26
; (after the expiry): exactly one interrupt. code 2/3: interrupt although the switch came first; 4: the interrupt at 22 was lost.
t_switch:
        SETIRQ h_plain
        lda #4
        sta t0
        lda #0
        sta t6
        jsr ms2
        lda irq_count
        beq @s4
        sta det0
        lda #0
        sta det1
        lda #2
        rts
@s4:    jsr ms4
        lda irq_count
        beq @s8
        sta det0
        lda #0
        sta det1
        lda #3
        rts
@s8:    jsr ms8
        lda irq_count
        cmp #1
        beq @ok
        sta det0
        lda #1
        sta det1
        lda #4
        rts
@ok:    sta MAP_IRQOFF
        lda #1
        sta MAP_IRQMODE         ; back to CPU mode for anything that follows
        lda #0
        rts

; ---------------------------------------------------------------- 9: absolute first expiry (discovery)
; tau(L = 0), tau(L = 4), tau(L = 255), low bytes. The chip expiry is 4(L+1)+2 = 6, 22, 1026 (low byte 02) after the `$C001`
; write; a constant offset from that is the CPU's poll point and the write cycle. Printed, never a failure.
t_abs:
        lda tau_lo
        sta det0
        lda tau_lo+4
        sta det1
        lda tau_lo+8
        sta det2
        lda #1
        sta disc
        lda #0
        rts


; ---------------------------------------------------------------- 10, 11: a write in the expiry's own cycle (F-071, F-072)
; L = 4: the first expiry is 22 cycles after the `$C001` write cycle. RACE trials put one write in a chosen cycle j after `$C001` (the write cycle
; is the last cycle of the STA: j = 4 + 2M for STA abs after M NOPs, 5 + 2M for STA abs,Y) and run a sled with interrupts enabled; the NOP at
; which the interrupt is taken says when /IRQ fell. kind 0/1: `$E001` in cycle 22 / 23 with the interrupt disabled before (the NOP count M = 9);
; kind 2: `$E001` right after `$C001`, then `$E000` in cycle 22 (M = 7); kind 3: the same with a store to RAM instead of the `$E000` (control).
.macro RACE name, sled, kind, M
name:
        sei
        sta MAP_IRQOFF
        lda t0
        sta MAP_IRQLATCH
        lda #0
        sta irq_count
        ldy #0
        lda #1
        cli
        sta MAP_IRQMODE         ; cycle 0
.if kind = 0
        .repeat M
        nop
        .endrepeat
        sta MAP_IRQON           ; write in cycle 4 + 2M
.elseif kind = 1
        .repeat M
        nop
        .endrepeat
        sta MAP_IRQON,y         ; write in cycle 5 + 2M
.elseif kind = 2
        sta MAP_IRQON
        .repeat M
        nop
        .endrepeat
        sta MAP_IRQOFF          ; write in cycle 8 + 2M
.else
        sta MAP_IRQON
        .repeat M
        nop
        .endrepeat
        sta scratch             ; control: same timing, no write to the chip
.endif
sled:
        .repeat 100
        nop
        .endrepeat
        sei
        rts
.endmacro

RACE r_e1a, sl_e1a, 0, 9        ; $E001 in cycle 22
RACE r_e1b, sl_e1b, 1, 9        ; $E001 in cycle 23
RACE r_e0a, sl_e0a, 2, 7        ; $E000 in cycle 22
RACE r_e0c, sl_e0c, 3, 7        ; control

.macro SETSLED label
        lda #<label
        sta ptr
        lda #>label
        sta ptr+1
.endmacro

; 10: `$E001` written in the expiry's own cycle (22) enables it in time: the interrupt comes at once (NOP index < 4); written in cycle 23 it is too late:
; the expiry is lost and the next one, 20 cycles later, comes at NOP index 7 to 13. Code 2: GOT = NOP index in trial 1 (cycle 22), expected < 4.
; 3: no interrupt in trial 1. 4: trial 2 (cycle 23) GOT = NOP index, expected 7 to 13 (< 7: the expiry at 22 was not lost; > 13 or none: too late).
t_e1cyc:
        SETIRQ h_plain
        lda #0
        sta t5
        lda #4
        sta t0
        jsr r_e1a
        SETSLED sl_e1a
        jsr calc_k
        bcs @none1
        cmp #4
        bcc @t2
        sta det0
        lda #3
        sta det1
        lda #2
        rts
@none1: lda irq_count
        sta det0
        lda #3
        rts
@t2:    jsr r_e1b
        SETSLED sl_e1b
        jsr calc_k
        bcs @none2
        sta det0
        cmp #7
        bcc @bad2
        cmp #14
        bcs @bad2
        sta MAP_IRQOFF
        lda #0
        rts
@none2: lda #$FF
        sta det0
@bad2:  lda #7
        sta det1
        sta MAP_IRQOFF
        lda #4
        rts

; 11: `$E000` written in the expiry's own cycle (22) prevents it: no interrupt at all. The control (a store to RAM in the same cycle) has the
; interrupt at once. Code 2: GOT = interrupts taken after the `$E000` in cycle 22 (expected 0). 3: the control saw no interrupt (harness).
t_e0cyc:
        SETIRQ h_plain
        lda #0
        sta t5
        lda #4
        sta t0
        jsr r_e0c
        SETSLED sl_e0c
        jsr calc_k
        bcs @badc
        cmp #4
        bcc @t
@badc:  sta det0
        lda #3
        rts
@t:     jsr r_e0a
        sta MAP_IRQOFF
        lda irq_count
        beq @ok
        sta det0
        lda #0
        sta det1
        lda #2
        rts
@ok:    lda #0
        rts

; ---------------------------------------------------------------- 12: A12 mode to CPU mode with the A12 filter not full (F-087)
; L = 2, interrupt disabled, A12 mode; A12 is raised and lowered through PPUADDR; then `$C001` := 1 (CPU mode) and `$E001`, and a sled of NOPs with
; interrupts enabled shows when the first expiry comes. Arm A: the `$C001` store follows the A12 fall by about 4 cycles (the filter is not full): the
; expiry is 4(L+1)-2 = 10 cycles after the write. Arm B: 44 cycles of NOPs in between (the filter is full): 4(L+1)+2 = 14 cycles. So B is taken 2 NOPs later.
.macro F87 name, sled, arm
name:
        sei
        sta MAP_IRQOFF
        lda #2
        sta MAP_IRQLATCH
        lda #0
        sta MAP_IRQMODE         ; A12 mode
        sta irq_count
        ldx #1
        lda PPUSTATUS
        lda #$10
        sta PPUADDR
        lda #$00
        sta PPUADDR             ; A12 high
        nop
        nop
        nop
        nop
        sta PPUADDR
        sta PPUADDR             ; A12 low
.if arm
        .repeat 22
        nop
        .endrepeat
.endif
        cli
        stx MAP_IRQMODE         ; cycle 0: CPU mode
        sta MAP_IRQON           ; cycle 4
sled:
        .repeat 100
        nop
        .endrepeat
        sei
        rts
.endmacro

F87 r_f87a, sl_f87a, 0
F87 r_f87b, sl_f87b, 1

; Code 2: GOT = (NOPs in arm B) - (NOPs in arm A), expected 2; AT = NOPs in arm A. 3: no usable interrupt (GOT = interrupts in the arm, det1 = arm 1 or 2).
t_f87:
        SETIRQ h_plain
        lda #0
        sta t5
        jsr r_f87a
        SETSLED sl_f87a
        jsr calc_k
        bcs @none1
        sta kA
        jsr r_f87b
        SETSLED sl_f87b
        jsr calc_k
        bcs @none2
        sta kB
        sec
        sbc kA
        cmp #2
        beq @ok
        sta det0
        lda #2
        sta det1
        lda kA
        sta det2
        lda #2
        rts
@ok:    lda #0
        rts
@none1: lda irq_count
        sta det0
        lda #1
        sta det1
        lda #3
        rts
@none2: lda irq_count
        sta det0
        lda #2
        sta det1
        lda #3
        rts

.include "common/lib.s"
