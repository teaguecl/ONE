; Cycle-exact delay primitive shared by the timing ROMs.
; 32-cycle delay blocks: X = q >= 1; time = 32q + 11 including JSR and RTS. Kept inside one page so no branch crosses it.
.segment "ALIGNED"
delay_q:
@l:     .repeat 12
        nop
        .endrepeat
        bit $00
        dex
        bne @l
        rts
.code

