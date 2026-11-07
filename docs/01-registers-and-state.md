# 01 — Register model and core state

What the CPU contains, and therefore what a savestate must hold: the two lists
are the same, and are kept in one document on purpose.

Sources: [UM], [SY] (`00-sources.md`). Implementation: `rtl/rmZ80_regfile.sv`
(registers), `rtl/rmZ80_seq.sv` (internal state).

## 1. Architectural state

> "The Z80 CPU contains **208 bits** of read/write memory that are available to
> the programmer... configured to **eighteen 8-bit registers and four 16-bit
> registers**." — [UM] p.2

18 × 8 + 4 × 16 = 144 + 64 = **208 bits**:

| Group | Registers | Bits |
|---|---|---|
| Main set | `A F B C D E H L` | 64 |
| Alternate set | `A' F' B' C' D' E' H' L'` | 64 |
| Index, stack, program | `IX IY SP PC` (16 bits each) | 64 |
| Special | `I` (interrupt page), `R` (memory refresh) | 16 |
| | **Total** | **208** |

`BC`, `DE` and `HL` are used as single 8-bit registers or as 16-bit pairs — [UM] p.3.

| Register | Behaviour | Source |
|---|---|---|
| `PC` | Incremented after its contents have been placed on the address bus; a jump loads the new value instead of the incremented one | [UM] p.2 |
| `SP` | Top of a last-in first-out stack in external RAM | [UM] p.2 |
| `IX` `IY` | 16-bit bases for indexed addressing; the displacement is an extra byte, a **two's-complement signed integer** | [UM] p.3 |
| `I` | Upper 8 bits of the mode 2 interrupt vector address; during refresh it is placed on the **upper 8 bits of the address bus** | [UM] p.3 |
| `R` | **Only 7 bits** are incremented after each instruction fetch; bit 7 stays as written by `LD R,A`. Placed on the lower part of the address bus during refresh | [UM] p.3 |

`R` is a 7-bit counter with bit 7 preserved, not an 8-bit counter. If `LD R,A`
writes `R` in the same clock enable as an increment, the write wins.

## 2. Flag register

Bit positions — [UM] table 21, p.65:

| Bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
|---|---|---|---|---|---|---|---|---|
| | `S` | `Z` | `YF` | `H` | `XF` | `P/V` | `N` | `C` |

| Flag | Name | [UM] table 22 |
|---|---|---|
| `C` | Carry | testable |
| `N` | Add/Subtract, used by `DAA` | not testable |
| `P/V` | Parity/Overflow | testable |
| `H` | Half carry, used by BCD arithmetic | not testable |
| `Z` | Zero | testable |
| `S` | Sign | testable |

[UM] p.66 declares bits 5 and 3 "not used". On silicon they carry values — the
undocumented flags `YF` and `XF` — and the test suites check them. The core
implements them: the rules are in `05-alu-and-flags.md` §9.

## 3. State the manual does not declare

| Name | What it is | Why it is needed | Source |
|---|---|---|---|
| `WZ` (MEMPTR) | 16-bit internal register used as temporary storage by memory accesses, jumps, calls and 16-bit arithmetic | its upper byte appears in `YF`/`XF` after `BIT n,(HL)`; checked by `z80memptr` | [SY] §4.1 (partial); rules in §7 below |
| `Q` | Whether the **previous** instruction wrote the flags | on NMOS parts, `YF`/`XF` after `SCF`/`CCF` are `A \| F` if the previous instruction did not write F, and `A` if it did | z80test 1.2a readme ("depends on whether the previous instruction modified the flags"); `05-alu-and-flags.md` §9 |
| Bank identity | Which logical set — main or alternate — is active for `AF` and for `BC DE HL` | `EXX` and `EX AF,AF'` swap the sets | [UM] p.3 describes the swap; the implementation is §6 |
| `DE`/`HL` renaming | One bit per bank: `EX DE,HL` is implemented as a renaming of the two pairs | see §6 | implementation choice |
| `IFF1`, `IFF2` | Interrupt enable flip-flops; `IFF2` keeps a copy during an NMI | `LD A,I` and `LD A,R` copy `IFF2` into `P/V` | [UM] p.18 |
| `IM` | Interrupt mode 0, 1 or 2 | | [UM] p.19 |
| `HALT` | The CPU is halted, waiting for an interrupt | `PC` does not advance | [UM] p.14 |

`Q` is implemented as two bits: "F written in this instruction" and "F written in
the previous one", updated at the first tick of **every** M1, prefixes included
(SingleStepTests give `A | F` for `DD 37`). Only flags computed by the ALU count
as a write: `POP AF` and `EX AF,AF'` do not (z80test `z80ccf`, case 125, `PUSH`+`POP`).

## 4. Sequencer state

Resuming at the same cycle needs more than the architectural state: where the CPU
is inside the instruction (M-cycle, T-state, half of the T-state, wait state),
the prefix state, the data byte in flight, the pending NMI edge, the interrupt
samples and the bus-request state. The list is in `06-sequencer.md` §5.

## 5. Savestate inventory

| Block | Bits |
|---|---|
| Registers, [UM] | 208 |
| Bank identity (`AF`, `BC/DE/HL`) | 2 |
| **Register file** (`rmZ80_regfile`) | **210** |
| **Sequencer** (`rmZ80_seq`): timing, `IR`, prefix state, data latch, `WZ`, `Q`, `IFF1/2`, `IM`, `NMI` edge, interrupt samples, `HALT`, bus request — `06-sequencer.md` §5 | **64** |
| **Total** | **274** |

`ss_dout[273:0]` = `{sequencer[63:0], register file[209:0]}`, most significant
bit first. The register-file part, in field order (MSB first): `a_bk[1]`,
`a_bk[0]`, `f_bk[1]`, `f_bk[0]`, `b_bk[1]`, `b_bk[0]`, … `l_bk[1]`, `l_bk[0]`
(8 bits each), `ix`, `iy`, `sp`, `pc` (16 bits each), `i_reg`, `r_reg`, `sel_af`,
`sel_gp`. In the banked fields index `[0]` is the **active** bank and `[1]` the
shadow (§6).

## 6. Implementation notes

- **No block memory.** The registers are static RAM inside the chip ([UM] p.2); in
  the FPGA they are flip-flops. The whole state is one packed struct per block, so
  no RAM can be inferred: 0 M10K.
- **One declaration per block, savestate generated from it.** The register-file
  state is one `struct packed` and the savestate is its vector view; the same for
  the sequencer. A register added to the struct is in the savestate by
  construction, and the width check of the `ss_dout` assignment fails until the port
  width is updated with it.
- **Physical banks.** In `a/f/b/c/d/e/h/l_bk`, index `[0]` is always the active
  bank and `[1]` the shadow. `EXX` and `EX AF,AF'` **swap the contents** (no write
  goes to a bank in the clock that swaps it) and toggle `sel_gp` / `sel_af`, which
  remain as the logical identity of the active bank. Reads therefore never go
  through a bank multiplexer.
- **`EX DE,HL` is a renaming.** A bit per bank (bits 2–3 of `q_reg`, `06-sequencer.md` §5)
  says whether `DE` and `HL` are swapped in that bank; reads and writes of `D`,
  `E`, `H`, `L`, `DE`, `HL` are redirected accordingly (after the `IX`/`IY`
  substitution). No data moves, and `WZ` is untouched.
- **Defined values at reset and at power-up** (`04-interrupts.md` §7): `AF` and
  `SP` = FFFFh ([SY] §2.4), everything else 0. The same values are the
  power-up values of the registers, so that no flip-flop is left to the
  synthesis tool.
- **Parallel savestate.** On the top module `rmZ80`:
  - `ss_dout[273:0]` **is** the state: wires on the registers, no logic. It only
    changes on clocks with `cen`, so it can be read at any time.
  - `ss_din[273:0]` + `ss_load`: with `ss_load` = 1 for **one** clock, with
    `cen` = 0 in that clock, the whole state becomes `ss_din` at the clock edge.
    `ss_load` overrides `cen`; `rst` overrides everything.
  - **Quiet time**: after the loading clock, **at least 2 clocks with `cen` = 0**
    before the next `cen`. The pipeline registers outside the savestate (`06-sequencer.md`
    §9) are recomputed from the loaded state in those two clocks.
  - The format is this core's own: a savestate taken from another Z80 core cannot
    be loaded.

## 7. How this document is verified

| Claim | Check |
|---|---|
| Registers and documented flags | `zexdoc`, `z80doc`, `z80docflags`, SingleStepTests |
| `YF`/`XF` | `z80full`, `z80flags`, SingleStepTests |
| `WZ`/MEMPTR | SingleStepTests (`WZ` compared after every case), `z80memptr` |
| `Q` (NMOS `SCF`/`CCF`) | `z80ccf` |
| 7-bit increment of `R` | SingleStepTests (`R` compared after every case) |
| Savestate complete | `regfile` bench (walking one over 210 bits, per-element completeness), `seq` bench (walking one over 64 bits), `exec` bench (walking one over 274 bits; save and restore at **every tick** of every program, resumed run identical) |

### Rules for `WZ`/MEMPTR implemented in the core

[SY] v0.91 predates the MEMPTR research and gives only a partial list
(§4.1 pp.15–16). The rules below are those of SingleStepTests, compared for every
opcode by `sim/tb_rmZ80_sst.cpp`. After the instruction, `WZ` holds:

| Instruction | `WZ` |
|---|---|
| `LD A,(BC)` `LD A,(DE)` `LD A,(nn)` | address + 1 |
| `LD (BC),A` `LD (DE),A` `LD (nn),A` `OUT (n),A` | `{A, (address + 1) & FFh}` |
| `LD rp,(nn)` `LD (nn),rp` (also IX/IY and the `ED` forms) | `nn + 1` |
| `ADD HL,rr` `ADC HL,rr` `SBC HL,rr` (also IX/IY) | `HL + 1`, with HL before the operation |
| `RLD` `RRD` | `HL + 1` |
| `JR e`, `JR cc,e` taken, `DJNZ` taken | target |
| `JP nn` `JP cc,nn` `CALL nn` `CALL cc,nn` (taken or not), `RST` | `nn` / vector |
| `RET`, `RET cc` taken, `RETI`, `RETN` | the popped PC |
| `IN A,(n)` | `{A, n} + 1` |
| `IN r,(C)` `OUT (C),r` | `BC + 1` |
| `INI` / `IND` | `BC + 1` / `BC - 1`, with B before the decrement; `OUTI` / `OUTD`: with B after |
| `CPI` / `CPD` | `WZ + 1` / `WZ - 1` |
| block instructions, when they repeat (`LDIR LDDR CPIR CPDR INIR INDR OTIR OTDR`) | `PC + 1`, with PC the address of the instruction |
| every `(IX+d)` / `(IY+d)` form, `DDCB` / `FDCB` included | `IX + d` |
| `EX (SP),HL/IX/IY` | the new value of HL/IX/IY |
| `EX DE,HL` | unchanged (it is a renaming, §6) |
| `BIT b,(HL)` | unchanged; `YF`/`XF` of the result come from bits 13 and 11 of `WZ` |

Other rules from the same comparison: `RETI` (and the `ED` duplicates of `RETN`)
copies `IFF2` into `IFF1` like `RETN` (`04-interrupts.md` §6); after `LD R,A`,
`LD A,R` reads `A + 2` (the write of `R` lands after the refresh of its own M1);
the repeating block instructions, when they repeat, take `YF`/`XF` from bits 13 and
11 of `PC`, and the I/O ones also adjust `H` and `P/V` from `B`, `C` and `N`
(`OP_IORF` in `rtl/rmZ80_alu_pkg.sv`, computed in `rtl/rmZ80_alu_core.sv`).
