# Build the RAMBO-1 test ROMs. Needs cc65 (ca65, ld65) and python3.
ROMS = g1_power_on g2_banking g3_bus g4_cpu_irq g5_a12_irq g6_raster timing_probe delay_probe
all: $(ROMS:%=build/%.nes)

build/banks.bin build/chr.bin: tools/gen_assets.py
	python3 -I tools/gen_assets.py build

build/%.o: %.s common/vars.inc common/nes.inc common/lib.s build/banks.bin
	ca65 -I . -o $@ $<

build/%.nes: build/%.o build/chr.bin rambo.cfg
	ld65 -C rambo.cfg -o build/$*.prg $<
	cat build/$*.prg build/chr.bin > $@
	@ls -l $@ | awk '{print $$5, $$9}'

clean:
	rm -f build/*
.PHONY: all clean
