; Shared test framework for the RAMBO-1 test ROMs (assembled into each ROM; included after the tests).
; The ROM source must define: test_table, rom_title, rom_note (zero-terminated strings).

.segment "HEADER"
        ; iNES 2.0: mapper 64, PRG 256 KiB, CHR 256 KiB, header mirroring horizontal (the chip powers on vertical)
        .byte "NES", $1A
        .byte 16, 32            ; PRG in 16 KiB units, CHR in 8 KiB units
        .byte $00               ; flags 6: mapper low nibble 0, horizontal, no battery
        .byte $48               ; flags 7: mapper bits 4-7 = 4 (mapper 64), NES 2.0
        .byte 0, 0, 0, 0, 0, 0, 0, 0

.segment "BANKS"
        .incbin "build/banks.bin"

.segment "IDENT"
        .byte $1F, $E0, "RAMBO-1 PRG ID"     ; identity block of the fixed bank, read back by the tests

.segment "CODE"

reset:
        sei
        cld
        ldx #$40
        stx APUFRAME            ; inhibit the APU frame IRQ so only the mapper can pull /IRQ low
        ldx #$FF
        txs
        inx
        stx PPUCTRL
        stx PPUMASK
        stx $4010
        lda APUSTATUS           ; clear a pending frame IRQ flag
        bit PPUSTATUS
@v1:    bit PPUSTATUS
        bpl @v1
        lda #0
        tax
@clr:   sta $00,x
        sta $0200,x
        sta $0300,x
        sta $0400,x
        sta $0500,x
        sta $0600,x
        sta $0700,x
        inx
        bne @clr
@v2:    bit PPUSTATUS
        bpl @v2
        lda #$80
        sta RESULT_STATUS
        lda #$DE
        sta RESULT_SIG
        lda #$B0
        sta RESULT_SIG+1
        lda #$61
        sta RESULT_SIG+2
        lda #<TEXT
        sta txtp
        lda #>TEXT
        sta txtp+1
        ldx #31
        lda #$FF
@rf:    sta results,x
        dex
        bpl @rf
        jmp main

.ifndef CUSTOM_NMI
nmi:    rti
.endif

.ifndef CUSTOM_IRQ
irq:    pha                     ; count the interrupt and return with I set so a level /IRQ cannot storm
        txa
        pha
        inc irq_count
        tsx
        lda $0103,x
        ora #$04
        sta $0103,x
        pla
        tax
        pla
        rti
.endif

; ---------------------------------------------------------------- text buffer
putc:   cmp #10                 ; A = character; wraps at 32 columns; preserves X, Y
        beq putc_raw
        pha
        lda col
        cmp #32
        bcc @ok
        lda #10
        jsr putc_raw
@ok:    pla
putc_raw:
        sty ytmp
        pha
        lda txtp+1
        cmp #>TEXT_END
        bcs @skip
        ldy #0
        pla
        pha
        sta (txtp),y
        inc txtp
        bne @nc
        inc txtp+1
@nc:    pla
        pha
        cmp #10
        bne @inc
        lda #0
        sta col
        beq @skip
@inc:   inc col
@skip:  pla
        ldy ytmp
        rts

print_str:                      ; ptr -> zero-terminated string
        ldy #0
@l:     lda (ptr),y
        beq @d
        jsr putc
        iny
        bne @l
@d:     rts

print_hex:                      ; A = byte, prints two hex digits
        pha
        lsr a
        lsr a
        lsr a
        lsr a
        jsr @nyb
        pla
        and #$0F
@nyb:   clc
        adc #'0'
        cmp #'9'+1
        bcc @ok
        adc #6
@ok:    jmp putc

newline:
        lda #10
        jmp putc

pad_to:                         ; A = column
        sta t7
@l:     lda col
        cmp t7
        bcs @d
        lda #' '
        jsr putc
        jmp @l
@d:     rts

.macro PRINT label
        lda #<label
        sta ptr
        lda #>label
        sta ptr+1
        jsr print_str
.endmacro

; ---------------------------------------------------------------- PPU helpers (rendering must be off)
chr_id:                         ; A = CHR window 0-7 -> A = identity byte 0 (bank number), X = byte 1 (~bank)
        asl a
        asl a
        clc
        adc #3
        bit PPUSTATUS
        sta PPUADDR
        lda #$F0
        sta PPUADDR
        lda PPUDATA             ; buffered dummy read
        lda PPUDATA
        tay
        ldx PPUDATA
        tya
        rts

ppu_poke:                       ; X = address high byte (low byte 0), A = value
        bit PPUSTATUS
        stx PPUADDR
        ldy #0
        sty PPUADDR
        sta PPUDATA
        rts

ppu_peek:                       ; X = address high byte (low byte 0) -> A
        bit PPUSTATUS
        stx PPUADDR
        ldy #0
        sty PPUADDR
        lda PPUDATA
        lda PPUDATA
        rts

mirror_probe:                   ; -> A = 0 vertical, 1 horizontal, 2 other (nametable aliasing)
        ldx #$20
        lda #$11
        jsr ppu_poke
        ldx #$24
        lda #$22
        jsr ppu_poke
        ldx #$28
        lda #$33
        jsr ppu_poke
        ldx #$2C
        lda #$44
        jsr ppu_poke
        ldx #$20
        jsr ppu_peek
        sta t1
        ldx #$24
        jsr ppu_peek
        sta t2
        ldx #$28
        jsr ppu_peek
        sta t3
        ldx #$2C
        jsr ppu_peek
        sta t4
        lda t1
        cmp t3
        bne @nv
        lda t2
        cmp t4
        bne @nv
        lda t1
        cmp t2
        beq @nv
        lda #0
        rts
@nv:    lda t1
        cmp t2
        bne @oth
        lda t3
        cmp t4
        bne @oth
        lda t1
        cmp t3
        beq @oth
        lda #1
        rts
@oth:   lda #2
        rts

.ifndef CUSTOM_MAIN
; ---------------------------------------------------------------- test runner
main:
        PRINT rom_title
        jsr newline
        PRINT rom_note
        jsr newline
        jsr newline
        lda #<test_table
        sta rptr
        lda #>test_table
        sta rptr+1
        lda #0
        sta tidx
@next:  ldy #1
        lda (rptr),y
        beq @done
        lda #0
        sta det0
        sta det1
        sta det2
        sta disc
        ldy #4
        lda (rptr),y
        cmp #$FF
        beq @run
        tax
        lda results,x
        beq @run                ; dependency passed
        lda #1                  ; skipped
        bne @store
@run:   ldy #0
        lda (rptr),y
        sta jvec
        iny
        lda (rptr),y
        sta jvec+1
        jsr @call
@store: ldx tidx
        sta results,x
        jsr print_result
        clc
        lda rptr
        adc #5
        sta rptr
        bcc @nc
        inc rptr+1
@nc:    inc tidx
        jmp @next
@call:  jmp (jvec)
@done:  jsr newline
        lda nfail
        bne @failed
        PRINT s_allpass
        lda #0
        beq @fin
@failed:
        PRINT s_failed
        lda nfail
        jsr print_hex
        lda first_fail
@fin:   sta t0
        jsr newline
        lda #0                  ; terminate the text
        ldy #0
        sta (txtp),y
        lda t0
        sta RESULT_STATUS
        jmp show_results
.endif

print_result:
        lda tidx
        clc
        adc #1
        jsr print_hex
        lda #' '
        jsr putc
        ldy #2
        lda (rptr),y
        sta ptr
        iny
        lda (rptr),y
        sta ptr+1
        jsr print_str
        lda #28
        jsr pad_to
        ldx tidx
        lda results,x
        bne @nz
        jmp @pass
@nz:    cmp #1
        bne @isf
        jmp @skip
@isf:   PRINT s_fail
        jsr newline
        inc nfail
        ldx tidx
        lda results,x
        ldy first_fail
        bne @has
        sta first_fail
@has:   pha
        lda #' '
        jsr putc
        jsr putc
        PRINT s_code
        pla
        jsr print_hex
        PRINT s_got
        lda det0
        jsr print_hex
        PRINT s_exp
        lda det1
        jsr print_hex
        PRINT s_at
        lda det2
        jsr print_hex
        jmp newline
@pass:  PRINT s_pass
        lda disc
        beq @pn
        jsr newline             ; discovery test: record the three values, never a failure
        PRINT s_info
        lda det0
        jsr print_hex
        lda #' '
        jsr putc
        lda det1
        jsr print_hex
        lda #' '
        jsr putc
        lda det2
        jsr print_hex
@pn:    jmp newline
@skip:  PRINT s_skip
        jmp newline

s_pass:   .asciiz "PASS"
s_fail:   .asciiz "FAIL"
s_skip:   .asciiz "SKIP"
s_info:   .asciiz "  INFO "
s_code:   .asciiz "CODE "
s_got:    .asciiz " GOT "
s_exp:    .asciiz " EXP "
s_at:     .asciiz " AT "
s_allpass: .asciiz "ALL TESTS PASSED"
s_failed:  .asciiz "TESTS FAILED: "

; ---------------------------------------------------------------- screen and beeps
draw_screen:                    ; draw the text buffer on the screen (rendering was off) and turn rendering on; returns
        sei
        ldx #5                  ; put every CHR window on bank 0 (1 KB mode, no inversion) so the font is
@bank:  txa                     ; fetched from the same bank as in G1, whatever state the tests left behind
        ora #$20
        sta $8000
        lda #0
        sta $8001
        dex
        bpl @bank
        sta $A000               ; vertical mirroring
        bit PPUSTATUS
        lda #$3F
        sta PPUADDR
        lda #0
        sta PPUADDR
        lda RESULT_STATUS
        beq @green
        lda #$16
        bne @pal
@green: lda #$1A
@pal:   sta PPUDATA
        lda #$30
        sta PPUDATA
        sta PPUDATA
        sta PPUDATA
        lda #$20                ; clear nametable 0 and its attributes (tile 0 = space)
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
        lda #$20                ; text starts on row 3 (rows 0-2 are lost to TV overscan)
        sta PPUADDR
        lda #$60
        sta PPUADDR
        lda #<TEXT
        sta ptr
        lda #>TEXT
        sta ptr+1
        lda #0
        sta col
        sta row
        tay
@t:     lda (ptr),y
        beq @end
        cmp #10
        beq @nl
        sec
        sbc #32
        sta PPUDATA
        inc col
        bne @adv
@nl:    lda col
        cmp #32
        bcs @nlend
        lda #0
        sta PPUDATA
        inc col
        jmp @nl
@nlend: lda #0
        sta col
        inc row
        lda row
        cmp #25
        bcs @end
@adv:   inc ptr
        bne @t
        inc ptr+1
        jmp @t
@end:   bit PPUSTATUS
        lda #$20
        sta PPUADDR
        lda #0
        sta PPUADDR
        sta PPUSCROLL
        sta PPUSCROLL
        sta PPUCTRL
        lda #$0A                ; background on, left 8 pixels shown
        sta PPUMASK
        rts

show_results:
        jsr draw_screen
beeps:  lda RESULT_STATUS       ; Blargg convention: 1 beep = passed, N beeps = failure code N
        bne @cnt
        lda #1
@cnt:   tax
@b:     lda #$01
        sta APUSTATUS
        lda #$9F
        sta $4000
        lda #$A9
        sta $4002
        lda #$08
        sta $4003
        lda #20
        jsr wait_frames
        lda #0
        sta APUSTATUS
        lda #20
        jsr wait_frames
        dex
        bne @b
        lda #120
        jsr wait_frames
        jmp beeps

wait_frames:                    ; A = frames
        tay
@w:     bit PPUSTATUS
        bpl @w
        dey
        bne @w
        rts

.segment "VECTORS"
        .addr nmi, reset, irq
