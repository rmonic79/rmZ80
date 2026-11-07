# This file is part of rmZ80, a Zilog Z80 core in SystemVerilog.
# Copyright (c) 2026 Umberto Parisi (rmonic79)
#
# rmZ80 is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
# License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any
# later version. It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
# more details: LICENSE in the root of the repository, or <https://www.gnu.org/licenses/>.
#
# A commercial license, for use without the obligations of the GPL (for example in closed-source products), is
# available from the author, who can also be contacted for other licensing needs.

# rmZ80 benches — one target per bench: each builds (Verilator) and runs.
# To be run in the MSYS2 mingw64 shell (where verilator and g++ live).

VERILATOR ?= verilator
RTL   = ../rtl/rmZ80_alu_pkg.sv ../rtl/rmZ80_alu_dec.sv ../rtl/rmZ80_alu_daa.sv ../rtl/rmZ80_alu_core.sv ../rtl/rmZ80_alu.sv
TB    = tb_rmZ80_alu.cpp
VOPTS = --cc --exe --build -j 2 -Wall -O3 --top-module rmZ80_alu --prefix Valu

# no target is a file: without this line `make z80test` finds the folder z80test/ and does nothing
.PHONY: test alu exec decode seq regfile zex zexf z80test sst

# the whole suite, one command, one verdict (run_tests.sh): make test
test:
	sh run_tests.sh

alu:
	$(VERILATOR) $(VOPTS) --Mdir obj_alu_nmos -GVARIANT_SCF_CCF_NMOS=1 \
	  -CFLAGS "-O2 -DTB_VARIANT_NMOS=1" $(RTL) $(TB) -o tb_alu_nmos
	$(VERILATOR) $(VOPTS) --Mdir obj_alu_cmos -GVARIANT_SCF_CCF_NMOS=0 \
	  -CFLAGS "-O2 -DTB_VARIANT_NMOS=0" $(RTL) $(TB) -o tb_alu_cmos
	obj_alu_nmos/tb_alu_nmos
	obj_alu_cmos/tb_alu_cmos

exec:
	$(VERILATOR) --cc --exe --build -j 2 -Wall -O3 --top-module rmZ80 --prefix Vcpu --Mdir obj_cpu -CFLAGS "-O2" ../rtl/rmZ80.sv ../rtl/rmZ80_cw.sv ../rtl/rmZ80_seq.sv ../rtl/rmZ80_regfile.sv ../rtl/rmZ80_alu_pkg.sv ../rtl/rmZ80_alu_dec.sv ../rtl/rmZ80_alu_daa.sv ../rtl/rmZ80_alu_core.sv ../rtl/rmZ80_alu.sv tb_rmZ80_exec.cpp -o tb_exec
	obj_cpu/tb_exec

decode:
	$(VERILATOR) --cc --exe --build -j 2 -Wall -O3 --top-module rmZ80_cw 	  --prefix Vdec --Mdir obj_dec -CFLAGS "-O2" 	  ../rtl/rmZ80_alu_pkg.sv ../rtl/rmZ80_cw.sv tb_rmZ80_decode.cpp -o tb_decode
	obj_dec/tb_decode

seq:
	$(VERILATOR) --cc --exe --build -j 2 -Wall -O3 --top-module rmZ80_seq \
	  --prefix Vseq --Mdir obj_seq -CFLAGS "-O2" \
	  ../rtl/rmZ80_alu_pkg.sv ../rtl/rmZ80_seq.sv tb_rmZ80_seq.cpp -o tb_seq
	obj_seq/tb_seq

regfile:
	$(VERILATOR) --cc --exe --build -j 2 -Wall -O3 --top-module rmZ80_regfile \
	  --prefix Vrf --Mdir obj_rf -CFLAGS "-O2" \
	  ../rtl/rmZ80_regfile.sv tb_rmZ80_regfile.cpp -o tb_regfile
	obj_rf/tb_regfile

# harness for the external suites: build only; it is run by hand (see tb_rmZ80_zex.cpp)
zex:
	$(VERILATOR) --cc --exe --build -j 2 -Wall -O3 --x-assign fast --x-initial fast --noassert --top-module rmZ80 --prefix Vcpu --Mdir obj_zex -CFLAGS "-O3" ../rtl/rmZ80.sv ../rtl/rmZ80_cw.sv ../rtl/rmZ80_seq.sv ../rtl/rmZ80_regfile.sv ../rtl/rmZ80_alu_pkg.sv ../rtl/rmZ80_alu_dec.sv ../rtl/rmZ80_alu_daa.sv ../rtl/rmZ80_alu_core.sv ../rtl/rmZ80_alu.sv tb_rmZ80_zex.cpp -o tb_zex

# z80test (sim/z80test): the same harness without the heavy checks of the RTL (RMZ80_NOCHK), build only
zexf:
	$(VERILATOR) --cc --exe --build -j 2 -Wall -O3 --x-assign fast --x-initial fast --noassert +define+RMZ80_NOCHK --top-module rmZ80 --prefix Vcpu --Mdir obj_zexf -CFLAGS "-O3" ../rtl/rmZ80.sv ../rtl/rmZ80_cw.sv ../rtl/rmZ80_seq.sv ../rtl/rmZ80_regfile.sv ../rtl/rmZ80_alu_pkg.sv ../rtl/rmZ80_alu_dec.sv ../rtl/rmZ80_alu_daa.sv ../rtl/rmZ80_alu_core.sv ../rtl/rmZ80_alu.sv tb_rmZ80_zex.cpp -o tb_zexf

# the six z80test programs in parallel, one verdict (run_z80test.sh): make z80test
z80test:
	sh run_z80test.sh

# SingleStepTests harness: build only
sst:
	$(VERILATOR) --cc --exe --build -j 2 -Wall -O3 --x-assign fast --x-initial fast --noassert --top-module rmZ80 --prefix Vcpu --Mdir obj_sst -CFLAGS "-O3" ../rtl/rmZ80.sv ../rtl/rmZ80_cw.sv ../rtl/rmZ80_seq.sv ../rtl/rmZ80_regfile.sv ../rtl/rmZ80_alu_pkg.sv ../rtl/rmZ80_alu_dec.sv ../rtl/rmZ80_alu_daa.sv ../rtl/rmZ80_alu_core.sv ../rtl/rmZ80_alu.sv tb_rmZ80_sst.cpp -o tb_sst
