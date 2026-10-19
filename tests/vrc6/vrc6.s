; VRC6 test ROM: what a CPU trace of the program shows of the mirroring ($B003), the CHR banking modes and the PRG banking. The program
; reads everything back through the PPU's $2007 and the CPU's own reads, so the values are in a Mesen trace (the register after each
; LDA $2007 / LDA $8000 line) and ONE's trace must show the same ones. About 60,000 CPU cycles; nothing needs pressing.
;   Part 1  the CIRAM pages get markers ($A0 page 0, $B0 page 1) and the CHR registers distinct numbers; then for every $B003 value 0-63 (Mesen ignores bits 6-7: checked with 0-255) the first
;           byte of the four name tables $2000, $2400, $2800, $2C00 is read: $A0 / $B0 = that CIRAM page, anything else = CHR-ROM (the number of the
;           1KB bank). Results at nt_result + value * 4.
;   Part 2  for CHR mode 0-3 ($B003 = $20 + mode): set the eight CHR registers to odd numbers $11, $23, $35, $47, $59, $6B, $7D, $0F and
;           read the first byte of each 1KB slot of the pattern tables (each 1KB of CHR-ROM starts with its own number).
;           Results at chr_result + mode * 8 + slot.
;   Part 4  Part 1 again for $B003 0-63 with the CHR registers R4-R7 = $44, $57, $68, $79 (the other parity). Results at nt2_result.
;   Part 5  Part 4 with R4-R7 = $45, $57, $68, $78. Results at nt3_result.
;   Part 6  Part 4 with R4-R7 = $45, $56, $66, $79. Results at nt4_result.
;   Part 3  PRG: for $8000 = 0-7 read $8000 and $A000 (each 8KB half of a 16KB bank starts with $80 + bank / $A0 + bank), for $C000 = 0-15
;           read $C000 (an 8KB bank starts with the byte of its half).  Results at prg_result.
; Build (mapper 24, VRC6a):  ca65 vrc6.s -o vrc6a.o && ld65 -C vrc6.cfg -o vrc6a.nes vrc6a.o
;       (mapper 26, VRC6b):  ca65 -D VRC6B vrc6.s -o vrc6b.o && ld65 -C vrc6.cfg -o vrc6b.nes vrc6b.o
.segment "HEADER"
.ifdef VRC6B
    .byte "NES", $1A, 8, 16, $A0, $10, 0,0,0,0,0,0,0,0 ; 128 KB PRG, 128 KB CHR, mapper 26
.else
    .byte "NES", $1A, 8, 16, $80, $10, 0,0,0,0,0,0,0,0 ; mapper 24
.endif

.ifdef VRC6B
VRC6B_SWAP = 1
.else
VRC6B_SWAP = 0
.endif

; STA to register N (0-3) of the group at BASE: the VRC6b swaps A0 and A1
.macro sta_reg base, n
.if VRC6B_SWAP
        sta base + (((n) .bitand 1) << 1) + ((n) >> 1)
.else
        sta base + (n)
.endif
.endmacro

.segment "BSS"
nt_result:  .res 64 * 4
chr_result: .res 4 * 8
prg_result: .res 8 * 2 + 16
nt2_result: .res 64 * 4
nt3_result: .res 64 * 4
nt4_result: .res 64 * 4

.segment "ZEROPAGE"
ptr: .res 2

.macro ppu_addr addr
        lda #>(addr)
        sta $2006
        lda #<(addr)
        sta $2006
.endmacro

; read VRAM at ADDR into A (the $2007 read is buffered: a dummy read first)
.macro ppu_read addr
        ppu_addr addr
        lda $2007
        lda $2007
.endmacro

.macro ppu_write addr, value
        ppu_addr addr
        lda #value
        sta $2007
.endmacro

.segment "CODE"
reset:  sei
        cld
        ldx #$FF
        txs
        lda #0
        sta $2000
        sta $2001
        bit $2002
:       bit $2002
        bpl :-
:       bit $2002
        bpl :-

        lda #0
        sta $8000               ; PRG 16KB bank 0
        sta $C000               ; PRG 8KB bank 0
        lda #$20
        sta $B003

        ; ---- Part 1: mirroring --------------------------------------------------------------------------------------------------
        ; the two CIRAM pages get their markers while $B003 = $20 (vertical): $A0 on page 0 (name table $2000), $B0 on page 1 ($2400);
        ; the CHR registers get distinct numbers, so a name table that comes from CHR-ROM reads the bank's own number
        lda #$20
        sta $B003
        ppu_write $2000, $A0
        ppu_write $2400, $B0
        lda #$41
        sta_reg $D000, 0
        lda #$52
        sta_reg $D000, 1
        lda #$63
        sta_reg $D000, 2
        lda #$74
        sta_reg $D000, 3
        lda #$45
        sta_reg $E000, 0
        lda #$56
        sta_reg $E000, 1
        lda #$67
        sta_reg $E000, 2
        lda #$78
        sta_reg $E000, 3
        lda #<nt_result
        sta ptr
        lda #>nt_result
        sta ptr + 1
        ldx #0
p1:     txa
        sta $B003
        ldy #0
        ppu_read $2000
        sta (ptr), y
        iny
        ppu_read $2400
        sta (ptr), y
        iny
        ppu_read $2800
        sta (ptr), y
        iny
        ppu_read $2C00
        sta (ptr), y
        clc
        lda ptr
        adc #4
        sta ptr
        bcc :+
        inc ptr + 1
:       inx
        cpx #64
        beq :+
        jmp p1
:

        ; ---- Part 2: CHR banking modes ---------------------------------------------------------------------------------------------
        ldx #0
p2:     txa
        ora #$20
        sta $B003
        lda #$11
        sta_reg $D000, 0
        lda #$23
        sta_reg $D000, 1
        lda #$35
        sta_reg $D000, 2
        lda #$47
        sta_reg $D000, 3
        lda #$59
        sta_reg $E000, 0
        lda #$6B
        sta_reg $E000, 1
        lda #$7D
        sta_reg $E000, 2
        lda #$0F
        sta_reg $E000, 3
        txa
        asl
        asl
        asl
        tay                     ; Y = mode * 8
        .repeat 8, slot
        ppu_read slot * $400
        sta chr_result + slot, y
        .endrepeat
        inx
        cpx #4
        beq :+
        jmp p2
:

        ; ---- Part 3: PRG banking ---------------------------------------------------------------------------------------------------
        ldx #0
p3:     txa
        sta $8000
        lda $8000
        txa
        asl
        tay
        lda $8000
        sta prg_result, y
        lda $A000
        sta prg_result + 1, y
        inx
        cpx #8
        bne p3
        ldx #0
p3c:    txa
        sta $C000
        lda $C000
        sta prg_result + 16, x
        inx
        cpx #16
        bne p3c

        ; ---- Part 4: the CHR-ROM name tables again with the other parity of R4-R7 ----------------------------------------------------
        ; Part 1 had R6 odd and R7 even, which cannot tell a bank number used as it is from one forced even / odd. Here R4 and R6 are even and
        ; R5 and R7 odd ($44, $57, $68, $79). $B003 0-63. Results at nt2_result + value * 4.
        lda #$44
        sta_reg $E000, 0
        lda #$57
        sta_reg $E000, 1
        lda #$68
        sta_reg $E000, 2
        lda #$79
        sta_reg $E000, 3
        lda #<nt2_result
        sta ptr
        lda #>nt2_result
        sta ptr + 1
        ldx #0
p4:     txa
        sta $B003
        ldy #0
        ppu_read $2000
        sta (ptr), y
        iny
        ppu_read $2400
        sta (ptr), y
        iny
        ppu_read $2800
        sta (ptr), y
        iny
        ppu_read $2C00
        sta (ptr), y
        clc
        lda ptr
        adc #4
        sta ptr
        bcc :+
        inc ptr + 1
:       inx
        cpx #64
        beq :+
        jmp p4
:

        ; ---- Part 5: the same with R4-R7 = $45, $57, $68, $78 ----------------------------------------------------
        ; Mesen's CIRAM name tables turned out to follow the low bit of R4-R7 (parts 1 and 4 flip them); here R4 and R5 are odd, R6 and R7 even,
        ; which tells R4 from R6 and R5 from R7. $B003 0-63. Results at nt3_result + value * 4.
        lda #$45
        sta_reg $E000, 0
        lda #$57
        sta_reg $E000, 1
        lda #$68
        sta_reg $E000, 2
        lda #$78
        sta_reg $E000, 3
        lda #<nt3_result
        sta ptr
        lda #>nt3_result
        sta ptr + 1
        ldx #0
p5:     txa
        sta $B003
        ldy #0
        ppu_read $2000
        sta (ptr), y
        iny
        ppu_read $2400
        sta (ptr), y
        iny
        ppu_read $2800
        sta (ptr), y
        iny
        ppu_read $2C00
        sta (ptr), y
        clc
        lda ptr
        adc #4
        sta ptr
        bcc :+
        inc ptr + 1
:       inx
        cpx #64
        beq :+
        jmp p5
:

        ; ---- Part 6: the same with R4-R7 = $45, $56, $66, $79 ----------------------------------------------------
        ; Parts 1, 4 and 5 gave R4/R7 and R5/R6 complementary low bits, so they could not tell those pairs apart; here the low bits are
        ; 1, 0, 0, 1 (R4 and R7 alike, R5 and R6 alike). $B003 0-63. Results at nt4_result + value * 4.
        lda #$45
        sta_reg $E000, 0
        lda #$56
        sta_reg $E000, 1
        lda #$66
        sta_reg $E000, 2
        lda #$79
        sta_reg $E000, 3
        lda #<nt4_result
        sta ptr
        lda #>nt4_result
        sta ptr + 1
        ldx #0
p6:     txa
        sta $B003
        ldy #0
        ppu_read $2000
        sta (ptr), y
        iny
        ppu_read $2400
        sta (ptr), y
        iny
        ppu_read $2800
        sta (ptr), y
        iny
        ppu_read $2C00
        sta (ptr), y
        clc
        lda ptr
        adc #4
        sta ptr
        bcc :+
        inc ptr + 1
:       inx
        cpx #64
        beq :+
        jmp p6
:

done:   jmp done

nmi:    rti

.segment "VECTORS"
    .word nmi, reset, nmi

; PRG banks 0-6 (the last 16KB is the dummy $C000-$DFFF and the code at $E000): $80 + bank at the start of the 16KB bank, $A0 + bank at its
; second 8KB
.segment "PRGBANKS"
.repeat 7, b
    .byte $80 + b
    .res $1FFF, 0
    .byte $A0 + b
    .res $1FFF, 0
.endrepeat

; CHR: 128 banks of 1KB, each starts with its own number
.segment "CHR"
.repeat 128, b
    .byte b
    .res $3FF, 0
.endrepeat
