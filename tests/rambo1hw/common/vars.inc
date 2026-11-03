.include "common/nes.inc"
.p02

.zeropage
ptr:        .res 2        ; scratch for test routines and print_str
ptr2:       .res 2
t0:         .res 1
t1:         .res 1
t2:         .res 1
t3:         .res 1
t4:         .res 1
t5:         .res 1
t6:         .res 1
t7:         .res 1
det0:       .res 1        ; failure detail: got
det1:       .res 1        ;                 expected
det2:       .res 1        ;                 where (window, index, value...)
irq_count:  .res 1
disc:       .res 1        ; set by a discovery test: the runner prints det0-det2 as an INFO line
rptr:       .res 2        ; runner state
tidx:       .res 1
jvec:       .res 2
txtp:       .res 2
col:        .res 1
row:        .res 1
ytmp:       .res 1
nfail:      .res 1
first_fail: .res 1

.bss
results:    .res 32       ; per test: 0 pass, 1 skipped, >=2 fail code
fpbuf:      .res 16
fpref:      .res 16

.code

; Test table entry: routine, name, dependency (test index or $FF). A zero word ends the table.
.macro TEST routine, name, dep
        .addr routine, name
        .byte dep
.endmacro

.macro SETPTR2 label
        lda #<label
        sta ptr2
        lda #>label
        sta ptr2+1
.endmacro
