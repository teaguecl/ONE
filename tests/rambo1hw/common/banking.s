; Helpers shared by the banking-aware ROMs (G2, G3): baseline load, CHR/PRG identity checks, state fingerprint.
; baseline register values: R0-R5, R8, R9, R6, R7, RF
base_idx: .byte 0, 1, 2, 3, 4, 5, 8, 9, 6, 7, 15
base_val: .byte $20, $40, $62, $83, $A4, $C5, $D0, $F0, $05, $0A, $14

.segment "CODE"

base_load:                      ; A = mode bits (K $20, P $40, C $80); loads the baseline, $A000 = 0 (vertical)
        sta t6
        ldy #0
@l:     lda base_idx,y
        ora t6
        sta MAP_BANKSEL
        lda base_val,y
        sta MAP_BANKDATA
        iny
        cpy #11
        bcc @l
        lda t6
        sta MAP_BANKSEL
        lda #0
        sta MAP_MIRROR
        rts

check_chr:                      ; ptr2 -> 8 expected bytes. Z set = ok; else GOT/EXP/AT(window) set
        lda #0
        sta t0
@l:     lda t0
        jsr chr_id
        ldy t0
        cmp (ptr2),y
        bne @bad
        inc t0
        lda t0
        cmp #8
        bcc @l
        lda #0
        rts
@bad:   sta det0
        lda (ptr2),y
        sta det1
        sty det2
        lda #2
        rts

check_prg:                      ; ptr2 -> 4 expected bytes for $8000 $A000 $C000 $E000
        ldy #0
        lda $8000
        cmp (ptr2),y
        bne @bad
        iny
        lda $A000
        cmp (ptr2),y
        bne @bad
        iny
        lda $C000
        cmp (ptr2),y
        bne @bad
        iny
        lda $E000
        cmp (ptr2),y
        bne @bad
        lda #0
        rts
@bad:   sta det0
        lda (ptr2),y
        sta det1
        sty det2
        lda #2
        rts

fp_take:                        ; fingerprint = identity byte of 8 CHR windows + 4 PRG windows
        lda #0
        sta t0
@c:     lda t0
        jsr chr_id
        ldy t0
        sta fpbuf,y
        inc t0
        lda t0
        cmp #8
        bcc @c
        lda $8000
        sta fpbuf+8
        lda $A000
        sta fpbuf+9
        lda $C000
        sta fpbuf+10
        lda $E000
        sta fpbuf+11
        rts

fp_save: jsr fp_take
        ldy #11
@l:     lda fpbuf,y
        sta fpref,y
        dey
        bpl @l
        rts

fp_check:                       ; Z set = fingerprint unchanged
        jsr fp_take
        ldy #11
@l:     lda fpbuf,y
        cmp fpref,y
        bne @bad
        dey
        bpl @l
        lda #0
        rts
@bad:   sta det0
        lda fpref,y
        sta det1
        sty det2
        lda #2
        rts

