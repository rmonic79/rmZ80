#!/bin/sh
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

# rmZ80 — the whole test suite, one command, one verdict.
#
#   make test                      (from sim/, in the MSYS2 mingw64 shell)
#   SST_DIR=/path/to/v1 make test  (SingleStepTests vectors elsewhere)
#
# Runs the block benches (ALU, decoder, sequencer, register file), the execution bench
# (19 programs, random WAIT and bus requests, savestate at every tick) and SingleStepTests
# (1604 opcode positions x 1000 cases: registers, WZ, Q, IFF1/IFF2, IM, memory,
# I/O writes and the number of T-states). Prints one line per bench and a final verdict;
# exit code 0 only if every bench passes. About 4 minutes.
#
# The SingleStepTests vectors are third-party data and are not in this repository:
# `sh get_sst.sh` downloads them (into ../reference/tests). Without them the SST step is
# reported as MISSING and the suite fails.

SST_DIR=${SST_DIR:-../reference/tests/SingleStepTests-z80/v1}
LOG=${LOG:-log}
mkdir -p "$LOG"
fail=0

run() {          # run <name> <command...>: one line with time and verdict
  name=$1; shift
  t0=$(date +%s)
  "$@" > "$LOG/$name.log" 2>&1
  rc=$?
  t1=$(date +%s)
  if [ $rc -eq 0 ]; then v=PASS; else v=FAIL; fail=1; fi
  printf '%-8s %-4s %4ss   %s\n' "$name" "$v" "$((t1 - t0))" "$(grep -E 'errors:|TOTAL:' "$LOG/$name.log" | tail -1)"
}

run alu     make alu
run decode  make decode
run seq     make seq
run regfile make regfile
run exec    make exec

if [ -d "$SST_DIR" ]; then
  run sst sh -c "make sst && obj_sst/tb_sst.exe --dir '$SST_DIR' --log '$LOG/sst_cases.log'"
else
  printf '%-8s %-4s          vectors not found in %s\n' sst MISSING "$SST_DIR"
  fail=1
fi

if [ $fail -eq 0 ]; then echo "RESULT: ALL PASS"; else echo "RESULT: FAIL (see $LOG/)"; fi
exit $fail
