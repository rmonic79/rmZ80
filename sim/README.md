# sim/ — verification benches

Every bench here builds the RTL with Verilator, runs it, prints a verdict and exits
non-zero on failure.

## Requirements

Verilator 5.x and a C++17 toolchain; the scripts were run in the MinGW64
environment of MSYS2. SingleStepTests need their vectors, downloaded once with
`sh get_sst.sh` (about 1.3 GB, into `../reference/tests/SingleStepTests-z80`, which the
script creates, outside version control).

## Running them

```sh
make test        # block benches, execution bench, SingleStepTests: one verdict, about 4 minutes
make z80test     # the six z80test programs, in parallel: 30 to 57 minutes each
make alu         # any single bench, by name: alu, decode, seq, regfile, exec
```

`make test` runs `run_tests.sh` and prints one line per bench and a final
`RESULT: ALL PASS` or `RESULT: FAIL`; `make z80test` runs `run_z80test.sh` and prints
one line per program and the same kind of verdict. Logs go to `log/` (or to `$LOG`),
build artefacts to `obj_*/`; git ignores both.

## What the benches are

Each block bench takes one block of the core and compares it against a model that is
independent of the RTL: written in C++ from the documents in `docs/`, in a different
form from the RTL (overflow from signed arithmetic, parity by counting bits, waveforms
as tables of levels per half T-state), or read at run time from the opcode table of
`docs/03-decoding.md`. The input space is covered exhaustively where the coverage
column says so; elsewhere (the 16-bit ALU steps, covered compositionally; the flag
operations of the block instructions, on chosen operand sets) the column states how.

| bench | block | what it checks | coverage |
|---|---|---|---|
| `alu` | `rmZ80_alu` | every ALU operation, result and all eight flags, against a model written from `docs/05-alu-and-flags.md`; `DAA` against both the [SY] rule and the [UM] table | exhaustive over operands and incoming flags: 1,325,056 cases; plus 1,572,864 for the 16-bit steps (compositional: each 8-bit step exhaustive with both carries), 262,144 for `RLD`/`RRD` (all `A` × `(HL)`), 131,072 for `PASS` and `TST` (all `A` × `F`), 1,048,576 for the `LDI`/`CPI` flag operations (all `A` × byte, 4 values of `BC`, 2 of `F`), 425,984 for the block I/O flag operations (all data, 52 operands, 16 values of `B`), 44,032 for the repeating forms, 8,192 for `BIT b,(IX+d)`, 1,024 for `LD A,I`/`LD A,R`, 131,072 for the reversed `SBC` steps (equal to `SBC` with the operands swapped); `DAA` on all 2048 combinations (each [SY] table gives exactly one row; same result and `C'` as [UM] on its 684); built twice, NMOS (full suite) and CMOS (`SCF`/`CCF`, 786,432 cases). `YF`/`XF` are checked where a source gives a per-instruction rule (10 operations) and masked where only the general rule exists (23, plus `TST`, `LD A,I`/`LD A,R` and the two `RLD`/`RRD` accumulator steps); `z80test` and SingleStepTests check those |
| `decode` | `rmZ80_cw` | the schedule of every opcode position: length in bytes, M-cycles, T-states and their breakdown, the alternative timing of conditional instructions | all 1792 rows of Appendix A, read from `docs/03-decoding.md` at run time: 698 from [UM], 908 from [SY] (totals; [SY] gives no breakdown), 180 unassigned `DD`/`FD` positions (174 against the rule of `docs/06-sequencer.md` §4.2, the 6 prefix bytes among them as prefixes), 6 prefixes. The cycle type is not in the table and is not checked here |
| `seq` | `rmZ80_seq` | the bus waveform half T-state by half T-state against `docs/02-bus-cycles.md`: `MREQ`, `IORQ`, `RD`, `WR`, `RFSH`, address, refresh (`MREQ` T3B–T4A, address stable over T3A–T4B, `R` incremented after the window), data sampling; `WAIT` on the falling edge; `INT` and `NMI` on the edge that opens the last T-state; `WZ` write ports, prefix space, `IM`/`IFF`, `HALT`; the 64-bit savestate | every cycle type with 0 to 8 wait states, `WAIT` held for whole T-states, low only in the first half and low only in the second half (2,284 half T-states); `INT` low only after the sampling edge, `INT` gone after it, an `NMI` edge after it and on it; savestate by walking one over the 64 bits, 10 patterns, and a restore in the middle of a cycle |
| `regfile` | `rmZ80_regfile` | every register through the 8- and 16-bit ports, the 7-bit increment of `R`, the bank swaps, the dedicated outputs (A and F of the active bank, I, R), the 210-bit savestate | the 8/16-bit aliasing of the 8 pairs on all 65,536 values in both directions (524,288); `R` 512; dedicated outputs 1,792; savestate by walking one over the 210 bits and a completeness check (24 elements, exact union) |
| `exec` | `rmZ80`, the whole CPU | 19 hand-written programs (A–T, no R): every instruction family, the `CB`, `ED`, `DD`/`FD` and `DDCB`/`FDCB` spaces, I/O and block instructions, `RLD`/`RRD`, `SCF`/`CCF` with `Q`, the three interrupt modes, `NMI` (nested), `HALT`, the `EI` window, mode 0 with an `RST` and with a `CALL` from the device, mode 2 with an odd vector, `LD A,I` with an interrupt | for each program: the sequence of fetch addresses, the number of T-states against the opcode table, the refresh of every M1, `M1` low from the start of T1 of every fetch and acknowledge (and never otherwise), the final registers and memory computed by hand. Each program is run with `cen` every 12 and every 2 clocks (results identical), with a save and restore in the middle and at **every tick** (resumed run identical), with random `WAIT` (all programs) and random bus requests (the 11 programs without interrupts): identical results, also with the restores. A directed test checks the bus request timing against [PS] figure 10. The 274-bit savestate is checked by walking one. Checks inside the RTL (P0 equal to the control word of the current state, NX equal to the next state, and others: `docs/06-sequencer.md` §9.3) run at every clock |
| `sst` | `rmZ80` | SingleStepTests: each case loads its initial state through the savestate, runs one instruction and compares registers, `WZ`, `Q`, `IFF1`/`IFF2`, `IM`, memory and I/O writes, and the number of T-states | 1604 files × 1000 cases = 1,604,000: every opcode position except the 12 prefix bytes on their own and 176 of the 178 `ED` positions that act as two NOPs (all but `ED 77` and `ED 7F`) |

## z80test and CP/M programs

`tb_rmZ80_zex.cpp` runs self-checking programs on the whole CPU. With `--tap` it
loads a ZX Spectrum `.tap` file — the z80test release in `z80test/` — and provides the
few ROM services the programs use (print a character, open a channel, read the
keyboard port); the exit code is 0 only if the program prints
"Result: all tests passed.". With `--bin F --org HHHH --mode cpm` it runs a CP/M
`.com` program with a minimal BDOS (functions 2 and 9); in CP/M mode the exit code is
always 0: the result is the program's own printout. `make zexf` builds it without
the heavy checks inside the RTL, which only slow a long run down.

## How the benches were validated

The checks added with the corrections of the last revision were run on the RTL as it
was before each correction, where they fail: programs Q, S and T and the bus request
tests of `exec`, and the `WAIT`, `INT` and `NMI` sampling tests of `seq`.
