# z80test 1.2a

The six test programs of **z80test 1.2a** by Patrik Rak, as distributed in the
official release (https://github.com/raxoft/z80test, release v1.2a, file
`z80test-1.2a.zip`): the `.tap` files are copied unmodified, with the release's
`license.txt` (MIT), `readme.txt` and `changes.txt`. The assembly sources are in
the release and are not reproduced here.

| File | Tests |
|---|---|
| `z80full.tap` | all flags and registers |
| `z80doc.tap` | all registers, documented flags only |
| `z80flags.tap` | all flags, registers ignored |
| `z80docflags.tap` | documented flags only |
| `z80ccf.tap` | all flags after `CCF` following each tested instruction |
| `z80memptr.tap` | all flags after `BIT n,(HL)` following each tested instruction |

`sim/tb_rmZ80_zex.cpp --tap` loads the code block of a `.tap` at its own address
(8000h) and provides the few ZX Spectrum ROM services the programs use (print a
character, open a channel, read the keyboard port). `make z80test` (from `sim/`)
runs all six; see `sim/run_z80test.sh`.
