; Namco 163 test ROM: CHR banks $E0 and above as CIRAM (the pattern table shows the name table RAM). A CPU trace shows the readbacks.
;   The name tables are set up as $E0 / $E1 (CIRAM pages 0 / 1, vertical) and given markers: $A0-$A3 at the start of page 0, $B0-$B3 at the
;   start of page 1. For each value of $E800 bits 6-7 (0, $40, $80, $C0) the eight CHR registers are set to $E0, $E2, $FF, $DF, $E1, $E3, $E0,
;   $E1 and the first two bytes of each 1KB slot of the pattern tables are read (16 values at slot_result + mode * 18), then $5A is written
;   through $2007 to $0002 (slot 0) and $B5 to $1002 (slot 4) and the name table bytes $2002 and $2402 are read back (2 more values), and
;   restored. CHR-ROM bytes: bank k starts with $40 + k, then zeros.
; Build: ca65 n163.s -o n163.o && ld65 -C n163.cfg -o n163.nes n163.o
.segment "HEADER"
    .byte "NES", $1A, 2, 2, $30, $10, 0,0,0,0,0,0,0,0 ; 32 KB PRG, 16 KB CHR, mapper 19

.segment "BSS"
result: .res 4 * 18

.segment "ZEROPAGE"
ptr: .res 2

.macro ppu_addr addr
        lda #>(addr)
        sta $2006
        lda #<(addr)
        sta $2006
.endmacro

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

.macro chr_reg reg, value
        lda #value
        sta reg
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

        lda #$40
        sta $E000               ; PRG bank 0, sound off
        lda #$00
        sta $F000
        chr_reg $C000, $E0      ; name tables: CIRAM page 0, 1, 0, 1
        chr_reg $C800, $E1
        chr_reg $D000, $E0
        chr_reg $D800, $E1
        ppu_write $2000, $A0
        ppu_write $2001, $A1
        ppu_write $2002, $A2
        ppu_write $2003, $A3
        ppu_write $2400, $B0
        ppu_write $2401, $B1
        ppu_write $2402, $B2
        ppu_write $2403, $B3

        lda #<result
        sta ptr
        lda #>result
        sta ptr + 1
        ldx #0                  ; $E800 mode 0-3
mode:   txa
        asl
        asl
        asl
        asl
        asl
        asl
        sta $E800               ; bits 6-7 = mode, PRG bank 0
        chr_reg $8000, $E0
        chr_reg $8800, $E2
        chr_reg $9000, $FF
        chr_reg $9800, $DF
        chr_reg $A000, $E1
        chr_reg $A800, $E3
        chr_reg $B000, $E0
        chr_reg $B800, $E1
        ldy #0
        .repeat 8, slot
        ppu_read slot * $400
        sta (ptr), y
        iny
        ppu_read slot * $400 + 1
        sta (ptr), y
        iny
        .endrepeat
        ppu_write $0002, $5A    ; into the pattern table: CIRAM page 0 if slot 0 is RAM
        ppu_write $1002, $B5    ; slot 4: page 1
        ppu_read $2002
        sta (ptr), y
        iny
        ppu_read $2402
        sta (ptr), y
        ppu_write $2002, $A2    ; restore
        ppu_write $2402, $B2
        clc
        lda ptr
        adc #18
        sta ptr
        bcc :+
        inc ptr + 1
:       inx
        cpx #4
        beq :+
        jmp mode
:
done:   jmp done

nmi:    rti

.segment "VECTORS"
    .word nmi, reset, nmi

; PRG banks 0-2 ($8000-$DFFF) are empty; the code is in the last 8KB
.segment "PRGBANKS"
    .res $6000, 0

.segment "CHR"
.repeat 16, b
    .byte $40 + b
    .res $3FF, 0
.endrepeat
