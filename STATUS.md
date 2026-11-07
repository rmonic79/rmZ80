# Status — version 1.0

## What is in this repository

- **The CPU**: `rmZ80` (`rtl/rmZ80.sv`) and the eight other files in `rtl/`: the six modules it instantiates, the
  package they import, and `rmZ80_alu`, the ALU wrapper tested by the `alu` bench.
- **The benches** (`sim/`), the z80test programs they run (`sim/z80test/`), and the scripts that run them.
- **A Quartus project** of the core on its own (`quartus/`).

## On hardware

In use on real hardware (DE10-Nano) in the author's MiSTer arcade cores, Galivan/Dangar and the Kaneko games of the
DJ Boy family among them, in place of the Z80 core they shipped with; save and restore work.

## Verification of this tree

All of it re-run on the contents of this commit (from `sim/`):

| bench | result |
|---|---|
| block benches `alu`, `decode`, `seq`, `regfile`, `make test` | PASS |
| execution bench `exec`, `make test`: 19 programs, `cen` every 2 and every 12 clocks, random `WAIT`, random bus requests on the 11 programs without interrupts, save and restore at every tick | PASS |
| SingleStepTests, `make test`: 1604 opcode positions × 1000 cases, vectors from `get_sst.sh` (revision `ebe1875`) | 1,604,000 / 1,604,000 passed, 0 cases with a different number of T-states |
| z80test 1.2a, `make z80test`: `z80full`, `z80doc`, `z80flags`, `z80docflags`, `z80ccf`, `z80memptr` | each: 156 tests OK, 0 failed, 4 skipped (NEC and ST variants of `SCF`/`CCF`); "Result: all tests passed." |

SingleStepTests do not cover the 12 prefix bytes on their own and 176 of the 178 `ED` positions that act as two NOPs
(all but `ED 77` and `ED 7F`); the `decode` bench checks the timing of those 176 against the opcode table, and that
the 12 prefix bytes are decoded as prefixes. SingleStepTests compare the number of T-states, not
the pins: the waveforms are checked by the `seq` and `exec` benches against `docs/02-bus-cycles.md`.

During development the core also ran zexdoc (67 of 67 tests pass), which is GPL-2 and is not part of this
repository.

## Interface notes

- `cen` is one pulse per **half** T-state, at least 2 clocks apart; after a savestate load, at least 2 clocks without
  `cen` (`docs/07-integration.md` §3).
- `rst` is active high and asynchronous; the pin of the part is active low.
- `busreq_n` is functional: tie it to 1 if the system has no bus request.
- The savestate format is this core's own.

## Before changing anything

- **Fit results vary from build to build**, even for a barely different netlist: a small change cannot be measured
  with a single build.
- **No state flop without a defined value**, at reset and at power-up. With `ALLOW_POWER_UP_DONT_CARE` enabled, a
  functionally neutral change alters power-up values and can break the system from one build to the next.
- **T-state timing is not negotiable**: SingleStepTests must keep 0 cases with a different number of T-states.
- The opcode table in `docs/03-decoding.md` is read by the `decode` and `exec` benches: its Appendix A is data.

## Third-party material

`sim/z80test/` holds the z80test 1.2a release programs, unmodified, with their MIT licence. `.gitignore` keeps out of
the repository the third-party material used locally: the documents cited in `docs/` (listed in
`docs/00-sources.md`) and the SingleStepTests vectors (downloaded by `sim/get_sst.sh`).
