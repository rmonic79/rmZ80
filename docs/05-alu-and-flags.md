# 05 — ALU and flag rules

`03-decoding.md` says, for each instruction, **which** flags it changes; this
document says **how they are computed**.

Sources: [UM] flag chapter pp.65–68, `DAA` p.173; [SY] where marked
(`00-sources.md`). The layout of `F` is in `01-registers-and-state.md` §2.
Implementation: `rtl/rmZ80_alu.sv`, `rtl/rmZ80_alu_core.sv`,
`rtl/rmZ80_alu_dec.sv`, `rtl/rmZ80_alu_daa.sv`, `rtl/rmZ80_alu_pkg.sv`.

## 1. ALU functions

> Add, Subtract, Logical AND, Logical OR, Logical exclusive OR, Compare, Left or
> right shifts or rotates (arithmetic and logical), Increment, Decrement, Set bit,
> Reset bit, Test bit — [UM] p.4

The ALU is 8 bits wide. There is no 16-bit adder in it: `ADD HL,rr`, `ADC HL,rr`
and `SBC HL,rr` run as two 8-bit steps through `WZ`. The only 16-bit adder of the
core is the address adder (PC, SP, WZ, relative jumps; `06-sequencer.md` §2).

## 2. `C` — Carry

Carry or borrow out of the most significant bit; in rotates and shifts, the bit
shifted out. `SCF` sets it, `CCF` complements it.

## 3. `N` — Add/Subtract

> "The Add/Subtract Flag (N) is used by the Decimal Adjust Accumulator
> instruction (DAA) to distinguish between the ADD and SUB instructions. For ADD
> instructions, N is cleared to 0. For SUB instructions, N is set to 1." — [UM] p.66

## 4. `P/V` — one bit, five meanings

| Context | Meaning | Source |
|---|---|---|
| Arithmetic | **Overflow**: result outside [−128, +127] | [UM] p.67 |
| Logic and rotates | **Parity**: even number of 1 bits → P = 1 | [UM] p.67 |
| Block transfer and search (`LDI LDIR LDD LDDR CPI CPIR CPD CPDR`) | `BC` after the decrement: 0 → reset, otherwise set | [UM] p.68 |
| `LD A,I` and `LD A,R` | `IFF2` (`04-interrupts.md` §9) | [UM] p.68 |
| `IN r,(C)` | Parity of the byte read | [UM] p.68 |

Overflow, in the form the RTL uses:

> "If there is a Carry in and no Carry out, or if there is no Carry in and a Carry
> out, then an Overflow has occurred." — [UM] p.67

`V = carry_in(bit 7) XOR carry_out(bit 7)`.

## 5. `H` — Half carry

> "The Half Carry Flag (H) is set (1) or cleared (0) depending on the Carry and
> Borrow status **between bits 3 and 4** of an 8-bit arithmetic operation." — [UM] p.68

| `H` | Add | Subtract |
|---|---|---|
| **1** | carry from bit 3 to bit 4 | borrow from bit 4 |
| **0** | no carry from bit 3 to bit 4 | no borrow from bit 4 |

## 6. `Z` — Zero

| Context | Meaning — [UM] p.68 |
|---|---|
| 8-bit arithmetic and logic | the result is 0 |
| Compare (search) instructions `CPI CPIR CPD CPDR` | `A` equals the byte at `(HL)` |
| **Bit test** (`BIT b,r`) | the **complement** of the tested bit |
| `INI IND OUTI OUTD` | the decremented `B` is 0 |
| `IN r,(C)` | the byte read is 0 |

## 7. `S` — Sign

Bit 7 of the result; for `BIT`, the rule of §9.

## 8. `DAA`

[UM] gives a conditional table, p.173. `DAA` takes 1 M-cycle, 4 T-states.

### After `ADD`, `ADC`, `INC` (`N` = 0)

| `C` before | High digit (bits 7–4) | `H` before | Low digit (bits 3–0) | Added | `C` after |
|---|---|---|---|---|---|
| 0 | 0–9 | 0 | 0–9 | `00` | 0 |
| 0 | 0–8 | 0 | A–F | `06` | 0 |
| 0 | 0–9 | 1 | 0–3 | `06` | 0 |
| 0 | A–F | 0 | 0–9 | `60` | 1 |
| 0 | 9–F | 0 | A–F | `66` | 1 |
| 0 | A–F | 1 | 0–3 | `66` | 1 |
| 1 | 0–2 | 0 | 0–9 | `60` | 1 |
| 1 | 0–2 | 0 | A–F | `66` | 1 |
| 1 | 0–3 | 1 | 0–3 | `66` | 1 |

### After `SUB`, `SBC`, `DEC`, `NEG` (`N` = 1)

| `C` before | High digit | `H` before | Low digit | Added | `C` after |
|---|---|---|---|---|---|
| 0 | 0–9 | 0 | 0–9 | `00` | 0 |
| 0 | 0–8 | 1 | 6–F | `FA` | 0 |
| 1 | 7–F | 0 | 0–9 | `A0` | 1 |
| 1 | 6–7 | 1 | 6–F | `9A` | 1 |

The PDF prints the high-digit range of the first row of the `N` = 0 table as `9–0`,
which is not a range:
a misprint for `0–9`, confirmed by §8.2.

### 8.1 The manual's table is not total

Over the 2048 input combinations (`N`, `C`, `H`, `A`): **684** are covered by
exactly one row, **1364** by none, 0 by more than one. The uncovered ones are the
states that cannot follow a valid BCD operation; but `DAA` after a non-BCD operation
is legal, programs do it, and the test suites check it. The manual alone is not
enough to implement `DAA`.

### 8.2 The total rule — [SY] §4.7

[SY] §4.7 pp.17–18 gives three tables (credited to Stefano Donati / Ramsoft) that
cover the whole input space: a `diff` is **added** if `N` = 0 and **subtracted** if
`N` = 1.

`diff`, from `C`, high digit, `H`, low digit:

| `C` | High digit | `H` | Low digit | `diff` |
|---|---|---|---|---|
| 0 | 0–9 | 0 | 0–9 | `00` |
| 0 | 0–9 | 1 | 0–9 | `06` |
| 0 | 0–8 | — | A–F | `06` |
| 0 | A–F | 0 | 0–9 | `60` |
| 1 | — | 0 | 0–9 | `60` |
| 1 | — | 1 | 0–9 | `66` |
| 1 | — | — | A–F | `66` |
| 0 | 9–F | — | A–F | `66` |
| 0 | A–F | 1 | 0–9 | `66` |

New `C`:

| `C` | High digit | Low digit | `C'` |
|---|---|---|---|
| 0 | 0–9 | 0–9 | 0 |
| 0 | 0–8 | A–F | 0 |
| 0 | 9–F | A–F | 1 |
| 0 | A–F | 0–9 | 1 |
| 1 | — | — | 1 |

New `H` — the manual has no such column:

| `N` | `H` | Low digit | `H'` |
|---|---|---|---|
| 0 | — | 0–9 | 0 |
| 0 | — | A–F | 1 |
| 1 | 0 | — | 0 |
| 1 | 1 | 6–F | 0 |
| 1 | 1 | 0–5 | 1 |

`S`, `YF`, `XF` are bits 7, 5 and 3 of the result; `Z` from the result; `P/V`
the parity of the result; **`N` unchanged** ([SY] §4.7; [UM] p.174).

### 8.3 Why [SY] can be relied on here

| Check | Result |
|---|---|
| Are the three tables total and exclusive over the 2048 combinations? | **2048/2048**, exactly one row per table, no overlaps |
| Where [UM] speaks (684 combinations), does [SY] agree? | **0 disagreements**, on the result and on `C'` |

A secondary source that disagreed with the manual where the manual speaks would
not be credible where it is silent; this one does not disagree in any of the 684.
In the `N` = 1 half [UM] writes as an addition what [SY] writes as a subtraction:
`+FA ≡ −06`, `+A0 ≡ −60`, `+9A ≡ −66`.

The `alu` bench checks both every time it runs (§11).

## 9. The undocumented flags `YF` and `XF`

[UM] declares bits 5 and 3 unused (`01-registers-and-state.md` §2). On silicon
they are not. General rule ([SY]): bits 5 and 3 of the **result**. The exceptions
are the cases the suites check:

| Case | `YF` / `XF` | Source |
|---|---|---|
| `CP s` | bits 5 and 3 of the **operand** `s`, not of the result ("YF and XF flags are copied from the operand s, not the result A-s") | [SY] §8.4 note 1, p.30 |
| `SCF` / `CCF` | NMOS: `A \| (F & ~Q)`, with `Q` = "the previous instruction wrote F" (`01-registers-and-state.md` §3): `A` after an instruction that writes F, `A \| F` otherwise. CMOS: `A` | [SY] §8.5 note 1 ("copied from register A"); the NMOS rule is z80test 1.2a (`z80ccf`) |
| `BIT b,r` | bits 5 and 3 of the operand `r`. `S` = 1 only if b = 7 and the bit is 1; `Z` = complement of the bit; `P/V` = `Z` | [SY] §4.1 p.15 for `S`/`Z`/`P/V`; z80test and SingleStepTests for `YF`/`XF` |
| `BIT b,(IX+d)` / `(IY+d)` | bits 5 and 3 of the **high byte of IX+d** | [SY] §4.1 p.15 |
| `BIT b,(HL)` | bits 13 and 11 of `WZ` (`01-registers-and-state.md` §7) | [SY] §4.1 pp.15–16 (partial); SingleStepTests; `z80memptr` |
| `LDI LDIR LDD LDDR` | with `n` = byte copied + `A`: `YF` = bit 1 of `n`, `XF` = bit 3 of `n`; `H` = 0, `P/V` = `BC` ≠ 0, `S` `Z` `C` unchanged | [SY] §4.2 p.16 |
| `CPI CPIR CPD CPDR` | with `n` = `A` − `(HL)` − `H'` (`H'` from the compare): `YF` = bit 1 of `n`, `XF` = bit 3 of `n`; `S` `Z` `H` from the compare, `P/V` = `BC` ≠ 0, `N` = 1, `C` unchanged | [SY] §4.2 p.16 |
| `INI OUTI IND OUTD` and their repeating forms | `S` `Z` `YF` `XF` from the decremented `B` (as `DEC B`); `N` = bit 7 of the byte transferred; `H`, `C`, `P/V` from the formulas of [SY] §4.3 | [SY] §4.3 pp.16–17 |
| Repeating block instructions, when they repeat | `YF`/`XF` from bits 13 and 11 of `PC`; the I/O forms also adjust `H` and `P/V` from `B`, `C` and `N` | SingleStepTests (`01-registers-and-state.md` §7) |

The NMOS/CMOS choice for `SCF`/`CCF` is the parameter `VARIANT_SCF_CCF_NMOS` of
`rmZ80_alu_core` (instance `u_alu` in `rmZ80.sv`; default 1, NMOS; not a parameter
of `rmZ80`); the constant of
`OUT (C),0` is the parameter `VARIANT_OUT_C0_FF` of `rmZ80`
(`03-decoding.md` §5.3).

## 10. `RLD` and `RRD` — moving the digits

[UM] pp.238–241.

### 10.1 `RLD` — `ED 6F`, [UM] p.238

> "The contents of the low-order four bits (bits 3, 2, 1, and 0) of the memory
> location (HL) are copied to the high-order four bits (7, 6, 5, and 4) of that
> same memory location; the previous contents of those high-order four bits are
> copied to the low-order four bits of the Accumulator (Register A); and the
> previous contents of the low-order four bits of the Accumulator are copied to
> the low-order four bits of memory location (HL). The contents of the
> high-order bits of the Accumulator are unaffected." — [UM] p.238

With `m` = `(HL)` before: **`(HL)' = {m[3:0], A[3:0]}`, `A' = {A[7:4], m[7:4]}`**.

### 10.2 `RRD` — `ED 67`, [UM] p.240

> "The contents of the low-order four bits (bits 3, 2, 1, and 0) of memory
> location (HL) are copied to the low-order four bits of the Accumulator
> (Register A). The previous contents of the low-order four bits of the
> Accumulator are copied to the high-order four bits (7, 6, 5, and 4) of
> location (HL); and the previous contents of the high-order four bits of (HL)
> are copied to the low-order four bits of (HL). The contents of the high-order
> bits of the Accumulator are unaffected." — [UM] p.240

**`A' = {A[7:4], m[3:0]}`, `(HL)' = {A[3:0], m[7:4]}`**.

### 10.3 Flags — [UM] pp.239 and 241, the same for both

| Flag | Rule |
|---|---|
| `S` | bit 7 of `A'` |
| `Z` | `A'` = 0 |
| `H` | 0 |
| `P/V` | parity of `A'` |
| `N` | 0 |
| `C` | unchanged |
| `YF`/`XF` | bits 5 and 3 of `A'` (general rule of §9; [UM] does not cover them) |

### 10.4 The manual's examples

| | `A` before | `(HL)` before | `A'` | `(HL)'` |
|---|---|---|---|---|
| `RLD` [UM] p.239 | `7Ah` | `31h` | `73h` | `1Ah` |
| `RRD` [UM] p.241 | `84h` | `20h` | `80h` | `42h` |

Both match the rules of §10.1–§10.2. Timing: 5 M-cycles, 18 T-states (4,4,3,4,3),
[UM] pp.238/240.

## 11. How this document is verified

| Claim | Check |
|---|---|
| Every 8-bit ALU operation | `alu` bench: exhaustive enumeration of the operands and of the incoming flags each operation reads (1,325,056 cases), against a model written from this document in a different form (overflow from signed arithmetic, parity by counting bits, and so on); the 16-bit steps compositionally (1,572,864 cases) |
| `YF`/`XF` in the `alu` bench | checked where a source gives a per-instruction rule (`CP`, `DAA`, `SCF`, `CCF`, `BIT`, `RES`, `SET`, the 16-bit `ADD`, `ADC` and `SBC`); masked where only the general rule of [SY] exists (23 operations, plus `TST`, `LD A,I`/`LD A,R` and the two `RLD`/`RRD` accumulator steps), and those are checked by the external suites below. `S` and `P/V` of `BIT` are masked as well |
| `SCF`/`CCF`, both variants | `alu` bench, NMOS build (full suite) and CMOS build (`SCF`/`CCF`, 786,432 cases) |
| `DAA` | `alu` bench: the [SY] rule on all 2048 combinations (each of the three tables gives exactly one row) and, on the 684 combinations the [UM] table covers, the same result and `C'` as [UM]; `zexdoc` |
| `RLD` / `RRD` | `alu` bench: all `A` × `(HL)` (262,144 cases) |
| Documented flags of every instruction | `zexdoc`, `z80doc`, `z80docflags`, SingleStepTests |
| `YF`/`XF` in every case, `SCF`/`CCF` after every instruction, `BIT n,(HL)` after every instruction | `z80full`, `z80flags`, `z80ccf`, `z80memptr`, SingleStepTests |
