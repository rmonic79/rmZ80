# 06 — Sequencer and control

How the datapath is driven: the control word, the schedule of M-cycles, the state
of the sequencer and the pipeline that makes it close timing.

Sources: the other documents — `02-bus-cycles.md` (cycles, half-T resolution),
`03-decoding.md` (the opcode table: lengths and T-state breakdown of all 1792
positions), `04-interrupts.md`, `01-registers-and-state.md`.
Implementation: `rtl/rmZ80_cw.sv` (control word and schedule), `rtl/rmZ80_seq.sv`
(counters, bus, interrupts, bus request, state), `rtl/rmZ80.sv` (pipeline,
datapath routing, output registers).

Design constraint from the start: **no block memory**. Control lives in logic, so
the width of the control word is its cost.

## 1. Microcode or direct decode

Two candidates were sized before any RTL:

- **A. Microcoded**: a table of micro-instructions addressed by a micro-PC, an
  entry point per opcode, sequencing in the table.
- **B. Direct decode**: the control word is a combinational function of (prefix
  state, opcode, M-cycle, T-state, half); sequencing is a few counters plus a
  *schedule* per instruction.

### 1.1 The numbers

| | A, microcode | B, direct |
|---|---|---|
| Entry map opcode → micro-PC | 4 spaces × 256 × 9 bits = 9,216 bits | absorbed by the regularity of the opcode fields (`03-decoding.md` §1) |
| Control table | ≈ 300 micro-rows × ≈ 55 bits ≈ 16,500 bits | schedule rows, a few thousand bits at most |
| Sequencing | next/branch field in every row | three counters |
| **Total in logic** | **≈ 25,700 bits** | **≤ 9,600 bits, typically ≈ 5,000** |

### 1.2 Verdict

**B, direct decode.** The ratio is at least 2.6× (4–5× typically) on the
structures that would sit in logic, and microcode pays the width of its word in
every row plus an entry map that alone weighs as much as all of B. In the final
RTL the control word and the schedule are generated from the same opcode patterns,
as a default plus exceptions, in one module (`rmZ80_cw.sv`).

## 2. The control word

`rmZ80_cw` computes, for (prefix space, `IR`, M, T, half), a word of **69 bits**.
Everything that can be read from the opcode fields (`03-decoding.md` §1: `r`,
`ddd`/`sss`, `dd`, `qq`, `cc`, `b`, `t`) goes straight from `IR`; the word carries
the modes.

| Field | Bits | Consumer | Content |
|---|---|---|---|
| `rd8_sel` | 5 | register file | 8-bit read: B C D E H L F A, SPh SPl, PCh PCl, I, R (IXh/IXl/IYh/IYl through `rd8_ix`/`rd8_fd`) |
| `rd16_raw`, `rd16_ix`, `rd16_xhl` | 3+1+1 | register file | 16-bit read (BC DE HL SP PC AF) with the IX/IY and DE↔HL substitutions applied on the data side |
| `jpcc_t` | 1 | datapath | the tick where `JP cc` decides `PC ← WZ` from F |
| `addr_wz` | 1 | bus | address = `WZ` instead of the 16-bit read |
| `dout_src` | 2 | bus | data out: 8-bit read, ALU result, data latch, `OUT (C),0` constant |
| `alu_op` | 6 | ALU | operation code (`rtl/rmZ80_alu_pkg.sv`) |
| `a_src`, `b_src` | 2+2 | ALU | operand A: `A`, 8-bit read, data latch; operand B: 8-bit read, data latch, `WZL`, `WZH` |
| `wr8_en`, `wr8_sel`, `wr8_src` | 1+5+1 | register file | 8-bit write: register, data = ALU result or flags |
| `wr16_en`, `wr16_sel`, `wr16_src` | 1+3+1 | register file | 16-bit write: pair, data = address adder or `{result, flags}` (AF) |
| `inc_op`, `inc_src` | 2+2 | address adder | +1, −1, + sign-extended data latch, +0; of the 16-bit read, `WZ`, or a restart vector |
| `wz_we`, `wz_src` | 2+2 | sequencer (`WZ`) | write `WZL`/`WZH` from the data latch, or `WZ` from the adder, or `{A, adder[7:0]}` |
| `cond_mode` | 2 | condition | `cc` on F, `cc` of `JR` on F, `B` ≠ 0, block repeat (P/V, and Z for `CPIR`/`CPDR`) |
| `swap_af`, `swap_gp`, `swap_dehl` | 3 | register file, sequencer | `EX AF,AF'`, `EXX`, `EX DE,HL` |
| `rd8_ix`, `rd8_fd`, `rd8_xhl` | 3 | register file | substitutions on the 8-bit port: H/L → IXh/IXl or IYh/IYl, D/E ↔ H/L |
| `im_we`, `im_val`, `iff_op`, `halt_set` | 1+2+2+1 | sequencer | `IM n`; `DI`, `EI`, `IFF1 ← IFF2`; `HALT` |
| `row_type`, `row_dur`, `row_ext`, `row_last`, `nxt_gate` | 3+3+1+1+1 | sequencer | the schedule row of the current M-cycle (§3.4) |
| `sp_set`, `ir_ld` | 1+1 | sequencer | M1 of a prefix (change of space); the read that loads `IR` (fourth byte of `DDCB`/`FDCB`) |

### 2.1 Rules outside the word

Fixed routing in `rmZ80.sv`, no bits in the word:

1. **8-bit write data** is the ALU result, or the flags when writing F. A
   register-to-register copy, or a load from the bus, goes through the ALU with
   the operation `PASS` (result = operand A, flags unchanged): one data path.
2. **`AF8`** = `{result, flags}` writes A and F together (operations on the
   accumulator). Instructions that write a register and F (`INC r`, `CB` rotates
   on a register) write F in a tick of their own, then the register.
3. **ALU operands** are selected before the pipeline register P1 (§9) and read by
   the ALU straight from it; the incoming flags are F of the active bank.
4. **ALU operation codes follow the opcode fields**: the 8 accumulator operations
   are `ooo` (`03-decoding.md` §1.5); the four rotates of A are `18h | ir[4:3]`;
   `DAA`, `CPL`, `SCF`, `CCF` are `1Ch | ir[4:3]`; the `CB` operations follow
   `ir[7:6]` and `ir[5:3]`.
5. **Conditions** gate the M-cycles of the schedule (§3.4) and the writes that
   depend on them; a repeating block instruction repeats by writing `PC ← PC − 2`
   in its last M-cycle.
6. **IX/IY substitution**: in the `DD`/`FD` spaces H, L and HL become IXh, IXl and
   IX (IYh, IYl, IY) in every instruction **except** those with `(IX+d)`, where H
   and L stay H and L (`03-decoding.md` §3.4, §5.2). It is applied by the register file on
   the data side, after the selectors.
7. **PC + 1**: for the opcode fetch in M1 T3A (the address is already the refresh
   address); for an operand read at `PC`, in T3B.
8. **Port address in `WZ`**: in `IN A,(n)` / `OUT (n),A` the port address `{A, n}` is
   formed in `WZ` by two writes: `WZ` ← `AF` in M1 T4A (so `WZH` = `A`), then
   `WZL` ← `n` in M2 T3B.
9. **`YF`/`XF` of `BIT b,(HL)` and `BIT b,(IX+d)`** come from `WZH`, operand B of
   the ALU in those rows (`05-alu-and-flags.md` §9).
10. **`LD A,I` / `LD A,R`**: `P/V` ← `IFF2` when F is written; cleared if an
    interrupt is accepted at the end of the instruction (`04-interrupts.md` §9).
11. **Prefixes**: the change of prefix space is a function of the prefix byte and
    of the current space, applied by the sequencer at the end of the prefix M1.
12. **Vectors**: `RST` uses `{00, ttt, 000}`; the interrupt responses use the
    synthetic `IR` itself (66h `NMI`, 38h mode 1), through the address adder.
13. **Mode 2**: `WZ` ← `PC` (the return address, pushed from `WZ`), `PC` ←
    `{I, vector}`; then two reads at `PC` and `PC + 1` → `WZ`, and `PC` ← `WZ`.
14. **`RLD`/`RRD`**: two ALU operations in different ticks: the new memory byte
    first, held for the write cycle, then the new A.

### 2.2 What the other blocks provide

| Block | Interface used by the control |
|---|---|
| `rmZ80_regfile` | one 8-bit read port, one 16-bit read port, one 8-bit and one 16-bit write port; **dedicated outputs** `A`, `F`, `B` of the active bank and `I`, `R` (wires on existing registers, no state) for the ALU, the conditions and the refresh address |
| the ALU (`rmZ80_alu_dec`, `rmZ80_alu_daa` before P1, `rmZ80_alu_core` after it) | 8-bit ALU with, besides the [UM] operations, `PASS`, `TST` (flags of `IN r,(C)`), the two halves of `RLD` and `RRD`, and the flag operations of the block instructions (they read `BC` for `P/V`, and `B`) |
| `rmZ80_seq` | write ports of `WZ` (byte-wise from the data latch, or whole); `IR` loaded from the bus (fetch, mode 0 acknowledge, fourth byte of `DDCB`/`FDCB`); prefix space; `IFF`, `IM`, `HALT`, `Q` |

### 2.3 Coverage

The word and the schedule cover **all 1792 positions** of the opcode table
(`03-decoding.md`, Appendix A: base 252 + 4 prefixes, `CB` 256, `ED` 256, `DD`
256, `FD` 256, `DDCB` 256, `FDCB` 256), including the undocumented ones
(`03-decoding.md` §5). The schedule — length in bytes, number of M-cycles,
T-states and their breakdown, and the alternative timing of the conditional
instructions — is checked against the table for all 1792 by the `decode` bench (the
cycle type is not in the table and is not checked there); the behaviour of
1604 of them by SingleStepTests (all except the 12 prefix bytes and 176 of the 178
`ED` positions that act as two NOPs; SingleStepTests cover `ED 77` and `ED 7F`), and of the documented instruction set by z80test
and `zexdoc`.

## 3. Advancement

### 3.1 The counters

| Counter | Bits | Advances |
|---|---|---|
| `phase` (A/B, first or second half of the T-state) | 1 | on every `cen` |
| `t_idx` (T-state within the M-cycle) | 3 | on B → A |
| `m_idx` (M-cycle within the instruction) | 3 | when `t_idx` reaches the length of the schedule row |
| `tw` (wait state) | 1 | §3.2 |

Events of `02-bus-cycles.md` and their halves: data sampled on the edge that
opens T3 (the end of T2B); `WR` released half a T-state before the bus changes;
`WAIT` read in T2 and in each wait state.

### 3.2 WAIT

`wait_n` is read on the falling edge of T2 and of every TW (the `cen` from A to
B) and the result is held in `tw` for the second half; on the rising edge, if
`tw` = 1, `t_idx` does not advance and the T-state is repeated as a TW (`tw` stays
1 through its first half). It is read again in every TW. The **automatic** wait
states do not go through `tw`: the I/O cycle types are one T-state longer and the
interrupt acknowledge two, as properties of the cycle type.

### 3.3 Cycle types

**M1** (fetch: 4 T, refresh with `{I, R}` in T3–T4, `R` incremented, `IR`
loaded), **RD**/**WR** memory (3 T), **IORD**/**IOWR** (3 + 1 T), **IACK**
(special M1, `IORQ` instead of `MREQ`, 4 + 2 T), **NONE** (internal cycle, no
bus). Longer cycles (M1 of 5 or 6 T, reads of 4 or 5, internal cycles of 5) come
from the T-state breakdown of the opcode table, row by row.

### 3.4 The schedule row

For the current M-cycle, the control word carries:

| Field | Content |
|---|---|
| `row_type` | cycle type |
| `row_dur` | length in T-states (3 + `row_dur`) |
| `row_ext` | the last T-state of this M-cycle exists only if the condition is true |
| `row_last` | last M-cycle of the instruction |
| `nxt_gate` | the following M-cycles exist only if the condition is true |

Every conditional timing of the opcode table is covered by these, with no special
case:

| Instruction | Taken / not taken | How |
|---|---|---|
| `JP cc` | 10 (4,3,3) both | no gate: the condition only decides `PC ← WZ` |
| `JR cc` | 12 (4,3,5) / 7 (4,3) | the internal M3 exists only if true |
| `DJNZ` | 13 (5,3,5) / 8 (5,3) | same, condition `B` ≠ 0 |
| `RET cc` | 11 (5,3,3) / 5 (5) | M2–M3 exist only if true |
| `CALL cc` | 17 (4,3,4,3,3) / 10 (4,3,3) | M3 has its fourth T only if true (`row_ext`), M4–M5 exist only if true |
| `LDIR`, `CPIR`, `INIR`, `OTIR` and the decrementing forms | 21 / 16 | the last M-cycle exists only if the instruction repeats, and writes `PC ← PC − 2` |

The condition is read from F (or `B`) as it stands before the end of the M-cycle
that precedes the first gated one; for the block repeat, from the flags the block
instruction has just written.

### 3.5 Interrupts and HALT in the flow

On the rising edge that opens the last T-state of every M-cycle the sequencer
samples `INT`, the stored `NMI` edge and `BUSREQ` (`04-interrupts.md` §1). At the
end of every instruction (last tick of the last M-cycle) the samples of its last
M-cycle decide: `NMI` if an edge was stored, otherwise `INT` if it was low, `IFF1`
= 1 and the instruction is not `EI` or `DI` (`04-interrupts.md` §3); never if a bus
request was sampled. Acceptance loads a synthetic `IR` in prefix space 7: 66h for
`NMI` (M1 5 T, two writes: 11 T), 38h for mode 1 (acknowledge 7 T, two writes:
13 T), 7Eh for mode 2 (acknowledge 7 T, two writes, two reads: 19 T). In mode 0 the
next M1 is an acknowledge cycle that loads `IR` from the bus, and the instruction
supplied runs with `PC` held (`04-interrupts.md` §5). `HALT` sets the halt state:
from then on every M1 reads the bus but loads a NOP, with the refresh running and
`PC` held, until an interrupt is accepted.

## 4. The decoder

### 4.1 No separate decoder

There is no class decoder between `IR` and the control word: the opcode patterns
(`03-decoding.md` §1, quadrants and fields) are computed from `IR` by one function
(`rmz80_pdec` in `rtl/rmZ80_alu_pkg.sv`), registered in the sequencer one clock
after `IR` (from the `IR` register; harmless because `cen` pulses are at least 2
clocks apart and no row uses `IR` in the half T-state in which it is loaded), and
used directly by `rmZ80_cw`. The prefix space has 3 bits: 0 base,
1 `CB`, 2 `ED`, 3 `DD`, 4 `FD`, 5 `DDCB`, 6 `FDCB`, 7 interrupt response. The
patterns are not state: they are recomputed from `IR` and are not in the savestate.

### 4.2 Positions the manual does not assign

| Positions | rmZ80 | Source |
|---|---|---|
| `CB 30–37` | `SLL` | [SY] §3.1, `03-decoding.md` §5.1 |
| `DD`/`FD` on H, L (without `(HL)`) | IXh, IXl, IYh, IYl | [SY] §3.2–3.3, `03-decoding.md` §5.2 |
| `DD`/`FD` before an instruction that does not use HL (180 positions) | the prefix is an M1 of 4 T, then the base instruction runs unchanged | [SY] §3.2 ("the instruction following the DD prefix is executed as is"), `03-decoding.md` §3.4, §5.2 |
| unassigned `ED` | duplicates of `NEG`, `RETN`, `IM` where [SY] says so; `ED 70` = `IN F,(C)`, `ED 71` = `OUT (C),0`; otherwise two M1 of 4 T and no effect | [SY] §3.4, `03-decoding.md` §5.3 |
| `DDCB`/`FDCB` with `rrr` ≠ 110 | rotate/shift, `RES`, `SET`: operation on `(IX+d)` and copy of the result into register `rrr`, 23 T (4,4,3,5,4,3); `BIT`: as the documented `BIT b,(IX+d)`, 20 T, no register written | [SY] §3.5–3.6, `03-decoding.md` §5.4 |

## 5. Sequencer state — 64 bits

One `struct packed` in `rmZ80_seq.sv`; the savestate is its vector view, in this
order (most significant first; `ss_dout[273]` = `phase`):

| Element | Bits | |
|---|---|---|
| `phase` | 1 | half of the T-state |
| `t_idx` | 3 | T-state within the M-cycle |
| `tw` | 1 | wait state: in the second half, `WAIT` sampled low (the T-state repeats); in the first half, the T-state is a repetition |
| `m_idx` | 3 | M-cycle within the instruction |
| `mtype` | 3 | type of the current M-cycle |
| `ir` | 8 | current opcode |
| `space` | 3 | prefix space (§4.1) |
| `din_q` | 8 | data latch: operand or displacement in flight |
| `wz` | 16 | `WZ`/MEMPTR |
| `q_reg` | 8 | bit 0: the previous instruction wrote F; bit 1: F written in this one; bits 2–3: DE↔HL renamed in bank 0 / 1; bit 4: bus request sampled; bit 5: release sampled; bit 6: Tx in its second half; bit 7: `INT` low at the sampling edge (`04-interrupts.md` §1) |
| `iff1`, `iff2` | 2 | |
| `im` | 2 | |
| `nmi_s` | 1 | `NMI` edge stored at the sampling edge (`04-interrupts.md` §1) |
| `nmi_pend` | 1 | `NMI` edge stored |
| `int_lat` | 1 | `INT` response in progress (mode 0) |
| `halt_q` | 1 | `HALT` |
| `nmi_lvl` | 1 | `NMI` level at the previous `cen`, for the edge |
| `busack` | 1 | bus released |
| **Total** | **64** | |

**Total CPU state: 210 (register file) + 64 = 274 bits.** There are no operand
latches in the ALU: operands are read live from the registers, the data latch and
`WZ`.

## 6. Cost and block memory

**0 M10K by construction**: no unpacked arrays, no synchronous table reads; the
control word and the schedule are combinational functions (`case`/`assign`). No
addressed synchronous pattern exists from which a RAM could be inferred.

Registers: 274 of state, plus the pipeline (§9: P0, 69 bits; NX, 30; P1), the registered bus outputs and a few registered conditions. `quartus/` builds
the core on its own, for anyone who wants to measure it.

## 7. What the sources do not fix — choices in the sequencer

The choices about pin timing are in `02-bus-cycles.md` §10, those about interrupts
in `04-interrupts.md` §10, those about unassigned positions in §4.2 above. The
T-state breakdown of the M-cycles within each instruction follows the opcode table
(`03-decoding.md`), from [UM] where it gives one and from [SY] otherwise; where
neither gives it (the unassigned `DD`/`FD` positions) it is the prefix M1 followed
by the base instruction.

## 8. How this document is verified

| Claim | Check |
|---|---|
| Schedule of all 1792 positions = the opcode table (bytes, M-cycles, T-states and breakdown, alternative timing) | `decode` bench (698 rows from [UM], 908 from [SY], 180 unassigned `DD`/`FD` positions — 174 against the rule of §4.2, the 6 prefix bytes among them as prefixes — and 6 prefixes) |
| Behaviour and T-states per instruction | SingleStepTests (1,604,000 cases, 0 with a different number of T-states); `exec` bench (19 programs, T-states against the table, final state and memory computed by hand) |
| Sequencer waveforms, counters, `WAIT`, interrupts, savestate of the 64 bits | `seq` bench |
| P0 = control word of the current state, NX = coordinates of the next state | assertions in `rmZ80.sv`, checked on every clock in the benches that build the whole CPU (`exec`, `sst`), not in the z80test build (`make zexf`, compiled with `RMZ80_NOCHK`) (§9.3) |
| The pipeline is invisible | `exec` bench: identical results with `cen` every 2 clocks and every 12 clocks, with random `WAIT`, random bus requests, and save/restore at every tick |
| 0 M10K, timing at 96 MHz | Quartus fit and timing reports (`quartus/`) |

## 9. Timing: the pipeline inside the half T-state

The CPU state changes only on clocks with `cen`, and two `cen` are at least 2
clocks apart. The control path — state → control word → register read → ALU →
register write — is cut into stages that each close in **one** clock, and the
stages between two `cen` add up to the same result the unpipelined design would
compute.

### 9.1 P1 — between the register read and the ALU

P1 (in `rmZ80.sv`) holds the ALU operands already selected (A,
B, the logic operand or bit mask, F, the 16-bit read), the ALU operation already
decoded (result and flag sources, effective subtraction, `DAA` corrections), and
the control fields used after it (write enables and selectors, address adder,
`WZ` writes, condition mode, bank swaps). It is updated on **every clock**, never
gated by `cen`: it is a function of the state, not state, and is not in the
savestate. With `rst`, only its write enables are cleared, which is the control
word of M1 T1A of a NOP, so the first `cen` may come on the first clock after
reset.

### 9.2 Why no tick changes

Let `e_k` be the clock of the *k*-th `cen` and *N* ≥ 2 the number of clocks between
two `cen`. The state changes only at `e_k`. At `e_k + 1`, P1 = F1(state_k); it then
stays constant until `e_{k+1}` ≥ `e_k + 2`, because its inputs are constant. At
`e_{k+1}` the write uses F2(P1) = F2(F1(state_k)) = F(state_k): **the same value at
the same clock** as without the pipeline. This holds for every register written on
`cen`, and so for every pin.

What the proof requires — the **contract** (`07-integration.md` §3):

1. **No unregistered input inside the path.** `data_in`, `wait_n`, `int_n`,
   `nmi_n`, `busreq_n` enter only the sequencer, where they are sampled on `cen`.
2. **`cen` at least 2 clocks apart.**
3. **After a savestate load, at least 2 clocks with `cen` = 0**, for the
   registers outside the savestate (P0, NX, P1) to be recomputed. After `rst`
   no quiet time is needed: they are loaded with the values of the reset state.

### 9.3 The control word registered ahead — P0 and NX

Computing the control word from the current state would put `state → control
word → register file` in one clock. Instead the word of the **next** state is
computed ahead and registered on the same `cen` at which the state changes:

| Register | Loaded | Content |
|---|---|---|
| **NX** (30 bits) | on every clock | coordinates of the **next** state: half, T, M (binary and one-hot), prefix space, halt, `INT` response, DE↔HL renaming, `(IX+d)` coordinates. Same rules as `rmZ80_seq`, **without pins** |
| **P0** (69 bits) | on `cen` | control word of NX = the word of the state that starts at that `cen`. The register file, P1, the sequencer and the bus read from P0 |

A T-state repeated by `WAIT` is foreseen by NX as well: the sequencer decides the
repetition on the falling edge and keeps it in `tw` for the whole second half (§3.2),
so NX already holds "same T-state, first half" when the rising edge comes.

Every stage closes in one clock: state/P0 → NX, NX → control word → P0, P0 →
register file → P1, P1 → ALU → state.

Events decided by a pin at the edge itself, which NX cannot foresee, were checked
by exhaustive enumeration of the control word over all its inputs:

- *acceptance of an interrupt at the end of an instruction*: at M1 T1A the word is
  the same for every instruction, `HALT` included, and differs from the interrupt
  response **only in the cycle type** (M1 or IACK), which is decided from the state
  in `rmZ80.sv` and sampled in T1A;
- *`IR` loaded from the bus* (the edge that opens T3 of M1, the mode 0 acknowledge,
  the fourth byte of `DDCB`/`FDCB`): in the half T-state that starts, the word does
  not depend on `IR` except for the HL → IX/IY substitutions, which only matter when
  reading HL, H or L, and none is read there.

The conditions that end an M-cycle early (`nxt_gate`, `row_ext`) are taken from
their sources: `cc`, `cc` of `JR` and `B` ≠ 0 are registered on every clock
(they depend only on F, `B` and `IR`, written long before they are used); the block
repeat condition is evaluated in place, because the block instruction writes its
flags in the last T-state.

The address leaves **one** flip-flop with no logic after it: `{I, R}` during
refresh, otherwise `WZ` or the 16-bit read.

**Checks**, on every clock (Verilator only), in the benches that build the whole CPU
(`exec`, `sst`) except the z80test build; the checks of point 3 that sit in
`rmZ80_seq` and `rmZ80_regfile` also run in the `seq` and `regfile` benches:

1. P0 equals the control word of the **current** state (a second instance of
   `rmZ80_cw`, without the delay), compared on its effects: reads per effective
   register, the schedule row and the cycle type where the sequencer consumes them;
2. NX equals the coordinates of the state after every `cen`, T-states repeated by
   `WAIT` included; when an interrupt is accepted, the fields the acceptance changes
   (prefix space, `INT` response, halt) are not compared;
3. the registered conditions equal those P1 would give where the sequencer uses
   them; the registered load selections equal those of the state; no load falls in
   the last T-state of an M-cycle; no write goes to a bank in the clock that swaps it.

The define `RMZ80_NOCHK` turns off checks 1 and 2 and the condition check of 3 for
long runs (the z80test build, `make zexf`); check 1 is the one that needs a second
instance of the control word.
