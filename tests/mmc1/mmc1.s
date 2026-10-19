; Tiny MMC1 (mapper 1, SNROM-like: 128 KB PRG-ROM, 32 KB CHR-ROM, 8 KB work RAM, no battery) test ROM for the
; save-state round trip (tests/CMakeLists.txt, savestate_roundtrip_mmc1). Every MMC1 register is in use all the
; time, so a register that a save state loses shows up in the trace soon after the state is loaded:
;   main loop: once a frame one bank register, in turn (PRG bank k: $E000, CHR $0000 = bank k: $A000, CHR $1000 =
;     bank 7-k: $C000; k = frame & 7), so each keeps its value for 3 frames, and the work RAM byte $6000+k; for the
;     rest of the frame the control register again and again (mirroring vertical for even k, horizontal for odd;
;     each write recomputes the banks from all three bank registers) with pauses that add LDA $8000 (bank k starts
;     with $11*k) to a sum. A save lands anywhere in this (the test saves at several frames).
;   NMI (never touches the mapper): CHR $0010 and $1010 (bank n is filled with $10*n+1), nametable $2400 (CIRAM B
;     holds $55, CIRAM A $AA: vertical mirroring reads $55, horizontal $AA) and $6000+(frame & 7), summed in A.
; Build: ca65 mmc1.s && ld65 -C mmc1.cfg -o mmc1.nes mmc1.o
.segment "HEADER"
    .byte "NES", $1A, 8, 4, $10, $00, 0,0,0,0,0,0,0,0 ; 8 x 16 KB PRG, 4 x 8 KB CHR, mapper 1, no battery
.segment "ZEROPAGE"
k:     .res 1
sum:   .res 1
nsum:  .res 1
frame: .res 1
reg:   .res 1
seen:  .res 1

.macro prg_bank n                      ; banks 0-6: signature byte, then filler
.segment .concat("BANK", .string(n))
    .byte $11 * n
    .res $4000 - 1, $EA
.endmacro
prg_bank 0
prg_bank 1
prg_bank 2
prg_bank 3
prg_bank 4
prg_bank 5
prg_bank 6

.segment "FIXED"                        ; bank 7, at $C000 in PRG mode 3
    .byte $77
.macro serial addr                      ; A -> MMC1 register: 5 writes, bit 0 first
    .repeat 5
        sta addr
        lsr a
    .endrepeat
.endmacro
.proc set_control
    serial $8000
    rts
.endproc
fill_nametable:                         ; A -> all 1 KB at $2000
    ldx #$20
    stx $2006
    ldx #$00
    stx $2006
    ldy #4
@loop: sta $2007
    inx
    bne @loop
    dey
    bne @loop
    rts

reset:  sei
        cld
        ldx #$FF
        txs
        inx
        stx $2000
        stx $2001
        stx k
        stx sum
        stx nsum
        stx frame
        stx reg
        lda #$80                        ; MMC1 reset
        sta $8000
        bit $2002
vbl1:   bit $2002
        bpl vbl1
vbl2:   bit $2002
        bpl vbl2
        lda #$1C                        ; 4 KB CHR, PRG mode 3, one-screen CIRAM A
        jsr set_control
        lda #$AA
        jsr fill_nametable
        lda #$1D                        ; one-screen CIRAM B
        jsr set_control
        lda #$55
        jsr fill_nametable
        lda #$3F                        ; palette
        sta $2006
        ldx #0
        stx $2006
pal:    lda palette,x
        sta $2007
        inx
        cpx #4
        bne pal
        lda #0
        sta $2005
        sta $2005
        lda #$80                        ; NMI on
        sta $2000
        lda #$0A                        ; background on
        sta $2001

main:   lda frame                       ; once a frame (after the NMI): one bank register, in turn, so each one
        sta seen                        ; keeps its value for 3 frames
        and #7
        sta k
        ldx reg
        inx
        cpx #3
        bne :+
        ldx #0
:       stx reg
        lda k
        cpx #1
        beq chr0
        bcs chr1
        serial $E000                    ; PRG bank k (bit 4 = 0: work RAM enabled)
        jmp banked
chr0:   serial $A000                    ; CHR $0000 = 4 KB bank k
        jmp banked
chr1:   eor #7
        serial $C000                    ; CHR $1000 = 4 KB bank 7-k
banked: ldx k
        lda sum
        sta $6000,x
idle:   lda k                           ; for the rest of the frame: the control register again and again
        and #1                          ; (4 KB CHR, PRG mode 3, vertical (k even) / horizontal (k odd); every
        ora #$1E                        ; write recomputes the banks from the bank registers), then a pause that
        serial $8000                    ; adds LDA $8000 (bank k starts with $11*k) to a sum
        ldy #20
pause:  lda $8000
        clc
        adc sum
        sta sum
        dey
        bne pause
        lda frame
        cmp seen
        beq idle
        jmp main

nmi:    pha
        txa
        pha
        bit $2002
        lda #$00                        ; CHR $0010
        sta $2006
        lda #$10
        sta $2006
        lda $2007
        lda $2007
        clc
        adc nsum
        sta nsum
        lda #$10                        ; CHR $1010
        sta $2006
        lda #$10
        sta $2006
        lda $2007
        lda $2007
        clc
        adc nsum
        sta nsum
        lda #$24                        ; nametable $2400
        sta $2006
        lda #$00
        sta $2006
        lda $2007
        lda $2007
        clc
        adc nsum
        sta nsum
        inc frame
        lda frame
        and #7
        tax
        lda $6000,x                     ; work RAM
        clc
        adc nsum
        sta nsum
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

.segment "VECTORS"
    .word nmi, reset, irq

.macro chr_bank n                       ; 4 KB bank n: tile 0 solid color n & 3, the rest $10*n+1
    .repeat 8
        .byte (n & 1) * $FF
    .endrepeat
    .repeat 8
        .byte ((n >> 1) & 1) * $FF
    .endrepeat
    .res 4096 - 16, $10 * n + 1
.endmacro
.segment "CHR"
    chr_bank 0
    chr_bank 1
    chr_bank 2
    chr_bank 3
    chr_bank 4
    chr_bank 5
    chr_bank 6
    chr_bank 7
