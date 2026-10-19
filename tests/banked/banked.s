; Tiny MMC1 test ROM for the debugger's bank-exact labels (tests/CMakeLists.txt, debugger_banked_labels):
; banks 0-2 each have a routine at $8000, bank 3 is fixed at $C000.  No battery, so no .sav file is written.
; Build: ca65 -g banked.s && ld65 -C banked.cfg -o banked.nes --dbgfile banked.dbg banked.o
.segment "HEADER"
    .byte "NES", $1A, 4, 1, $10, $00, 0,0,0,0,0,0,0,0
.segment "ZEROPAGE"
counter: .res 1
.segment "SRAM"
save_flag: .res 1
.segment "BANK0"
bank0_func:  lda #0
             rts
.segment "BANK1"
bank1_func:  lda #1
             sta counter
             rts
.segment "BANK2"
bank2_func:  lda #2
             sta counter
             rts
.segment "FIXED"
.proc select_bank          ; A = bank: 5 serial writes to $E000
    sta $E000
    lsr a
    sta $E000
    lsr a
    sta $E000
    lsr a
    sta $E000
    lsr a
    sta $E000
    rts
.endproc
reset:  sei
        cld
        ldx #$FF
        txs
        lda #$80
        sta $8000
        lda #1
        jsr select_bank
        jsr bank1_func
after_bank1:
        lda #2
        jsr select_bank
        jsr bank2_func
after_bank2:
        lda #1
        sta save_flag
forever: jmp forever
nmi:    rti
.segment "VECTORS"
    .word nmi, reset, nmi
.segment "CHR"
    .res 8192, 0
