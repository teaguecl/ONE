; Tiny CNROM (mapper 3) test ROM for the save-state round trip (tests/CMakeLists.txt, savestate_roundtrip_cnrom).
; 4 CHR banks: tile 0 is a solid color (bank n draws the screen in color n) and every other byte is $10*n+1.
; Every NMI reads CHR $0010 through $2007 with the selected bank into a running sum (in the trace as register A),
; then selects bank (frame & 3). A state saved at scanline 240 is taken between a bank write and the next read,
; so a lost CHR bank register shows up in the first frame after loading it. The NMI also adds $6000+(frame & 7)
; to the sum and writes the sum back there (CNROM work RAM, MESEN_COMPATIBILITY builds only).
; Build: ca65 cnrom.s && ld65 -C cnrom.cfg -o cnrom.nes cnrom.o
.segment "HEADER"
    .byte "NES", $1A, 2, 4, $30, $00, 0,0,0,0,0,0,0,0 ; 32 KB PRG, 4 x 8 KB CHR, mapper 3, horizontal mirroring
.segment "ZEROPAGE"
frame: .res 1
sum:   .res 1
.segment "CODE"
reset:  sei
        cld
        ldx #$FF
        txs
        inx
        stx $2000               ; NMI off
        stx $2001               ; rendering off
        stx frame
        stx sum
        bit $2002
vbl1:   bit $2002
        bpl vbl1
vbl2:   bit $2002
        bpl vbl2
        lda #$3F                ; palette: background colors 0-3
        sta $2006
        ldx #0
        stx $2006
pal:    lda palette,x
        sta $2007
        inx
        cpx #4
        bne pal
        lda #$20                ; nametable 0 + attributes: all tile 0, palette 0
        sta $2006
        lda #0
        sta $2006
        ldy #4
        tax
fill:   sta $2007
        inx
        bne fill
        dey
        bne fill
        txa                     ; bank 0 (bus conflicts: write a ROM byte holding the same value)
        sta banks
        sta $2005
        sta $2005
        lda #$80                ; NMI on
        sta $2000
        lda #$0A                ; background on, also in the left 8 pixels
        sta $2001
forever: jmp forever

nmi:    pha
        txa
        pha
        bit $2002
        lda #$00                ; CHR $0010 in the selected bank
        sta $2006
        lda #$10
        sta $2006
        lda $2007               ; (the read buffer's old contents)
        lda $2007
        clc
        adc sum
        sta sum
        inc frame
        lda frame
        and #7
        tax
        lda $6000,x             ; work RAM (MESEN_COMPATIBILITY builds; otherwise open bus): the byte written
        adc sum                 ; 8 frames ago
        sta $6000,x
        lda frame
        and #3
        tax
        sta banks,x             ; select bank (frame & 3)
        lda #0
        sta $2006
        sta $2006
        sta $2005
        sta $2005
        lda #$80
        sta $2000
        pla
        tax
        pla
irq:    rti

palette: .byte $0F, $16, $2A, $12
banks:   .byte 0, 1, 2, 3

.segment "VECTORS"
    .word nmi, reset, irq

; bank n: tile 0 = color n (plane 0 = bit 0 of n, plane 1 = bit 1), the rest $10*n+1
.macro chr_bank n
    .repeat 8
        .byte (n & 1) * $FF
    .endrepeat
    .repeat 8
        .byte ((n >> 1) & 1) * $FF
    .endrepeat
    .res 8192 - 16, $10 * n + 1
.endmacro
.segment "CHR"
    chr_bank 0
    chr_bank 1
    chr_bank 2
    chr_bank 3
