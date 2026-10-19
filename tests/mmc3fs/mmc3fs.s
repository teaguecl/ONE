; Four-screen MMC3 test ROM (iNES mapper 4 with the four-screen flag, as Gauntlet and Rad Racer II): the cartridge has 4KB of name table RAM, and $A000 does
; nothing. Markers $A0-$A3 are written to the first byte and $B0-$B3 to the last byte ($3FF) of the four name tables, then all eight are read back
; (results at result + 0..7, then the same after $A000 = 0 at result + 8..15 and $A000 = 1 at result + 16..23). Then $5A is written to $2800 and
; $2000 is read back (it must keep its marker), and the pattern table byte at $0000 is checked not to change (result + 24, 25).
; Build: ca65 mmc3fs.s -o mmc3fs.o && ld65 -C mmc3fs.cfg -o mmc3fs.nes mmc3fs.o
.segment "HEADER"
    .byte "NES", $1A, 2, 1, $48, $00, 0,0,0,0,0,0,0,0 ; 32 KB PRG, 8 KB CHR, mapper 4, four-screen

.segment "BSS"
result: .res 32

.segment "ZEROPAGE"
ptr: .res 2

.macro ppu_addr addr
        lda #>(addr)
        sta $2006
        lda #<(addr)
        sta $2006
.endmacro

.macro ppu_write addr, value
        ppu_addr addr
        lda #value
        sta $2007
.endmacro

.macro ppu_read addr
        ppu_addr addr
        lda $2007
        lda $2007
.endmacro

.macro readback
        ppu_read $2000
        sta (ptr), y
        iny
        ppu_read $23FF
        sta (ptr), y
        iny
        ppu_read $2400
        sta (ptr), y
        iny
        ppu_read $27FF
        sta (ptr), y
        iny
        ppu_read $2800
        sta (ptr), y
        iny
        ppu_read $2BFF
        sta (ptr), y
        iny
        ppu_read $2C00
        sta (ptr), y
        iny
        ppu_read $2FFF
        sta (ptr), y
        iny
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

        lda #<result
        sta ptr
        lda #>result
        sta ptr + 1
        ppu_write $2000, $A0
        ppu_write $23FF, $B0
        ppu_write $2400, $A1
        ppu_write $27FF, $B1
        ppu_write $2800, $A2
        ppu_write $2BFF, $B2
        ppu_write $2C00, $A3
        ppu_write $2FFF, $B3
        ldy #0
        readback
        lda #0
        sta $A000
        readback
        lda #1
        sta $A000
        readback
        ppu_write $2800, $5A
        ppu_read $2000
        sta (ptr), y
        iny
        ppu_read $2800
        sta (ptr), y
        iny
done:   jmp done

nmi:    rti

.segment "VECTORS"
    .word nmi, reset, nmi

.segment "PRGBANKS"
    .res $6000, 0

.segment "CHR"
    .res $2000, 0
