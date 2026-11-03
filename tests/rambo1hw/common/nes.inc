; NES hardware and RAMBO-1 register addresses
PPUCTRL    = $2000
PPUMASK    = $2001
PPUSTATUS  = $2002
PPUOAMADDR = $2003
PPUOAMDATA = $2004
PPUSCROLL  = $2005
PPUADDR    = $2006
PPUDATA    = $2007
APUSTATUS  = $4015
APUFRAME   = $4017
JOY1       = $4016

MAP_BANKSEL  = $8000
MAP_BANKDATA = $8001
MAP_MIRROR   = $A000
MAP_A001     = $A001
MAP_IRQLATCH = $C000
MAP_IRQMODE  = $C001
MAP_IRQOFF   = $E000
MAP_IRQON    = $E001

; Result block (replaces Blargg's $6000 protocol: this mapper has no PRG-RAM)
RESULT_STATUS = $0300     ; $80 running, $00 all passed, else code of the first failing test
RESULT_SIG    = $0301     ; $DE $B0 $61
TEXT          = $0304     ; zero-terminated report text
TEXT_END      = $0800
