; Group 6: real rendering. Raster splits driven by the A12 scanline counter and by the CPU-cycle counter.
; The PPU fetches sprites on every scanline (and on the pre-render line), so with the background at $0000 and the
; sprites at $1000 A12 rises once per line at dot 260. The filter must ignore the shorter A12 toggles inside a fetch.
; Method: sprite 0 is placed so that its hit lands on line H0 (a known line), late on the line (x = 200, dot 202), so the
; hit comes after the handler would read $2002 for an interrupt in the previous line's hblank (A12 rising at dot 324). The IRQ handler reads $2002 at once: the hit
; flag is set only if the interrupt came on a line at or after H0, so the smallest latch value L that sees the flag
; marks the IRQ line = H0. The offset L - H0 is printed (test 3) and must be the same for every H0 (test 2) and every
; PPU configuration (tests 4 to 6). Counting interrupts over 8 frames checks the repeat and the CPU-cycle mode.
; Hardware expectations: F-018, F-021, F-022, F-037, F-051, F-052, F-056 and the NESdev description of the scanline counter.
.include "common/vars.inc"

CUSTOM_IRQ = 1
CUSTOM_NMI = 1

rom_title: .asciiz "RAMBO-1 G6: RASTER AND PPU"
rom_note:  .asciiz "POWER-ON OR RESET. ANY START."

.zeropage
frames:  .res 1                 ; NMI counter
g_ctrl:  .res 1                 ; PPUCTRL value (without the NMI bit)
g_tile:  .res 1                 ; tile of sprite 0
g_y:     .res 1                 ; OAM Y of sprite 0
done:    .res 1
hitreg:  .res 1
irqn:    .res 1
ti1:     .res 1                 ; demo: next PPUMASK values
ti2:     .res 1
ti3:     .res 1
tmp:     .res 1
xoff:    .res 1                 ; expected smallest latch relative to the reference configuration
irq_vec: .res 2
.bss
lstar:   .res 3                 ; smallest latch that saw the hit flag, for H0 = 40, 96, 152
.code

test_table:
        TEST t_demo,   n_demo,   $FF            ; 0
        TEST t_ref,    n_ref,    $FF            ; 1
        TEST t_line,   n_line,   1              ; 2
        TEST t_off,    n_off,    2              ; 3
        TEST t_swap,   n_swap,   2              ; 4
        TEST t_x16,    n_x16,    2              ; 5
        TEST t_inv,    n_inv,    2              ; 6
        TEST t_none,   n_none,   1              ; 7
        TEST t_repeat, n_repeat, 2              ; 8
        TEST t_cpu,    n_cpu,    1              ; 9
        TEST t_both,   n_both,   1              ; 10
        .word 0

n_demo:   .asciiz "RASTER SPLIT DEMO"
n_ref:    .asciiz "SPRITE 0 HIT REFERENCE"
n_line:   .asciiz "IRQ LINE FOLLOWS LATCH"
n_off:    .asciiz "IRQ LINE OFFSET"
n_swap:   .asciiz "BG $1000: LINE L-1"
n_x16:    .asciiz "8X16 SPRITES"
n_inv:    .asciiz "CHR INVERSION BIT"
n_none:   .asciiz "NO A12 EDGE: NO IRQ"
n_repeat: .asciiz "REPEATED SPLITS"
n_cpu:    .asciiz "CPU-CYCLE SPLITS"
n_both:   .asciiz "BOTH TABLES AT $1000"

h0tab:   .byte 40, 96, 152
rl:      .byte 15, 7, 127             ; latches for the repeat test and the interrupts expected in 8 frames
re:      .byte 120, 240, 15           ; floor((8*241 - (L+2)) / (L+1)) + 1: 241 counted pulses per frame
s_demo1: .asciiz "RAMBO-1 RASTER SPLITS"
s_demo2: .asciiz "BANDS MUST BE STRAIGHT"

.macro SETPTR label
        lda #<label
        sta ptr
        lda #>label
        sta ptr+1
.endmacro

.macro SETIRQ handler
        lda #<handler
        sta irq_vec
        lda #>handler
        sta irq_vec+1
.endmacro

irq:    jmp (irq_vec)
nmi:    inc frames
        rti

; ---------------------------------------------------------------- IRQ handlers
h_hit:  pha                     ; record the sprite-0 hit flag at the instant of the interrupt, then disable
        lda PPUSTATUS
        sta hitreg
        sta MAP_IRQOFF
        inc done
        pla
        rti

h_count:                        ; count, acknowledge and re-enable (the counter keeps running)
        pha
        inc irqn
        sta MAP_IRQOFF
        sta MAP_IRQON
        pla
        rti

h_demo: pha                     ; the PPUMASK write comes first: it must land in the horizontal blank
        lda ti1
        sta PPUMASK
        sta tmp
        lda ti2
        sta ti1
        lda ti3
        sta ti2
        lda tmp
        sta ti3
        sta MAP_IRQOFF
        sta MAP_IRQON
        pla
        rti

; ---------------------------------------------------------------- video helpers
wait_vbl:                       ; wait for the next NMI (rendering and NMI must be on)
        lda frames
@w:     cmp frames
        beq @w
        rts

gfx_setup:                      ; A = PPUCTRL value, X = sprite 0 tile: rendering on at line H0 = 40, interrupts off
        sta g_ctrl
        stx g_tile
        lda #39
        sta g_y
        jsr gfx_prep
        jmp gfx_start

gfx_prep:                       ; rendering off: palette, nametable 0 with a column of 'L' at x = 200, OAM
        sei
        lda #0
        sta PPUCTRL
        sta PPUMASK
        bit PPUSTATUS
        lda #$3F
        sta PPUADDR
        lda #0
        sta PPUADDR
        tax
@p:     txa
        and #3
        beq @u
        lda #$30
        bne @w
@u:     lda #$1A
@w:     sta PPUDATA
        inx
        cpx #32
        bne @p
        lda #$20
        sta PPUADDR
        lda #0
        sta PPUADDR
        ldx #4
        ldy #0
@c:     sta PPUDATA
        iny
        bne @c
        dex
        bne @c
        lda g_y
        cmp #$FF                ; sprite 0 hidden (the demo): no reference column
        beq @nocol
        lda #$20
        sta PPUADDR
        lda #25
        sta PPUADDR
        lda #4
        sta PPUCTRL             ; increment 32
        lda #44                 ; 'L': its left column is solid, so sprite and background overlap there
        ldx #30
@col:   sta PPUDATA
        dex
        bne @col
        lda #0
        sta PPUCTRL
@nocol: lda #0
        sta PPUOAMADDR
        lda g_y
        sta PPUOAMDATA
        lda g_tile
        sta PPUOAMDATA
        lda #0
        sta PPUOAMDATA
        lda #200
        sta PPUOAMDATA
        ldx #63                 ; the other 63 sprites off screen
@o:     lda #$FF
        sta PPUOAMDATA
        lda #0
        sta PPUOAMDATA
        sta PPUOAMDATA
        sta PPUOAMDATA
        dex
        bne @o
        sta PPUADDR             ; v = t = 0, A12 low
        sta PPUADDR
        sta PPUSCROLL
        sta PPUSCROLL
        rts

gfx_start:                      ; turn rendering and NMI on at the start of a vertical blank
        lda #0
        sta frames
        bit PPUSTATUS
@v:     bit PPUSTATUS
        bpl @v
        lda g_ctrl
        ora #$80
        sta PPUCTRL
        lda #$1E
        sta PPUMASK
        jmp wait_vbl

gfx_off:                        ; rendering and NMI off, interrupt disabled
        jsr wait_vbl
        lda #0
        sta PPUMASK
        sta PPUCTRL
        sei
        sta MAP_IRQOFF
        sta MAP_BANKSEL         ; C = 0 again
        rts

finish: pha                     ; A = test result; leave the screen state clean
        jsr gfx_off
        pla
        rts

puts_at:                        ; ptr -> string, A:X = PPU address (rendering off)
        bit PPUSTATUS
        sta PPUADDR
        stx PPUADDR
        ldy #0
@l:     lda (ptr),y
        beq @d
        sec
        sbc #32
        sta PPUDATA
        iny
        bne @l
@d:     rts

; ---------------------------------------------------------------- one trial: arm the A12 counter in vblank, see which line the interrupt falls on
; In: t2 = latch, t3 = H0. Out: C set = no interrupt within the frame; else Z clear = the hit flag was set at the interrupt.
trial:  jsr wait_vbl
        lda #0
        sta PPUOAMADDR
        lda t3
        sec
        sbc #1
        sta PPUOAMDATA          ; sprite 0 hit lands on line H0
        sei
        sta MAP_IRQOFF
        lda t2
        sta MAP_IRQLATCH
        lda #0
        sta done
        sta hitreg
        sta MAP_IRQMODE         ; A12 mode; A12 has been low since the last line, so the filter is full
        sta MAP_IRQON
        SETIRQ h_hit
        cli
        jsr wait_vbl
        sei
        sta MAP_IRQOFF
        lda done
        beq @none
        lda hitreg
        and #$40
        clc
        rts
@none:  sec
        rts

; Latches H0-3 .. H0+3. Out: A = the smallest latch whose interrupt saw the hit flag; 0 = never, or already at the lowest.
sweep:  lda t3
        sec
        sbc #3
        sta t2
        lda #0
        sta t5
@l:     jsr trial
        bcs @bad
        bne @hit
        inc t2
        inc t5
        lda t5
        cmp #7
        bne @l
@bad:   lda #0
        rts
@hit:   lda t5
        beq @bad
        lda t2
        rts

; Run 8 frames with the counter armed in the mode of A (0 = A12, 1 = CPU cycles), latch t2; Out: A = interrupts counted
count_run:
        sta t1
        jsr wait_vbl
        sei
        sta MAP_IRQOFF
        lda t2
        sta MAP_IRQLATCH
        lda #0
        sta irqn
        SETIRQ h_count
        lda t1
        sta MAP_IRQMODE
        sta MAP_IRQON
        cli
        ldx #8
@f:     jsr wait_vbl
        dex
        bne @f
        sei
        sta MAP_IRQOFF
        lda irqn
        rts

.macro FAILX code, got, exp, at
        lda got
        sta det0
        lda exp
        sta det1
        lda at
        sta det2
        lda #code
        jmp finish
.endmacro

; ---------------------------------------------------------------- 0: demo (never fails)
; Five colour bands (the PPUMASK emphasis bits change at the IRQ): lines 0-59, 60-119, 120-179, 180-239. Rearmed every frame.
t_demo:
        lda #$08
        sta g_ctrl
        lda #44
        sta g_tile
        lda #$FF
        sta g_y
        jsr gfx_prep
        SETPTR s_demo1
        lda #$20
        ldx #$84
        jsr puts_at
        SETPTR s_demo2
        lda #$20
        ldx #$C4
        jsr puts_at
        lda #0
        sta PPUADDR
        sta PPUADDR
        sta PPUSCROLL
        sta PPUSCROLL
        jsr gfx_start
        SETIRQ h_demo
        lda #240
        sta t4
@f:     jsr wait_vbl
        sei
        sta MAP_IRQOFF
        lda #59
        sta MAP_IRQLATCH
        lda #0
        sta MAP_IRQMODE
        sta MAP_IRQON
        lda #$3E
        sta ti1
        lda #$5E
        sta ti2
        lda #$9E
        sta ti3
        lda #$1E
        sta PPUMASK
        cli
        dec t4
        bne @f
        lda #0
        jmp finish

; ---------------------------------------------------------------- 1: sprite 0 hit reference (no mapper involved)
; Code 2: no hit flag at the start of vblank, so the picture is not set up as the other tests assume.
t_ref:  lda #$08
        ldx #44
        jsr gfx_setup
        jsr wait_vbl
        lda PPUSTATUS
        and #$40
        bne @ok
        sta det0
        lda #$40
        sta det1
        lda #2
        jmp finish
@ok:    lda #0
        jmp finish

; ---------------------------------------------------------------- 2: the interrupt line follows the latch, one line per step
; The smallest latch seeing the hit flag, minus H0, is the same for H0 = 40, 96 and 152. Code 2: no latch in the window
; saw the flag (AT = H0); code 3: the offset differs (GOT = offset, EXP = the first offset, AT = index of the H0).
t_line: lda #$08
        ldx #44
        jsr gfx_setup
        ldy #0
@lp:    sty t4
        lda h0tab,y
        sta t3
        jsr sweep
        ldy t4
        sta lstar,y
        cmp #0
        bne @ok1
        lda #0
        sta det0
        sta det1
        lda t3
        sta det2
        lda #2
        jmp finish
@ok1:   iny
        cpy #3
        bne @lp
        lda lstar
        sec
        sbc h0tab
        sta t0
        lda lstar+1
        sec
        sbc h0tab+1
        cmp t0
        bne @bad1
        lda lstar+2
        sec
        sbc h0tab+2
        cmp t0
        beq @pass
        sta det0
        lda #2
        sta det2
        bne @bad
@bad1:  sta det0
        lda #1
        sta det2
@bad:   lda t0
        sta det1
        lda #3
        jmp finish
@pass:  lda #0
        jmp finish

; ---------------------------------------------------------------- 3: the line offset (discovery)
; INFO: offset (smallest latch seeing the flag minus H0, 0 = the interrupt is on the line the latch names), then the
; smallest latches for H0 = 40 and 152.
t_off:  lda lstar
        sec
        sbc h0tab
        sta det0
        lda lstar
        sta det1
        lda lstar+2
        sta det2
        lda #1
        sta disc
        lda #0
        rts

; ---------------------------------------------------------------- 4 to 6: the offset in other PPU configurations (H0 = 96)
; Code 2: GOT = smallest latch, EXP = expected, AT = H0. With the sprites at $1000 (or 8x16 sprites) A12 rises once per line at
; dot 260 and was low in vblank, so the first pulse after `$C001` is swallowed and the offset is the same as in test 2.
; With the background at $1000 and the sprites at $0000 A12 is high from the last background fetch through vblank, so a
; `$C001` written there finds A12 high: the first pulse counts (F-052) and the interrupt comes one line earlier, L-1.
; Measured on the chip.
t_swap: lda #1
        sta xoff
        lda #$10                ; background at $1000, sprites at $0000
        ldx #44
        jsr cfg_run
        rts
t_x16:  lda #0
        sta xoff
        lda #$20                ; 8x16 sprites: the tile's bit 0 selects $1000
        ldx #$2D                ; top half is tile $2C ('L')
        jsr cfg_run
        rts
t_inv:  lda #0
        sta xoff
        lda #$08                ; $8000 bit 7 set: CHR A12 inversion. The PPU's A12 is not affected
        ldx #44
        jsr gfx_setup
        lda #$80
        sta MAP_BANKSEL
        jmp cfg_sweep
cfg_run:
        jsr gfx_setup
cfg_sweep:
        lda #96
        sta t3
        jsr sweep
        sta det0
        lda lstar+1
        clc
        adc xoff
        sta det1
        cmp det0
        beq @ok
        lda #96
        sta det2
        lda #2
        jmp finish
@ok:    lda #0
        jmp finish

; ---------------------------------------------------------------- 7: no A12 edge, no interrupt
; Both tables at $0000: A12 never rises, so nothing is counted. Code 2: GOT = interrupts in 8 frames.
t_none: lda #$00
        ldx #44
        jsr gfx_setup
        lda #0
        sta t2
        lda #0
        jsr count_run
        beq @ok
        sta det0
        lda #0
        sta det1
        lda #2
        jmp finish
@ok:    lda #0
        jmp finish

; ---------------------------------------------------------------- 8: repeated splits (F-037, F-056 in a picture)
; Latch L, 8 frames of 241 counted pulses each: floor((1928 - (L+2)) / (L+1)) + 1 interrupts.
; Code 2: GOT = interrupts, EXP = expected, AT = L.
t_repeat:
        lda #$08
        ldx #44
        jsr gfx_setup
        ldy #0
@lp:    sty t4
        lda rl,y
        sta t2
        lda #0
        jsr count_run
        ldy t4
        cmp re,y
        beq @ok
        sta det0
        lda re,y
        sta det1
        lda rl,y
        sta det2
        lda #2
        jmp finish
@ok:    iny
        cpy #3
        bne @lp
        lda #0
        jmp finish

; ---------------------------------------------------------------- 9: CPU-cycle mode over 8 frames
; L = 255: an interrupt every 1024 cycles after the first at 1026, 8 frames of 29780.5 cycles: 232. Code 2: GOT = count.
t_cpu:  lda #$08
        ldx #44
        jsr gfx_setup
        lda #255
        sta t2
        lda #1
        jsr count_run
        cmp #232
        beq @ok
        sta det0
        lda #232
        sta det1
        lda #2
        jmp finish
@ok:    lda #0
        jmp finish

; ---------------------------------------------------------------- 10: both tables at $1000 (discovery)
; A12 stays high through the fetches, so at most one rise per frame is counted. INFO: interrupts in 8 frames with L = 0.
t_both: lda #$18
        ldx #44
        jsr gfx_setup
        lda #0
        sta t2
        lda #0
        jsr count_run
        sta det0
        lda #1
        sta disc
        lda #0
        jmp finish

.include "common/lib.s"
