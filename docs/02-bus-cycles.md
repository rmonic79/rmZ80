# 02 — Bus cycles: M-cycles, T-states, WAIT

How the CPU paces time and what it does on its pins. Sources: [UM] chapter
*Timing*, pp.7–16; [PS] figure 10; [SY] where marked (`00-sources.md`).
Implementation: `rtl/rmZ80_seq.sv` (waveforms), `rtl/rmZ80.sv` (output registers).

## 1. The principle

> "All instructions are a series of basic operations. Each of these operations can
> take from **three to six clock periods** to complete, or they can be lengthened
> to synchronize the CPU to the speed of external devices. These clock periods are
> referred to as **time (T) cycles**, and the operations are referred to as
> **machine (M) cycles**." — [UM] p.7

Hierarchy: **instruction → M-cycle → T-state**. `M1` (opcode fetch) takes 4, 5 or
6 T-states, the other M-cycles 3 to 5, all of them extendable by `WAIT` — [UM]
p.7. The basic operations are memory read/write, I/O read/write and interrupt
acknowledge — [UM] p.7.

Each T-state is split in two halves (§8): **A** from the rising edge of the clock,
**B** from the falling edge. Below, `T3B` means the second half of T3.

## 2. WAIT

> "During **T2** and every subsequent automatic WAIT state (TW), the CPU samples
> the WAIT line with the **falling edge** of the clock. If the WAIT line is active
> at this time, another WAIT state is entered during the following cycle." — [UM] p.7

In rmZ80 `wait_n` is read in T2 and in every wait state, automatic or not, on
the clock enable of the **falling edge** (from the first half of the T-state to the
second); the result is held for the second half, and if `WAIT` was low the T-state
is repeated as a TW on the next rising edge.

## 3. M1 — opcode fetch

[UM] p.8 and figure 5.

| T-state | What happens | Source |
|---|---|---|
| **T1** | `PC` on the address bus from the start of T1. `MREQ` and `RD` active from the falling edge of T1 (T1B): the address has had half a clock to settle, so the falling edge of `MREQ` can be used as a DRAM chip enable. `M1` low from the start of T1 | [UM] p.8; figure 5 |
| **T2** | `WAIT` read (§2) | |
| **T3** | The opcode is sampled on the **rising edge that opens T3**; the same edge releases `RD`, `MREQ` and `M1`. From the same edge the address is the refresh address and `RFSH` is active | [UM] p.8 |
| **T3–T4** | **Refresh** (§3.1). Meanwhile the CPU decodes and executes the fetched instruction | [UM] p.8 |

### 3.1 Refresh — [UM] p.8

> "During T3 and T4, the lower seven bits of the address bus contain a memory
> refresh address and the RFSH signal becomes active, indicating that a refresh
> read of all dynamic memories must be performed. To prevent data from different
> memory segments from being gated onto the data bus, an RD signal is not
> generated during this refresh period. **The MREQ signal during this refresh
> period should be used to perform a refresh read of all memory elements. The
> refresh signal cannot be used by itself, because the refresh address is only
> guaranteed to be stable during the MREQ period.**" — [UM] p.8

| # | Behaviour in rmZ80 | Source |
|---|---|---|
| 1 | Address = `{I, R}`: `R` on the low byte, `I` on the high byte | [UM] p.8; `I` from [UM] p.3 |
| 2 | `RFSH` active **T3A → T4B** | figure 5 |
| 3 | No `RD` and no `M1` during refresh | [UM] p.8 |
| 4 | `MREQ` **active** during refresh, **T3B → T4A** (low from the falling edge of T3 to the falling edge of T4) | [UM] p.8; phase from figure 5 |
| 5 | The refresh address is stable over the whole window **T3A → T4B** | [UM] p.8 (guarantee during `MREQ`); figure 5 |

### 3.2 When `R` is incremented

The address cannot change while `MREQ` is active (T3B–T4A), and figure 5 holds it
over the whole window T3A–T4B. [UM] p.3 says the seven bits are incremented "after
each instruction fetch" and [SY] §6.1 p.23 states openly that it is unknown
whether the increment comes before or after `R` is placed on the bus. An increment
inside the window would change the address with `RFSH` active, so it can only fall
before T3A or after T4B.

rmZ80 increments `R` **after T4B**: the bus shows `R` before the increment. This is
a choice (§10). `A7` during refresh is bit 7 of `R` ([SY] §6.1 p.23: "the whole
IR register is put on the address bus"; [UM] only speaks of "the lower seven bits").

## 4. Memory read and write

[UM] p.9, figure 6. Three T-states unless `WAIT` adds more.

- Read: "The MREQ signal and the RD signal are used the same way as in a fetch
  cycle" — [UM] p.9. `MREQ` and `RD` T1B → T2B, data sampled on the edge that
  opens T3, which releases `MREQ` and `RD`.
- Write: `MREQ` active when the address is stable (T1B), `WR` active when the data
  is stable (T2A in rmZ80, §10), and **`WR` inactive half a T-state before** the
  address and data change: `WR` and `MREQ` end with T3A.

## 5. I/O cycles

[UM] p.10, figure 7.

> "During I/O operations, **a single wait state is automatically inserted**."

The reason given: between `IORQ` going active and the CPU sampling `WAIT` there
is too little time for a port to decode its address and assert `WAIT`. I/O read or
write = 3 + 1 automatic = **4 T-states**, plus any `WAIT`. `WAIT` is read in T2
and in the automatic wait state. `IORQ` takes the place of `MREQ`; `RD` enables
the port in a read, `WR` clocks it in a write.

## 6. Bus request / acknowledge

[UM] p.11–12, figure 8; [PS] p.29, figure 10 ("TLM = Last state of any M cycle").

- `BUSREQ` is sampled on the rising edge that **opens** the last T-state of any
  M-cycle (TLM).
- If it is active, on the rising edge that closes TLM (start of the first Tx)
  `BUSACK` goes low and the address, data, `MREQ`, `IORQ`, `RD` and `WR` are
  released; `RFSH` and `M1` stay **driven high**, `HALT` is unchanged. ([UM] figure
  8 also lists `RFSH` among the floating signals; rmZ80 follows [PS], which draws it
  driven.)
- In every Tx `BUSREQ` is sampled again on the rising edge that opens it; when it
  is high, `BUSACK` rises on the **falling** edge of that Tx and on the next rising
  edge the CPU takes the bus back with T1 of the next cycle (`M1` goes low if it is
  a fetch).
- No refresh while the bus is released (the external controller does it, [UM] p.11).
- **During a bus request the CPU cannot be interrupted, by either `NMI` or `INT`**
  — [UM] p.12. An `NMI` edge during the request is stored and taken at the first
  instruction end after the bus is taken back.

In rmZ80 the state is held at T1 of the next cycle during the Tx and no register
is written. An FPGA has no high impedance inside: the released signals go inactive
(strobes high, `data_oe` = 0) and the address must not be used; the system switches
the bus to the requester with `busack_n`. If `BUSREQ` is not used, tie `busreq_n`
to 1.

## 7. Interrupts and HALT

### INT — maskable

[UM] p.12, figure 9.

- `INT` is sampled on the rising edge of the last clock at the end of any
  instruction — figure 9 draws it on the rising edge that **opens** the last
  T-state.
- It is not accepted if the interrupt enable flip-flop is reset, **or if `BUSREQ`
  is active**.
- When accepted, a **special M1** is generated: `IORQ` instead of `MREQ`, so that
  the device places an 8-bit value on the data bus, with **two wait states added
  automatically** for the daisy chain to settle: 4 + 2 = **6 T-states**.

In rmZ80 `INT` is sampled on the rising edge that opens the last T-state of
every M-cycle — the same edge where `BUSREQ` is sampled — and the sample taken in
the last M-cycle of the instruction decides at its end. An `INT` that goes low
during the last T-state is not taken at that end (if it is still low at the next
sampling edge, it is taken at the end of the next instruction); one that goes away
during the last T-state is still taken. In the acknowledge cycle `M1` stays
low through the two automatic wait states (T1–T4), the data is sampled on the edge
that opens T5, and T5–T6 are the refresh.

### NMI — non-maskable

[UM] p.13, figure 10. Sampled at the same time as `INT`, with priority over it,
and cannot be disabled. The response is like a normal memory read **but the data
bus is ignored**; the CPU pushes `PC` and jumps to **0066h**. `NMI` is an edge
input: the falling edge is stored, so an `NMI` shorter than an instruction is not
lost, and the stored edge is sampled on the same edge as `INT`: an edge that arrives
during the last T-state of an instruction is taken at the end of the next one.
Details in `04-interrupts.md` §4.

### HALT

[UM] p.14, figure 11.

- The CPU **executes NOPs** until an interrupt arrives. Each cycle is a normal
  fetch M1 whose data is ignored, with a NOP forced internally, to keep the
  refresh going. `PC` does not advance.
- The interrupt lines are sampled on the rising edge in T4; an NMI, or an INT
  with interrupts enabled, ends HALT on the next rising edge; if both arrive, NMI
  wins.
- `HALT` is active for the whole time.

HALT is not a stopped CPU: M1 cycles keep running, with refresh and `R`
incrementing, and the savestate holds it as such.

## 8. Time resolution: half a T-state

The chip uses both clock edges:

| Behaviour | When |
|---|---|
| `MREQ` active half a clock after the address | falling edge |
| `WAIT` sampled | falling edge, in T2 and each TW |
| Opcode sampled in T3 | rising edge |
| `WR` inactive half a T-state before the address and data change | half T |

rmZ80 therefore advances **one step per half T-state**: the system provides a
clock enable `cen` with **two pulses per T-state** (a Z80 at 4 MHz needs `cen` at
8 MHz). A half-T core can always behave like a whole-T one; the reverse would mean
re-timing every control signal.

All bus outputs leave flip-flops that are updated on every clock: the system sees
the same waveform delayed by one clock on all pins together (`07-integration.md`
§5).

## 9. Sequencer state

The position inside the instruction (M-cycle, T-state, half, wait state), the
cycle type, `IR`, the prefix state, the data latch, the stored `NMI` edge and the
bus-request state are part of the state and of the savestate:
`06-sequencer.md` §5.

## 10. What the sources do not fix — choices in rmZ80

| Point | rmZ80 |
|---|---|
| When `R` is incremented (§3.2) | After T4B: the bus shows `R` before the increment |
| `A7` during refresh | Bit 7 of `R` ([SY]) |
| `WR` assertion in memory and I/O writes | T2A |
| `IORQ` assertion in I/O and interrupt-acknowledge cycles, `RD` in I/O reads | T1B |
| Release of the strobes in I/O cycles | Read: `IORQ` and `RD` on the edge that opens T4, where the data is sampled. Write: `IORQ` and `WR` at the end of T4A, half a T-state before the cycle ends, as in a memory write |
| Interrupt acknowledge | No `RD` (the value is read with `IORQ` alone); refresh in T5–T6 without `MREQ` |
| Extra T-states of a 5- or 6-T M1 | Internal, no bus activity |
| Address during HALT | The address of the instruction after `HALT` (as SingleStepTests); the PC pushed by the interrupt is the same ([SY]) |

## 11. How this document is verified

| Claim | Check |
|---|---|
| Length of every M-cycle and T-state | `decode` bench (the T-state breakdown of every row of Appendix A); `seq` bench (the length of every cycle type, with 0 to 8 wait states); SingleStepTests (the total number of T-states of every case, 1,604,000 cases, 0 differences); `exec` bench (the T-states of each program against the totals of the opcode table in `03-decoding.md`) |
| Waveforms of M1, refresh, memory, I/O, interrupt acknowledge and `WAIT`, half T by half T | `seq` bench: every cycle type with 0 to 8 wait states, `WAIT` held for whole T-states, low only in the first half (counted) and low only in the second half (ignored) |
| `M1` low from the start of T1 | `exec` bench, every program: on every fetch and acknowledge `M1` is already low half a T-state before `MREQ`/`RD` or `IORQ` fall, and it never falls without one of them following |
| `INT` and `NMI` sampled on the edge that opens the last T-state | `seq` bench: `INT` low only after that edge is not taken, `INT` gone after that edge is taken, an `NMI` edge after it is taken at the end of the next instruction, an `NMI` edge on it is taken |
| Automatic wait state in I/O (4 T), two in interrupt acknowledge (6 T) | `seq` bench; `exec` bench (interrupt programs) |
| `BUSREQ`/`BUSACK` timing | `exec` bench, directed test (`BUSACK` low at the end of T4 of an M1, high half a T after the release is sampled, `M1` low at T1) and random `BUSREQ` on every program without interrupts, also with save and restore at every tick |
| HALT keeps refreshing | `seq` and `exec` benches |
| Waveform on silicon | not measured on a real Z80; checked in simulation by the `seq` and `exec` benches |
