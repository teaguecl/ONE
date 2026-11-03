; Group 2: PRG and CHR banking, mode bits, register decode, mirroring (design: docs/test_rom_design.md, section 5).
; Hardware expectations: docs/findings.md F-006 to F-011, F-039 to F-042, F-049. Banks are identified by reading back
; the identity block of each bank (PRG through the CPU, CHR through $2007), never by looking at the screen.
.include "common/vars.inc"

rom_title: .asciiz "RAMBO-1 G2: BANKING AND DECODE"
rom_note:  .asciiz "ANY START. USES 256K PRG, 256K CHR."

; test indices (0-based) are used as dependencies
test_table:
        TEST t_prg_basic, n_prg_basic, $FF      ; 0
        TEST t_prg_mode,  n_prg_mode,  0        ; 1
        TEST t_prg_width, n_prg_width, 0        ; 2
        TEST t_prg_all,   n_prg_all,   0        ; 3
        TEST t_chr_1k,    n_chr_1k,    $FF      ; 4
        TEST t_chr_2k,    n_chr_2k,    4        ; 5
        TEST t_chr_inv,   n_chr_inv,   5        ; 6
        TEST t_chr_width, n_chr_width, 4        ; 7
        TEST t_chr_odd,   n_chr_odd,   5        ; 8
        TEST t_r8r9,      n_r8r9,      5        ; 9
        TEST t_modebits,  n_modebits,  5        ; 10
        TEST t_idx,       n_idx,       4        ; 11
        TEST t_mirror,    n_mirror,    $FF      ; 12
        TEST t_a001,      n_a001,      12       ; 13
        .word 0

n_prg_basic: .asciiz "PRG R6 R7 RF BANKS"
n_prg_mode:  .asciiz "PRG MODE BIT SWAPS"
n_prg_width: .asciiz "PRG 5-BIT BANK WIDTH"
n_prg_all:   .asciiz "PRG ALL 32 BANKS"
n_chr_1k:    .asciiz "CHR 1K MODE R0-R5 R8 R9"
n_chr_2k:    .asciiz "CHR 2K MODE PAIRS"
n_chr_inv:   .asciiz "CHR INVERSION BIT"
n_chr_width: .asciiz "CHR 8-BIT BANK WIDTH"
n_chr_odd:   .asciiz "CHR 2K ODD R0 R1 (A10)"
n_r8r9:      .asciiz "R8 R9 ONLY WITH K=1"
n_modebits:  .asciiz "MODE BITS BY $8000 ALONE"
n_idx:       .asciiz "INDEX A-E, BIT 4 IGNORED"
n_mirror:    .asciiz "MIRRORING $A000 BIT 0"
n_a001:      .asciiz "$A001 NO VISIBLE EFFECT"

; expected identity byte per CHR window $0000..$1C00 for the baseline, by mode (K = 1 KB mode, C = inversion)
exp_k0c0: .byte $20, $21, $40, $41, $62, $83, $A4, $C5
exp_k1c0: .byte $20, $D0, $40, $F0, $62, $83, $A4, $C5
exp_k0c1: .byte $62, $83, $A4, $C5, $20, $21, $40, $41
exp_k1c1: .byte $62, $83, $A4, $C5, $20, $D0, $40, $F0
exp_odd:  .byte $22, $23, $44, $45, $62, $83, $A4, $C5     ; R0 = $23, R1 = $45 in 2 KB mode, bit 0 replaced by PPU A10
exp_r8:   .byte $20, $11, $40, $F0, $62, $83, $A4, $C5     ; K = 1 after R8 := $11
; expected PRG per window $8000, $A000, $C000, $E000
exp_p0:   .byte $05, $0A, $14, $1F
exp_p1:   .byte $14, $0A, $05, $1F

.include "common/banking.s"

; one step of a sequence of $8000-only mode writes: write MODE, check with CHK against TABLE, else fail with CODE
.macro MODECHK mode, chk, table, code
        lda #mode
        sta MAP_BANKSEL
        SETPTR2 table
        jsr chk
        beq :+
        lda #code
        rts
:
.endmacro

; ---------------------------------------------------------------- PRG
t_prg_basic:                    ; P = 0: R6 at $8000, R7 at $A000, RF at $C000, fixed $1F at $E000 (F-009)
        lda #$00
        jsr base_load
        SETPTR2 exp_p0
        jmp check_prg

t_prg_mode:                     ; P = 1 swaps R6 and RF; code 3: swapping back by $8000 alone failed
        lda #$40
        jsr base_load
        SETPTR2 exp_p1
        jsr check_prg
        beq :+
        rts
:       lda #$00
        sta MAP_BANKSEL
        SETPTR2 exp_p0
        jsr check_prg
        beq :+
        lda #3
:       rts

reg_idx:    .byte 6, 7, 15
reg_win:    .byte $80, $A0, $C0
width_vals: .byte $25, $5F, $91, $FF, $E0

t_prg_width:                    ; only the low 5 bits of R6, R7, RF reach PRG A17-A13 (F-010). AT = register number
        lda #0
        sta t0
@reg:   ldx t0
        lda reg_idx,x
        sta MAP_BANKSEL
        lda reg_win,x
        sta ptr+1
        lda #0
        sta ptr
        sta t1
@val:   ldx t1
        lda width_vals,x
        sta MAP_BANKDATA
        and #$1F
        sta t2
        ldy #0
        lda (ptr),y
        cmp t2
        bne @bad
        inc t1
        lda t1
        cmp #5
        bcc @val
        inc t0
        lda t0
        cmp #3
        bcc @reg
        lda #0
        rts
@bad:   sta det0
        lda t2
        sta det1
        lda t0
        sta det2
        lda #2
        rts

t_prg_all:                      ; R6 = 0..31 shows bank n (identity bytes n and ~n). code 3: byte 1 wrong
        lda #6
        sta MAP_BANKSEL
        lda #0
        sta t0
@l:     lda t0
        sta MAP_BANKDATA
        lda $8000
        cmp t0
        bne @b0
        lda $8001
        eor #$FF
        cmp t0
        bne @b1
        inc t0
        lda t0
        cmp #32
        bcc @l
        lda #0
        rts
@b0:    sta det0
        lda t0
        sta det1
        sta det2
        lda #2
        rts
@b1:    sta det0
        lda t0
        sta det1
        sta det2
        lda #3
        rts

; ---------------------------------------------------------------- CHR
t_chr_1k:                       ; K = 1: R0 R8 R1 R9 R2 R3 R4 R5 (F-006)
        lda #$20
        jsr base_load
        SETPTR2 exp_k1c0
        jmp check_chr

t_chr_2k:                       ; K = 0: R0 and R1 each cover a pair of 1 KB halves
        lda #$00
        jsr base_load
        SETPTR2 exp_k0c0
        jmp check_chr

t_chr_inv:                      ; C = 1 swaps the 4 KB halves. code 2: K=1, code 3: K=0
        lda #$A0
        jsr base_load
        SETPTR2 exp_k1c1
        jsr check_chr
        beq :+
        rts
:       lda #$80
        jsr base_load
        SETPTR2 exp_k0c1
        jsr check_chr
        beq :+
        lda #3
:       rts

cw_vals: .byte $01, $02, $04, $08, $10, $20, $40, $80, $FF, $A5, $5A, $00
cw_win:  .byte 4, 7                     ; windows driven by R2 and R5 in 1 KB mode, no inversion
cw_idx:  .byte $22, $25

t_chr_width:                    ; all 8 bits of R2 and R5 reach CHR A17-A10 (F-007). AT = value written
        lda #$20
        jsr base_load
        lda #0
        sta t0
@reg:   ldx t0
        lda cw_idx,x
        sta MAP_BANKSEL
        lda #0
        sta t1
@val:   ldx t1
        lda cw_vals,x
        sta MAP_BANKDATA
        sta t2
        ldx t0
        lda cw_win,x
        jsr chr_id
        cmp t2
        bne @bad
        inc t1
        lda t1
        cmp #12
        bcc @val
        inc t0
        lda t0
        cmp #2
        bcc @reg
        lda #0
        rts
@bad:   sta det0
        lda t2
        sta det1
        sta det2
        lda #2
        rts

t_chr_odd:                      ; 2 KB mode: bit 0 of R0 and R1 is replaced by PPU A10 (F-007)
        lda #$00
        jsr base_load
        lda #0
        sta MAP_BANKSEL
        lda #$23
        sta MAP_BANKDATA
        lda #1
        sta MAP_BANKSEL
        lda #$45
        sta MAP_BANKDATA
        SETPTR2 exp_odd
        jmp check_chr

; R8 and R9 are written while K = 0, kept, and only used when K = 1 (F-049). code 2: K=1 does not show R8/R9;
; 3: new R8 not used; 4: K=0 does not ignore R8; 5: R8 lost when K went 0 -> 1.
t_r8r9: lda #$00
        jsr base_load           ; R8 = $D0, R9 = $F0 were written with K = 0
        MODECHK $20, check_chr, exp_k1c0, 2
        lda #$28
        sta MAP_BANKSEL
        lda #$11
        sta MAP_BANKDATA        ; R8 := $11 with K = 1
        lda #$20
        sta MAP_BANKSEL
        SETPTR2 exp_r8
        jsr check_chr
        beq :+
        lda #3
        rts
:       MODECHK $00, check_chr, exp_k0c0, 4
        MODECHK $20, check_chr, exp_r8, 5
        lda #0
        rts

; K, P and C take effect from an $8000 write alone, with no $8001 following (F-040, run 35). code = step number.
t_modebits:
        lda #$20
        jsr base_load
        MODECHK $00, check_chr, exp_k0c0, 2
        MODECHK $20, check_chr, exp_k1c0, 3
        MODECHK $60, check_prg, exp_p1, 4
        MODECHK $20, check_prg, exp_p0, 5
        MODECHK $A0, check_chr, exp_k1c1, 6
        MODECHK $80, check_chr, exp_k0c1, 7
        lda #0
        rts

; Indices $A-$E select nothing (F-039); bit 4 of $8000 changes nothing (F-040). code 2: an index A-E changed the mapping;
; 3: bit 4 set stopped R2 being written; 4: bit 4 set stopped R6 being written.
t_idx:  lda #$20
        jsr base_load
        jsr fp_save
        ldx #10
@l:     txa
        ora #$20
        sta MAP_BANKSEL
        lda #$77
        sta MAP_BANKDATA
        inx
        cpx #15
        bcc @l
        lda #$20
        sta MAP_BANKSEL
        jsr fp_check
        beq :+
        lda #2
        rts
:       lda #$32
        sta MAP_BANKSEL
        lda #$99
        sta MAP_BANKDATA
        lda #4
        jsr chr_id
        cmp #$99
        beq :+
        sta det0
        lda #$99
        sta det1
        lda #3
        rts
:       lda #$36
        sta MAP_BANKSEL
        lda #$07
        sta MAP_BANKDATA
        lda $8000
        cmp #7
        beq :+
        sta det0
        lda #7
        sta det1
        lda #4
        rts
:       lda #0
        rts

mir_vals: .byte $00, $01, $FE, $FF, $02, $80, $7E
mir_exp:  .byte 0,   1,   0,   1,   0,   0,   0

t_mirror:                       ; only bit 0 of $A000 matters: 0 = vertical (GOT 0), 1 = horizontal (F-041). AT = value written
        lda #0
        sta t0
@l:     ldx t0
        lda mir_vals,x
        sta MAP_MIRROR
        jsr mirror_probe
        ldx t0
        cmp mir_exp,x
        bne @bad
        inc t0
        lda t0
        cmp #7
        bcc @l
        lda #0
        rts
@bad:   sta det0
        lda mir_exp,x
        sta det1
        lda mir_vals,x
        sta det2
        lda #2
        rts

a001_vals: .byte $00, $01, $40, $80, $C0, $FF

t_a001:                         ; $A001 changes no mapping and no mirroring (F-042). code 3: mirroring changed
        lda #$20
        jsr base_load
        jsr fp_save
        lda #0
        sta t0
@l:     ldx t0
        lda a001_vals,x
        sta MAP_A001
        jsr fp_check
        beq :+
        lda #2
        rts
:       jsr mirror_probe
        beq :+
        sta det0
        lda #0
        sta det1
        lda #3
        rts
:       inc t0
        lda t0
        cmp #6
        bcc @l
        lda #0
        rts

.include "common/lib.s"
