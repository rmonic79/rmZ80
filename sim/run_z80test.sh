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

# rmZ80 — z80test 1.2a (Patrik Rak, MIT): the six programs of the official release, run on the core.
#
#   make z80test                   (from sim/, in the MSYS2 mingw64 shell)
#
# The programs are the release files in sim/z80test/, loaded as they are (tb_rmZ80_zex --tap).
# Each one runs every tested instruction over many operand values and compares a CRC of the
# registers and flags with values taken from a real Zilog Z80. They run in parallel; each takes
# 0.56 to 1.14 billion T-states of simulation, which is 30 to 57 minutes each on the machine where
# the results in STATUS.md were measured.
#
# One line per program with the number of tests passed, failed and skipped (the NEC and ST
# variants of SCF/CCF are skipped on a Zilog CPU), then a final verdict; exit code 0 only if
# every program prints "Result: all tests passed.".

LOG=${LOG:-log/z80test}
mkdir -p "$LOG"
PROGS="z80full z80doc z80flags z80docflags z80ccf z80memptr"

make zexf > "$LOG/build.log" 2>&1 || { echo "build FAILED: $LOG/build.log"; exit 1; }

t0=$(date +%s)
for p in $PROGS; do
  obj_zexf/tb_zexf --tap z80test/$p.tap --log "$LOG/$p.log" > "$LOG/$p.out" 2>&1 &
done
wait

fail=0
for p in $PROGS; do
  ok=$(grep -c 'OK$' "$LOG/$p.out")
  bad=$(grep -c 'FAILED' "$LOG/$p.out")
  skip=$(grep -c 'Skipped$' "$LOG/$p.out")
  if grep -q '^VERDICT: PASS' "$LOG/$p.out"; then v=PASS; else v=FAIL; fail=1; fi
  printf '%-12s %-4s  %3s OK, %s failed, %s skipped\n' "$p" "$v" "$ok" "$bad" "$skip"
done
echo "time: $(( $(date +%s) - t0 )) s"
if [ $fail -eq 0 ]; then echo "RESULT: ALL PASS"; else echo "RESULT: FAIL"; fi
exit $fail
