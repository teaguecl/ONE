; RAMBO-1 (mapper 64) IRQ test ROMs, one source, built with -D MODE=n (see build.sh). They isolate what Mesen's trace shows about the IRQ timing:
;   MODE 0..5: A12 clocked by rendering (BG at $0000, sprites at $1000: one rise per line at dot ~260), latch 0 = an IRQ on every line. The phase of
;     the rise against the CPU cycle shifts by 2 dots per line, so three consecutive lines show all three phases. The main loop is one of six
;     instruction types (0 taken BNE, 1 INC zp + JMP, 2 STA abs + JMP, 3 LDA abs + JMP, 4 INC abs + JMP, 5 NOP + JMP), so the trace shows how many
;     cycles after the rise the IRQ is taken, for each phase and for each kind of instruction the IRQ interrupts. 240 IRQs, then IRQs are left disabled.
;   MODE 6: cycle mode ($C001 bit 0 = 1), latch 20: an IRQ every 21 * 4 CPU cycles. 240 IRQs, then stop.
;   MODE 7: A12 clocked by $2006 writes (rendering off), for latch 0, 1, 2, 3, 10, 200: how many rises until the IRQ. Results at hits + 0..5 (expected
;     Mesen's formula (latch <= 1: latch + 1, else latch + 2, then decrement): 1, 2, 4, 5, 12, 202), then the program loops forever.
;   MODE 8: as 0..5 with Skull & Crossbones' loop (LDA zp, BEQ taken backwards: 6 cycles), and the handler waits 0 to 3 x 4 extra cycles (count & 3) before RTI, so over the
;     240 IRQs the loop meets the rise at every alignment and phase (Mesen takes the IRQ after a taken BEQ one cycle earlier than ONE does in Skull & Crossbones).
;   MODE 9: as 8 with a 3-cycle taken branch loop `BEQ *` (2 bytes) in place of LDA + BEQ.
;   MODE 10..13: as 8, with 64 real sprites (OAM filled, tiles 0..63 at y = 3 * n, CHR tiles non-empty) so sprite fetches use real tiles: 10 = 8x8 sprites at $1000;
;     11 = 8x16 sprites (the tile number's bit 0 picks the table: odd = $1000, even = $0000); 12 = 8x8 sprites at $0000 and the background at $1000 (the A12 rise comes
;     from the background fetch at dot ~327); 13 = 8x16 sprites (even tiles only) with the background at $1000.
;   MODE 14..17: the IRQ clocked from the pre-render line. 16 frames; each waits for vblank, sets latch (frame & 7), reloads with $C001, enables, and waits in an
;     LDA / BEQ loop for the IRQ (the handler disables it). The first clock is the pre-render line's, so the IRQ comes on line latch + 1 (more for the reload formula's
;     small latches). 14 = 8x8 sprites at $1000 (rise from the sprite fetch), 15 = background at $1000 (rise from the background fetch), 16 = 8x16 sprites (the
;     empty-slot dummy fetches use tile $FF = table $1000), 17 = 8x16 and background at $1000.
;   MODES 18..21: modes 10..13 with rendering started at the beginning of vblank (the OAM fill ends in the middle of a frame otherwise, so sprite evaluation starts
;     mid-line in modes 10..13).
; Build: bash build.sh
.segment "HEADER"
    .byte "NES", $1A, 2, 1, $00, $40, 0,0,0,0,0,0,0,0 ; 32 KB PRG, 8 KB CHR, mapper 64

.segment "ZEROPAGE"
count:  .res 1
flag:   .res 1
hit:    .res 1
.if MODE >= 10
tmp:    .res 1
.endif
idx:    .res 1
.if MODE < 10
tmp:    .res 1
.endif
.segment "BSS"
hits:   .res 8

.segment "PRGBANKS"
    .byte 0
.segment "CODE"
.macro delay n
        .repeat n
        nop
        .endrepeat
.endmacro

.macro ppu_addr addr
        lda #>(addr)
        sta $2006
        lda #<(addr)
        sta $2006
.endmacro

reset:
        sei
        cld
        ldx #$FF
        txs
        inx
        stx $2000
        stx $2001
        lda #$40            ; no APU frame IRQ
        sta $4017
        stx $4010
        bit $2002
:       bit $2002
        bpl :-
:       bit $2002
        bpl :-
        lda #0
        sta count
        sta flag
        sta idx
.if MODE <= 5 || (MODE >= 8 && MODE <= 13)
.if MODE >= 10
        lda #0
        sta $2003
        ldx #0
oam:    txa                 ; sprite n: y = 3 * n, tile = n, attributes 0, x = 4 * n
        lsr
        lsr
        sta tmp
        asl
        adc tmp
        sta $2004
        lda tmp
.if MODE = 13
        asl                 ; even tiles only: A12 stays low through the sprite fetches
.endif
        sta $2004
        lda #0
        sta $2004
        txa
        sta $2004
        inx
        inx
        inx
        inx
        bne oam
.endif
.if MODE = 10
        lda #$08            ; 8x8 sprites at $1000, BG at $0000
.elseif MODE = 11
        lda #$20            ; 8x16 sprites, BG at $0000
.elseif MODE = 12
        lda #$10            ; 8x8 sprites at $0000, BG at $1000
.elseif MODE = 13
        lda #$30            ; 8x16 sprites, BG at $1000
.else
        lda #$08            ; sprites at $1000, BG at $0000
.endif
        sta $2000
        lda #0
        sta $C000           ; latch 0: an IRQ on every clock
        sta $C001           ; reload, A12 mode
        sta $E000
        sta $E001
        lda #$1E            ; BG + sprites on
.ifdef VBLANK
:       bit $2002           ; start rendering at the beginning of a vblank, not in the middle of a line
        bpl :-
.endif
        sta $2001
        cli
.if MODE = 0
        ldx #0
:       beq :-
.elseif MODE = 1
:       inc hit
        jmp :-
.elseif MODE = 2
:       sta $0300
        jmp :-
.elseif MODE = 3
:       lda $0300
        jmp :-
.elseif MODE = 4
:       inc $0300
        jmp :-
.elseif MODE = 8 || MODE >= 10
        ldx #0
        stx hit
:       lda hit
        beq :-
.elseif MODE = 9
        ldx #0
:       beq :-
.else
:       nop
        jmp :-
.endif
.elseif MODE >= 14
.if MODE = 14
        lda #$08
.elseif MODE = 15
        lda #$10
.elseif MODE = 16
        lda #$20
.else
        lda #$30
.endif
        sta $2000
        lda #$1E
        sta $2001
        cli
frame:  bit $2002
        bpl frame           ; wait for vblank
        lda idx
        and #7
        sta $E000
        sta $C000
        ldx #0
        stx $C001           ; reload, A12 mode
        stx hit
        sta $E001
wait:   lda hit
        beq wait
        inc idx
        lda idx
        cmp #16
        bne frame
done:   jmp done
.elseif MODE = 6
        lda #20
        sta $C000
        lda #1
        sta $C001           ; cycle mode, prescaler restarts
        sta $E000
        sta $E001
        cli
:       nop
        jmp :-
.else
        ldx #0
next:   lda latches, x
        sta $E000
        sta $C000
        ldy #0
        sty $C001           ; reload, A12 mode
        sty flag
        sta $E001
        cli
rise:   ppu_addr $0000      ; A12 low
        delay 14
        iny
        ppu_addr $1000      ; A12 high: one clock
        delay 20
        lda flag
        beq rise
        sei
        lda hit
        sta hits, x
        lda #0
        sta flag
        inx
        cpx #6
        bne next
done:   jmp done
latches: .byte 0, 1, 2, 3, 10, 200
.endif

irq:
.if MODE >= 14
        sta $E000
        lda #1
        sta hit
        rti
.elseif MODE = 7
        sta $E000
        sty hit
        lda #1
        sta flag
        rti
.else
        sta $E000
        inc count
        lda count
.if MODE >= 8
        pha
        and #3
        tax
        beq :++
:       dex
        bne :-
:       pla
.endif
        cmp #240
        bcs :+
        sta $E001
:       rti
.endif

nmi:    rti

.segment "VECTORS"
    .word nmi, reset, irq

.segment "CHR"
.repeat $2000
    .byte $5A
.endrepeat
