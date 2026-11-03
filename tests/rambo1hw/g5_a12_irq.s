; Group 5: A12-mode (scanline) IRQ (design: docs/test_rom_design.md, section 5, group 5).
; Hardware expectations: F-021 to F-023, F-026, F-029 to F-031, F-037, F-038, F-051, F-052, F-055, F-056.
;
; A12 is driven with PPUADDR pairs while rendering is off: $10,$00 puts A12 high, $00,$00 low. A "pulse" is one rise and
; one fall. The chip counts a rise only if A12 was low for 16 or more M2 falls before it (the filter), and the first pulse
; after a `$C001` write is swallowed when the filter was already full. Expiry model (all measured):
;       first expiry at pulse L+2 (L+1 if the filter was not full at `$C001`), then every L+1 counted pulses.
; After each pulse the ROM opens a short interrupt window (CLI, NOP, NOP, SEI) and looks at the interrupt count, so the
; unit of every measurement is a whole pulse and no cycle-exact CPU timing is involved, except the filter sweep, which
; spaces the pulses by a chosen number of cycles. /IRQ stays asserted until `$E000`.
.include "common/vars.inc"

rom_title: .asciiz "RAMBO-1 G5: A12-MODE IRQ"
rom_note:  .asciiz "POWER-ON OR RESET. ANY START."

.zeropage
wpos:   .res 1                  ; write wval to $C000 after this many pulses ($FF = never)
wval:   .res 1
gapvec: .res 2
cnt:    .res 1
lend:   .res 1
.bss
fres:   .res 2                  ; filter sweep results, one bit per gap (low byte, high byte)
.code

test_table:
        TEST t_first,    n_first,    $FF        ; 0
        TEST t_first01,  n_first01,  0          ; 1
        TEST t_period,   n_period,   0          ; 2
        TEST t_filter,   n_filter,   0          ; 3
        TEST t_thresh,   n_thresh,   3          ; 4
        TEST t_latch,    n_latch,    0          ; 5
        TEST t_notfull,  n_notfull,  0          ; 6
        TEST t_ack,      n_ack,      0          ; 7
        TEST t_tocpu,    n_tocpu,    0          ; 8
        TEST t_toa12,    n_toa12,    0          ; 9
        TEST t_l0once,   n_l0once,   0          ; 10
        TEST t_l1alt,    n_l1alt,    0          ; 11
        .word 0

n_first:   .asciiz "FIRST EXPIRY AT L+2"
n_first01: .asciiz "FIRST EXPIRY L=0, L=1"
n_period:  .asciiz "REPEAT PERIOD L+1"
n_filter:  .asciiz "FILTER NEEDS 16 FALLS"
n_thresh:  .asciiz "FILTER THRESHOLD"
n_latch:   .asciiz "C000 AT FIRST COUNT"
n_notfull: .asciiz "FILTER NOT FULL AT C001"
n_ack:     .asciiz "E000/E001 IN A12 MODE"
n_tocpu:   .asciiz "A12 TO CPU MODE"
n_toa12:   .asciiz "CPU TO A12 MODE"
n_l0once:  .asciiz "LATCH 0: ONE EXPIRY"
n_l1alt:   .asciiz "LATCH 1: EVERY 2ND PULSE"

lvals:   .byte 0, 1, 2, 3, 4, 5, 6, 7, 8, 16     ; t_first runs L >= 2, t_first01 runs L = 0 and 1
NLV = 10
plvals:  .byte 1, 2, 3, 4, 5, 6, 7, 8, 16
NPL = 9
nflv:    .byte 0, 1, 2
glist:   .byte 13, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24     ; cycles from the A12 fall to the next rise
NG = 11
gvec_lo: .byte <(sledA_end-0), <(sledA_end-1), <(sledB_end-0), <(sledA_end-2), <(sledB_end-1), <(sledA_end-3)
         .byte <(sledB_end-2), <(sledA_end-4), <(sledB_end-3), <(sledA_end-5), <(sledB_end-4)
gvec_hi: .byte >(sledA_end-0), >(sledA_end-1), >(sledB_end-0), >(sledA_end-2), >(sledB_end-1), >(sledA_end-3)
         .byte >(sledB_end-2), >(sledA_end-4), >(sledB_end-3), >(sledA_end-5), >(sledB_end-4)

; ---------------------------------------------------------------- primitives
pulse:                          ; one A12 pulse (rise then fall); A12 must be low on entry and is low on exit
        lda #$10
        sta PPUADDR
        lda #$00
        sta PPUADDR             ; rise
        nop
        nop
        nop
        nop
        sta PPUADDR
        sta PPUADDR             ; fall
        rts

poll:   cli                     ; interrupt window: irq_count counts the interrupts taken
        nop
        nop
        sei
        rts

npulse: jsr pulse
        jmp poll

setup:                          ; A = latch. A12 mode, filter full, counter reloaded at the next counted pulse, enabled
        sei
        pha
        sta MAP_IRQOFF
        pla
        sta MAP_IRQLATCH
        ldx #8
@w:     dex
        bne @w                  ; about 40 cycles with A12 low
        lda #$FF
        sta wpos
        lda #0
        sta irq_count
        sta MAP_IRQMODE
        sta MAP_IRQON
        rts

firstirq:                       ; X = pulse limit. Out: A = number of the pulse after which the interrupt was seen, 0 = none
        stx t5
        lda #0
        sta t6
runp:   lda t6
        cmp wpos
        bne @nw
        lda wval
        sta MAP_IRQLATCH
@nw:    jsr pulse
        inc t6
        jsr poll
        lda irq_count
        bne @hit
        lda t6
        cmp t5
        bcc runp
        lda #0
        rts
@hit:   lda t6
        rts

nextirq:                        ; acknowledge, re-enable and continue counting pulses; same result as firstirq
        lda #0
        sta irq_count
        sta MAP_IRQOFF
        sta MAP_IRQON
        lda t6
        clc
        adc #40
        sta t5
        jmp runp

; Pulses with a chosen gap (cycles from fall to next rise = 13 + c; one NOP = one byte = 2 cycles). cnt = number of pulses, gapvec = entry in a sled.
gp:     ldx #$10
        ldy #0
sledA:  .repeat 8
        nop
        .endrepeat
sledA_end:
gp_loop:
        stx PPUADDR
        sty PPUADDR             ; rise
        dec cnt
        bne @more
        nop
        nop
        sty PPUADDR
        sty PPUADDR             ; last fall
        rts
@more:  nop
        nop
        sty PPUADDR
        sty PPUADDR             ; fall
        jmp (gapvec)
sledB:  .repeat 8
        nop
        .endrepeat
sledB_end:
        jmp gp_loop

.macro FAILX code, got, exp, at
        lda got
        sta det0
        lda exp
        sta det1
        lda at
        sta det2
        lda #code
        rts
.endmacro

; ---------------------------------------------------------------- 0: first expiry at pulse L+2 (F-021)
; Code 2: GOT = pulse of the first interrupt (0 = none), EXP = L+2, AT = L. t_first covers L = 2 to 16; L = 0 and 1
t_first:
        lda #NLV
        sta lend
        ldy #2
        jmp first_run
t_first01:
        lda #2
        sta lend
        ldy #0
first_run:
@lp:    sty t4
        lda lvals,y
        sta t3
        jsr setup
        ldx #40
        jsr firstirq
        sta t0
        lda t3
        clc
        adc #2
        sta t1
        cmp t0
        beq @ok
        FAILX 2, t0, t1, t3
@ok:    ldy t4
        iny
        cpy lend
        bne @lp
        sta MAP_IRQOFF
        lda #0
        rts

; ---------------------------------------------------------------- 1: repeat period L+1 (F-037, F-056)
; Interrupts at pulses L+2, then every L+1 pulses (acknowledged and re-enabled after each). Code 2: first wrong,
; code 3: second or third wrong (GOT = pulse seen, EXP = pulse expected, AT = L).
t_period:
        ldy #0
@lp:    sty t4
        lda plvals,y
        sta t3
        jsr setup
        ldx #80
        jsr firstirq
        sta t0
        lda t3
        clc
        adc #2
        sta t2
        cmp t0
        beq @p2
        FAILX 2, t0, t2, t3
@p2:    jsr nextirq
        sta t1
        lda t3
        sec                     ; carry set: adds L + 1
        adc t0
        sta t2
        cmp t1
        beq @p3
        FAILX 3, t1, t2, t3
@p3:    jsr nextirq
        sta t0
        lda t3
        sec
        adc t1
        sta t2
        cmp t0
        beq @ok
        FAILX 3, t0, t2, t3
@ok:    ldy t4
        iny
        cpy #NPL
        bne @lp
        sta MAP_IRQOFF
        lda #0
        rts

; ---------------------------------------------------------------- 2: the filter needs about 16 M2 falls (F-022, F-051)
; L = 1; after one long-gap pulse (swallowed), six pulses are spaced by G cycles. Counted pulses give an interrupt at the
; second one; uncounted pulses never do. G = 13 and 15 must not count, G = 18 and more must. G = 16 and 17 are left to the
; next test, because the CPU/PPU alignment moves the threshold by one cycle. Code 2: G <= 15 counted (AT = G); code 3: G >= 18
; did not count (AT = G).
t_filter:
        lda #0
        sta fres
        sta fres+1
        ldy #0
@lp:    sty t4
        lda #1
        jsr setup
        jsr pulse               ; pulse 1: swallowed
        ldy t4
        lda gvec_lo,y
        sta gapvec
        lda gvec_hi,y
        sta gapvec+1
        lda #6
        sta cnt
        jsr gp
        jsr poll
        lda irq_count
        sta t0
        sta MAP_IRQOFF
        ldy t4
        lda t0
        beq @next
        cpy #8                  ; record bit y of fres
        bcs @hi
        lda bitmask,y
        ora fres
        sta fres
        jmp @next
@hi:    tya
        sec
        sbc #8
        tax
        lda bitmask,x
        ora fres+1
        sta fres+1
@next:  lda glist,y
        sta t3
        cpy #2
        bcs @big
        lda t0                  ; G = 13, 15: must not count
        beq @cont
        FAILX 2, t0, zero, t3
@big:   cpy #4
        bcc @cont               ; G = 16, 17: not asserted
        lda t0
        bne @cont
        lda #1
        sta det1
        lda #0
        sta det0
        lda t3
        sta det2
        lda #3
        rts
@cont:  ldy t4
        iny
        cpy #NG
        beq @end
        jmp @lp
@end:   lda #0
        rts
zero:   .byte 0
bitmask: .byte 1, 2, 4, 8, 16, 32, 64, 128

; ---------------------------------------------------------------- 3: filter threshold (discovery)
; INFO: smallest gap that counted (cycles, FF = none), then the result bits of the sweep G = 13 15 16 17 18 19 20 21 | 22 23 24.
t_thresh:
        lda #$FF
        sta det0
        ldy #0
@lp:    cpy #8
        bcs @hi
        lda bitmask,y
        and fres
        jmp @t
@hi:    tya
        sec
        sbc #8
        tax
        lda bitmask,x
        and fres+1
@t:     beq @n
        lda glist,y
        sta det0
        jmp @done
@n:     iny
        cpy #NG
        bne @lp
@done:  lda fres
        sta det1
        lda fres+1
        sta det2
        lda #1
        sta disc
        lda #0
        rts

; ---------------------------------------------------------------- 4: C000 written before the first counted pulse is used; later it is not (F-038)
; L = 4 and a new latch of 1 written after wpos pulses. Written after 0 or 1 pulses: the interrupt comes at pulse 3.
; After 2 or 4 pulses: at pulse 6 (old latch). Code 2: GOT = pulse seen, EXP = expected, AT = wpos.
t_latch:
        ldy #0
@lp:    sty t4
        lda #4
        jsr setup
        ldy t4
        lda wps,y
        sta wpos
        lda #1
        sta wval
        ldx #20
        jsr firstirq
        sta t0
        ldy t4
        lda wexp,y
        sta t1
        lda wps,y
        sta t2
        lda t0
        cmp t1
        beq @ok
        FAILX 2, t0, t1, t2
@ok:    ldy t4
        iny
        cpy #4
        bne @lp
        sta MAP_IRQOFF
        lda #0
        rts
wps:    .byte 0, 1, 2, 4
wexp:   .byte 3, 3, 6, 6

; ---------------------------------------------------------------- 5: filter not full at $C001: first pulse counts, interrupt at L+1 (F-052, F-055)
; A pulse, then `$C001` about 12 cycles after the fall, then a long wait. Code 2: GOT = pulse seen, EXP = L+1, AT = L.
t_notfull:
        ldy #0
@lp:    sty t4
        lda nflv,y
        sta t3
        sei
        sta MAP_IRQOFF
        lda t3
        sta MAP_IRQLATCH
        lda #$FF
        sta wpos
        lda #0
        sta irq_count
        jsr pulse
        lda #0
        sta MAP_IRQMODE         ; A12 mode, 12 cycles after the fall
        sta MAP_IRQON
        ldx #6
@w:     dex
        bne @w
        ldx #20
        jsr firstirq
        sta t0
        lda t3
        clc
        adc #1
        sta t1
        cmp t0
        beq @ok
        FAILX 2, t0, t1, t3
@ok:    ldy t4
        iny
        cpy #3
        bne @lp
        sta MAP_IRQOFF
        lda #0
        rts

; ---------------------------------------------------------------- 6: $E000 and $E001 in A12 mode (F-029, F-030, F-031)
; Code 2: first interrupt not at pulse 3 (L = 1). 3: $E001 released /IRQ. 4: $E000 did not release it.
; 5: interrupt while disabled. 6: an expiry while disabled was kept and delivered at the enable. 7: no interrupt at pulse 7.
t_ack:
        lda #1
        jsr setup
        ldx #10
        jsr firstirq
        sta t0
        cmp #3
        beq @a
        lda #3
        sta det1
        lda t0
        sta det0
        lda #2
        rts
@a:     lda #0
        sta irq_count
        sta MAP_IRQON           ; enable alone does not release
        jsr poll
        lda irq_count
        bne @b
        sta det0
        lda #1
        sta det1
        lda #3
        rts
@b:     sta MAP_IRQOFF
        lda #0
        sta irq_count
        jsr poll
        lda irq_count
        beq @c
        sta det0
        lda #0
        sta det1
        lda #4
        rts
@c:     lda #1                  ; expiries at pulses 3, 5, 7
        jsr setup
        jsr npulse
        jsr npulse
        lda irq_count
        beq @d
        sta det0
        lda #0
        sta det1
        lda #5
        rts
@d:     sta MAP_IRQOFF          ; disabled for pulses 3 to 6: expiries at 3 and 5 are lost
        jsr npulse
        jsr npulse
        jsr npulse
        jsr npulse
        lda irq_count
        beq @e
        sta det0
        lda #0
        sta det1
        lda #5
        rts
@e:     sta MAP_IRQON
        jsr poll
        lda irq_count
        beq @f
        sta det0
        lda #0
        sta det1
        lda #6
        rts
@f:     jsr npulse              ; pulse 7
        lda irq_count
        bne @ok
        sta det0
        lda #1
        sta det1
        lda #7
        rts
@ok:    sta MAP_IRQOFF
        lda #0
        rts

; ---------------------------------------------------------------- 7: A12 to CPU mode restarts cleanly (F-026)
; L = 4, two pulses, then `$C001` = 1: /IRQ 22 cycles later, not before. Code 2: interrupt during the A12 pulses,
; 3: interrupt within 10 cycles of the switch, 4: none after 50 cycles.
t_tocpu:
        lda #4
        jsr setup
        jsr npulse
        jsr npulse
        lda irq_count
        beq @a
        sta det0
        lda #0
        sta det1
        lda #2
        rts
@a:     lda #1
        sta MAP_IRQMODE
        jsr poll                ; ends before cycle 22
        lda irq_count
        beq @b
        sta det0
        lda #0
        sta det1
        lda #3
        rts
@b:     ldx #8
@w:     dex
        bne @w
        jsr poll
        lda irq_count
        bne @ok
        sta det0
        lda #1
        sta det1
        lda #4
        rts
@ok:    sta MAP_IRQOFF
        lda #0
        sta MAP_IRQMODE         ; back to A12 mode for the next test
        lda #0
        rts

; ---------------------------------------------------------------- 8: CPU to A12 mode: nothing carries over (F-026, F-057)
; CPU mode L = 4 started, switched to A12 mode 14 cycles later: the cycle count is cancelled and the A12 counter starts clean,
; so the interrupt comes at pulse 6 (L + 2). Code 2: GOT = pulse seen (0 = none), EXP = 6.
t_toa12:
        sei
        sta MAP_IRQOFF
        lda #4
        sta MAP_IRQLATCH
        lda #0
        sta irq_count
        lda #$FF
        sta wpos
        ldx #8
@w:     dex
        bne @w                  ; A12 low long enough for a full filter
        lda #1
        sta MAP_IRQMODE         ; CPU mode: expiry would come after 22 cycles
        sta MAP_IRQON
        nop
        nop
        lda #0
        sta MAP_IRQMODE         ; A12 mode, about 14 cycles after the first write
        ldx #20
        jsr firstirq
        sta t0
        cmp #6
        beq @ok
        lda #6
        sta det1
        lda t0
        sta det0
        lda #2
        rts
@ok:    sta MAP_IRQOFF
        lda #0
        rts

; ---------------------------------------------------------------- 10, 11: latch 0 expires only on the reload after $C001 (F-075)
; Twelve pulses with the interrupt acknowledged and re-enabled after each one that is seen; the number of interrupts is counted.
; Latch 0: the reload after $C001 (pulse 2) expires, the automatic reload at zero does not: 1 interrupt (an emulator that expires on every
; reload to 0 gives 11). Latch 1 (control, F-059): expiries at pulses 3, 5, 7, 9, 11: 5 interrupts.
; Code 2: GOT = interrupts counted, EXP = the expected number.
countirq:                       ; A = latch; out A = number of interrupts seen in 12 pulses
        jsr setup
        lda #0
        sta t6                  ; pulses done
        sta t7                  ; interrupts seen
@p:     jsr pulse
        inc t6
        jsr poll
        lda irq_count
        beq @n
        inc t7
        lda #0
        sta irq_count
        sta MAP_IRQOFF
        sta MAP_IRQON           ; acknowledge and enable again
@n:     lda t6
        cmp #12
        bcc @p
        lda t7
        rts

t_l0once:
        lda #0
        jsr countirq
        ldx #1
        jmp l_check
t_l1alt:
        lda #1
        jsr countirq
        ldx #5
l_check:
        sta det0
        stx det1
        cpx det0
        beq @ok
        lda #2
        rts
@ok:    sta MAP_IRQOFF
        lda #0
        rts

.include "common/lib.s"
