# 04 — Interrupts: NMI, INT, the three modes, reset

`02-bus-cycles.md` says **when** an interrupt is sampled and how long the
acknowledge cycle lasts; this document says **what the CPU does after accepting
it**, and how the two enable flip-flops behave.

Sources: [UM] chapter *Interrupt Response*, pp.17–20, and timing pp.12–14; [PS];
[SY] where marked (`00-sources.md`). Implementation: `rtl/rmZ80_seq.sv`
(acceptance, `IFF`, `IM`), `rtl/rmZ80_cw.sv` (the response sequences).

## 1. Two inputs, two flip-flops

| | Role |
|---|---|
| **`IFF1`** | Inhibits the acceptance of `INT` |
| **`IFF2`** | **Only temporary storage** for `IFF1`; inhibits nothing |

> "The state of IFF1 is used to inhibit interrupts while IFF2 is used as a
> temporary storage location for IFF1." — [UM] p.18

`NMI` is not maskable: no flip-flop blocks it.

**When the inputs are sampled.** "The CPU samples the interrupt signal (INT) with
the rising edge of the final clock at the end of any instruction" ([UM] p.12);
figure 9 draws `INT` low across the rising edge that opens the last T-state, and in
`HALT` "the two interrupt lines are sampled with the rising clock edge during each
T4" ([UM] p.14). `NMI` "is sampled at the same time as the interrupt line" ([UM]
p.13); it is edge-triggered, so what is sampled is whether a falling edge has been
stored. rmZ80 samples both on the rising edge that opens the last T-state of every
M-cycle, together with `BUSREQ`, and decides at the end of the instruction with the
sample of its last M-cycle. The enable conditions (`IFF1`, the `EI`/`DI` window)
are those at the end of the instruction.

## 2. What changes the flip-flops

From the text of [UM] pp.18–19:

| Action | `IFF1` | `IFF2` | Note |
|---|---|---|---|
| Reset | 0 | 0 | |
| `DI` | 0 | 0 | |
| `EI` | 1 | 1 | `INT` accepted only after the next instruction — §3 |
| `INT` accepted | 0 | 0 | "both IFF1 and IFF2 are automatically reset" |
| `NMI` accepted | 0 | **unchanged** | |
| `RETN`, `RETI` | ← `IFF2` | unchanged | `RETI`: §6 |
| `LD A,I`, `LD A,R` | unchanged | unchanged | copy `IFF2` into `P/V` — §9 |

The mechanism: `NMI` clears `IFF1` and leaves `IFF2` as it was, so `IFF2` keeps
the state from before the `NMI`; `RETN` copies it back. `NMI` does **not** copy
`IFF1` into `IFF2`: [SY] measured it with an `NMI` inside an `NMI` (pp.20–21,
erratum p.24), and the copy would lose the saved state at the second `NMI`.

> "When a nonmaskable interrupt is accepted, IFF1 resets to prevent further
> interrupts... but the previous state of IFF1 is saved so that the complete state
> of the CPU just prior to the nonmaskable interrupt can be restored at any
> time." — [UM] p.18

## 3. `EI` and the one-instruction delay

> "When an EI instruction is executed, **any pending interrupt request is not
> accepted until after the instruction following EI is executed**. This single
> instruction delay is necessary when the next instruction is a return
> instruction." — [UM] p.18

`EI` does not enable at its own end: the first window is at the end of the
**following** instruction, so that `EI` / `RET` at the end of a service routine
returns before it can be interrupted again.

[SY] §5.5 p.21 adds two points, adopted:

> "Directly after an EI or DI instruction, interrupts aren't accepted. They're
> accepted again after the instruction after the EI" — [SY] §5.5 p.21

- **`EI` followed by `EI`**: no interrupt is accepted during a sequence of `EI`
  ([SY] measured it with a long sequence); the window opens after the instruction
  that follows the **last** `EI`.
- **After `DI`**: no `INT` window at the end of `DI` either (with `IFF1` = 0 it
  is not observable anyway).

In rmZ80: `INT` is not accepted at the end of an `EI` or a `DI`; `NMI` is (§10).
On the base delay, the only point where both sources speak, [SY] agrees with [UM].

## 4. `NMI` response

> "The CPU always accepts a nonmaskable interrupt. When this nonmaskable
> interrupt is accepted, the CPU **ignores the next instruction that it fetches**
> and instead performs a restart at address 0066h." — [UM] p.19

1. The acknowledge cycle is an M1 whose data is **ignored** ([UM] p.13).
2. `PC` is pushed on the stack.
3. Jump to **0066h**.

Duration **11 T-states** ([SY] §5.1 p.19: "Accepting an NMI costs 11 t-states"):
M1 of 5 T, two writes of 3 T. [UM] gives no total, only that the CPU behaves "as
if it had recycled a restart instruction" (`RST` = 11 T, 5+3+3). Cross-check on the
durations where both sources speak — mode 0 with `RST` 13 T, mode 1 13 T, mode 2
19 T — **0 disagreements out of 3**.

`NMI` has priority over `INT` when both are present ([UM] p.14). It is an edge
input: the falling edge is stored, and is taken at the first instruction end whose
sampling edge (§1) comes after it.

## 5. `INT` response: the three modes

Common to all three: the acknowledge cycle is a **special M1** with `IORQ`
instead of `MREQ` and **two automatic wait states** (`02-bus-cycles.md` §7),
so **+2 T-states**.

### Mode 0 — the 8080 mode

> "The interrupting device can place **any instruction** on the data bus and the
> CPU executes it." — [UM] p.19

- Usually an `RST`, because one byte is enough, but it can be any instruction,
  including a 3-byte `CALL`.
- Duration: **two clocks more** than the normal duration of that instruction
  ([UM] p.19). `RST`: 11 + 2 = **13 T-states**; `CALL nn`: 17 + 2 = **19**.
- Multi-byte instructions: "The first byte of a multi-byte instruction is read
  during the interrupt acknowledge cycle. Subsequent bytes are read in by a normal
  memory read sequence" ([UM] p.184); "one interrupt acknowledge cycle followed by
  two memory read cycles" for a `CALL` ([PS] p.7). The device supplies bytes 2 and
  3 in those reads.
- **`PC` does not advance** during the instruction supplied by the device, so a
  `CALL` pushes the interrupted `PC`. None of the sources says so explicitly: it is
  a **deduction** from [UM] p.19 ("a 3-byte call to any location in memory could be
  executed") — with `PC` advancing, the `CALL` would push `PC + 2` and the return
  would skip two bytes of the interrupted program.
- **Mode 0 is the mode after reset** ([UM] p.19).

### Mode 1 — fixed restart

- Restart at **0038h**; otherwise identical to the `NMI` response ([UM] p.19).
- Duration: restart plus the two automatic waits, **13 T-states**.

### Mode 2 — indirect call through a table

[UM] p.20. The 16-bit pointer:

| Part | From |
|---|---|
| upper 8 bits | register **`I`**, loaded by the program (`LD I,A`); reset clears it |
| lower 8 bits | the **device**: **all 8 bits**, bit 0 included |

[UM] p.20 says that "the least-significant bit must be a 0", but that is a rule for
the device and the table (entries at even addresses), not an action of the CPU:
[UM] p.64 and p.186 say that the lower 8 bits of the pointer come from the device.
What the chip does was **measured** by [SY] (p.20 and erratum p.24): with FFh on the
bus the CPU reads the address from xxFFh, not xxFEh. rmZ80 uses the vector as it
is, odd values included. The **first byte of the table is the low byte** of the
address.

The CPU fetches the vector, pushes `PC`, reads the start address from the table
and jumps:

> "This mode of response requires **19 clock periods** to complete (seven to fetch
> the lower eight bits from the interrupting device, six to save the program
> counter, and six to obtain the jump address)." — [UM] p.20

## 6. `RETI` and `RETN`

| Instruction | What it does | T-states |
|---|---|---|
| `RETI` (`ED 4D`) | Return from an `INT` service routine; **`IFF1` ← `IFF2`** | 14 (4,4,3,3) — [UM] p.288 |
| `RETN` (`ED 45`) | Return from an `NMI` service routine: **`IFF1` ← `IFF2`** | 14 (4,4,3,3) — [UM] p.290 |

`RETI` exists as a separate instruction because Z80 daisy-chain peripherals
**recognise its opcode on the bus** to know that the routine has ended.

**On `RETI` the sources differ.** [UM] p.18 attributes the copy of `IFF2` into
`IFF1` to `RETN` only and the description of `RETI` (p.288) does not mention the
flip-flops; [SY] §5.3 pp.20–21 and table §8.10 p.34 report a measurement on silicon:
"I tested this by putting the machine in EI mode, wait for an NMI and end with one
of the ED RET instructions. Then execute a HALT instruction. If IFF1 was not
restored, the machine would hang but **this did not happen with any of the
instructions, including the documented RETI!**". [UM] does not state the opposite,
it is silent on `RETI`; [SY] describes a measurement, and SingleStepTests show the
same. rmZ80 follows [SY]: `RETI`, `RETN` and the undocumented `ED` duplicates all
copy `IFF2` into `IFF1`. In correct programs the point is rarely observable: an
`INT` routine ends with `EI` + `RETI`, so both flip-flops are already 1.

`IM 0`, `IM 1`, `IM 2` take 8 T-states (4,4) — [UM] pp.184–186. `LD A,I` and
`LD A,R` take 9 (4,5).

## 7. State after reset

From [UM] pp.18–19:

| Element | After reset |
|---|---|
| `IFF1`, `IFF2` | 0 |
| Interrupt mode | **0** |
| `I` | 0 |
| `PC` | 0000h ([UM], pin functions) |

**General registers.** [SY] §2.4 ("Power on defaults", p.9, and the `RESET` pin,
p.10): `AF` and `SP` are FFFFh after reset (measured); the other registers are
undefined. rmZ80: `AF` and `SP` = FFFFh; everything else — `AF'`, `BC`, `DE`, `HL`
and their alternates, `IX`, `IY`, `R`, `WZ`, the data latch, the bank selectors —
is **0**. `IR` is reset to 00h (NOP), so the first fetch carries no action of an
interrupted instruction.

These are also the **power-up** values. This matters on an FPGA: with
`ALLOW_POWER_UP_DONT_CARE` (enabled in MiSTer projects) the power-up value of a
register without a defined value is chosen by the synthesis tool and can change
with every netlist, so a program that reads a register before writing it would
behave differently from one build to the next, even after changes that do not
touch the logic. No state flip-flop in rmZ80 is left without a defined value.

`R` = 0 after reset comes from [SY] §6.1, not from [UM]; `I` = 0 is [UM].

## 8. Interrupt state in the savestate

| Element | Bits | Why |
|---|---|---|
| `IFF1`, `IFF2` | 2 | |
| Interrupt mode | 2 | |
| `NMI` edge stored, last `NMI` level | 2 | `NMI` is edge-triggered |
| `INT` level and stored `NMI` edge at the sampling edge (§1) | 2 | Taken at the end of the instruction; the sampling edge comes a T-state before it |
| `INT` response in progress | 1 | Mode 0: the instruction supplied by the device |
| `HALT` | 1 | |

All of them are in the sequencer state, `06-sequencer.md` §5. The `EI`/`DI` window
needs no bit of its own: it is a property of the instruction in progress, which the
savestate holds (`IR` and the position within the instruction).

## 9. Edge cases

| Case | rmZ80 | Source |
|---|---|---|
| Interrupt between a prefix and its opcode | **Not accepted**, neither `INT` nor `NMI`, also during long `DD`/`FD` sequences: the window is only at the end of an instruction | [SY] §5.5 pp.21–22 measured it for `INT` ("for all instructions, including the prefixed ones, interrupts are never accepted during an instruction"); for `NMI` [SY] §5.5 p.22 says "Although I haven't tested this" (choice: §10 below) |
| `LD A,I` / `LD A,R` with an interrupt accepted at their end | `P/V` = 0 instead of `IFF2`: "If an interrupt occurs during execution of this instruction, the Parity flag contains a 0". The source does not say which interrupt: rmZ80 applies it to both `INT` and `NMI` | [UM] pp.94–95 |
| `EI` followed by `EI` | Window closed for the whole sequence — §3 | [SY] §5.5 p.21 |
| `HALT` and an interrupt | Exit at the end of the current HALT M1; `NMI` wins if both are present | [UM] p.14 |
| Interrupt during a bus request | **Not accepted**, neither `INT` nor `NMI` | [UM] p.12 |

## 10. What the sources do not fix — choices in rmZ80

| Point | rmZ80 |
|---|---|
| `NMI` at the end of an `EI` or `DI` | Accepted ([SY] tests the closed window with `INT` only) |
| `NMI` between a prefix and its opcode | Not accepted, like `INT` |
| Breakdown of the 11 T-states of `NMI` | M1 5 T, write 3 T, write 3 T |
| `PC` during a multi-byte instruction in mode 0 | Held (§5, deduction from [UM] p.19) |

## 11. How this document is verified

| Claim | Check |
|---|---|
| `IFF1`/`IFF2` after every instruction | SingleStepTests (`iff1`, `iff2` compared after every case); `exec` bench through `LD A,I` → `P/V` |
| `EI` delay | `exec` bench, program F (INT in mode 1, `EI` window) |
| `INT` and `NMI` sampled on the edge that opens the last T-state | `seq` bench (`02-bus-cycles.md` §11) |
| `NMI`, `HALT`, `RETN` | `exec` bench, program G |
| Nested `NMI`: `IFF2` preserved, `IFF1` restored by two `RETN` | `exec` bench, program Q (fails on an RTL that copies `IFF1` into `IFF2`) |
| Mode 2 with an odd vector, 19 T-states | `exec` bench, program H (vector 41h: the right address at xx41h, a wrong one at xx40h) |
| Mode 0 with `RST` from the bus, 13 T-states | `exec` bench, program I |
| Mode 0 with `CALL nn` from the device: `PC` held, interrupted `PC` pushed, 19 T-states | `exec` bench, program S |
| `LD A,I` with an interrupt at its end: `P/V` = 0 | `exec` bench, program T |
| Interrupt durations | the `exec` bench counts the T-states of every program against the expected totals |
