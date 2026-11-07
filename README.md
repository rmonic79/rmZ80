# rmZ80 — a Zilog Z80 in SystemVerilog

![version](https://img.shields.io/badge/version-1.0-blue) [![license](https://img.shields.io/badge/license-GPL--3.0--or--later-green)](LICENSE)

A from-scratch FPGA implementation of the Zilog Z80, written from the
manufacturer's documentation and from published measurements of the chip, and
verified instruction by instruction and T-state by T-state — in simulation and on
real hardware (Cyclone V, DE10-Nano).

The core reproduces the Z80 **T-state for T-state**: every instruction, prefix and
interrupt response takes the same number of T-states as the chip, with the same
M-cycles in the same order, and the bus is driven at half-T-state resolution —
`MREQ` from the falling edge, the refresh window, `WR` released half a T-state
early, `WAIT` sampled on the falling edge. Undocumented behaviour is included: the
flags in bits 5 and 3, `WZ` (MEMPTR), `SLL`, the halves of IX and IY, the unassigned
`ED` and `DD`/`FD` positions, the double effect of `DD CB`. It is not a translation
of an existing core.

---

## Table of contents

- [Status](#status) · [Design goals](#design-goals) · [How it is built](#how-it-is-built)
- [Using the core](#using-the-core) · [Repository layout](#repository-layout)
- [How this was written](#how-this-was-written) · [Verification](#verification) · [Reproducing the tests](#reproducing-the-tests)
- [What is not here](#what-is-not-here) · [Documentation](#documentation) · [License](#license)

---

## Status

Working and in use. It runs on real hardware in the author's MiSTer arcade cores,
Galivan/Dangar and the Kaneko games of the DJ Boy family among them, including save
and restore of the full machine state.

What is implemented: the complete documented instruction set, every undocumented
opcode position, the three interrupt modes and `NMI` (mode 2 with the full 8-bit
vector, mode 0 with multi-byte instructions from the device), `HALT`, `WAIT`, bus
request and acknowledge, refresh, and a savestate of the complete state that can be
read at any time and loaded in one clock.

## Design goals

rmZ80 was written to be **T-state accurate** — the M-cycles, their T-states and the
bus waveform of the silicon, half T-state by half T-state — and, at the same time, to
**run at high clock frequencies**: it is meant to close timing with margin on
mid-range FPGAs (the Cyclone V class found in MiSTer and in the Analogue Pocket, for
instance), not to fit one board, while staying **small** next to the rest of a system.

For size, the control is decoded directly instead of microcoded, the registers live
in flip-flops with physical banks, and the ALU is 8 bits wide with a single 16-bit
adder for addresses; there is no block memory. For speed, logic depth was treated as
a resource: the control word of the next state is computed ahead and registered on
the clock enable at which the state changes, a pipeline register sits between the
register read and the ALU, and every stage closes in one clock. At the edges, every
input the system drives — data, `WAIT`, `INT`, `NMI`, `BUSREQ` — enters only the
sequencer, where it is registered, and every output, the address included, leaves a
flip-flop with no logic after it (`data_oe` is the inverse of the `wr_n` flip-flop). The reset is asynchronous, so the enable of the
state registers is the clock enable and nothing else. The paths from the rest of a
design end at those flops instead of running into the core in the same clock.

No figures are given here: size and maximum clock depend on the design around the
core and on the tools, and the core is simple to measure in your own build —
`quartus/` is a project of the core on its own.

## How it is built

- **Direct decode, no microcode.** The control word (69 bits) is a function of
  (prefix space, opcode, M-cycle, T-state, half), generated from the opcode
  patterns of the manual as a default plus exceptions, together with the
  *schedule* of the M-cycles of each instruction. A microcoded control was sized
  first and needed 2.6 to 5 times more table bits in logic (`docs/06-sequencer.md` §1).
- **Half-T-state resolution.** One clock enable per half T-state, so the events
  the chip places on the falling edge of the clock land where they belong.
- **The control word registered ahead.** The word of the *next* state is computed
  and registered on the same clock enable at which the state changes, and a
  pipeline register sits between the register read and the ALU. The result is the
  same, tick for tick, as without the pipeline (`docs/06-sequencer.md` §9).
- **Registers in flip-flops, physical banks.** `EXX` and `EX AF,AF'` swap the
  contents of the two banks, so reads never go through a bank multiplexer;
  `EX DE,HL` is a renaming.
- **Defined values at reset and at power-up** for every state flip-flop, so that no
  value is left to the synthesis tool (`docs/04-interrupts.md` §7).
- **Parallel savestate.** The 274 bits of state are readable at any time and
  loaded in one clock.

## Using the core

The CPU is `rmZ80` (`rtl/rmZ80.sv`), nine files in `rtl/`. It presents the Z80
pins — `addr`, `data_in`, `data_out`, `mreq_n`, `iorq_n`, `rd_n`, `wr_n`, `rfsh_n`,
`m1_n`, `halt_n`, `wait_n`, `int_n`, `nmi_n`, `busreq_n`, `busack_n` — plus
`data_oe`, the savestate ports `ss_dout`, `ss_din`, `ss_load`, and:

- `cen`, the clock enable: **one pulse per half T-state**, at least 2 clocks apart —
  twice the rate that cores advancing one T-state per enable expect;
- `rst`, **active high** (the pin of the part is active low);
- `busreq_n`: tie it to 1 if the system has no bus request.

`docs/07-integration.md` has the ports, the contract (clock enable, reset,
savestate) and what looks different from other Z80 cores.

## Repository layout

```
rtl/              the CPU: nine SystemVerilog files
docs/             one document per subject: behaviour and timing, with the source
                  of every claim noted
sim/              Verilator benches and the test scripts
sim/z80test/      the z80test 1.2a release programs (MIT)
quartus/          Quartus project of the core on its own
reference/        created by `sim/get_sst.sh` for the SingleStepTests vectors; not versioned
```

## How this was written

The method was fixed before the first line of RTL. The one addition since — test
data as the reference where the documents are silent, the third class below — is
stated here and marked in the documents.

### Which sources were allowed

Three classes, and nothing else:

1. **Manufacturer documentation** — the Zilog Z80 CPU User Manual and Product
   Specification.
2. **Published measurements of the chip** — *The Undocumented Z80 Documented* by
   Sean Young, a description of behaviour measured on real hardware, with declared
   provenance. It is used where the manual is silent, every fact taken from it is
   marked in the documents, and it was checked against the manual where both speak.
3. **Test data, where both are silent** — SingleStepTests, the state before and
   after every instruction, produced by a software model of the Z80. Used only as
   behaviour (what goes in, what comes out), never as code, and only where the
   manual and the measurements say nothing: chiefly the per-instruction rules of the
   internal register `WZ` (`docs/01-registers-and-state.md` §7). z80test, whose
   values come from real Z80s, observes those rules through `BIT n,(HL)`
   (`z80memptr`) and passes.

And the list of what was deliberately **not** used:

- **no source of any existing Z80 core or emulator**, under any licence;
- no opcode maps or wikis without traceable provenance;
- no binaries read as a behavioural reference.

`docs/00-sources.md` is the full register.

### Document first, RTL second

No block was written before a document in `docs/` described its behaviour and
timing from the allowed sources, **each claim with its source inline** — the
manual page, the figure, the section of the measurements. The RTL was then written
from the document. The opcode table (`docs/03-decoding.md`: 1792 positions with
length, M-cycles, T-states and their breakdown, flags and page) is read directly by
two of the benches.

### Where the sources are silent, it says so

The documents say where the sources are silent and what rmZ80 does there —
`docs/02-bus-cycles.md` §10, `docs/03-decoding.md` §5.2, `docs/04-interrupts.md` §10,
`docs/06-sequencer.md` §4.2 and §7.

### Test programs and test data

z80test and zexdoc run on the CPU and compare CRCs with values taken from real
Z80s: they are stimulus only. SingleStepTests are the output of a software model of
the Z80: they are used as test vectors and, where no other allowed source fixes a
behaviour (chiefly the per-instruction rules of `WZ`), as the reference the core
follows; z80test observes those rules on silicon through `BIT n,(HL)` (`z80memptr`)
and passes (`docs/00-sources.md`).

### Nothing was accepted on the strength of an argument

Every change had to pass the full suite before it landed. The checks added with the
corrections of the last revision were also run on the RTL as it was before them, to
see them fail.

## Verification

The benches in `sim/`, re-run on this tree before publishing, and the hardware:

1. **Block benches** — the ALU against a model written from the documents in a
   different form, on every operand combination of every 8-bit operation and on the
   whole input space of `DAA`; the schedule of the control (bytes, M-cycles,
   T-states and their breakdown, alternative timing) against the 1792 rows of the
   opcode table; the sequencer half T-state by half T-state against the waveforms of
   `docs/02-bus-cycles.md`, for every cycle type with 0 to 8 wait states, with `WAIT`
   and the interrupt inputs changing around their sampling edges; the register file,
   every bit of its savestate included.
2. **Execution bench** — 19 hand-written programs covering every instruction
   family, the three interrupt modes, `NMI` (nested), `HALT`, `EI`, mode 0 with an
   `RST` and with a `CALL` from the device, mode 2 with an odd vector; T-states
   counted against the opcode table, final state and memory computed by hand. Each
   program runs with `cen` every 2 and every 12 clocks, with random `WAIT` and with a
   save and restore of the whole state at **every tick**, which must resume
   identically; the 11 programs without interrupts also run with random bus requests.
   Plus a directed test of the bus request timing.
3. **SingleStepTests** — 1,604,000 cases (1604 opcode positions × 1000):
   registers, `WZ`, `Q`, `IFF1`/`IFF2`, `IM`, memory and I/O writes, and the number of
   T-states, compared after every case. The 1604 positions are every opcode except
   the 12 prefix bytes themselves and 176 of the 178 `ED` positions that act as two
   NOPs (all but `ED 77` and `ED 7F`).
4. **z80test 1.2a** — `z80full`, `z80doc`, `z80flags`, `z80docflags`, `z80ccf`,
   `z80memptr`: every test passes; the NEC and ST variants of `SCF`/`CCF` are
   skipped, as they are on a Zilog CPU.
5. **Hardware** — the design is built, loaded on a DE10-Nano and played.

Steps 1 to 4 run from this repository (step 3 after downloading the vectors, see
[Reproducing the tests](#reproducing-the-tests)). Step 5 has been done on the
author's MiSTer arcade cores: in each, only the Z80 core and its clock enable (one
pulse per half T-state) were changed; the rest of the design is untouched, because
rmZ80 presents the pins of the part.

During development the core also ran zexdoc, the CP/M exerciser of the documented
instruction set: 67 of 67 tests pass. zexdoc is GPL-2 and is not redistributed here;
`sim/tb_rmZ80_zex.cpp` runs CP/M programs with a minimal BDOS (`--mode cpm`).

## Reproducing the tests

You need **Verilator** (5.x) and a C++ toolchain; the scripts were run in the
MSYS2 mingw64 shell.

```sh
cd sim
sh get_sst.sh      # once: the SingleStepTests vectors, about 1.3 GB
make test
```

That runs the block benches, the execution bench and SingleStepTests in about 4
minutes, and prints:

```
alu      PASS   16s   errors: 0 -> PASS
decode   PASS   10s   errors: 0 -> PASS
seq      PASS    9s   errors: 0 -> PASS
regfile  PASS    9s   errors: 0 -> PASS
exec     PASS   21s   errors: 0 -> PASS
sst      PASS  184s   TOTAL: 1604000 cases, 1604000 passed, 0 failed; files with failures 0/1604; cases with different T 0
RESULT: ALL PASS
```

```sh
make z80test
```

runs the six z80test programs from `sim/z80test/` in parallel — 0.56 to 1.14
billion T-states each, 30 to 57 minutes on the machine used here — and prints one
line per program, the elapsed time, then `RESULT: ALL PASS`.

Both are expected to be green on a fresh clone; they are re-run here before
anything is published. `sim/README.md` describes each bench.

## What is not here

- **No system.** This is a CPU: no memory, no peripherals.
- **No high impedance.** Inside an FPGA the released bus goes inactive (strobes
  high, `data_oe` = 0) and the system switches the bus with `busack_n`.

## Documentation

`docs/` describes the core as built, one file per subject, each with its sources,
the implementation and the checks that verify it:

| File | What it covers |
|---|---|
| `00-sources.md` | the sources, how each is classified, what was not used |
| `01-registers-and-state.md` | registers, flags, hidden state (`WZ`, `Q`), savestate layout |
| `02-bus-cycles.md` | M-cycles, T-states, `WAIT`, refresh, bus request, half-T resolution |
| `03-decoding.md` | the opcode table: 1792 positions, prefixes, undocumented opcodes |
| `04-interrupts.md` | `NMI`, `INT` in the three modes, `EI`, `RETI`/`RETN`, reset |
| `05-alu-and-flags.md` | the ALU and every flag rule, `DAA`, bits 5 and 3 |
| `06-sequencer.md` | control word, schedule, sequencer state, the pipeline |
| `07-integration.md` | how to put the core in a design |

The code and its comments are the normative description; the documents explain the
reasoning.

## License

rmZ80 is Copyright (c) 2026 Umberto Parisi (rmonic79). It is released under the
**GNU General Public License, version 3 or (at your option) any later version**: the
full text is in [LICENSE](LICENSE), and every source file carries the same notice.

For use without the obligations of the GPL — for example in a closed-source product —
a **commercial license** is available from the author, who can also be contacted for
other licensing needs: rmonic79 on GitHub.
