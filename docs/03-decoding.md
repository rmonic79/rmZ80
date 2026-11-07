# 03 — Decoding tables

The opcode table of the Z80, from which the control word and the schedule of
`06-sequencer.md` are written: all 1792 positions of the decoding spaces (six
tables; `DD CB` and `FD CB` share one table and are counted separately in Appendix A:
7 × 256),
with length, M-cycles, T-states and their breakdown, flags, `R` increments and
the source page of each.

It covers the field structure of the opcode (§1), the six decoding tables (§2),
the prefix rules (§3), a summary of what the decoder has to do (§4), the
undocumented behaviour in a separate section (§5), the self-checks of the tables
(§6), and Appendix A, the same data in a rigid machine-readable format, which the
`decode` and `exec` benches read directly.

## Sources

| Tag | Source | Used for |
|---|---|---|
| **[UM]** | Zilog *Z80 CPU User Manual*, UM008011-0816 | the tables of §2: every assigned row |
| **[SY]** | Sean Young, *The Undocumented Z80 Documented*, v0.91 | the undocumented behaviour (§5), the prefix rules where marked (§3), the timing of the positions [UM] does not assign (Appendix A, `NOTE` = `SY`) |

Rule: **every assigned row cites the page of the manual** it comes from (printed
page; in the PDF, printed page + 14).

## 0. Reading conventions

- **Bytes** = total length of the instruction in memory, prefixes, displacement and
  immediates included.
- **M** = machine cycles; **T-states** = total, with the breakdown per M-cycle in
  brackets as the manual prints it (`19 (4,4,3,5,3)` = 5 M-cycles of 4, 4, 3, 5, 3
  T-states). Convention stated by the manual, [UM] p.69.
- **Two timings, two pairs of columns.** `M`/`T-states` = condition **true**
  (jump taken, call made) or an iteration **that repeats** (block instructions);
  `M (f)`/`T-states (f)` = condition **false** or the **last** iteration. `—` in the
  (f) columns = a single timing stated by the manual.
- **R** = increments of `R` per execution of the instruction (visible on the bus
  during refresh). Without a prefix: 1 — [UM] p.3 (one increment per instruction
  fetch). With a prefix (`CB` `ED` `DD` `FD`, and also `DD CB` / `FD CB`): 2 —
  [SY] §6.1; the manual does not fix it (§3.2). Repeating block instructions run
  the instruction again (`PC − 2`), so the increments repeat at every iteration.
  In the prefix rows the column is `—`: the count belongs to the whole instruction.
- **Unassigned** positions are declared as such, row by row or as an explicit
  range with its count: a declared gap, never a silent one.
- `[R1]`…`[R7]` = misprints **inside the manual** found while transcribing, listed
  in §6.2 with their page; the cell holds the value consistent with the T-state
  breakdown, §6.2 the printed one.
- `[n1]`…`[n5]` = notes: [n4] after the base table, [n1] [n2] [n3] [n5] after the
  `ED` table.

### Flags column

Six symbols in the fixed order **S Z H P/V N C**. The exact rule of each flag is
on the page the row cites; the symbols:

| Symbol | Meaning | Basis |
|---|---|---|
| `-` | unchanged | |
| `r` | from the result (sign for S, zero for Z) | page of the row |
| `c` | carry/borrow: for H between bits 3 and 4 (8 bits) or 11 and 12 (16 bits); for C from bit 7 / borrow, or the bit shifted out in rotates | [UM] p.68 table 23 and page of the row |
| `0` / `1` | forced to 0 / to 1 | page of the row |
| `v` | arithmetic overflow (including "operand was 7Fh/80h" for INC/DEC/NEG) | [UM] p.67 and page of the row |
| `p` | even parity of the result | [UM] p.67 |
| `i` | copy of IFF2 | [UM] p.68 |
| `b` | 1 if BC − 1 ≠ 0 (block instructions) | [UM] p.68 |
| `z` | Z = 1 if the tested bit is 0 (BIT) | [UM] p.243 |
| `x` | "unknown" — the manual's own word | page of the row |
| `d` | from the DAA table | [UM] p.173 |
| `k` | copy of the previous carry (H of CCF) | [UM] p.178 |
| `t` | inverted carry (C of CCF) | [UM] p.178 |
| `—` | (whole cell) no flag changed | page of the row |

Example: `rrcv0c` on `ADD A, r` = S and Z from the result, H carry from bit 3 to
bit 4, P/V overflow, N reset, C carry from bit 7 — [UM] pp.145–146. The
undocumented flags `YF`/`XF` are not in this column: `05-alu-and-flags.md` §9.

## 1. Opcode structure

The manual does not state a "grid" of fields: it follows from the binary encodings
each description gives. Every pattern below is cited; the quadrant summary at the
end is **our observation** on those patterns, not a statement of the manual.

### 1.1 3-bit register field `r`

The same in every description that uses it ([UM] p.71 ff.):

| Register | r |
|---|---|
| B | 000 |
| C | 001 |
| D | 010 |
| E | 011 |
| H | 100 |
| L | 101 |
| A | 111 |

The value **110 is not a register**: where the encoding has `r`, the pattern with
110 is taken by `(HL)` — compare `LD r, r'` = `01 rrr rrr'` ([UM] p.71) with
`LD r, (HL)` = `01 rrr 110` ([UM] p.74) and `LD (HL), r` = `01 110 rrr` ([UM]
p.79); `76` (= `01 110 110`) is HALT ([UM] p.181). The same rule holds in the `CB`
space ([UM] p.213/215) and in `LD r, n` = `00 rrr 110` against `LD (HL), n` =
`36` ([UM] p.72/85).

Exception stated by the manual: in `IN r, (C)` the value 110 is "Undefined op
code; set the flag" ([UM] p.296) — see §2.3 and §5.3.

### 1.2 16-bit pairs: four different alphabets

| Field | 00 | 01 | 10 | 11 | Where | Page |
|---|---|---|---|---|---|---|
| `dd`/`ss` | BC | DE | HL | SP | 16-bit LD, INC/DEC/ADD/ADC/SBC | [UM] p.99, 188 |
| `qq` | BC | DE | HL | **AF** | PUSH/POP | [UM] p.115, 119 |
| `pp` | BC | DE | **IX** | SP | ADD IX, pp | [UM] p.194 |
| `rr` | BC | DE | **IY** | SP | ADD IY, rr | [UM] p.196 |

The field is bits 5–4 of the opcode byte (`LD dd, nn` = `00 dd 0001`, [UM] p.99;
`PUSH qq` = `11 qq 0101`, [UM] p.115).

### 1.3 Conditions `cc` (3 bits, bits 5–3)

From [UM] p.263 (JP cc), the same at p.284 (CALL cc) and p.286 (RET cc):

| cc | Condition | Flag tested |
|---|---|---|
| 000 | NZ (non-zero) | Z |
| 001 | Z (zero) | Z |
| 010 | NC (no carry) | C |
| 011 | C (carry) | C |
| 100 | PO (parity odd) | P/V |
| 101 | PE (parity even) | P/V |
| 110 | P (sign positive) | S |
| 111 | M (sign negative) | S |

Used in `JP cc, nn` = `11 ccc 010` ([UM] p.263), `CALL cc, nn` = `11 ccc 100`
([UM] p.283), `RET cc` = `11 ccc 000` ([UM] p.286). **JR does not use this
field**: it has only 4 conditions with their own opcodes (20/28/30/38, [UM]
pp.267–273); DJNZ stands alone (10, [UM] p.278).

### 1.4 Bit field `b` and restart field `t`

- `b` (bits 5–3, `CB` space): bit 0 = 000 to 7 = 111 — [UM] p.243. In
  `BIT b` = `01 bbb rrr` (p.243), `RES b` = `10 bbb rrr` (p.259),
  `SET b` = `11 bbb rrr` (p.251).
- `t` (bits 5–3): restart address `p = t × 8` — 00h = 000, 08h = 001, 10h = 010,
  18h = 011, 20h = 100, 28h = 101, 30h = 110, 38h = 111, in `RST p` =
  `11 ttt 111` — [UM] p.292.

### 1.5 ALU operation field (bits 5–3, quadrant 10)

From the encodings of the eight arithmetic and logic families (`10 ooo rrr` on a
register, `11 ooo 110` with an immediate):

| ooo | Operation | Page |
|---|---|---|
| 000 | ADD A | [UM] p.145 |
| 001 | ADC A | [UM] p.151 |
| 010 | SUB | [UM] p.153 |
| 011 | SBC A | [UM] p.155 |
| 100 | AND | [UM] p.157 |
| 101 | XOR | [UM] p.161 |
| 110 | OR | [UM] p.159 |
| 111 | CP | [UM] p.163 |

In the `CB` space quadrant 00 has the same scheme `00 ooo rrr` with RLC = 000
(p.213), RRC = 001 (p.224), RL = 010 (p.221), RR = 011 (p.227), SLA = 100
(p.230), SRA = 101 (p.233), SRL = 111 (p.236); **ooo = 110 is not assigned** by
the manual (it is the SLL slot, §5.1).

In the `ED` space quadrant 01 encodes: `01 rrr 000` IN r,(C) (p.296),
`01 rrr 001` OUT (C),r (p.307), `01 ss 0010` SBC HL (p.192), `01 ss 1010` ADC HL
(p.190), `01 dd 0011` LD (nn),dd (p.108), `01 dd 1011` LD dd,(nn) (p.103).

### 1.6 Quadrant summary (our observation on the patterns above)

First byte without a prefix, bits [7:6]:

| [7:6] | Content |
|---|---|
| 00 | mixed: immediate loads, INC/DEC, rotates of A, relative jumps, 16-bit |
| 01 | `LD r, r'` (+ (HL)); `01 110 110` = HALT |
| 10 | ALU `10 ooo rrr` |
| 11 | flow control (`ccc`), PUSH/POP, ALU with immediate, RST, prefixes |

### 1.7 Order of the operands in memory

- 16-bit `nn`: **low byte first, then high** — [UM] p.44 and p.99.
- With indexed addressing the displacement `d` (signed, two's complement)
  **immediately follows the opcode**: `DD 36 d n` for `LD (IX+d), n` — [UM] p.45
  and p.86. The only exception is `DD CB` / `FD CB`, where `d` comes **before** the
  final opcode (§2.6 and §3.1).
- Relative jumps: memory holds `e − 2` (the assembler compensates for the PC
  already incremented by 2) — [UM] p.265.

## 2. The six decoding tables

Six separate spaces. Each has 256 positions; assigned + unassigned = 256, count
stated after each table and re-checked in §6.1.

| Space | Prefix | § |
|---|---|---|
| Base | none | 2.1 |
| Bit | `CB` | 2.2 |
| Extended | `ED` | 2.3 |
| Indexed IX | `DD` | 2.4 |
| Indexed IY | `FD` | 2.5 |
| Indexed bit | `DD CB` / `FD CB` (opcode **after** the displacement) | 2.6 |

### 2.1 Base space (no prefix) — 256 positions

All 256 positions are assigned: 252 instructions + 4 prefix bytes (`CB` `DD`
`ED` `FD`). For the prefixes the Page column cites the first page where the
manual shows the encoding as the first byte of an instruction.

| Op | Mnemonic | Bytes | M | T-states | M (f) | T-states (f) | Flags | R | Page |
|---|---|---|---|---|---|---|---|---|---|
| 00 | NOP | 1 | 1 | 4 | — | — | — | 1 | 180 |
| 01 | LD BC, nn | 3 | 3 [R1] | 10 (4,3,3) | — | — | — | 1 | 99 |
| 02 | LD (BC), A | 1 | 2 | 7 (4,3) | — | — | — | 1 | 91 |
| 03 | INC BC | 1 | 1 | 6 | — | — | — | 1 | 198 |
| 04 | INC B | 1 | 1 | 4 | — | — | rrcv0- | 1 | 165 |
| 05 | DEC B | 1 | 1 | 4 | — | — | rrcv1- | 1 | 170 |
| 06 | LD B, n | 2 | 2 | 7 (4,3) | — | — | — | 1 | 72 |
| 07 | RLCA | 1 | 1 | 4 | — | — | --0-0c | 1 | 205 |
| 08 | EX AF, AF' | 1 | 1 | 4 | — | — | — | 1 | 125 |
| 09 | ADD HL, BC | 1 | 3 | 11 (4,4,3) | — | — | --c-0c | 1 | 188 |
| 0A | LD A, (BC) | 1 | 2 | 7 (4,3) | — | — | — | 1 | 88 |
| 0B | DEC BC | 1 | 1 | 6 | — | — | — | 1 | 201 |
| 0C | INC C | 1 | 1 | 4 | — | — | rrcv0- | 1 | 165 |
| 0D | DEC C | 1 | 1 | 4 | — | — | rrcv1- | 1 | 170 |
| 0E | LD C, n | 2 | 2 | 7 (4,3) | — | — | — | 1 | 72 |
| 0F | RRCA | 1 | 1 | 4 | — | — | --0-0c | 1 | 209 |
| 10 | DJNZ e | 2 | 3 | 13 (5,3,5) | 2 | 8 (5,3) | — | 1 | 278 |
| 11 | LD DE, nn | 3 | 3 [R1] | 10 (4,3,3) | — | — | — | 1 | 99 |
| 12 | LD (DE), A | 1 | 2 | 7 (4,3) | — | — | — | 1 | 92 |
| 13 | INC DE | 1 | 1 | 6 | — | — | — | 1 | 198 |
| 14 | INC D | 1 | 1 | 4 | — | — | rrcv0- | 1 | 165 |
| 15 | DEC D | 1 | 1 | 4 | — | — | rrcv1- | 1 | 170 |
| 16 | LD D, n | 2 | 2 | 7 (4,3) | — | — | — | 1 | 72 |
| 17 | RLA | 1 | 1 | 4 | — | — | --0-0c | 1 | 207 |
| 18 | JR e | 2 | 3 | 12 (4,3,5) | — | — | — | 1 | 265 |
| 19 | ADD HL, DE | 1 | 3 | 11 (4,4,3) | — | — | --c-0c | 1 | 188 |
| 1A | LD A, (DE) | 1 | 2 | 7 (4,3) | — | — | — | 1 | 89 |
| 1B | DEC DE | 1 | 1 | 6 | — | — | — | 1 | 201 |
| 1C | INC E | 1 | 1 | 4 | — | — | rrcv0- | 1 | 165 |
| 1D | DEC E | 1 | 1 | 4 | — | — | rrcv1- | 1 | 170 |
| 1E | LD E, n | 2 | 2 | 7 (4,3) | — | — | — | 1 | 72 |
| 1F | RRA | 1 | 1 | 4 | — | — | --0-0c | 1 | 211 |
| 20 | JR NZ, e | 2 | 3 | 12 (4,3,5) | 2 | 7 (4,3) | — | 1 | 273 |
| 21 | LD HL, nn | 3 | 3 [R1] | 10 (4,3,3) | — | — | — | 1 | 99 |
| 22 | LD (nn), HL | 3 | 5 | 16 (4,3,3,3,3) | — | — | — | 1 | 107 |
| 23 | INC HL | 1 | 1 | 6 | — | — | — | 1 | 198 |
| 24 | INC H | 1 | 1 | 4 | — | — | rrcv0- | 1 | 165 |
| 25 | DEC H | 1 | 1 | 4 | — | — | rrcv1- | 1 | 170 |
| 26 | LD H, n | 2 | 2 | 7 (4,3) | — | — | — | 1 | 72 |
| 27 | DAA | 1 | 1 | 4 | — | — | rrdp-d | 1 | 173 |
| 28 | JR Z, e | 2 | 3 | 12 (4,3,5) | 2 | 7 (4,3) | — | 1 | 271 |
| 29 | ADD HL, HL | 1 | 3 | 11 (4,4,3) | — | — | --c-0c | 1 | 188 |
| 2A | LD HL, (nn) | 3 | 5 | 16 (4,3,3,3,3) | — | — | — | 1 | 102 |
| 2B | DEC HL | 1 | 1 | 6 | — | — | — | 1 | 201 |
| 2C | INC L | 1 | 1 | 4 | — | — | rrcv0- | 1 | 165 |
| 2D | DEC L | 1 | 1 | 4 | — | — | rrcv1- | 1 | 170 |
| 2E | LD L, n | 2 | 2 | 7 (4,3) | — | — | — | 1 | 72 |
| 2F | CPL | 1 | 1 | 4 | — | — | --1-1- | 1 | 175 |
| 30 | JR NC, e | 2 | 3 | 12 (4,3,5) | 2 [R3] | 7 (4,3) | — | 1 | 269 |
| 31 | LD SP, nn | 3 | 3 [R1] | 10 (4,3,3) | — | — | — | 1 | 99 |
| 32 | LD (nn), A | 3 | 4 | 13 (4,3,3,3) | — | — | — | 1 | 93 |
| 33 | INC SP | 1 | 1 | 6 | — | — | — | 1 | 198 |
| 34 | INC (HL) | 1 | 3 | 11 (4,4,3) | — | — | rrcv0- | 1 | 167 |
| 35 | DEC (HL) | 1 | 3 | 11 (4,4,3) | — | — | rrcv1- | 1 | 170 |
| 36 | LD (HL), n | 2 | 3 | 10 (4,3,3) | — | — | — | 1 | 85 |
| 37 | SCF | 1 | 1 | 4 | — | — | --0-01 | 1 | 179 |
| 38 | JR C, e | 2 | 3 | 12 (4,3,5) | 2 | 7 (4,3) | — | 1 | 267 |
| 39 | ADD HL, SP | 1 | 3 | 11 (4,4,3) | — | — | --c-0c | 1 | 188 |
| 3A | LD A, (nn) | 3 | 4 | 13 (4,3,3,3) | — | — | — | 1 | 90 |
| 3B | DEC SP | 1 | 1 | 6 | — | — | — | 1 | 201 |
| 3C | INC A | 1 | 1 | 4 | — | — | rrcv0- | 1 | 165 |
| 3D | DEC A | 1 | 1 | 4 | — | — | rrcv1- | 1 | 170 |
| 3E | LD A, n | 2 | 2 | 7 (4,3) | — | — | — | 1 | 72 |
| 3F | CCF | 1 | 1 | 4 | — | — | --k-0t | 1 | 178 |
| 40 | LD B, B | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 41 | LD B, C | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 42 | LD B, D | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 43 | LD B, E | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 44 | LD B, H | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 45 | LD B, L | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 46 | LD B, (HL) | 1 | 2 | 7 (4,3) | — | — | — | 1 | 74 |
| 47 | LD B, A | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 48 | LD C, B | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 49 | LD C, C | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 4A | LD C, D | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 4B | LD C, E | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 4C | LD C, H | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 4D | LD C, L | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 4E | LD C, (HL) | 1 | 2 | 7 (4,3) | — | — | — | 1 | 74 |
| 4F | LD C, A | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 50 | LD D, B | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 51 | LD D, C | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 52 | LD D, D | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 53 | LD D, E | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 54 | LD D, H | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 55 | LD D, L | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 56 | LD D, (HL) | 1 | 2 | 7 (4,3) | — | — | — | 1 | 74 |
| 57 | LD D, A | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 58 | LD E, B | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 59 | LD E, C | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 5A | LD E, D | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 5B | LD E, E | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 5C | LD E, H | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 5D | LD E, L | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 5E | LD E, (HL) | 1 | 2 | 7 (4,3) | — | — | — | 1 | 74 |
| 5F | LD E, A | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 60 | LD H, B | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 61 | LD H, C | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 62 | LD H, D | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 63 | LD H, E | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 64 | LD H, H | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 65 | LD H, L | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 66 | LD H, (HL) | 1 | 2 | 7 (4,3) | — | — | — | 1 | 74 |
| 67 | LD H, A | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 68 | LD L, B | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 69 | LD L, C | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 6A | LD L, D | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 6B | LD L, E | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 6C | LD L, H | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 6D | LD L, L | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 6E | LD L, (HL) | 1 | 2 | 7 (4,3) | — | — | — | 1 | 74 |
| 6F | LD L, A | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 70 | LD (HL), B | 1 | 2 | 7 (4,3) | — | — | — | 1 | 79 |
| 71 | LD (HL), C | 1 | 2 | 7 (4,3) | — | — | — | 1 | 79 |
| 72 | LD (HL), D | 1 | 2 | 7 (4,3) | — | — | — | 1 | 79 |
| 73 | LD (HL), E | 1 | 2 | 7 (4,3) | — | — | — | 1 | 79 |
| 74 | LD (HL), H | 1 | 2 | 7 (4,3) | — | — | — | 1 | 79 |
| 75 | LD (HL), L | 1 | 2 | 7 (4,3) | — | — | — | 1 | 79 |
| 76 | HALT | 1 | 1 | 4 | — | — | — | 1 | 181 |
| 77 | LD (HL), A | 1 | 2 | 7 (4,3) | — | — | — | 1 | 79 |
| 78 | LD A, B | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 79 | LD A, C | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 7A | LD A, D | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 7B | LD A, E | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 7C | LD A, H | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 7D | LD A, L | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 7E | LD A, (HL) | 1 | 2 | 7 (4,3) | — | — | — | 1 | 74 |
| 7F | LD A, A | 1 | 1 | 4 | — | — | — | 1 | 71 |
| 80 | ADD A, B | 1 | 1 | 4 | — | — | rrcv0c | 1 | 145 |
| 81 | ADD A, C | 1 | 1 | 4 | — | — | rrcv0c | 1 | 145 |
| 82 | ADD A, D | 1 | 1 | 4 | — | — | rrcv0c | 1 | 145 |
| 83 | ADD A, E | 1 | 1 | 4 | — | — | rrcv0c | 1 | 145 |
| 84 | ADD A, H | 1 | 1 | 4 | — | — | rrcv0c | 1 | 145 |
| 85 | ADD A, L | 1 | 1 | 4 | — | — | rrcv0c | 1 | 145 |
| 86 | ADD A, (HL) | 1 | 2 | 7 (4,3) | — | — | rrcv0c | 1 | 148 |
| 87 | ADD A, A | 1 | 1 | 4 | — | — | rrcv0c | 1 | 145 |
| 88 | ADC A, B | 1 | 1 | 4 | — | — | rrcv0c | 1 | 151 |
| 89 | ADC A, C | 1 | 1 | 4 | — | — | rrcv0c | 1 | 151 |
| 8A | ADC A, D | 1 | 1 | 4 | — | — | rrcv0c | 1 | 151 |
| 8B | ADC A, E | 1 | 1 | 4 | — | — | rrcv0c | 1 | 151 |
| 8C | ADC A, H | 1 | 1 | 4 | — | — | rrcv0c | 1 | 151 |
| 8D | ADC A, L | 1 | 1 | 4 | — | — | rrcv0c | 1 | 151 |
| 8E | ADC A, (HL) | 1 | 2 | 7 (4,3) | — | — | rrcv0c | 1 | 151 |
| 8F | ADC A, A | 1 | 1 | 4 | — | — | rrcv0c | 1 | 151 |
| 90 | SUB B | 1 | 1 | 4 | — | — | rrcv1c | 1 | 153 |
| 91 | SUB C | 1 | 1 | 4 | — | — | rrcv1c | 1 | 153 |
| 92 | SUB D | 1 | 1 | 4 | — | — | rrcv1c | 1 | 153 |
| 93 | SUB E | 1 | 1 | 4 | — | — | rrcv1c | 1 | 153 |
| 94 | SUB H | 1 | 1 | 4 | — | — | rrcv1c | 1 | 153 |
| 95 | SUB L | 1 | 1 | 4 | — | — | rrcv1c | 1 | 153 |
| 96 | SUB (HL) | 1 | 2 | 7 (4,3) | — | — | rrcv1c | 1 | 153 |
| 97 | SUB A | 1 | 1 | 4 | — | — | rrcv1c | 1 | 153 |
| 98 | SBC A, B | 1 | 1 | 4 | — | — | rrcv1c [R4] | 1 | 155 |
| 99 | SBC A, C | 1 | 1 | 4 | — | — | rrcv1c [R4] | 1 | 155 |
| 9A | SBC A, D | 1 | 1 | 4 | — | — | rrcv1c [R4] | 1 | 155 |
| 9B | SBC A, E | 1 | 1 | 4 | — | — | rrcv1c [R4] | 1 | 155 |
| 9C | SBC A, H | 1 | 1 | 4 | — | — | rrcv1c [R4] | 1 | 155 |
| 9D | SBC A, L | 1 | 1 | 4 | — | — | rrcv1c [R4] | 1 | 155 |
| 9E | SBC A, (HL) | 1 | 2 | 7 (4,3) | — | — | rrcv1c [R4] | 1 | 155 |
| 9F | SBC A, A | 1 | 1 | 4 | — | — | rrcv1c [R4] | 1 | 155 |
| A0 | AND B | 1 | 1 | 4 | — | — | rr1p00 [R4] | 1 | 157 |
| A1 | AND C | 1 | 1 | 4 | — | — | rr1p00 [R4] | 1 | 157 |
| A2 | AND D | 1 | 1 | 4 | — | — | rr1p00 [R4] | 1 | 157 |
| A3 | AND E | 1 | 1 | 4 | — | — | rr1p00 [R4] | 1 | 157 |
| A4 | AND H | 1 | 1 | 4 | — | — | rr1p00 [R4] | 1 | 157 |
| A5 | AND L | 1 | 1 | 4 | — | — | rr1p00 [R4] | 1 | 157 |
| A6 | AND (HL) | 1 | 2 | 7 (4,3) | — | — | rr1p00 [R4] | 1 | 157 |
| A7 | AND A | 1 | 1 | 4 | — | — | rr1p00 [R4] | 1 | 157 |
| A8 | XOR B | 1 | 1 | 4 | — | — | rr0p00 | 1 | 161 |
| A9 | XOR C | 1 | 1 | 4 | — | — | rr0p00 | 1 | 161 |
| AA | XOR D | 1 | 1 | 4 | — | — | rr0p00 | 1 | 161 |
| AB | XOR E | 1 | 1 | 4 | — | — | rr0p00 | 1 | 161 |
| AC | XOR H | 1 | 1 | 4 | — | — | rr0p00 | 1 | 161 |
| AD | XOR L | 1 | 1 | 4 | — | — | rr0p00 | 1 | 161 |
| AE | XOR (HL) | 1 | 2 | 7 (4,3) | — | — | rr0p00 | 1 | 161 |
| AF | XOR A | 1 | 1 | 4 | — | — | rr0p00 | 1 | 161 |
| B0 | OR B | 1 | 1 | 4 | — | — | rr0p00 [R4] | 1 | 159 |
| B1 | OR C | 1 | 1 | 4 | — | — | rr0p00 [R4] | 1 | 159 |
| B2 | OR D | 1 | 1 | 4 | — | — | rr0p00 [R4] | 1 | 159 |
| B3 | OR E | 1 | 1 | 4 | — | — | rr0p00 [R4] | 1 | 159 |
| B4 | OR H | 1 | 1 | 4 | — | — | rr0p00 [R4] | 1 | 159 |
| B5 | OR L | 1 | 1 | 4 | — | — | rr0p00 [R4] | 1 | 159 |
| B6 | OR (HL) | 1 | 2 | 7 (4,3) | — | — | rr0p00 [R4] | 1 | 159 |
| B7 | OR A | 1 | 1 | 4 | — | — | rr0p00 [R4] | 1 | 159 |
| B8 | CP B | 1 | 1 | 4 | — | — | rrcv1c | 1 | 163 |
| B9 | CP C | 1 | 1 | 4 | — | — | rrcv1c | 1 | 163 |
| BA | CP D | 1 | 1 | 4 | — | — | rrcv1c | 1 | 163 |
| BB | CP E | 1 | 1 | 4 | — | — | rrcv1c | 1 | 163 |
| BC | CP H | 1 | 1 | 4 | — | — | rrcv1c | 1 | 163 |
| BD | CP L | 1 | 1 | 4 | — | — | rrcv1c | 1 | 163 |
| BE | CP (HL) | 1 | 2 | 7 (4,3) | — | — | rrcv1c | 1 | 163 |
| BF | CP A | 1 | 1 | 4 | — | — | rrcv1c | 1 | 163 |
| C0 | RET NZ | 1 | 3 | 11 (5,3,3) | 1 | 5 | — | 1 | 286 |
| C1 | POP BC | 1 | 3 | 10 (4,3,3) | — | — | — | 1 | 119 |
| C2 | JP NZ, nn | 3 | 3 | 10 (4,3,3) | — | — | — [n4] | 1 | 263 |
| C3 | JP nn | 3 | 3 | 10 (4,3,3) | — | — | — | 1 | 262 |
| C4 | CALL NZ, nn | 3 | 5 | 17 (4,3,4,3,3) | 3 | 10 (4,3,3) | — | 1 | 283 |
| C5 | PUSH BC | 1 | 3 | 11 (5,3,3) | — | — | — | 1 | 115 |
| C6 | ADD A, n | 2 | 2 | 7 (4,3) | — | — | rrcv0c | 1 | 147 |
| C7 | RST 00h | 1 | 3 | 11 (5,3,3) | — | — | — | 1 | 292 |
| C8 | RET Z | 1 | 3 | 11 (5,3,3) | 1 | 5 | — | 1 | 286 |
| C9 | RET | 1 | 3 | 10 (4,3,3) | — | — | — | 1 | 285 |
| CA | JP Z, nn | 3 | 3 | 10 (4,3,3) | — | — | — [n4] | 1 | 263 |
| CB | **prefix** → Bit space (§2.2) | — | — | — | — | — | — | — | 213 ff. |
| CC | CALL Z, nn | 3 | 5 | 17 (4,3,4,3,3) | 3 | 10 (4,3,3) | — | 1 | 283 |
| CD | CALL nn | 3 | 5 | 17 (4,3,4,3,3) | — | — | — | 1 | 281 |
| CE | ADC A, n | 2 | 2 | 7 (4,3) | — | — | rrcv0c | 1 | 151 |
| CF | RST 08h | 1 | 3 | 11 (5,3,3) | — | — | — | 1 | 292 |
| D0 | RET NC | 1 | 3 | 11 (5,3,3) | 1 | 5 | — | 1 | 286 |
| D1 | POP DE | 1 | 3 | 10 (4,3,3) | — | — | — | 1 | 119 |
| D2 | JP NC, nn | 3 | 3 | 10 (4,3,3) | — | — | — [n4] | 1 | 263 |
| D3 | OUT (n), A | 2 | 3 | 11 (4,3,4) | — | — | — | 1 | 306 |
| D4 | CALL NC, nn | 3 | 5 | 17 (4,3,4,3,3) | 3 | 10 (4,3,3) | — | 1 | 283 |
| D5 | PUSH DE | 1 | 3 | 11 (5,3,3) | — | — | — | 1 | 115 |
| D6 | SUB n | 2 | 2 | 7 (4,3) | — | — | rrcv1c | 1 | 153 |
| D7 | RST 10h | 1 | 3 | 11 (5,3,3) | — | — | — | 1 | 292 |
| D8 | RET C | 1 | 3 | 11 (5,3,3) | 1 | 5 | — | 1 | 286 |
| D9 | EXX | 1 | 1 | 4 | — | — | — | 1 | 126 |
| DA | JP C, nn | 3 | 3 | 10 (4,3,3) | — | — | — [n4] | 1 | 263 |
| DB | IN A, (n) | 2 | 3 | 11 (4,3,4) | — | — | — | 1 | 295 |
| DC | CALL C, nn | 3 | 5 | 17 (4,3,4,3,3) | 3 | 10 (4,3,3) | — | 1 | 283 |
| DD | **prefix** → IX space (§2.4) | — | — | — | — | — | — | — | 75 ff. |
| DE | SBC A, n | 2 | 2 | 7 (4,3) | — | — | rrcv1c [R4] | 1 | 155 |
| DF | RST 18h | 1 | 3 | 11 (5,3,3) | — | — | — | 1 | 292 |
| E0 | RET PO | 1 | 3 | 11 (5,3,3) | 1 | 5 | — | 1 | 286 |
| E1 | POP HL | 1 | 3 | 10 (4,3,3) | — | — | — | 1 | 119 |
| E2 | JP PO, nn | 3 | 3 | 10 (4,3,3) | — | — | — [n4] | 1 | 263 |
| E3 | EX (SP), HL | 1 | 5 | 19 (4,3,4,3,5) | — | — | — | 1 | 127 |
| E4 | CALL PO, nn | 3 | 5 | 17 (4,3,4,3,3) | 3 | 10 (4,3,3) | — | 1 | 283 |
| E5 | PUSH HL | 1 | 3 | 11 (5,3,3) | — | — | — | 1 | 115 |
| E6 | AND n | 2 | 2 | 7 (4,3) | — | — | rr1p00 [R4] | 1 | 157 |
| E7 | RST 20h | 1 | 3 | 11 (5,3,3) | — | — | — | 1 | 292 |
| E8 | RET PE | 1 | 3 | 11 (5,3,3) | 1 | 5 | — | 1 | 286 |
| E9 | JP (HL) | 1 | 1 | 4 | — | — | — | 1 | 275 |
| EA | JP PE, nn | 3 | 3 | 10 (4,3,3) | — | — | — [n4] | 1 | 263 |
| EB | EX DE, HL | 1 | 1 | 4 | — | — | — | 1 | 124 |
| EC | CALL PE, nn | 3 | 5 | 17 (4,3,4,3,3) | 3 | 10 (4,3,3) | — | 1 | 283 |
| ED | **prefix** → Extended space (§2.3) | — | — | — | — | — | — | — | 94 ff. |
| EE | XOR n | 2 | 2 | 7 (4,3) | — | — | rr0p00 | 1 | 161 |
| EF | RST 28h | 1 | 3 | 11 (5,3,3) | — | — | — | 1 | 292 |
| F0 | RET P | 1 | 3 | 11 (5,3,3) | 1 | 5 | — | 1 | 286 |
| F1 | POP AF | 1 | 3 | 10 (4,3,3) | — | — | — | 1 | 119 |
| F2 | JP P, nn | 3 | 3 | 10 (4,3,3) | — | — | — [n4] | 1 | 263 |
| F3 | DI | 1 | 1 | 4 | — | — | — | 1 | 182 |
| F4 | CALL P, nn | 3 | 5 | 17 (4,3,4,3,3) | 3 | 10 (4,3,3) | — | 1 | 283 |
| F5 | PUSH AF | 1 | 3 | 11 (5,3,3) | — | — | — | 1 | 115 |
| F6 | OR n | 2 | 2 | 7 (4,3) | — | — | rr0p00 [R4] | 1 | 159 |
| F7 | RST 30h | 1 | 3 | 11 (5,3,3) | — | — | — | 1 | 292 |
| F8 | RET M | 1 | 3 | 11 (5,3,3) | 1 | 5 | — | 1 | 286 |
| F9 | LD SP, HL | 1 | 1 | 6 | — | — | — | 1 | 112 |
| FA | JP M, nn | 3 | 3 | 10 (4,3,3) | — | — | — [n4] | 1 | 263 |
| FB | EI | 1 | 1 | 4 | — | — | — | 1 | 183 |
| FC | CALL M, nn | 3 | 5 | 17 (4,3,4,3,3) | 3 | 10 (4,3,3) | — | 1 | 283 |
| FD | **prefix** → IY space (§2.5) | — | — | — | — | — | — | — | 77 ff. |
| FE | CP n | 2 | 2 | 7 (4,3) | — | — | rrcv1c | 1 | 163 |
| FF | RST 38h | 1 | 3 | 11 (5,3,3) | — | — | — | 1 | 292 |

**Count 2.1**: assigned 256, unassigned 0 — total 256.

Note:
- **[n4]** `JP cc, nn`: the manual gives **one** timing, 10 (4,3,3), for both a
  true and a false condition (the instruction reads the two address bytes anyway)
  — [UM] p.264. It is the only conditional instruction with a single timing: hence
  the (f) columns stay `—`.

### 2.2 Bit space (prefix `CB`) — 256 positions

Second byte after `CB`. Total bytes = 2 for every assigned position. The 8
positions `30–37` (pattern `00 110 rrr`) are **not assigned** by the manual: they
are the SLL slot, [SY] only — see §5.1.

| Op | Mnemonic | Bytes | M | T-states | M (f) | T-states (f) | Flags | R | Page |
|---|---|---|---|---|---|---|---|---|---|
| 00 | RLC B | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 213 |
| 01 | RLC C | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 213 |
| 02 | RLC D | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 213 |
| 03 | RLC E | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 213 |
| 04 | RLC H | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 213 |
| 05 | RLC L | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 213 |
| 06 | RLC (HL) | 2 | 4 | 15 (4,4,4,3) | — | — | rr0p0c | 2 | 215 |
| 07 | RLC A | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 213 |
| 08 | RRC B | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 224 |
| 09 | RRC C | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 224 |
| 0A | RRC D | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 224 |
| 0B | RRC E | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 224 |
| 0C | RRC H | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 224 |
| 0D | RRC L | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 224 |
| 0E | RRC (HL) | 2 | 4 | 15 (4,4,4,3) | — | — | rr0p0c | 2 | 224 |
| 0F | RRC A | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 224 |
| 10 | RL B | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 221 |
| 11 | RL C | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 221 |
| 12 | RL D | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 221 |
| 13 | RL E | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 221 |
| 14 | RL H | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 221 |
| 15 | RL L | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 221 |
| 16 | RL (HL) | 2 | 4 | 15 (4,4,4,3) | — | — | rr0p0c | 2 | 221 |
| 17 | RL A | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 221 |
| 18 | RR B | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 227 |
| 19 | RR C | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 227 |
| 1A | RR D | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 227 |
| 1B | RR E | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 227 |
| 1C | RR H | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 227 |
| 1D | RR L | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 227 |
| 1E | RR (HL) | 2 | 4 | 15 (4,4,4,3) | — | — | rr0p0c | 2 | 227 |
| 1F | RR A | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 227 |
| 20 | SLA B | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 230 |
| 21 | SLA C | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 230 |
| 22 | SLA D | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 230 |
| 23 | SLA E | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 230 |
| 24 | SLA H | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 230 |
| 25 | SLA L | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 230 |
| 26 | SLA (HL) | 2 | 4 | 15 (4,4,4,3) | — | — | rr0p0c | 2 | 230 |
| 27 | SLA A | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 230 |
| 28 | SRA B | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 233 |
| 29 | SRA C | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 233 |
| 2A | SRA D | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 233 |
| 2B | SRA E | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 233 |
| 2C | SRA H | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 233 |
| 2D | SRA L | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 233 |
| 2E | SRA (HL) | 2 | 4 | 15 (4,4,4,3) | — | — | rr0p0c | 2 | 233 |
| 2F | SRA A | 2 | 2 | 8 (4,4) | — | — | rr0p0c | 2 | 233 |
| 30 | *unassigned* | — | — | — | — | — | — | — | — |
| 31 | *unassigned* | — | — | — | — | — | — | — | — |
| 32 | *unassigned* | — | — | — | — | — | — | — | — |
| 33 | *unassigned* | — | — | — | — | — | — | — | — |
| 34 | *unassigned* | — | — | — | — | — | — | — | — |
| 35 | *unassigned* | — | — | — | — | — | — | — | — |
| 36 | *unassigned* | — | — | — | — | — | — | — | — |
| 37 | *unassigned* | — | — | — | — | — | — | — | — |
| 38 | SRL B | 2 | 2 | 8 (4,4) | — | — | 0r0p0c | 2 | 236 |
| 39 | SRL C | 2 | 2 | 8 (4,4) | — | — | 0r0p0c | 2 | 236 |
| 3A | SRL D | 2 | 2 | 8 (4,4) | — | — | 0r0p0c | 2 | 236 |
| 3B | SRL E | 2 | 2 | 8 (4,4) | — | — | 0r0p0c | 2 | 236 |
| 3C | SRL H | 2 | 2 | 8 (4,4) | — | — | 0r0p0c | 2 | 236 |
| 3D | SRL L | 2 | 2 | 8 (4,4) | — | — | 0r0p0c | 2 | 236 |
| 3E | SRL (HL) | 2 | 4 | 15 (4,4,4,3) | — | — | 0r0p0c | 2 | 236 |
| 3F | SRL A | 2 | 2 | 8 (4,4) | — | — | 0r0p0c | 2 | 236 |
| 40 | BIT 0, B | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 41 | BIT 0, C | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 42 | BIT 0, D | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 43 | BIT 0, E | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 44 | BIT 0, H | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 45 | BIT 0, L | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 46 | BIT 0, (HL) | 2 | 3 | 12 (4,4,4) | — | — | xz1x0- [R6] | 2 | 245 |
| 47 | BIT 0, A | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 48 | BIT 1, B | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 49 | BIT 1, C | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 4A | BIT 1, D | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 4B | BIT 1, E | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 4C | BIT 1, H | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 4D | BIT 1, L | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 4E | BIT 1, (HL) | 2 | 3 | 12 (4,4,4) | — | — | xz1x0- [R6] | 2 | 245 |
| 4F | BIT 1, A | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 50 | BIT 2, B | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 51 | BIT 2, C | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 52 | BIT 2, D | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 53 | BIT 2, E | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 54 | BIT 2, H | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 55 | BIT 2, L | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 56 | BIT 2, (HL) | 2 | 3 | 12 (4,4,4) | — | — | xz1x0- [R6] | 2 | 245 |
| 57 | BIT 2, A | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 58 | BIT 3, B | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 59 | BIT 3, C | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 5A | BIT 3, D | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 5B | BIT 3, E | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 5C | BIT 3, H | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 5D | BIT 3, L | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 5E | BIT 3, (HL) | 2 | 3 | 12 (4,4,4) | — | — | xz1x0- [R6] | 2 | 245 |
| 5F | BIT 3, A | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 60 | BIT 4, B | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 61 | BIT 4, C | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 62 | BIT 4, D | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 63 | BIT 4, E | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 64 | BIT 4, H | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 65 | BIT 4, L | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 66 | BIT 4, (HL) | 2 | 3 | 12 (4,4,4) | — | — | xz1x0- [R6] | 2 | 245 |
| 67 | BIT 4, A | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 68 | BIT 5, B | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 69 | BIT 5, C | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 6A | BIT 5, D | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 6B | BIT 5, E | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 6C | BIT 5, H | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 6D | BIT 5, L | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 6E | BIT 5, (HL) | 2 | 3 | 12 (4,4,4) | — | — | xz1x0- [R6] | 2 | 245 |
| 6F | BIT 5, A | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 70 | BIT 6, B | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 71 | BIT 6, C | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 72 | BIT 6, D | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 73 | BIT 6, E | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 74 | BIT 6, H | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 75 | BIT 6, L | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 76 | BIT 6, (HL) | 2 | 3 | 12 (4,4,4) | — | — | xz1x0- [R6] | 2 | 245 |
| 77 | BIT 6, A | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 78 | BIT 7, B | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 79 | BIT 7, C | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 7A | BIT 7, D | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 7B | BIT 7, E | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 7C | BIT 7, H | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 7D | BIT 7, L | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 7E | BIT 7, (HL) | 2 | 3 | 12 (4,4,4) | — | — | xz1x0- [R6] | 2 | 245 |
| 7F | BIT 7, A | 2 | 2 | 8 (4,4) | — | — | xz1x0- | 2 | 243 |
| 80 | RES 0, B | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| 81 | RES 0, C | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| 82 | RES 0, D | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| 83 | RES 0, E | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| 84 | RES 0, H | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| 85 | RES 0, L | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| 86 | RES 0, (HL) | 2 | 4 | 15 (4,4,4,3) | — | — | — | 2 | 259 |
| 87 | RES 0, A | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| 88 | RES 1, B | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| 89 | RES 1, C | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| 8A | RES 1, D | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| 8B | RES 1, E | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| 8C | RES 1, H | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| 8D | RES 1, L | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| 8E | RES 1, (HL) | 2 | 4 | 15 (4,4,4,3) | — | — | — | 2 | 259 |
| 8F | RES 1, A | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| 90 | RES 2, B | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| 91 | RES 2, C | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| 92 | RES 2, D | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| 93 | RES 2, E | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| 94 | RES 2, H | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| 95 | RES 2, L | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| 96 | RES 2, (HL) | 2 | 4 | 15 (4,4,4,3) | — | — | — | 2 | 259 |
| 97 | RES 2, A | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| 98 | RES 3, B | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| 99 | RES 3, C | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| 9A | RES 3, D | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| 9B | RES 3, E | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| 9C | RES 3, H | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| 9D | RES 3, L | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| 9E | RES 3, (HL) | 2 | 4 | 15 (4,4,4,3) | — | — | — | 2 | 259 |
| 9F | RES 3, A | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| A0 | RES 4, B | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| A1 | RES 4, C | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| A2 | RES 4, D | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| A3 | RES 4, E | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| A4 | RES 4, H | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| A5 | RES 4, L | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| A6 | RES 4, (HL) | 2 | 4 | 15 (4,4,4,3) | — | — | — | 2 | 259 |
| A7 | RES 4, A | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| A8 | RES 5, B | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| A9 | RES 5, C | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| AA | RES 5, D | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| AB | RES 5, E | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| AC | RES 5, H | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| AD | RES 5, L | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| AE | RES 5, (HL) | 2 | 4 | 15 (4,4,4,3) | — | — | — | 2 | 259 |
| AF | RES 5, A | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| B0 | RES 6, B | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| B1 | RES 6, C | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| B2 | RES 6, D | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| B3 | RES 6, E | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| B4 | RES 6, H | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| B5 | RES 6, L | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| B6 | RES 6, (HL) | 2 | 4 | 15 (4,4,4,3) | — | — | — | 2 | 259 |
| B7 | RES 6, A | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| B8 | RES 7, B | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| B9 | RES 7, C | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| BA | RES 7, D | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| BB | RES 7, E | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| BC | RES 7, H | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| BD | RES 7, L | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| BE | RES 7, (HL) | 2 | 4 | 15 (4,4,4,3) | — | — | — | 2 | 259 |
| BF | RES 7, A | 2 | 2 [R2] | 8 (4,4) | — | — | — | 2 | 259 |
| C0 | SET 0, B | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| C1 | SET 0, C | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| C2 | SET 0, D | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| C3 | SET 0, E | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| C4 | SET 0, H | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| C5 | SET 0, L | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| C6 | SET 0, (HL) | 2 | 4 | 15 (4,4,4,3) | — | — | — | 2 | 253 |
| C7 | SET 0, A | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| C8 | SET 1, B | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| C9 | SET 1, C | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| CA | SET 1, D | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| CB | SET 1, E | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| CC | SET 1, H | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| CD | SET 1, L | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| CE | SET 1, (HL) | 2 | 4 | 15 (4,4,4,3) | — | — | — | 2 | 253 |
| CF | SET 1, A | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| D0 | SET 2, B | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| D1 | SET 2, C | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| D2 | SET 2, D | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| D3 | SET 2, E | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| D4 | SET 2, H | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| D5 | SET 2, L | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| D6 | SET 2, (HL) | 2 | 4 | 15 (4,4,4,3) | — | — | — | 2 | 253 |
| D7 | SET 2, A | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| D8 | SET 3, B | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| D9 | SET 3, C | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| DA | SET 3, D | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| DB | SET 3, E | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| DC | SET 3, H | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| DD | SET 3, L | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| DE | SET 3, (HL) | 2 | 4 | 15 (4,4,4,3) | — | — | — | 2 | 253 |
| DF | SET 3, A | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| E0 | SET 4, B | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| E1 | SET 4, C | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| E2 | SET 4, D | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| E3 | SET 4, E | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| E4 | SET 4, H | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| E5 | SET 4, L | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| E6 | SET 4, (HL) | 2 | 4 | 15 (4,4,4,3) | — | — | — | 2 | 253 |
| E7 | SET 4, A | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| E8 | SET 5, B | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| E9 | SET 5, C | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| EA | SET 5, D | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| EB | SET 5, E | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| EC | SET 5, H | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| ED | SET 5, L | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| EE | SET 5, (HL) | 2 | 4 | 15 (4,4,4,3) | — | — | — | 2 | 253 |
| EF | SET 5, A | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| F0 | SET 6, B | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| F1 | SET 6, C | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| F2 | SET 6, D | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| F3 | SET 6, E | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| F4 | SET 6, H | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| F5 | SET 6, L | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| F6 | SET 6, (HL) | 2 | 4 | 15 (4,4,4,3) | — | — | — | 2 | 253 |
| F7 | SET 6, A | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| F8 | SET 7, B | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| F9 | SET 7, C | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| FA | SET 7, D | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| FB | SET 7, E | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| FC | SET 7, H | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| FD | SET 7, L | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |
| FE | SET 7, (HL) | 2 | 4 | 15 (4,4,4,3) | — | — | — | 2 | 253 |
| FF | SET 7, A | 2 | 2 | 8 (4,4) | — | — | — | 2 | 251 |

**Count 2.2**: assigned 248, unassigned 8 (range `30–37`) — total 256.

### 2.3 Extended space (prefix `ED`) — 256 positions

The manual assigns positions only in the blocks `40–7F` and `A0–BF`; these two
blocks are listed row by row below (with the unassigned positions declared), and
the completely empty ranges are declared right after the table.

Two duplicates are **documented**: `ED 63` = `LD (nn), HL` via `01 dd 0011` with
dd = HL ([UM] p.108) duplicates the base opcode `22`; `ED 6B` = `LD HL, (nn)` via
`01 dd 1011` ([UM] p.103) duplicates `2A`. Different timings (20 T against 16 T).

`ED 70`: the manual itself, in the table of the `r` field of `IN r, (C)`, marks the
value 110 as "Undefined op code; set the flag" ([UM] p.296). It is declared
unassigned here; its behaviour is in §5.3.

| Op | Mnemonic | Bytes | M | T-states | M (f) | T-states (f) | Flags | R | Page |
|---|---|---|---|---|---|---|---|---|---|
| 40 | IN B, (C) | 2 | 3 | 12 (4,4,4) | — | — | rr0p0- | 2 | 296 |
| 41 | OUT (C), B | 2 | 3 | 12 (4,4,4) | — | — | — | 2 | 307 |
| 42 | SBC HL, BC | 2 | 4 | 15 (4,4,4,3) | — | — | rrcv1c | 2 | 192 |
| 43 | LD (nn), BC | 4 | 6 | 20 (4,4,3,3,3,3) | — | — | — | 2 | 108 |
| 44 | NEG | 2 | 2 | 8 (4,4) | — | — | rrcv1c [n3] | 2 | 176 |
| 45 | RETN | 2 | 4 | 14 (4,4,3,3) | — | — | — | 2 | 290 |
| 46 | IM 0 | 2 | 2 | 8 (4,4) | — | — | — | 2 | 184 |
| 47 | LD I, A | 2 | 2 | 9 (4,5) | — | — | — | 2 | 96 |
| 48 | IN C, (C) | 2 | 3 | 12 (4,4,4) | — | — | rr0p0- | 2 | 296 |
| 49 | OUT (C), C | 2 | 3 | 12 (4,4,4) | — | — | — | 2 | 307 |
| 4A | ADC HL, BC | 2 | 4 | 15 (4,4,4,3) | — | — | rrcv0c | 2 | 190 |
| 4B | LD BC, (nn) | 4 | 6 | 20 (4,4,3,3,3,3) | — | — | — | 2 | 103 |
| 4C | *unassigned* | — | — | — | — | — | — | — | — |
| 4D | RETI | 2 | 4 | 14 (4,4,3,3) | — | — | — | 2 | 288 |
| 4E | *unassigned* | — | — | — | — | — | — | — | — |
| 4F | LD R, A | 2 | 2 | 9 (4,5) | — | — | — | 2 | 97 |
| 50 | IN D, (C) | 2 | 3 | 12 (4,4,4) | — | — | rr0p0- | 2 | 296 |
| 51 | OUT (C), D | 2 | 3 | 12 (4,4,4) | — | — | — | 2 | 307 |
| 52 | SBC HL, DE | 2 | 4 | 15 (4,4,4,3) | — | — | rrcv1c | 2 | 192 |
| 53 | LD (nn), DE | 4 | 6 | 20 (4,4,3,3,3,3) | — | — | — | 2 | 108 |
| 54 | *unassigned* | — | — | — | — | — | — | — | — |
| 55 | *unassigned* | — | — | — | — | — | — | — | — |
| 56 | IM 1 | 2 | 2 | 8 (4,4) | — | — | — | 2 | 185 |
| 57 | LD A, I | 2 | 2 | 9 (4,5) | — | — | rr0i0- [n1] | 2 | 94 |
| 58 | IN E, (C) | 2 | 3 | 12 (4,4,4) | — | — | rr0p0- | 2 | 296 |
| 59 | OUT (C), E | 2 | 3 | 12 (4,4,4) | — | — | — | 2 | 307 |
| 5A | ADC HL, DE | 2 | 4 | 15 (4,4,4,3) | — | — | rrcv0c | 2 | 190 |
| 5B | LD DE, (nn) | 4 | 6 | 20 (4,4,3,3,3,3) | — | — | — | 2 | 103 |
| 5C | *unassigned* | — | — | — | — | — | — | — | — |
| 5D | *unassigned* | — | — | — | — | — | — | — | — |
| 5E | IM 2 | 2 | 2 | 8 (4,4) | — | — | — | 2 | 186 |
| 5F | LD A, R | 2 | 2 | 9 (4,5) | — | — | rr0i0- [n1] | 2 | 95 |
| 60 | IN H, (C) | 2 | 3 | 12 (4,4,4) | — | — | rr0p0- | 2 | 296 |
| 61 | OUT (C), H | 2 | 3 | 12 (4,4,4) | — | — | — | 2 | 307 |
| 62 | SBC HL, HL | 2 | 4 | 15 (4,4,4,3) | — | — | rrcv1c | 2 | 192 |
| 63 | LD (nn), HL | 4 | 6 | 20 (4,4,3,3,3,3) | — | — | — | 2 | 108 |
| 64 | *unassigned* | — | — | — | — | — | — | — | — |
| 65 | *unassigned* | — | — | — | — | — | — | — | — |
| 66 | *unassigned* | — | — | — | — | — | — | — | — |
| 67 | RRD | 2 | 5 | 18 (4,4,3,4,3) | — | — | rr0p0- | 2 | 240 |
| 68 | IN L, (C) | 2 | 3 | 12 (4,4,4) | — | — | rr0p0- | 2 | 296 |
| 69 | OUT (C), L | 2 | 3 | 12 (4,4,4) | — | — | — | 2 | 307 |
| 6A | ADC HL, HL | 2 | 4 | 15 (4,4,4,3) | — | — | rrcv0c | 2 | 190 |
| 6B | LD HL, (nn) | 4 | 6 | 20 (4,4,3,3,3,3) | — | — | — | 2 | 103 |
| 6C | *unassigned* | — | — | — | — | — | — | — | — |
| 6D | *unassigned* | — | — | — | — | — | — | — | — |
| 6E | *unassigned* | — | — | — | — | — | — | — | — |
| 6F | RLD | 2 | 5 | 18 (4,4,3,4,3) | — | — | rr0p0- | 2 | 238 |
| 70 | *unassigned* | — | — | — | — | — | — | — | — |
| 71 | *unassigned* | — | — | — | — | — | — | — | — |
| 72 | SBC HL, SP | 2 | 4 | 15 (4,4,4,3) | — | — | rrcv1c | 2 | 192 |
| 73 | LD (nn), SP | 4 | 6 | 20 (4,4,3,3,3,3) | — | — | — | 2 | 108 |
| 74 | *unassigned* | — | — | — | — | — | — | — | — |
| 75 | *unassigned* | — | — | — | — | — | — | — | — |
| 76 | *unassigned* | — | — | — | — | — | — | — | — |
| 77 | *unassigned* | — | — | — | — | — | — | — | — |
| 78 | IN A, (C) | 2 | 3 | 12 (4,4,4) | — | — | rr0p0- | 2 | 296 |
| 79 | OUT (C), A | 2 | 3 | 12 (4,4,4) | — | — | — | 2 | 307 |
| 7A | ADC HL, SP | 2 | 4 | 15 (4,4,4,3) | — | — | rrcv0c | 2 | 190 |
| 7B | LD SP, (nn) | 4 | 6 | 20 (4,4,3,3,3,3) | — | — | — | 2 | 103 |
| 7C | *unassigned* | — | — | — | — | — | — | — | — |
| 7D | *unassigned* | — | — | — | — | — | — | — | — |
| 7E | *unassigned* | — | — | — | — | — | — | — | — |
| 7F | *unassigned* | — | — | — | — | — | — | — | — |
| A0 | LDI | 2 | 4 | 16 (4,4,3,5) | — | — | --0b0- | 2 | 130 |
| A1 | CPI | 2 | 4 | 16 (4,4,3,5) | — | — | rrcb1- | 2 | 138 |
| A2 | INI | 2 | 4 | 16 (4,5,4,3) | — | — | xrxx1- [n2] | 2 | 298 |
| A3 | OUTI | 2 | 4 | 16 (4,5,3,4) | — | — | xrxx1- [n2] | 2 | 309 |
| A4 | *unassigned* | — | — | — | — | — | — | — | — |
| A5 | *unassigned* | — | — | — | — | — | — | — | — |
| A6 | *unassigned* | — | — | — | — | — | — | — | — |
| A7 | *unassigned* | — | — | — | — | — | — | — | — |
| A8 | LDD | 2 | 4 | 16 (4,4,3,5) | — | — | --0b0- | 2 | 134 |
| A9 | CPD | 2 | 4 | 16 (4,4,3,5) | — | — | rrcb1- | 2 | 141 |
| AA | IND | 2 | 4 | 16 (4,5,4,3) | — | — | xrxx1- [n2] | 2 | 302 |
| AB | OUTD | 2 | 4 | 16 (4,5,3,4) | — | — | xrxx1- [n2] | 2 | 313 |
| AC | *unassigned* | — | — | — | — | — | — | — | — |
| AD | *unassigned* | — | — | — | — | — | — | — | — |
| AE | *unassigned* | — | — | — | — | — | — | — | — |
| AF | *unassigned* | — | — | — | — | — | — | — | — |
| B0 | LDIR | 2 | 5 | 21 (4,4,3,5,5) | 4 | 16 (4,4,3,5) | --0b0- | 2 | 132 |
| B1 | CPIR | 2 | 5 | 21 (4,4,3,5,5) | 4 | 16 (4,4,3,5) | rrcb1- [n5] | 2 | 139 |
| B2 | INIR | 2 | 5 | 21 (4,5,4,3,5) | 4 | 16 (4,5,4,3) | x1xx1- [n2] | 2 | 300 |
| B3 | OTIR | 2 | 5 | 21 (4,5,3,4,5) | 4 | 16 (4,5,3,4) | x1xx1- [n2] | 2 | 311 |
| B4 | *unassigned* | — | — | — | — | — | — | — | — |
| B5 | *unassigned* | — | — | — | — | — | — | — | — |
| B6 | *unassigned* | — | — | — | — | — | — | — | — |
| B7 | *unassigned* | — | — | — | — | — | — | — | — |
| B8 | LDDR | 2 | 5 | 21 (4,4,3,5,5) | 4 | 16 (4,4,3,5) | --0b0- [R5] | 2 | 136 |
| B9 | CPDR | 2 | 5 | 21 (4,4,3,5,5) | 4 | 16 (4,4,3,5) | rrcb1- [n5] | 2 | 142 |
| BA | INDR | 2 | 5 | 21 (4,5,4,3,5) | 4 | 16 (4,5,4,3) | x1xx1- [n2] | 2 | 304 |
| BB | OTDR | 2 | 5 | 21 (4,5,3,4,5) | 4 | 16 (4,5,3,4) | x1xx1- [n2] | 2 | 315 |
| BC | *unassigned* | — | — | — | — | — | — | — | — |
| BD | *unassigned* | — | — | — | — | — | — | — | — |
| BE | *unassigned* | — | — | — | — | — | — | — | — |
| BF | *unassigned* | — | — | — | — | — | — | — | — |

Notes to the `ED` table:
- **[n1]** `LD A, I` / `LD A, R`: if an interrupt occurs during execution, P/V is
  0 — [UM] p.94/95.
- **[n2]** Block I/O: Z = 1 if B − 1 = 0 (in the repeating forms Z = 1 at the
  end); S, H, P/V are declared "unknown" by the manual — [UM] p.298 ff.
  Order of the cycles of INI/IND (and their repeating forms): [UM] lists the
  breakdown as (4,5,3,4), but the port is read **before** the memory write and the
  I/O cycle always lasts 3 + 1 T (`02-bus-cycles.md` §5), so 4,5,4,3 (M1, long M1,
  I/O read, memory write); OUTI/OUTD stay 4,5,3,4 (memory read, then I/O write).
  Port address on the bus: INI/IND with B **before** the decrement, OUTI/OUTD with
  B **after** ("placed on the top half (A8 through A15) of the address bus at this time" p.298;
  "after the byte counter (B) is decremented" p.309).
- **[n3]** `NEG`: P/V = 1 if A was 80h; C = 1 if A was not 00h — [UM] p.176.
- **[n5]** `CPIR`/`CPDR`: repeat (main columns) if BC ≠ 0 **and** A ≠ (HL); end
  ((f) columns) if BC = 0 **or** A = (HL) — [UM] p.139/142.

**`ED` positions not listed above, unassigned in the manual** (explicit
declaration): `00–3F` (64), `80–9F` (32), `C0–FF` (64) — 160 positions; plus the
38 declared row by row in the table (`4C, 4E, 54–55, 5C–5D, 64–66, 6C–6E, 70–71,
74–77, 7C–7F` = 22 in the block 40–7F; `A4–A7, AC–AF, B4–B7, BC–BF` = 16 in the
block A0–BF).

**Count 2.3**: assigned 58, unassigned 198 (160 + 22 + 16) — total 256. What the
unassigned positions do: §5.3 [SY].

### 2.4 Indexed IX space (prefix `DD`) — 256 positions

The manual documents 40 positions: 39 instructions + the sub-prefix `CB`. The
table lists the assigned ones only; all the others are declared unassigned right
after it, with the exact ranges. What the manual says about `DD` in front of an
instruction that does not use HL: **nothing** — see §3.4.

| Op | Mnemonic | Bytes | M | T-states | M (f) | T-states (f) | Flags | R | Page |
|---|---|---|---|---|---|---|---|---|---|
| 09 | ADD IX, BC | 2 | 4 | 15 (4,4,4,3) | — | — | --c-0c | 2 | 194 |
| 19 | ADD IX, DE | 2 | 4 | 15 (4,4,4,3) | — | — | --c-0c | 2 | 194 |
| 21 | LD IX, nn | 4 | 4 | 14 (4,4,3,3) | — | — | — | 2 | 100 |
| 22 | LD (nn), IX | 4 | 6 | 20 (4,4,3,3,3,3) | — | — | — | 2 | 110 |
| 23 | INC IX | 2 | 2 | 10 (4,6) | — | — | — | 2 | 199 |
| 29 | ADD IX, IX | 2 | 4 | 15 (4,4,4,3) | — | — | --c-0c | 2 | 194 |
| 2A | LD IX, (nn) | 4 | 6 | 20 (4,4,3,3,3,3) | — | — | — | 2 | 105 |
| 2B | DEC IX | 2 | 2 | 10 (4,6) | — | — | — | 2 | 202 |
| 34 | INC (IX+d) | 3 | 6 | 23 (4,4,3,5,4,3) | — | — | rrcv0- | 2 | 168 |
| 35 | DEC (IX+d) | 3 | 6 | 23 (4,4,3,5,4,3) | — | — | rrcv1- | 2 | 170 |
| 36 | LD (IX+d), n | 4 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 86 |
| 39 | ADD IX, SP | 2 | 4 | 15 (4,4,4,3) | — | — | --c-0c | 2 | 194 |
| 46 | LD B, (IX+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 75 |
| 4E | LD C, (IX+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 75 |
| 56 | LD D, (IX+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 75 |
| 5E | LD E, (IX+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 75 |
| 66 | LD H, (IX+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 75 |
| 6E | LD L, (IX+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 75 |
| 70 | LD (IX+d), B | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 81 |
| 71 | LD (IX+d), C | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 81 |
| 72 | LD (IX+d), D | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 81 |
| 73 | LD (IX+d), E | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 81 |
| 74 | LD (IX+d), H | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 81 |
| 75 | LD (IX+d), L | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 81 |
| 77 | LD (IX+d), A | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 81 |
| 7E | LD A, (IX+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 75 |
| 86 | ADD A, (IX+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | rrcv0c | 2 | 149 |
| 8E | ADC A, (IX+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | rrcv0c | 2 | 151 |
| 96 | SUB (IX+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | rrcv1c | 2 | 153 |
| 9E | SBC A, (IX+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | rrcv1c [R4] | 2 | 155 |
| A6 | AND (IX+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | rr1p00 [R4] | 2 | 157 |
| AE | XOR (IX+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | rr0p00 | 2 | 161 |
| B6 | OR (IX+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | rr0p00 [R4] | 2 | 159 |
| BE | CP (IX+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | rrcv1c | 2 | 163 |
| CB | **sub-prefix** → indexed bit space (§2.6) | — | — | — | — | — | — | — | 217 ff. |
| E1 | POP IX | 2 | 4 | 14 (4,4,3,3) | — | — | — | 2 | 121 |
| E3 | EX (SP), IX | 2 | 6 | 23 (4,4,3,4,3,5) | — | — | — | 2 | 128 |
| E5 | PUSH IX | 2 | 4 | 15 (4,5,3,3) | — | — | — | 2 | 117 |
| E9 | JP (IX) | 2 | 2 | 8 (4,4) | — | — | — | 2 | 276 |
| F9 | LD SP, IX | 2 | 2 | 10 (4,6) | — | — | — | 2 | 113 |

**`DD` positions unassigned in the manual** (explicit declaration, 216):
`00–08, 0A–18, 1A–20, 24–28, 2C–33, 37–38, 3A–45, 47–4D, 4F–55, 57–5D,
5F–65, 67–6D, 6F, 76, 78–7D, 7F–85, 87–8D, 8F–95, 97–9D, 9F–A5, A7–AD,
AF–B5, B7–BD, BF–CA, CC–E0, E2, E4, E6–E8, EA–F8, FA–FF`.

Note: `DD 76` is not assigned — the pattern that is HALT in the base space has no
documented indexed variant (no page of the manual gives it). Behaviour of the
unassigned positions: §5.2 [SY].

**Count 2.4**: assigned 40, unassigned 216 — total 256.

### 2.5 Indexed IY space (prefix `FD`) — 256 positions

The mirror of `DD` with IY instead of IX; every row cites its own page (the manual
documents the IY forms in separate descriptions).

| Op | Mnemonic | Bytes | M | T-states | M (f) | T-states (f) | Flags | R | Page |
|---|---|---|---|---|---|---|---|---|---|
| 09 | ADD IY, BC | 2 | 4 | 15 (4,4,4,3) | — | — | --c-0c | 2 | 196 |
| 19 | ADD IY, DE | 2 | 4 | 15 (4,4,4,3) | — | — | --c-0c | 2 | 196 |
| 21 | LD IY, nn | 4 | 4 | 14 (4,4,3,3) | — | — | — | 2 | 101 |
| 22 | LD (nn), IY | 4 | 6 | 20 (4,4,3,3,3,3) | — | — | — | 2 | 111 |
| 23 | INC IY | 2 | 2 | 10 (4,6) | — | — | — | 2 | 200 |
| 29 | ADD IY, IY | 2 | 4 | 15 (4,4,4,3) | — | — | --c-0c | 2 | 196 |
| 2A | LD IY, (nn) | 4 | 6 | 20 (4,4,3,3,3,3) | — | — | — | 2 | 106 |
| 2B | DEC IY | 2 | 2 | 10 (4,6) | — | — | — | 2 | 203 |
| 34 | INC (IY+d) | 3 | 6 | 23 (4,4,3,5,4,3) | — | — | rrcv0- | 2 | 169 |
| 35 | DEC (IY+d) | 3 | 6 | 23 (4,4,3,5,4,3) | — | — | rrcv1- | 2 | 170 |
| 36 | LD (IY+d), n | 4 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 87 |
| 39 | ADD IY, SP | 2 | 4 | 15 (4,4,4,3) | — | — | --c-0c | 2 | 196 |
| 46 | LD B, (IY+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 77 |
| 4E | LD C, (IY+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 77 |
| 56 | LD D, (IY+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 77 |
| 5E | LD E, (IY+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 77 |
| 66 | LD H, (IY+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 77 |
| 6E | LD L, (IY+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 77 |
| 70 | LD (IY+d), B | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 83 |
| 71 | LD (IY+d), C | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 83 |
| 72 | LD (IY+d), D | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 83 |
| 73 | LD (IY+d), E | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 83 |
| 74 | LD (IY+d), H | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 83 |
| 75 | LD (IY+d), L | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 83 |
| 77 | LD (IY+d), A | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 83 |
| 7E | LD A, (IY+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | — | 2 | 77 |
| 86 | ADD A, (IY+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | rrcv0c | 2 | 150 |
| 8E | ADC A, (IY+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | rrcv0c | 2 | 151 |
| 96 | SUB (IY+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | rrcv1c | 2 | 153 |
| 9E | SBC A, (IY+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | rrcv1c [R4] | 2 | 155 |
| A6 | AND (IY+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | rr1p00 [R4] | 2 | 157 |
| AE | XOR (IY+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | rr0p00 | 2 | 161 |
| B6 | OR (IY+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | rr0p00 [R4] | 2 | 159 |
| BE | CP (IY+d) | 3 | 5 | 19 (4,4,3,5,3) | — | — | rrcv1c | 2 | 163 |
| CB | **sub-prefix** → indexed bit space (§2.6) | — | — | — | — | — | — | — | 219 ff. |
| E1 | POP IY | 2 | 4 | 14 (4,4,3,3) | — | — | — | 2 | 122 |
| E3 | EX (SP), IY | 2 | 6 | 23 (4,4,3,4,3,5) | — | — | — | 2 | 129 |
| E5 | PUSH IY | 2 | 4 | 15 (4,5,3,3) | — | — | — | 2 | 118 |
| E9 | JP (IY) | 2 | 2 | 8 (4,4) | — | — | — | 2 | 277 |
| F9 | LD SP, IY | 2 | 2 | 10 (4,6) | — | — | — | 2 | 114 |

**`FD` positions unassigned in the manual** (explicit declaration, 216): the same
ranges as the `DD` space: `00–08, 0A–18, 1A–20, 24–28, 2C–33, 37–38, 3A–45,
47–4D, 4F–55, 57–5D, 5F–65, 67–6D, 6F, 76, 78–7D, 7F–85, 87–8D, 8F–95, 97–9D,
9F–A5, A7–AD, AF–B5, B7–BD, BF–CA, CC–E0, E2, E4, E6–E8, EA–F8, FA–FF`.

**Count 2.5**: assigned 40, unassigned 216 — total 256.

### 2.6 Indexed bit space (`DD CB` / `FD CB`) — 256 positions

Sequence in memory: `DD (or FD), CB, d, final opcode` — the displacement comes
**before** the final opcode; the table is indexed by the **final opcode** (fourth
byte). The manual shows the encoding explicitly for every instruction, e.g.
`RLC (IX+d)` = `DD CB d 06` — [UM] p.217. Total bytes = 4 for all.

One table for both prefixes: mnemonic and timing are identical, only the index
register and the page change (last two columns).

| Op | Mnemonic | Bytes | M | T-states | M (f) | T-states (f) | Flags | R | Page `DD CB` | Page `FD CB` |
|---|---|---|---|---|---|---|---|---|---|---|
| 06 | RLC (IX+d) / RLC (IY+d) | 4 | 6 | 23 (4,4,3,5,4,3) | — | — | rr0p0c | 2 | 217 | 219 |
| 0E | RRC (IX+d) / RRC (IY+d) | 4 | 6 | 23 (4,4,3,5,4,3) | — | — | rr0p0c | 2 | 224 | 224 |
| 16 | RL (IX+d) / RL (IY+d) | 4 | 6 | 23 (4,4,3,5,4,3) | — | — | rr0p0c | 2 | 221 | 221 |
| 1E | RR (IX+d) / RR (IY+d) | 4 | 6 | 23 (4,4,3,5,4,3) | — | — | rr0p0c | 2 | 227 | 227 |
| 26 | SLA (IX+d) / SLA (IY+d) | 4 | 6 | 23 (4,4,3,5,4,3) | — | — | rr0p0c | 2 | 230 | 230 |
| 2E | SRA (IX+d) / SRA (IY+d) | 4 | 6 | 23 (4,4,3,5,4,3) | — | — | rr0p0c | 2 | 233 | 233 |
| 3E | SRL (IX+d) / SRL (IY+d) | 4 | 6 | 23 (4,4,3,5,4,3) | — | — | 0r0p0c | 2 | 236 | 236 |
| 46 | BIT 0, (IX+d) / BIT 0, (IY+d) | 4 | 5 | 20 (4,4,3,5,4) | — | — | xz1x0- [R6, FD CB page] | 2 | 247 | 249 |
| 4E | BIT 1, (IX+d) / BIT 1, (IY+d) | 4 | 5 | 20 (4,4,3,5,4) | — | — | xz1x0- [R6, FD CB page] | 2 | 247 | 249 |
| 56 | BIT 2, (IX+d) / BIT 2, (IY+d) | 4 | 5 | 20 (4,4,3,5,4) | — | — | xz1x0- [R6, FD CB page] | 2 | 247 | 249 |
| 5E | BIT 3, (IX+d) / BIT 3, (IY+d) | 4 | 5 | 20 (4,4,3,5,4) | — | — | xz1x0- [R6, FD CB page] | 2 | 247 | 249 |
| 66 | BIT 4, (IX+d) / BIT 4, (IY+d) | 4 | 5 | 20 (4,4,3,5,4) | — | — | xz1x0- [R6, FD CB page] | 2 | 247 | 249 |
| 6E | BIT 5, (IX+d) / BIT 5, (IY+d) | 4 | 5 | 20 (4,4,3,5,4) | — | — | xz1x0- [R6, FD CB page] | 2 | 247 | 249 |
| 76 | BIT 6, (IX+d) / BIT 6, (IY+d) | 4 | 5 | 20 (4,4,3,5,4) | — | — | xz1x0- [R6, FD CB page] | 2 | 247 | 249 |
| 7E | BIT 7, (IX+d) / BIT 7, (IY+d) | 4 | 5 | 20 (4,4,3,5,4) | — | — | xz1x0- [R6, FD CB page] | 2 | 247 | 249 |
| 86 | RES 0, (IX+d) / RES 0, (IY+d) | 4 | 6 | 23 (4,4,3,5,4,3) | — | — | — | 2 | 259 | 259 |
| 8E | RES 1, (IX+d) / RES 1, (IY+d) | 4 | 6 | 23 (4,4,3,5,4,3) | — | — | — | 2 | 259 | 259 |
| 96 | RES 2, (IX+d) / RES 2, (IY+d) | 4 | 6 | 23 (4,4,3,5,4,3) | — | — | — | 2 | 259 | 259 |
| 9E | RES 3, (IX+d) / RES 3, (IY+d) | 4 | 6 | 23 (4,4,3,5,4,3) | — | — | — | 2 | 259 | 259 |
| A6 | RES 4, (IX+d) / RES 4, (IY+d) | 4 | 6 | 23 (4,4,3,5,4,3) | — | — | — | 2 | 259 | 259 |
| AE | RES 5, (IX+d) / RES 5, (IY+d) | 4 | 6 | 23 (4,4,3,5,4,3) | — | — | — | 2 | 259 | 259 |
| B6 | RES 6, (IX+d) / RES 6, (IY+d) | 4 | 6 | 23 (4,4,3,5,4,3) | — | — | — | 2 | 259 | 259 |
| BE | RES 7, (IX+d) / RES 7, (IY+d) | 4 | 6 | 23 (4,4,3,5,4,3) | — | — | — | 2 | 259 | 259 |
| C6 | SET 0, (IX+d) / SET 0, (IY+d) | 4 | 6 | 23 (4,4,3,5,4,3) | — | — | — | 2 | 255 | 257 |
| CE | SET 1, (IX+d) / SET 1, (IY+d) | 4 | 6 | 23 (4,4,3,5,4,3) | — | — | — | 2 | 255 | 257 |
| D6 | SET 2, (IX+d) / SET 2, (IY+d) | 4 | 6 | 23 (4,4,3,5,4,3) | — | — | — | 2 | 255 | 257 |
| DE | SET 3, (IX+d) / SET 3, (IY+d) | 4 | 6 | 23 (4,4,3,5,4,3) | — | — | — | 2 | 255 | 257 |
| E6 | SET 4, (IX+d) / SET 4, (IY+d) | 4 | 6 | 23 (4,4,3,5,4,3) | — | — | — | 2 | 255 | 257 |
| EE | SET 5, (IX+d) / SET 5, (IY+d) | 4 | 6 | 23 (4,4,3,5,4,3) | — | — | — | 2 | 255 | 257 |
| F6 | SET 6, (IX+d) / SET 6, (IY+d) | 4 | 6 | 23 (4,4,3,5,4,3) | — | — | — | 2 | 255 | 257 |
| FE | SET 7, (IX+d) / SET 7, (IY+d) | 4 | 6 | 23 (4,4,3,5,4,3) | — | — | — | 2 | 255 | 257 |

**`DD CB` / `FD CB` positions unassigned in the manual** (explicit declaration,
225): every position whose final opcode has its 3 low bits ≠ 110, plus `36` (SLL
slot): `00–05, 07–0D, 0F–15, 17–1D, 1F–25, 27–2D, 2F–3D, 3F–45, 47–4D, 4F–55,
57–5D, 5F–65, 67–6D, 6F–75, 77–7D, 7F–85, 87–8D, 8F–95, 97–9D, 9F–A5, A7–AD,
AF–B5, B7–BD, BF–C5, C7–CD, CF–D5, D7–DD, DF–E5, E7–ED, EF–F5, F7–FD, FF`.
Behaviour (double effect): §5.4 [SY].

**Count 2.6**: assigned 31, unassigned 225 — total 256.

## 3. Prefixes: the rules

### 3.1 Byte order in `DD CB` / `FD CB` — [UM]

Sequence in memory: **prefix, `CB`, displacement `d`, final opcode**. The manual
shows it explicitly in every encoding of the group: `RLC (IX+d)` = `DD CB d 06`
([UM] p.217), `BIT b, (IX+d)` = `DD CB d 01bbb110` ([UM] p.247),
`SET b, (IY+d)` = `FD CB d 11bbb110` ([UM] p.257).

It is the **exception** to the general rule "the displacement always follows
directly after the op code" ([UM] p.45): here `d` sits in the middle, before the final opcode. The
fourth byte is read **after** `d`; in rmZ80 it is loaded into `IR` by a memory
read, not by an M1 (`06-sequencer.md` §2.2).

### 3.2 Register R across prefixes

The manual fixes only the base rule: R is incremented (the low 7 bits, bit 7
preserved) after **every instruction fetch**, and the refresh places I and R on the
address bus ([UM] p.3). It does not say how many increments R gets with prefixes.

[SY] §6.1:
- every M1 cycle increments R; prefixes have their own M1;
- instruction without a prefix: R += 1;
- prefix `CB`, `ED`, `DD`, `FD`: R += 2 — and `DD CB` / `FD CB` also increment by 2
  (not 3);
- a `DD`/`FD` that does not form an instruction (§3.3): R += 1;
- `LD A, R` / `LD R, A` see R **already incremented** by the instruction itself;
- repeating block instructions run the instruction again (PC −= 2): R += 2 per
  iteration;
- accepting an interrupt (INT or NMI): R += 1;
- after reset R = 0.

rmZ80 follows these rules; SingleStepTests compare `R` after every case and
agree.

### 3.3 Prefix followed by a prefix

The manual does not cover the case. [SY] §3.7:
- `CB` and `ED` **always** form an instruction with the byte that follows;
- `DD` and `FD` are not instructions but a **flag**: "use IX instead of HL" (or IY)
  for the instruction that follows. In a sequence of consecutive `DD`/`FD` **the
  last one counts**; every other byte consumes or resets the flag. Example [SY]:
  `FD DD 00 21 00 10` = NOP, NOP, NOP, `LD HL, 1000h`.

In rmZ80 a `DD`/`FD` that follows a `DD`/`FD` sets the new prefix space and the
count of M-cycles starts again; `CB` after `DD`/`FD` selects the indexed bit space.

### 3.4 `DD`/`FD` in front of instructions that do not use HL

**The manual says nothing**: it documents only the forms listed in §2.4/§2.5 and
does not define the effect of the prefix on the other positions.

[SY] §3.2/3.3:
- the instruction that follows is executed **as is**, but every use of HL becomes
  IX (or IY), every `(HL)` becomes `(IX+d)` with `d` read after the opcode;
- exceptions: `EX DE, HL` and `EXX` are **not** affected; `ED` instructions that use
  HL are not affected; `JP (HL)` becomes `JP (IX)` without a displacement (it is not
  a memory access);
- every use of H becomes IXh, every use of L becomes IXl, **except** when the same
  instruction has `(IX+d)` (see §5.2);
- never IX and IY in the same instruction.

## 4. What the decoder has to do — summary

1. Byte ≠ `CB`/`DD`/`ED`/`FD` → base space (§2.1), fields of §1.
2. `CB` → second byte in the bit space (§2.2).
3. `ED` → second byte in the extended space (§2.3); outside the 58 assigned
   positions: §5.3.
4. `DD`/`FD` → IX/IY flag; next byte: if `CB` → read `d`, then the final opcode in
   the indexed bit space (§2.6); otherwise → §2.4/§2.5, with the rules of §3.3/§3.4.
5. R: increments as in §3.2.

---

## 5. Undocumented behaviour — [SY]

A separate section on purpose: **nothing below comes from the manual**. Source:
Sean Young, *The Undocumented Z80 Documented* v0.91. Where SingleStepTests or
z80test cover these positions, they agree with what is implemented (`STATUS.md`);
176 of the 178 `ED` positions that act as two NOPs (all but `ED 77` and `ED 7F`) are
covered by neither.

### 5.1 SLL — the `CB 30–37` slot [SY §3.1]

Shift Logical Left: like SLA but bit 0 is **set** (SLA clears it).

| Opcode | Mnemonic |
|---|---|
| CB 30 | SLL B |
| CB 31 | SLL C |
| CB 32 | SLL D |
| CB 33 | SLL E |
| CB 34 | SLL H |
| CB 35 | SLL L |
| CB 36 | SLL (HL) |
| CB 37 | SLL A |

Likewise `DD CB d 36` / `FD CB d 36` = SLL (IX+d) / (IY+d) (slot `36` of the space
of §2.6), with the double-effect rule of §5.4.

**Timing** — [SY] §8.7 p.32 (columns Bytes/Cycles/States; no breakdown per
M-cycle): `SLL r` 2 bytes, 2 M, 8 T; `SLL (HL)` 2/4/15; `SLL (IX+d)` and
`SLL (IY+d)` 4/6/23 — identical to the documented SLA/SRL families. In Appendix A
with `NOTE` = `SY`.

### 5.2 `DD`/`FD` on the halves of IX/IY: IXh, IXl, IYh, IYl [SY §3.2–3.3]

In the `DD`/`FD` positions the manual does not assign, every access to **H**
becomes **IXh** (high byte of IX) and every access to **L** becomes **IXl** —
except when the instruction also uses `(IX+d)`; with `FD`, IYh/IYl. Examples [SY]:

| Without prefix | With `DD` |
|---|---|
| LD H, (HL) | LD H, (IX+d) — H stays H: there is (IX+d) |
| LD H, A | LD IXh, A |
| LD L, H | LD IXl, IXh |
| JP (HL) | JP (IX) |
| LD HL, 0 | LD IX, 0 |
| LD DE, 0 | LD DE, 0 — unchanged: does not use HL |

Never IX and IY in the same instruction (`LD IXl, IYh` does not exist).

**Timing** — [SY] chapter 8 tabulates the forms on the halves with explicit
Bytes/Cycles/States (§8.1 p.26, §8.4 p.30; no breakdown per M-cycle):

| Form | Bytes | M | T |
|---|---|---|---|
| `LD p,p'` / `LD q,q'` (49 positions per prefix) | 2 | 2 | 8 |
| `LD p,n` / `LD q,n` (7) | 3 | 3 | 11 |
| ALU `A,p` / `A,q` — ADD ADC SUB SBC AND XOR OR CP (56) | 2 | 2 | 8 |
| `INC p` / `DEC p` (14) | 2 | 2 | 8 |

(p = B C D E IXh IXl A; q = B C D E IYh IYl A.) These are the **126 positions per
prefix** added to Appendix A with `NOTE` = `SY`.

**The remaining 90 positions per prefix (180 in total)**: those with no explicit
row in [SY] chapter 8 — instructions outside the `LD`/ALU/`INC`/`DEC` forms above
that do not use HL, H or L, the exceptions of §3.4 (`EX DE,HL`, `EXX`), and the
prefix bytes `DD`, `ED`, `FD` (e.g. `DD 00`, `DD 3F`, `DD C3`, `DD 76`). [SY] §3.2 gives their **semantics** ("executed
as is") but chapter 8 has no explicit row with the timing. Every explicit row of
[SY] for a pair with and without the prefix is "documented + 4 T" (the M1 of the
prefix); for these 90 the timing is a **deduction**, not a datum. In Appendix A they
stay `UNASSIGNED`; rmZ80 runs them as the prefix M1 (4 T) followed by the base
instruction (`06-sequencer.md` §4.2), and SingleStepTests agree.

### 5.3 The unassigned `ED` positions [SY §3.4]

General rule [SY]: every `ED` position **not listed** below and not assigned in
§2.3 has no effect — it behaves like **two NOPs**.

Duplicates and special cases (opcode after `ED`):

| Opcode | Effect [SY] |
|---|---|
| 4C, 54, 5C, 64, 6C, 74, 7C | NEG |
| 55, 5D, 65, 6D, 75, 7D | RETN |
| 4E, 66, 6E | IM 0 |
| 76 | IM 1 |
| 7E | IM 2 |
| 77, 7F | no effect (NOP) |
| 70 | IN (C) — reads port C, **does not store** the result, sets the flags like the other IN r,(C); also written IN F,(C) |
| 71 | OUT (C), 0 — writes the value 0 to port C |

Notes [SY]:
- **every** RETI/RETN (the documented RETI `ED 4D` included) behaves like RETN:
  it copies IFF2 into IFF1 — `04-interrupts.md` §6;
- `ED 4E` / `ED 6E` observed equivalent to IM 0 (test reported by Gerton Lunter on
  a Spectrum, cited in [SY]).

**Timing** — in Appendix A with `NOTE` = `SY`, on these grounds:
- `IN F,(C)` (70) and `OUT (C),0` (71): explicit rows of [SY] §8.11 p.35, 2 bytes,
  3 M, 12 T like the documented IN/OUT. For `OUT (C),0` the **value written depends
  on the silicon** (NMOS 00h, CMOS FFh): parameter `VARIANT_OUT_C0_FF` of `rmZ80`;
- IM duplicates (4E, 66, 6E, 76, 7E) and RETN duplicates (55, 5D, 65, 6D, 75, 7D):
  explicit note of [SY] §8.5/§8.10 ("this instruction has other undocumented
  opcodes") → timing of the instruction: 2/2/8 and 2/4/14;
- NEG duplicates (7): same mnemonic stated by [SY] §3.4 → 2/2/8 from the NEG row of
  §8.5 p.30;
- all the others (178, `ED-NOP` in the appendix): "same behaviour as **2 NOP
  instructions**" ([SY] §3.4 p.12) → 2 bytes, 2 M, 8 T. Here the timing is part of
  the equivalence the source states, not a tabulated number.

### 5.4 `DD CB` / `FD CB`: the double effect [SY §3.5–3.6]

In the positions whose 3 low bits are ≠ 110, the rotate/shift/SET/RES operation is
carried out on `(IX+d)` **and** the result is also copied into the register
selected by the 3 low bits of the final opcode:

| Bits 2–0 | Register |
|---|---|
| 000 | B |
| 001 | C |
| 010 | D |
| 011 | E |
| 100 | H |
| 101 | L |
| 110 | (none: documented form) |
| 111 | A |

Example [SY]: `DD CB 01 00` = `RLC (IX+01h), B`, equivalent to
`LD B,(IX+01h); RLC B; LD (IX+01h),B`. The register receives the rotated value even
if `(IX+d)` points to ROM (the memory write has no effect, the copy into the
register does).

**BIT** writes nothing: the 7 undocumented variants of each `BIT b, (IX+d)` cannot
be told apart from the documented one (same test, same flags). With `FD CB`
everything is identical on IY.

**Timing** — [SY] §8.7 p.32 tabulates `RLC (IX+d),r` = 4 bytes, 6 M, 23 T
explicitly (the other rotates/shifts by substitution of the field, as noted in the
table); §8.8 p.33: `SET b,(IX+d),r` = 4/6/23, `RES` by substitution, and the
undocumented BIT = 4/5/20 like the documented ones (note "has other undocumented
opcodes"). Identical to the documented forms. All in Appendix A with `NOTE` = `SY`
(450 positions over the two sub-prefixes).

---

## 6. Self-checks of the tables

The six tables were expanded from the patterns of the manual and checked
mechanically position by position (sum, uniqueness, page present).

### 6.1 Coverage and collisions

| Space | Assigned | Unassigned | Total | Collisions |
|---|---|---|---|---|
| Base | 256 | 0 | 256 | 0 |
| CB | 248 | 8 | 256 | 0 |
| ED | 58 | 198 | 256 | 0 |
| DD | 40 | 216 | 256 | 0 |
| FD | 40 | 216 | 256 | 0 |
| DD CB / FD CB | 31 | 225 | 256 | 0 |

No opcode appears twice in the same space (uniqueness check over all 673 assigned
rows).

The table above counts the assignments **of the manual**. In Appendix A, in a
second pass, 908 of the unassigned positions received Bytes/M/T from [SY]
(`NOTE` = `SY`, see §5 and the legend of the appendix): CB 8, ED 198, DD 126,
FD 126, DDCB 225, FDCB 225. Reliability check made before adopting them: on the
**documented** forms, the Bytes/Cycles/States columns of chapter 8 of [SY] agree with
[UM] on **every** row compared (0 disagreements) — and independently confirm the
corrections [R1] (`LD dd,nn`: [SY] prints M = 3) and [R2] (`RES b,r`: M = 2). 180 rows
stay `UNASSIGNED` (90 + 90 `DD`/`FD`, §5.2).

### 6.2 Misprints inside the manual (printed value ≠ consistent value)

The cells of the tables hold the value consistent with the T-state breakdown and
with the twin pages; here is the printed one:

| Ref. | Page | What the manual prints | Value used and why |
|---|---|---|---|
| [R1] | p.99 | `LD dd, nn`: M = "2" with T = 10 (4,3,3) | M = 3: the breakdown has 3 cycles; twins at p.119 (POP, M = 3, 10 (4,3,3)) and p.262 (JP nn) |
| [R2] | p.260 | `RES b, r`: M = "4" with T = 8 (4,4) | M = 2: breakdown of 2 cycles; twin SET b, r p.251 (M = 2) |
| [R3] | p.269 | `JR NC` not taken: M = "7" with T = 7 (4,3) | M = 2: breakdown of 2 cycles; twins JR C/Z/NZ p.267/271/273 (M = 2) |
| [R4] | p.156, 158, 160 | P/V: SBC "reset if overflow; otherwise reset"; AND likewise; OR "set if overflow" | p.67 fixes the rule: arithmetic → overflow (`v` for SBC), logic → even parity (`p` for AND/OR, like XOR p.162) |
| [R5] | p.137 | `LDDR`: flag list without the C row; "P/V is reset" | LDIR p.133: C unchanged, P/V = BC − 1 ≠ 0; general rule p.68 (P/V follows BC) |
| [R6] | p.246, 250 | `BIT b,(HL)` and `BIT b,(IY+d)`: the N row printed as a second "H is reset" | N = 0 from the twins p.244 (BIT b,r) and p.248 (BIT b,(IX+d)) |
| [R7] | p.284 | `CALL cc`: condition 011 "Carry (C)" paired with flag "Z" | flag C, as p.263 (JP cc) and p.286 (RET cc) |

Minor misprints in the manual's "4 MHz E.T." columns (not reproduced in this
document) are not listed.

### 6.3 Sources of the cells

- Assigned rows without a page of the manual: **0 out of 673** (the only "ff."
  pages are the 4 prefix rows of the base table and the 2 `CB` sub-prefix rows of
  `DD`/`FD`, which point to the corresponding space).
- In the tables of §2 every cell comes from [UM] **except the R column in the
  prefixed spaces** (value 2), which comes from [SY] §6.1 because the manual does
  not fix the increments of R with prefixes (§0 and §3.2). The R column of the base
  table (value 1) follows [UM] p.3 (one increment per instruction fetch).
- From [SY] or from the silence of the manual: the R column of the prefixed
  spaces, §3.2 (R with prefixes), §3.3 (prefix chains), §3.4 (`DD`/`FD` on non-HL
  instructions), the whole of §5, and the behaviour of `ED 70`/`ED 71`.

### 6.4 Consistency of the lengths

Rule checked on every assigned row:
`Bytes = 1 (opcode) + 1 per prefix byte + 1 if there is d + 1 if there is n +
2 if there is nn`, with `DD CB` / `FD CB` = 2 prefixes + d + final opcode = 4.
Examples at the extremes: `LD B, C` = 1; `LD (IX+d), n` = DD + 36 + d + n = 4 ([UM]
p.86, sequence shown at pp.44–45); `LD IX, (nn)` = DD + 2A + n + n = 4 ([UM]
p.105); relative jumps = opcode + (e − 2) = 2 ([UM] p.265). No row breaks the rule.

### 6.5 Cross-check with `02-bus-cycles.md`

Building blocks fixed by document 02: M1 = 4 T (extendable to 5 or 6, [UM] p.7);
non-M1 M-cycle = 3 T (extendable to 4 or 5, [UM] p.7); I/O cycle = 4 T (3 + 1
automatic wait, [UM] p.10); interrupt acknowledge M1 = 6 T (4 + 2 automatic waits,
[UM] p.12).

Check made on **every** assigned row of **every** space, for **both** timings where
there are two (696 counts: 667 primary + 29 alternative):

1. **sum(breakdown) = total stated by the manual**, and M = number of elements of
   the breakdown: **0 rows fail**;
2. **compatibility with the building blocks**: the first k M-cycles of every
   breakdown are M1 fetches (k = 1 without a prefix, k = 2 with a prefix and also
   for `DD CB` / `FD CB`, where `d` and the final opcode are read as operands, not as
   M1 — consistent with R += 2 of §3.2) and must lie in [4..6]; the rest in [3..5]:
   **0 M-cycles out of range**.

Specific matches with the building blocks: every IN/OUT row contains exactly one
4-T M-cycle besides the fetches — the I/O cycle with its automatic wait (e.g.
`IN A, (n)` = 11 (4,3,4): fetch 4, read of n 3, I/O 4 — [UM] p.295;
`IN r, (C)` = 12 (4,4,4) — [UM] p.296). The 6-T acknowledge block appears in no row,
correctly: the interrupt acknowledge is not an instruction (`02-bus-cycles.md` §7).

The check did **not** "rescue" the three misprints already marked: the rows where
the printed M contradicts the breakdown are exactly those of [R1] (4 rows
`LD dd, nn`), [R2] (56 rows `RES b, r`), [R3] (1 row `JR NC` not taken) — **61
rows**, reported in §6.2 and not "adjusted": the cells hold the value the breakdown
imposes; the discrepancy is recorded here and in §6.2.

---

## Appendix A — rigid format

One row per position, **every** position of every space (7 blocks × 256 = 1792
rows: `DDCB` and `FDCB` are emitted separately because they differ in index
register and page). Fields separated by `;`, always in the same order, no free text:

```
SPACE;OP;MNEMONIC;BYTES;M;T;BREAKDOWN;M2;T2;BREAKDOWN2;FLAGS;R;NOTE;PAGE
```

| Field | Content |
|---|---|
| `SPACE` | `BASE` `CB` `ED` `DD` `FD` `DDCB` `FDCB` |
| `OP` | opcode within the space, 2 hex digits (for DDCB/FDCB: the final opcode, fourth byte) |
| `MNEMONIC` | as in the tables of §2; `UNASSIGNED` = position not assigned by the manual and not covered by an explicit row of [SY]; `PREFIX_xx` / `SUBPREFIX_CB` = prefix byte |
| `BYTES` | total length of the instruction |
| `M`;`T`;`BREAKDOWN` | primary timing (condition true / iteration that repeats); breakdown = T per M-cycle separated by `,` |
| `M2`;`T2`;`BREAKDOWN2` | alternative timing (condition false / last iteration); `-` = single timing |
| `FLAGS` | 6 symbols in the order S Z H P/V N C, legend in §0 (`------` = no flag changed) |
| `R` | increments of R per execution (§0; the value 2 of the prefixed spaces is [SY]) |
| `NOTE` | tags `R*`/`n*` separated by `,` (references to §6.2 and §2), `SY` (below), `-` = none |
| `PAGE` | page of the manual (for prefixes: first occurrence) |

Field not applicable = `-`. The `UNASSIGNED` positions have every data field at
`-`; their behaviour is in §5.2.

**Rows with `NOTE` = `SY`** (second pass, 908 rows): positions not assigned by the
manual whose Bytes/M/T come from the tables of chapter 8 of [SY] (columns
Bytes/Cycles/States) or from the equivalences stated in [SY] §3.4, as explained in
§5. For these rows **`PAGE` cites the page of [SY]**, not of the manual;
`BREAKDOWN` is `-` because [SY] gives no breakdown per M-cycle. `ED-NOP` = `ED`
position with no effect ("2 NOP", [SY] §3.4): 2 bytes, 2 M, 8 T. **180 rows** stay
`UNASSIGNED` (90 `DD` + 90 `FD`), those with no explicit row in [SY] — see §5.2.

```text
BASE;00;NOP;1;1;4;4;-;-;-;------;1;-;180
BASE;01;LD BC,nn;3;3;10;4,3,3;-;-;-;------;1;R1;99
BASE;02;LD (BC),A;1;2;7;4,3;-;-;-;------;1;-;91
BASE;03;INC BC;1;1;6;6;-;-;-;------;1;-;198
BASE;04;INC B;1;1;4;4;-;-;-;rrcv0-;1;-;165
BASE;05;DEC B;1;1;4;4;-;-;-;rrcv1-;1;-;170
BASE;06;LD B,n;2;2;7;4,3;-;-;-;------;1;-;72
BASE;07;RLCA;1;1;4;4;-;-;-;--0-0c;1;-;205
BASE;08;EX AF,AF';1;1;4;4;-;-;-;------;1;-;125
BASE;09;ADD HL,BC;1;3;11;4,4,3;-;-;-;--c-0c;1;-;188
BASE;0A;LD A,(BC);1;2;7;4,3;-;-;-;------;1;-;88
BASE;0B;DEC BC;1;1;6;6;-;-;-;------;1;-;201
BASE;0C;INC C;1;1;4;4;-;-;-;rrcv0-;1;-;165
BASE;0D;DEC C;1;1;4;4;-;-;-;rrcv1-;1;-;170
BASE;0E;LD C,n;2;2;7;4,3;-;-;-;------;1;-;72
BASE;0F;RRCA;1;1;4;4;-;-;-;--0-0c;1;-;209
BASE;10;DJNZ e;2;3;13;5,3,5;2;8;5,3;------;1;-;278
BASE;11;LD DE,nn;3;3;10;4,3,3;-;-;-;------;1;R1;99
BASE;12;LD (DE),A;1;2;7;4,3;-;-;-;------;1;-;92
BASE;13;INC DE;1;1;6;6;-;-;-;------;1;-;198
BASE;14;INC D;1;1;4;4;-;-;-;rrcv0-;1;-;165
BASE;15;DEC D;1;1;4;4;-;-;-;rrcv1-;1;-;170
BASE;16;LD D,n;2;2;7;4,3;-;-;-;------;1;-;72
BASE;17;RLA;1;1;4;4;-;-;-;--0-0c;1;-;207
BASE;18;JR e;2;3;12;4,3,5;-;-;-;------;1;-;265
BASE;19;ADD HL,DE;1;3;11;4,4,3;-;-;-;--c-0c;1;-;188
BASE;1A;LD A,(DE);1;2;7;4,3;-;-;-;------;1;-;89
BASE;1B;DEC DE;1;1;6;6;-;-;-;------;1;-;201
BASE;1C;INC E;1;1;4;4;-;-;-;rrcv0-;1;-;165
BASE;1D;DEC E;1;1;4;4;-;-;-;rrcv1-;1;-;170
BASE;1E;LD E,n;2;2;7;4,3;-;-;-;------;1;-;72
BASE;1F;RRA;1;1;4;4;-;-;-;--0-0c;1;-;211
BASE;20;JR NZ,e;2;3;12;4,3,5;2;7;4,3;------;1;-;273
BASE;21;LD HL,nn;3;3;10;4,3,3;-;-;-;------;1;R1;99
BASE;22;LD (nn),HL;3;5;16;4,3,3,3,3;-;-;-;------;1;-;107
BASE;23;INC HL;1;1;6;6;-;-;-;------;1;-;198
BASE;24;INC H;1;1;4;4;-;-;-;rrcv0-;1;-;165
BASE;25;DEC H;1;1;4;4;-;-;-;rrcv1-;1;-;170
BASE;26;LD H,n;2;2;7;4,3;-;-;-;------;1;-;72
BASE;27;DAA;1;1;4;4;-;-;-;rrdp-d;1;-;173
BASE;28;JR Z,e;2;3;12;4,3,5;2;7;4,3;------;1;-;271
BASE;29;ADD HL,HL;1;3;11;4,4,3;-;-;-;--c-0c;1;-;188
BASE;2A;LD HL,(nn);3;5;16;4,3,3,3,3;-;-;-;------;1;-;102
BASE;2B;DEC HL;1;1;6;6;-;-;-;------;1;-;201
BASE;2C;INC L;1;1;4;4;-;-;-;rrcv0-;1;-;165
BASE;2D;DEC L;1;1;4;4;-;-;-;rrcv1-;1;-;170
BASE;2E;LD L,n;2;2;7;4,3;-;-;-;------;1;-;72
BASE;2F;CPL;1;1;4;4;-;-;-;--1-1-;1;-;175
BASE;30;JR NC,e;2;3;12;4,3,5;2;7;4,3;------;1;R3;269
BASE;31;LD SP,nn;3;3;10;4,3,3;-;-;-;------;1;R1;99
BASE;32;LD (nn),A;3;4;13;4,3,3,3;-;-;-;------;1;-;93
BASE;33;INC SP;1;1;6;6;-;-;-;------;1;-;198
BASE;34;INC (HL);1;3;11;4,4,3;-;-;-;rrcv0-;1;-;167
BASE;35;DEC (HL);1;3;11;4,4,3;-;-;-;rrcv1-;1;-;170
BASE;36;LD (HL),n;2;3;10;4,3,3;-;-;-;------;1;-;85
BASE;37;SCF;1;1;4;4;-;-;-;--0-01;1;-;179
BASE;38;JR C,e;2;3;12;4,3,5;2;7;4,3;------;1;-;267
BASE;39;ADD HL,SP;1;3;11;4,4,3;-;-;-;--c-0c;1;-;188
BASE;3A;LD A,(nn);3;4;13;4,3,3,3;-;-;-;------;1;-;90
BASE;3B;DEC SP;1;1;6;6;-;-;-;------;1;-;201
BASE;3C;INC A;1;1;4;4;-;-;-;rrcv0-;1;-;165
BASE;3D;DEC A;1;1;4;4;-;-;-;rrcv1-;1;-;170
BASE;3E;LD A,n;2;2;7;4,3;-;-;-;------;1;-;72
BASE;3F;CCF;1;1;4;4;-;-;-;--k-0t;1;-;178
BASE;40;LD B,B;1;1;4;4;-;-;-;------;1;-;71
BASE;41;LD B,C;1;1;4;4;-;-;-;------;1;-;71
BASE;42;LD B,D;1;1;4;4;-;-;-;------;1;-;71
BASE;43;LD B,E;1;1;4;4;-;-;-;------;1;-;71
BASE;44;LD B,H;1;1;4;4;-;-;-;------;1;-;71
BASE;45;LD B,L;1;1;4;4;-;-;-;------;1;-;71
BASE;46;LD B,(HL);1;2;7;4,3;-;-;-;------;1;-;74
BASE;47;LD B,A;1;1;4;4;-;-;-;------;1;-;71
BASE;48;LD C,B;1;1;4;4;-;-;-;------;1;-;71
BASE;49;LD C,C;1;1;4;4;-;-;-;------;1;-;71
BASE;4A;LD C,D;1;1;4;4;-;-;-;------;1;-;71
BASE;4B;LD C,E;1;1;4;4;-;-;-;------;1;-;71
BASE;4C;LD C,H;1;1;4;4;-;-;-;------;1;-;71
BASE;4D;LD C,L;1;1;4;4;-;-;-;------;1;-;71
BASE;4E;LD C,(HL);1;2;7;4,3;-;-;-;------;1;-;74
BASE;4F;LD C,A;1;1;4;4;-;-;-;------;1;-;71
BASE;50;LD D,B;1;1;4;4;-;-;-;------;1;-;71
BASE;51;LD D,C;1;1;4;4;-;-;-;------;1;-;71
BASE;52;LD D,D;1;1;4;4;-;-;-;------;1;-;71
BASE;53;LD D,E;1;1;4;4;-;-;-;------;1;-;71
BASE;54;LD D,H;1;1;4;4;-;-;-;------;1;-;71
BASE;55;LD D,L;1;1;4;4;-;-;-;------;1;-;71
BASE;56;LD D,(HL);1;2;7;4,3;-;-;-;------;1;-;74
BASE;57;LD D,A;1;1;4;4;-;-;-;------;1;-;71
BASE;58;LD E,B;1;1;4;4;-;-;-;------;1;-;71
BASE;59;LD E,C;1;1;4;4;-;-;-;------;1;-;71
BASE;5A;LD E,D;1;1;4;4;-;-;-;------;1;-;71
BASE;5B;LD E,E;1;1;4;4;-;-;-;------;1;-;71
BASE;5C;LD E,H;1;1;4;4;-;-;-;------;1;-;71
BASE;5D;LD E,L;1;1;4;4;-;-;-;------;1;-;71
BASE;5E;LD E,(HL);1;2;7;4,3;-;-;-;------;1;-;74
BASE;5F;LD E,A;1;1;4;4;-;-;-;------;1;-;71
BASE;60;LD H,B;1;1;4;4;-;-;-;------;1;-;71
BASE;61;LD H,C;1;1;4;4;-;-;-;------;1;-;71
BASE;62;LD H,D;1;1;4;4;-;-;-;------;1;-;71
BASE;63;LD H,E;1;1;4;4;-;-;-;------;1;-;71
BASE;64;LD H,H;1;1;4;4;-;-;-;------;1;-;71
BASE;65;LD H,L;1;1;4;4;-;-;-;------;1;-;71
BASE;66;LD H,(HL);1;2;7;4,3;-;-;-;------;1;-;74
BASE;67;LD H,A;1;1;4;4;-;-;-;------;1;-;71
BASE;68;LD L,B;1;1;4;4;-;-;-;------;1;-;71
BASE;69;LD L,C;1;1;4;4;-;-;-;------;1;-;71
BASE;6A;LD L,D;1;1;4;4;-;-;-;------;1;-;71
BASE;6B;LD L,E;1;1;4;4;-;-;-;------;1;-;71
BASE;6C;LD L,H;1;1;4;4;-;-;-;------;1;-;71
BASE;6D;LD L,L;1;1;4;4;-;-;-;------;1;-;71
BASE;6E;LD L,(HL);1;2;7;4,3;-;-;-;------;1;-;74
BASE;6F;LD L,A;1;1;4;4;-;-;-;------;1;-;71
BASE;70;LD (HL),B;1;2;7;4,3;-;-;-;------;1;-;79
BASE;71;LD (HL),C;1;2;7;4,3;-;-;-;------;1;-;79
BASE;72;LD (HL),D;1;2;7;4,3;-;-;-;------;1;-;79
BASE;73;LD (HL),E;1;2;7;4,3;-;-;-;------;1;-;79
BASE;74;LD (HL),H;1;2;7;4,3;-;-;-;------;1;-;79
BASE;75;LD (HL),L;1;2;7;4,3;-;-;-;------;1;-;79
BASE;76;HALT;1;1;4;4;-;-;-;------;1;-;181
BASE;77;LD (HL),A;1;2;7;4,3;-;-;-;------;1;-;79
BASE;78;LD A,B;1;1;4;4;-;-;-;------;1;-;71
BASE;79;LD A,C;1;1;4;4;-;-;-;------;1;-;71
BASE;7A;LD A,D;1;1;4;4;-;-;-;------;1;-;71
BASE;7B;LD A,E;1;1;4;4;-;-;-;------;1;-;71
BASE;7C;LD A,H;1;1;4;4;-;-;-;------;1;-;71
BASE;7D;LD A,L;1;1;4;4;-;-;-;------;1;-;71
BASE;7E;LD A,(HL);1;2;7;4,3;-;-;-;------;1;-;74
BASE;7F;LD A,A;1;1;4;4;-;-;-;------;1;-;71
BASE;80;ADD A,B;1;1;4;4;-;-;-;rrcv0c;1;-;145
BASE;81;ADD A,C;1;1;4;4;-;-;-;rrcv0c;1;-;145
BASE;82;ADD A,D;1;1;4;4;-;-;-;rrcv0c;1;-;145
BASE;83;ADD A,E;1;1;4;4;-;-;-;rrcv0c;1;-;145
BASE;84;ADD A,H;1;1;4;4;-;-;-;rrcv0c;1;-;145
BASE;85;ADD A,L;1;1;4;4;-;-;-;rrcv0c;1;-;145
BASE;86;ADD A,(HL);1;2;7;4,3;-;-;-;rrcv0c;1;-;148
BASE;87;ADD A,A;1;1;4;4;-;-;-;rrcv0c;1;-;145
BASE;88;ADC A,B;1;1;4;4;-;-;-;rrcv0c;1;-;151
BASE;89;ADC A,C;1;1;4;4;-;-;-;rrcv0c;1;-;151
BASE;8A;ADC A,D;1;1;4;4;-;-;-;rrcv0c;1;-;151
BASE;8B;ADC A,E;1;1;4;4;-;-;-;rrcv0c;1;-;151
BASE;8C;ADC A,H;1;1;4;4;-;-;-;rrcv0c;1;-;151
BASE;8D;ADC A,L;1;1;4;4;-;-;-;rrcv0c;1;-;151
BASE;8E;ADC A,(HL);1;2;7;4,3;-;-;-;rrcv0c;1;-;151
BASE;8F;ADC A,A;1;1;4;4;-;-;-;rrcv0c;1;-;151
BASE;90;SUB B;1;1;4;4;-;-;-;rrcv1c;1;-;153
BASE;91;SUB C;1;1;4;4;-;-;-;rrcv1c;1;-;153
BASE;92;SUB D;1;1;4;4;-;-;-;rrcv1c;1;-;153
BASE;93;SUB E;1;1;4;4;-;-;-;rrcv1c;1;-;153
BASE;94;SUB H;1;1;4;4;-;-;-;rrcv1c;1;-;153
BASE;95;SUB L;1;1;4;4;-;-;-;rrcv1c;1;-;153
BASE;96;SUB (HL);1;2;7;4,3;-;-;-;rrcv1c;1;-;153
BASE;97;SUB A;1;1;4;4;-;-;-;rrcv1c;1;-;153
BASE;98;SBC A,B;1;1;4;4;-;-;-;rrcv1c;1;R4;155
BASE;99;SBC A,C;1;1;4;4;-;-;-;rrcv1c;1;R4;155
BASE;9A;SBC A,D;1;1;4;4;-;-;-;rrcv1c;1;R4;155
BASE;9B;SBC A,E;1;1;4;4;-;-;-;rrcv1c;1;R4;155
BASE;9C;SBC A,H;1;1;4;4;-;-;-;rrcv1c;1;R4;155
BASE;9D;SBC A,L;1;1;4;4;-;-;-;rrcv1c;1;R4;155
BASE;9E;SBC A,(HL);1;2;7;4,3;-;-;-;rrcv1c;1;R4;155
BASE;9F;SBC A,A;1;1;4;4;-;-;-;rrcv1c;1;R4;155
BASE;A0;AND B;1;1;4;4;-;-;-;rr1p00;1;R4;157
BASE;A1;AND C;1;1;4;4;-;-;-;rr1p00;1;R4;157
BASE;A2;AND D;1;1;4;4;-;-;-;rr1p00;1;R4;157
BASE;A3;AND E;1;1;4;4;-;-;-;rr1p00;1;R4;157
BASE;A4;AND H;1;1;4;4;-;-;-;rr1p00;1;R4;157
BASE;A5;AND L;1;1;4;4;-;-;-;rr1p00;1;R4;157
BASE;A6;AND (HL);1;2;7;4,3;-;-;-;rr1p00;1;R4;157
BASE;A7;AND A;1;1;4;4;-;-;-;rr1p00;1;R4;157
BASE;A8;XOR B;1;1;4;4;-;-;-;rr0p00;1;-;161
BASE;A9;XOR C;1;1;4;4;-;-;-;rr0p00;1;-;161
BASE;AA;XOR D;1;1;4;4;-;-;-;rr0p00;1;-;161
BASE;AB;XOR E;1;1;4;4;-;-;-;rr0p00;1;-;161
BASE;AC;XOR H;1;1;4;4;-;-;-;rr0p00;1;-;161
BASE;AD;XOR L;1;1;4;4;-;-;-;rr0p00;1;-;161
BASE;AE;XOR (HL);1;2;7;4,3;-;-;-;rr0p00;1;-;161
BASE;AF;XOR A;1;1;4;4;-;-;-;rr0p00;1;-;161
BASE;B0;OR B;1;1;4;4;-;-;-;rr0p00;1;R4;159
BASE;B1;OR C;1;1;4;4;-;-;-;rr0p00;1;R4;159
BASE;B2;OR D;1;1;4;4;-;-;-;rr0p00;1;R4;159
BASE;B3;OR E;1;1;4;4;-;-;-;rr0p00;1;R4;159
BASE;B4;OR H;1;1;4;4;-;-;-;rr0p00;1;R4;159
BASE;B5;OR L;1;1;4;4;-;-;-;rr0p00;1;R4;159
BASE;B6;OR (HL);1;2;7;4,3;-;-;-;rr0p00;1;R4;159
BASE;B7;OR A;1;1;4;4;-;-;-;rr0p00;1;R4;159
BASE;B8;CP B;1;1;4;4;-;-;-;rrcv1c;1;-;163
BASE;B9;CP C;1;1;4;4;-;-;-;rrcv1c;1;-;163
BASE;BA;CP D;1;1;4;4;-;-;-;rrcv1c;1;-;163
BASE;BB;CP E;1;1;4;4;-;-;-;rrcv1c;1;-;163
BASE;BC;CP H;1;1;4;4;-;-;-;rrcv1c;1;-;163
BASE;BD;CP L;1;1;4;4;-;-;-;rrcv1c;1;-;163
BASE;BE;CP (HL);1;2;7;4,3;-;-;-;rrcv1c;1;-;163
BASE;BF;CP A;1;1;4;4;-;-;-;rrcv1c;1;-;163
BASE;C0;RET NZ;1;3;11;5,3,3;1;5;5;------;1;-;286
BASE;C1;POP BC;1;3;10;4,3,3;-;-;-;------;1;-;119
BASE;C2;JP NZ,nn;3;3;10;4,3,3;-;-;-;------;1;n4;263
BASE;C3;JP nn;3;3;10;4,3,3;-;-;-;------;1;-;262
BASE;C4;CALL NZ,nn;3;5;17;4,3,4,3,3;3;10;4,3,3;------;1;-;283
BASE;C5;PUSH BC;1;3;11;5,3,3;-;-;-;------;1;-;115
BASE;C6;ADD A,n;2;2;7;4,3;-;-;-;rrcv0c;1;-;147
BASE;C7;RST 00h;1;3;11;5,3,3;-;-;-;------;1;-;292
BASE;C8;RET Z;1;3;11;5,3,3;1;5;5;------;1;-;286
BASE;C9;RET;1;3;10;4,3,3;-;-;-;------;1;-;285
BASE;CA;JP Z,nn;3;3;10;4,3,3;-;-;-;------;1;n4;263
BASE;CB;PREFIX_CB;-;-;-;-;-;-;-;-;-;-;213
BASE;CC;CALL Z,nn;3;5;17;4,3,4,3,3;3;10;4,3,3;------;1;-;283
BASE;CD;CALL nn;3;5;17;4,3,4,3,3;-;-;-;------;1;-;281
BASE;CE;ADC A,n;2;2;7;4,3;-;-;-;rrcv0c;1;-;151
BASE;CF;RST 08h;1;3;11;5,3,3;-;-;-;------;1;-;292
BASE;D0;RET NC;1;3;11;5,3,3;1;5;5;------;1;-;286
BASE;D1;POP DE;1;3;10;4,3,3;-;-;-;------;1;-;119
BASE;D2;JP NC,nn;3;3;10;4,3,3;-;-;-;------;1;n4;263
BASE;D3;OUT (n),A;2;3;11;4,3,4;-;-;-;------;1;-;306
BASE;D4;CALL NC,nn;3;5;17;4,3,4,3,3;3;10;4,3,3;------;1;-;283
BASE;D5;PUSH DE;1;3;11;5,3,3;-;-;-;------;1;-;115
BASE;D6;SUB n;2;2;7;4,3;-;-;-;rrcv1c;1;-;153
BASE;D7;RST 10h;1;3;11;5,3,3;-;-;-;------;1;-;292
BASE;D8;RET C;1;3;11;5,3,3;1;5;5;------;1;-;286
BASE;D9;EXX;1;1;4;4;-;-;-;------;1;-;126
BASE;DA;JP C,nn;3;3;10;4,3,3;-;-;-;------;1;n4;263
BASE;DB;IN A,(n);2;3;11;4,3,4;-;-;-;------;1;-;295
BASE;DC;CALL C,nn;3;5;17;4,3,4,3,3;3;10;4,3,3;------;1;-;283
BASE;DD;PREFIX_DD;-;-;-;-;-;-;-;-;-;-;75
BASE;DE;SBC A,n;2;2;7;4,3;-;-;-;rrcv1c;1;R4;155
BASE;DF;RST 18h;1;3;11;5,3,3;-;-;-;------;1;-;292
BASE;E0;RET PO;1;3;11;5,3,3;1;5;5;------;1;-;286
BASE;E1;POP HL;1;3;10;4,3,3;-;-;-;------;1;-;119
BASE;E2;JP PO,nn;3;3;10;4,3,3;-;-;-;------;1;n4;263
BASE;E3;EX (SP),HL;1;5;19;4,3,4,3,5;-;-;-;------;1;-;127
BASE;E4;CALL PO,nn;3;5;17;4,3,4,3,3;3;10;4,3,3;------;1;-;283
BASE;E5;PUSH HL;1;3;11;5,3,3;-;-;-;------;1;-;115
BASE;E6;AND n;2;2;7;4,3;-;-;-;rr1p00;1;R4;157
BASE;E7;RST 20h;1;3;11;5,3,3;-;-;-;------;1;-;292
BASE;E8;RET PE;1;3;11;5,3,3;1;5;5;------;1;-;286
BASE;E9;JP (HL);1;1;4;4;-;-;-;------;1;-;275
BASE;EA;JP PE,nn;3;3;10;4,3,3;-;-;-;------;1;n4;263
BASE;EB;EX DE,HL;1;1;4;4;-;-;-;------;1;-;124
BASE;EC;CALL PE,nn;3;5;17;4,3,4,3,3;3;10;4,3,3;------;1;-;283
BASE;ED;PREFIX_ED;-;-;-;-;-;-;-;-;-;-;94
BASE;EE;XOR n;2;2;7;4,3;-;-;-;rr0p00;1;-;161
BASE;EF;RST 28h;1;3;11;5,3,3;-;-;-;------;1;-;292
BASE;F0;RET P;1;3;11;5,3,3;1;5;5;------;1;-;286
BASE;F1;POP AF;1;3;10;4,3,3;-;-;-;------;1;-;119
BASE;F2;JP P,nn;3;3;10;4,3,3;-;-;-;------;1;n4;263
BASE;F3;DI;1;1;4;4;-;-;-;------;1;-;182
BASE;F4;CALL P,nn;3;5;17;4,3,4,3,3;3;10;4,3,3;------;1;-;283
BASE;F5;PUSH AF;1;3;11;5,3,3;-;-;-;------;1;-;115
BASE;F6;OR n;2;2;7;4,3;-;-;-;rr0p00;1;R4;159
BASE;F7;RST 30h;1;3;11;5,3,3;-;-;-;------;1;-;292
BASE;F8;RET M;1;3;11;5,3,3;1;5;5;------;1;-;286
BASE;F9;LD SP,HL;1;1;6;6;-;-;-;------;1;-;112
BASE;FA;JP M,nn;3;3;10;4,3,3;-;-;-;------;1;n4;263
BASE;FB;EI;1;1;4;4;-;-;-;------;1;-;183
BASE;FC;CALL M,nn;3;5;17;4,3,4,3,3;3;10;4,3,3;------;1;-;283
BASE;FD;PREFIX_FD;-;-;-;-;-;-;-;-;-;-;77
BASE;FE;CP n;2;2;7;4,3;-;-;-;rrcv1c;1;-;163
BASE;FF;RST 38h;1;3;11;5,3,3;-;-;-;------;1;-;292
CB;00;RLC B;2;2;8;4,4;-;-;-;rr0p0c;2;-;213
CB;01;RLC C;2;2;8;4,4;-;-;-;rr0p0c;2;-;213
CB;02;RLC D;2;2;8;4,4;-;-;-;rr0p0c;2;-;213
CB;03;RLC E;2;2;8;4,4;-;-;-;rr0p0c;2;-;213
CB;04;RLC H;2;2;8;4,4;-;-;-;rr0p0c;2;-;213
CB;05;RLC L;2;2;8;4,4;-;-;-;rr0p0c;2;-;213
CB;06;RLC (HL);2;4;15;4,4,4,3;-;-;-;rr0p0c;2;-;215
CB;07;RLC A;2;2;8;4,4;-;-;-;rr0p0c;2;-;213
CB;08;RRC B;2;2;8;4,4;-;-;-;rr0p0c;2;-;224
CB;09;RRC C;2;2;8;4,4;-;-;-;rr0p0c;2;-;224
CB;0A;RRC D;2;2;8;4,4;-;-;-;rr0p0c;2;-;224
CB;0B;RRC E;2;2;8;4,4;-;-;-;rr0p0c;2;-;224
CB;0C;RRC H;2;2;8;4,4;-;-;-;rr0p0c;2;-;224
CB;0D;RRC L;2;2;8;4,4;-;-;-;rr0p0c;2;-;224
CB;0E;RRC (HL);2;4;15;4,4,4,3;-;-;-;rr0p0c;2;-;224
CB;0F;RRC A;2;2;8;4,4;-;-;-;rr0p0c;2;-;224
CB;10;RL B;2;2;8;4,4;-;-;-;rr0p0c;2;-;221
CB;11;RL C;2;2;8;4,4;-;-;-;rr0p0c;2;-;221
CB;12;RL D;2;2;8;4,4;-;-;-;rr0p0c;2;-;221
CB;13;RL E;2;2;8;4,4;-;-;-;rr0p0c;2;-;221
CB;14;RL H;2;2;8;4,4;-;-;-;rr0p0c;2;-;221
CB;15;RL L;2;2;8;4,4;-;-;-;rr0p0c;2;-;221
CB;16;RL (HL);2;4;15;4,4,4,3;-;-;-;rr0p0c;2;-;221
CB;17;RL A;2;2;8;4,4;-;-;-;rr0p0c;2;-;221
CB;18;RR B;2;2;8;4,4;-;-;-;rr0p0c;2;-;227
CB;19;RR C;2;2;8;4,4;-;-;-;rr0p0c;2;-;227
CB;1A;RR D;2;2;8;4,4;-;-;-;rr0p0c;2;-;227
CB;1B;RR E;2;2;8;4,4;-;-;-;rr0p0c;2;-;227
CB;1C;RR H;2;2;8;4,4;-;-;-;rr0p0c;2;-;227
CB;1D;RR L;2;2;8;4,4;-;-;-;rr0p0c;2;-;227
CB;1E;RR (HL);2;4;15;4,4,4,3;-;-;-;rr0p0c;2;-;227
CB;1F;RR A;2;2;8;4,4;-;-;-;rr0p0c;2;-;227
CB;20;SLA B;2;2;8;4,4;-;-;-;rr0p0c;2;-;230
CB;21;SLA C;2;2;8;4,4;-;-;-;rr0p0c;2;-;230
CB;22;SLA D;2;2;8;4,4;-;-;-;rr0p0c;2;-;230
CB;23;SLA E;2;2;8;4,4;-;-;-;rr0p0c;2;-;230
CB;24;SLA H;2;2;8;4,4;-;-;-;rr0p0c;2;-;230
CB;25;SLA L;2;2;8;4,4;-;-;-;rr0p0c;2;-;230
CB;26;SLA (HL);2;4;15;4,4,4,3;-;-;-;rr0p0c;2;-;230
CB;27;SLA A;2;2;8;4,4;-;-;-;rr0p0c;2;-;230
CB;28;SRA B;2;2;8;4,4;-;-;-;rr0p0c;2;-;233
CB;29;SRA C;2;2;8;4,4;-;-;-;rr0p0c;2;-;233
CB;2A;SRA D;2;2;8;4,4;-;-;-;rr0p0c;2;-;233
CB;2B;SRA E;2;2;8;4,4;-;-;-;rr0p0c;2;-;233
CB;2C;SRA H;2;2;8;4,4;-;-;-;rr0p0c;2;-;233
CB;2D;SRA L;2;2;8;4,4;-;-;-;rr0p0c;2;-;233
CB;2E;SRA (HL);2;4;15;4,4,4,3;-;-;-;rr0p0c;2;-;233
CB;2F;SRA A;2;2;8;4,4;-;-;-;rr0p0c;2;-;233
CB;30;SLL B;2;2;8;-;-;-;-;rr0p0c;2;SY;32
CB;31;SLL C;2;2;8;-;-;-;-;rr0p0c;2;SY;32
CB;32;SLL D;2;2;8;-;-;-;-;rr0p0c;2;SY;32
CB;33;SLL E;2;2;8;-;-;-;-;rr0p0c;2;SY;32
CB;34;SLL H;2;2;8;-;-;-;-;rr0p0c;2;SY;32
CB;35;SLL L;2;2;8;-;-;-;-;rr0p0c;2;SY;32
CB;36;SLL (HL);2;4;15;-;-;-;-;rr0p0c;2;SY;32
CB;37;SLL A;2;2;8;-;-;-;-;rr0p0c;2;SY;32
CB;38;SRL B;2;2;8;4,4;-;-;-;0r0p0c;2;-;236
CB;39;SRL C;2;2;8;4,4;-;-;-;0r0p0c;2;-;236
CB;3A;SRL D;2;2;8;4,4;-;-;-;0r0p0c;2;-;236
CB;3B;SRL E;2;2;8;4,4;-;-;-;0r0p0c;2;-;236
CB;3C;SRL H;2;2;8;4,4;-;-;-;0r0p0c;2;-;236
CB;3D;SRL L;2;2;8;4,4;-;-;-;0r0p0c;2;-;236
CB;3E;SRL (HL);2;4;15;4,4,4,3;-;-;-;0r0p0c;2;-;236
CB;3F;SRL A;2;2;8;4,4;-;-;-;0r0p0c;2;-;236
CB;40;BIT 0,B;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;41;BIT 0,C;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;42;BIT 0,D;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;43;BIT 0,E;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;44;BIT 0,H;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;45;BIT 0,L;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;46;BIT 0,(HL);2;3;12;4,4,4;-;-;-;xz1x0-;2;R6;245
CB;47;BIT 0,A;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;48;BIT 1,B;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;49;BIT 1,C;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;4A;BIT 1,D;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;4B;BIT 1,E;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;4C;BIT 1,H;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;4D;BIT 1,L;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;4E;BIT 1,(HL);2;3;12;4,4,4;-;-;-;xz1x0-;2;R6;245
CB;4F;BIT 1,A;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;50;BIT 2,B;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;51;BIT 2,C;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;52;BIT 2,D;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;53;BIT 2,E;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;54;BIT 2,H;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;55;BIT 2,L;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;56;BIT 2,(HL);2;3;12;4,4,4;-;-;-;xz1x0-;2;R6;245
CB;57;BIT 2,A;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;58;BIT 3,B;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;59;BIT 3,C;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;5A;BIT 3,D;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;5B;BIT 3,E;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;5C;BIT 3,H;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;5D;BIT 3,L;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;5E;BIT 3,(HL);2;3;12;4,4,4;-;-;-;xz1x0-;2;R6;245
CB;5F;BIT 3,A;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;60;BIT 4,B;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;61;BIT 4,C;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;62;BIT 4,D;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;63;BIT 4,E;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;64;BIT 4,H;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;65;BIT 4,L;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;66;BIT 4,(HL);2;3;12;4,4,4;-;-;-;xz1x0-;2;R6;245
CB;67;BIT 4,A;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;68;BIT 5,B;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;69;BIT 5,C;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;6A;BIT 5,D;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;6B;BIT 5,E;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;6C;BIT 5,H;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;6D;BIT 5,L;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;6E;BIT 5,(HL);2;3;12;4,4,4;-;-;-;xz1x0-;2;R6;245
CB;6F;BIT 5,A;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;70;BIT 6,B;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;71;BIT 6,C;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;72;BIT 6,D;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;73;BIT 6,E;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;74;BIT 6,H;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;75;BIT 6,L;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;76;BIT 6,(HL);2;3;12;4,4,4;-;-;-;xz1x0-;2;R6;245
CB;77;BIT 6,A;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;78;BIT 7,B;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;79;BIT 7,C;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;7A;BIT 7,D;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;7B;BIT 7,E;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;7C;BIT 7,H;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;7D;BIT 7,L;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;7E;BIT 7,(HL);2;3;12;4,4,4;-;-;-;xz1x0-;2;R6;245
CB;7F;BIT 7,A;2;2;8;4,4;-;-;-;xz1x0-;2;-;243
CB;80;RES 0,B;2;2;8;4,4;-;-;-;------;2;R2;259
CB;81;RES 0,C;2;2;8;4,4;-;-;-;------;2;R2;259
CB;82;RES 0,D;2;2;8;4,4;-;-;-;------;2;R2;259
CB;83;RES 0,E;2;2;8;4,4;-;-;-;------;2;R2;259
CB;84;RES 0,H;2;2;8;4,4;-;-;-;------;2;R2;259
CB;85;RES 0,L;2;2;8;4,4;-;-;-;------;2;R2;259
CB;86;RES 0,(HL);2;4;15;4,4,4,3;-;-;-;------;2;-;259
CB;87;RES 0,A;2;2;8;4,4;-;-;-;------;2;R2;259
CB;88;RES 1,B;2;2;8;4,4;-;-;-;------;2;R2;259
CB;89;RES 1,C;2;2;8;4,4;-;-;-;------;2;R2;259
CB;8A;RES 1,D;2;2;8;4,4;-;-;-;------;2;R2;259
CB;8B;RES 1,E;2;2;8;4,4;-;-;-;------;2;R2;259
CB;8C;RES 1,H;2;2;8;4,4;-;-;-;------;2;R2;259
CB;8D;RES 1,L;2;2;8;4,4;-;-;-;------;2;R2;259
CB;8E;RES 1,(HL);2;4;15;4,4,4,3;-;-;-;------;2;-;259
CB;8F;RES 1,A;2;2;8;4,4;-;-;-;------;2;R2;259
CB;90;RES 2,B;2;2;8;4,4;-;-;-;------;2;R2;259
CB;91;RES 2,C;2;2;8;4,4;-;-;-;------;2;R2;259
CB;92;RES 2,D;2;2;8;4,4;-;-;-;------;2;R2;259
CB;93;RES 2,E;2;2;8;4,4;-;-;-;------;2;R2;259
CB;94;RES 2,H;2;2;8;4,4;-;-;-;------;2;R2;259
CB;95;RES 2,L;2;2;8;4,4;-;-;-;------;2;R2;259
CB;96;RES 2,(HL);2;4;15;4,4,4,3;-;-;-;------;2;-;259
CB;97;RES 2,A;2;2;8;4,4;-;-;-;------;2;R2;259
CB;98;RES 3,B;2;2;8;4,4;-;-;-;------;2;R2;259
CB;99;RES 3,C;2;2;8;4,4;-;-;-;------;2;R2;259
CB;9A;RES 3,D;2;2;8;4,4;-;-;-;------;2;R2;259
CB;9B;RES 3,E;2;2;8;4,4;-;-;-;------;2;R2;259
CB;9C;RES 3,H;2;2;8;4,4;-;-;-;------;2;R2;259
CB;9D;RES 3,L;2;2;8;4,4;-;-;-;------;2;R2;259
CB;9E;RES 3,(HL);2;4;15;4,4,4,3;-;-;-;------;2;-;259
CB;9F;RES 3,A;2;2;8;4,4;-;-;-;------;2;R2;259
CB;A0;RES 4,B;2;2;8;4,4;-;-;-;------;2;R2;259
CB;A1;RES 4,C;2;2;8;4,4;-;-;-;------;2;R2;259
CB;A2;RES 4,D;2;2;8;4,4;-;-;-;------;2;R2;259
CB;A3;RES 4,E;2;2;8;4,4;-;-;-;------;2;R2;259
CB;A4;RES 4,H;2;2;8;4,4;-;-;-;------;2;R2;259
CB;A5;RES 4,L;2;2;8;4,4;-;-;-;------;2;R2;259
CB;A6;RES 4,(HL);2;4;15;4,4,4,3;-;-;-;------;2;-;259
CB;A7;RES 4,A;2;2;8;4,4;-;-;-;------;2;R2;259
CB;A8;RES 5,B;2;2;8;4,4;-;-;-;------;2;R2;259
CB;A9;RES 5,C;2;2;8;4,4;-;-;-;------;2;R2;259
CB;AA;RES 5,D;2;2;8;4,4;-;-;-;------;2;R2;259
CB;AB;RES 5,E;2;2;8;4,4;-;-;-;------;2;R2;259
CB;AC;RES 5,H;2;2;8;4,4;-;-;-;------;2;R2;259
CB;AD;RES 5,L;2;2;8;4,4;-;-;-;------;2;R2;259
CB;AE;RES 5,(HL);2;4;15;4,4,4,3;-;-;-;------;2;-;259
CB;AF;RES 5,A;2;2;8;4,4;-;-;-;------;2;R2;259
CB;B0;RES 6,B;2;2;8;4,4;-;-;-;------;2;R2;259
CB;B1;RES 6,C;2;2;8;4,4;-;-;-;------;2;R2;259
CB;B2;RES 6,D;2;2;8;4,4;-;-;-;------;2;R2;259
CB;B3;RES 6,E;2;2;8;4,4;-;-;-;------;2;R2;259
CB;B4;RES 6,H;2;2;8;4,4;-;-;-;------;2;R2;259
CB;B5;RES 6,L;2;2;8;4,4;-;-;-;------;2;R2;259
CB;B6;RES 6,(HL);2;4;15;4,4,4,3;-;-;-;------;2;-;259
CB;B7;RES 6,A;2;2;8;4,4;-;-;-;------;2;R2;259
CB;B8;RES 7,B;2;2;8;4,4;-;-;-;------;2;R2;259
CB;B9;RES 7,C;2;2;8;4,4;-;-;-;------;2;R2;259
CB;BA;RES 7,D;2;2;8;4,4;-;-;-;------;2;R2;259
CB;BB;RES 7,E;2;2;8;4,4;-;-;-;------;2;R2;259
CB;BC;RES 7,H;2;2;8;4,4;-;-;-;------;2;R2;259
CB;BD;RES 7,L;2;2;8;4,4;-;-;-;------;2;R2;259
CB;BE;RES 7,(HL);2;4;15;4,4,4,3;-;-;-;------;2;-;259
CB;BF;RES 7,A;2;2;8;4,4;-;-;-;------;2;R2;259
CB;C0;SET 0,B;2;2;8;4,4;-;-;-;------;2;-;251
CB;C1;SET 0,C;2;2;8;4,4;-;-;-;------;2;-;251
CB;C2;SET 0,D;2;2;8;4,4;-;-;-;------;2;-;251
CB;C3;SET 0,E;2;2;8;4,4;-;-;-;------;2;-;251
CB;C4;SET 0,H;2;2;8;4,4;-;-;-;------;2;-;251
CB;C5;SET 0,L;2;2;8;4,4;-;-;-;------;2;-;251
CB;C6;SET 0,(HL);2;4;15;4,4,4,3;-;-;-;------;2;-;253
CB;C7;SET 0,A;2;2;8;4,4;-;-;-;------;2;-;251
CB;C8;SET 1,B;2;2;8;4,4;-;-;-;------;2;-;251
CB;C9;SET 1,C;2;2;8;4,4;-;-;-;------;2;-;251
CB;CA;SET 1,D;2;2;8;4,4;-;-;-;------;2;-;251
CB;CB;SET 1,E;2;2;8;4,4;-;-;-;------;2;-;251
CB;CC;SET 1,H;2;2;8;4,4;-;-;-;------;2;-;251
CB;CD;SET 1,L;2;2;8;4,4;-;-;-;------;2;-;251
CB;CE;SET 1,(HL);2;4;15;4,4,4,3;-;-;-;------;2;-;253
CB;CF;SET 1,A;2;2;8;4,4;-;-;-;------;2;-;251
CB;D0;SET 2,B;2;2;8;4,4;-;-;-;------;2;-;251
CB;D1;SET 2,C;2;2;8;4,4;-;-;-;------;2;-;251
CB;D2;SET 2,D;2;2;8;4,4;-;-;-;------;2;-;251
CB;D3;SET 2,E;2;2;8;4,4;-;-;-;------;2;-;251
CB;D4;SET 2,H;2;2;8;4,4;-;-;-;------;2;-;251
CB;D5;SET 2,L;2;2;8;4,4;-;-;-;------;2;-;251
CB;D6;SET 2,(HL);2;4;15;4,4,4,3;-;-;-;------;2;-;253
CB;D7;SET 2,A;2;2;8;4,4;-;-;-;------;2;-;251
CB;D8;SET 3,B;2;2;8;4,4;-;-;-;------;2;-;251
CB;D9;SET 3,C;2;2;8;4,4;-;-;-;------;2;-;251
CB;DA;SET 3,D;2;2;8;4,4;-;-;-;------;2;-;251
CB;DB;SET 3,E;2;2;8;4,4;-;-;-;------;2;-;251
CB;DC;SET 3,H;2;2;8;4,4;-;-;-;------;2;-;251
CB;DD;SET 3,L;2;2;8;4,4;-;-;-;------;2;-;251
CB;DE;SET 3,(HL);2;4;15;4,4,4,3;-;-;-;------;2;-;253
CB;DF;SET 3,A;2;2;8;4,4;-;-;-;------;2;-;251
CB;E0;SET 4,B;2;2;8;4,4;-;-;-;------;2;-;251
CB;E1;SET 4,C;2;2;8;4,4;-;-;-;------;2;-;251
CB;E2;SET 4,D;2;2;8;4,4;-;-;-;------;2;-;251
CB;E3;SET 4,E;2;2;8;4,4;-;-;-;------;2;-;251
CB;E4;SET 4,H;2;2;8;4,4;-;-;-;------;2;-;251
CB;E5;SET 4,L;2;2;8;4,4;-;-;-;------;2;-;251
CB;E6;SET 4,(HL);2;4;15;4,4,4,3;-;-;-;------;2;-;253
CB;E7;SET 4,A;2;2;8;4,4;-;-;-;------;2;-;251
CB;E8;SET 5,B;2;2;8;4,4;-;-;-;------;2;-;251
CB;E9;SET 5,C;2;2;8;4,4;-;-;-;------;2;-;251
CB;EA;SET 5,D;2;2;8;4,4;-;-;-;------;2;-;251
CB;EB;SET 5,E;2;2;8;4,4;-;-;-;------;2;-;251
CB;EC;SET 5,H;2;2;8;4,4;-;-;-;------;2;-;251
CB;ED;SET 5,L;2;2;8;4,4;-;-;-;------;2;-;251
CB;EE;SET 5,(HL);2;4;15;4,4,4,3;-;-;-;------;2;-;253
CB;EF;SET 5,A;2;2;8;4,4;-;-;-;------;2;-;251
CB;F0;SET 6,B;2;2;8;4,4;-;-;-;------;2;-;251
CB;F1;SET 6,C;2;2;8;4,4;-;-;-;------;2;-;251
CB;F2;SET 6,D;2;2;8;4,4;-;-;-;------;2;-;251
CB;F3;SET 6,E;2;2;8;4,4;-;-;-;------;2;-;251
CB;F4;SET 6,H;2;2;8;4,4;-;-;-;------;2;-;251
CB;F5;SET 6,L;2;2;8;4,4;-;-;-;------;2;-;251
CB;F6;SET 6,(HL);2;4;15;4,4,4,3;-;-;-;------;2;-;253
CB;F7;SET 6,A;2;2;8;4,4;-;-;-;------;2;-;251
CB;F8;SET 7,B;2;2;8;4,4;-;-;-;------;2;-;251
CB;F9;SET 7,C;2;2;8;4,4;-;-;-;------;2;-;251
CB;FA;SET 7,D;2;2;8;4,4;-;-;-;------;2;-;251
CB;FB;SET 7,E;2;2;8;4,4;-;-;-;------;2;-;251
CB;FC;SET 7,H;2;2;8;4,4;-;-;-;------;2;-;251
CB;FD;SET 7,L;2;2;8;4,4;-;-;-;------;2;-;251
CB;FE;SET 7,(HL);2;4;15;4,4,4,3;-;-;-;------;2;-;253
CB;FF;SET 7,A;2;2;8;4,4;-;-;-;------;2;-;251
ED;00;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;01;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;02;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;03;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;04;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;05;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;06;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;07;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;08;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;09;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;0A;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;0B;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;0C;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;0D;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;0E;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;0F;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;10;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;11;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;12;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;13;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;14;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;15;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;16;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;17;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;18;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;19;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;1A;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;1B;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;1C;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;1D;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;1E;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;1F;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;20;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;21;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;22;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;23;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;24;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;25;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;26;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;27;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;28;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;29;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;2A;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;2B;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;2C;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;2D;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;2E;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;2F;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;30;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;31;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;32;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;33;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;34;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;35;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;36;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;37;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;38;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;39;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;3A;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;3B;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;3C;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;3D;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;3E;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;3F;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;40;IN B,(C);2;3;12;4,4,4;-;-;-;rr0p0-;2;-;296
ED;41;OUT (C),B;2;3;12;4,4,4;-;-;-;------;2;-;307
ED;42;SBC HL,BC;2;4;15;4,4,4,3;-;-;-;rrcv1c;2;-;192
ED;43;LD (nn),BC;4;6;20;4,4,3,3,3,3;-;-;-;------;2;-;108
ED;44;NEG;2;2;8;4,4;-;-;-;rrcv1c;2;n3;176
ED;45;RETN;2;4;14;4,4,3,3;-;-;-;------;2;-;290
ED;46;IM 0;2;2;8;4,4;-;-;-;------;2;-;184
ED;47;LD I,A;2;2;9;4,5;-;-;-;------;2;-;96
ED;48;IN C,(C);2;3;12;4,4,4;-;-;-;rr0p0-;2;-;296
ED;49;OUT (C),C;2;3;12;4,4,4;-;-;-;------;2;-;307
ED;4A;ADC HL,BC;2;4;15;4,4,4,3;-;-;-;rrcv0c;2;-;190
ED;4B;LD BC,(nn);4;6;20;4,4,3,3,3,3;-;-;-;------;2;-;103
ED;4C;NEG;2;2;8;-;-;-;-;rrcv1c;2;SY;30
ED;4D;RETI;2;4;14;4,4,3,3;-;-;-;------;2;-;288
ED;4E;IM 0;2;2;8;-;-;-;-;------;2;SY;30
ED;4F;LD R,A;2;2;9;4,5;-;-;-;------;2;-;97
ED;50;IN D,(C);2;3;12;4,4,4;-;-;-;rr0p0-;2;-;296
ED;51;OUT (C),D;2;3;12;4,4,4;-;-;-;------;2;-;307
ED;52;SBC HL,DE;2;4;15;4,4,4,3;-;-;-;rrcv1c;2;-;192
ED;53;LD (nn),DE;4;6;20;4,4,3,3,3,3;-;-;-;------;2;-;108
ED;54;NEG;2;2;8;-;-;-;-;rrcv1c;2;SY;30
ED;55;RETN;2;4;14;-;-;-;-;------;2;SY;34
ED;56;IM 1;2;2;8;4,4;-;-;-;------;2;-;185
ED;57;LD A,I;2;2;9;4,5;-;-;-;rr0i0-;2;n1;94
ED;58;IN E,(C);2;3;12;4,4,4;-;-;-;rr0p0-;2;-;296
ED;59;OUT (C),E;2;3;12;4,4,4;-;-;-;------;2;-;307
ED;5A;ADC HL,DE;2;4;15;4,4,4,3;-;-;-;rrcv0c;2;-;190
ED;5B;LD DE,(nn);4;6;20;4,4,3,3,3,3;-;-;-;------;2;-;103
ED;5C;NEG;2;2;8;-;-;-;-;rrcv1c;2;SY;30
ED;5D;RETN;2;4;14;-;-;-;-;------;2;SY;34
ED;5E;IM 2;2;2;8;4,4;-;-;-;------;2;-;186
ED;5F;LD A,R;2;2;9;4,5;-;-;-;rr0i0-;2;n1;95
ED;60;IN H,(C);2;3;12;4,4,4;-;-;-;rr0p0-;2;-;296
ED;61;OUT (C),H;2;3;12;4,4,4;-;-;-;------;2;-;307
ED;62;SBC HL,HL;2;4;15;4,4,4,3;-;-;-;rrcv1c;2;-;192
ED;63;LD (nn),HL;4;6;20;4,4,3,3,3,3;-;-;-;------;2;-;108
ED;64;NEG;2;2;8;-;-;-;-;rrcv1c;2;SY;30
ED;65;RETN;2;4;14;-;-;-;-;------;2;SY;34
ED;66;IM 0;2;2;8;-;-;-;-;------;2;SY;30
ED;67;RRD;2;5;18;4,4,3,4,3;-;-;-;rr0p0-;2;-;240
ED;68;IN L,(C);2;3;12;4,4,4;-;-;-;rr0p0-;2;-;296
ED;69;OUT (C),L;2;3;12;4,4,4;-;-;-;------;2;-;307
ED;6A;ADC HL,HL;2;4;15;4,4,4,3;-;-;-;rrcv0c;2;-;190
ED;6B;LD HL,(nn);4;6;20;4,4,3,3,3,3;-;-;-;------;2;-;103
ED;6C;NEG;2;2;8;-;-;-;-;rrcv1c;2;SY;30
ED;6D;RETN;2;4;14;-;-;-;-;------;2;SY;34
ED;6E;IM 0;2;2;8;-;-;-;-;------;2;SY;30
ED;6F;RLD;2;5;18;4,4,3,4,3;-;-;-;rr0p0-;2;-;238
ED;70;IN F,(C);2;3;12;-;-;-;-;rr0p0-;2;SY;35
ED;71;OUT (C),0;2;3;12;-;-;-;-;------;2;SY;35
ED;72;SBC HL,SP;2;4;15;4,4,4,3;-;-;-;rrcv1c;2;-;192
ED;73;LD (nn),SP;4;6;20;4,4,3,3,3,3;-;-;-;------;2;-;108
ED;74;NEG;2;2;8;-;-;-;-;rrcv1c;2;SY;30
ED;75;RETN;2;4;14;-;-;-;-;------;2;SY;34
ED;76;IM 1;2;2;8;-;-;-;-;------;2;SY;30
ED;77;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;78;IN A,(C);2;3;12;4,4,4;-;-;-;rr0p0-;2;-;296
ED;79;OUT (C),A;2;3;12;4,4,4;-;-;-;------;2;-;307
ED;7A;ADC HL,SP;2;4;15;4,4,4,3;-;-;-;rrcv0c;2;-;190
ED;7B;LD SP,(nn);4;6;20;4,4,3,3,3,3;-;-;-;------;2;-;103
ED;7C;NEG;2;2;8;-;-;-;-;rrcv1c;2;SY;30
ED;7D;RETN;2;4;14;-;-;-;-;------;2;SY;34
ED;7E;IM 2;2;2;8;-;-;-;-;------;2;SY;30
ED;7F;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;80;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;81;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;82;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;83;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;84;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;85;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;86;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;87;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;88;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;89;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;8A;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;8B;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;8C;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;8D;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;8E;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;8F;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;90;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;91;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;92;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;93;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;94;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;95;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;96;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;97;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;98;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;99;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;9A;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;9B;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;9C;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;9D;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;9E;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;9F;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;A0;LDI;2;4;16;4,4,3,5;-;-;-;--0b0-;2;-;130
ED;A1;CPI;2;4;16;4,4,3,5;-;-;-;rrcb1-;2;-;138
ED;A2;INI;2;4;16;4,5,4,3;-;-;-;xrxx1-;2;n2;298
ED;A3;OUTI;2;4;16;4,5,3,4;-;-;-;xrxx1-;2;n2;309
ED;A4;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;A5;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;A6;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;A7;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;A8;LDD;2;4;16;4,4,3,5;-;-;-;--0b0-;2;-;134
ED;A9;CPD;2;4;16;4,4,3,5;-;-;-;rrcb1-;2;-;141
ED;AA;IND;2;4;16;4,5,4,3;-;-;-;xrxx1-;2;n2;302
ED;AB;OUTD;2;4;16;4,5,3,4;-;-;-;xrxx1-;2;n2;313
ED;AC;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;AD;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;AE;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;AF;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;B0;LDIR;2;5;21;4,4,3,5,5;4;16;4,4,3,5;--0b0-;2;-;132
ED;B1;CPIR;2;5;21;4,4,3,5,5;4;16;4,4,3,5;rrcb1-;2;n5;139
ED;B2;INIR;2;5;21;4,5,4,3,5;4;16;4,5,4,3;x1xx1-;2;n2;300
ED;B3;OTIR;2;5;21;4,5,3,4,5;4;16;4,5,3,4;x1xx1-;2;n2;311
ED;B4;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;B5;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;B6;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;B7;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;B8;LDDR;2;5;21;4,4,3,5,5;4;16;4,4,3,5;--0b0-;2;R5;136
ED;B9;CPDR;2;5;21;4,4,3,5,5;4;16;4,4,3,5;rrcb1-;2;n5;142
ED;BA;INDR;2;5;21;4,5,4,3,5;4;16;4,5,4,3;x1xx1-;2;n2;304
ED;BB;OTDR;2;5;21;4,5,3,4,5;4;16;4,5,3,4;x1xx1-;2;n2;315
ED;BC;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;BD;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;BE;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;BF;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;C0;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;C1;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;C2;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;C3;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;C4;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;C5;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;C6;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;C7;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;C8;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;C9;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;CA;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;CB;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;CC;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;CD;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;CE;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;CF;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;D0;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;D1;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;D2;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;D3;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;D4;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;D5;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;D6;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;D7;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;D8;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;D9;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;DA;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;DB;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;DC;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;DD;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;DE;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;DF;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;E0;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;E1;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;E2;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;E3;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;E4;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;E5;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;E6;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;E7;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;E8;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;E9;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;EA;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;EB;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;EC;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;ED;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;EE;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;EF;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;F0;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;F1;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;F2;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;F3;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;F4;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;F5;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;F6;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;F7;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;F8;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;F9;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;FA;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;FB;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;FC;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;FD;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;FE;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
ED;FF;ED-NOP;2;2;8;-;-;-;-;------;2;SY;12
DD;00;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;01;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;02;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;03;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;04;INC B;2;2;8;-;-;-;-;rrcv0-;2;SY;30
DD;05;DEC B;2;2;8;-;-;-;-;rrcv1-;2;SY;30
DD;06;LD B,n;3;3;11;-;-;-;-;------;2;SY;26
DD;07;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;08;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;09;ADD IX,BC;2;4;15;4,4,4,3;-;-;-;--c-0c;2;-;194
DD;0A;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;0B;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;0C;INC C;2;2;8;-;-;-;-;rrcv0-;2;SY;30
DD;0D;DEC C;2;2;8;-;-;-;-;rrcv1-;2;SY;30
DD;0E;LD C,n;3;3;11;-;-;-;-;------;2;SY;26
DD;0F;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;10;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;11;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;12;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;13;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;14;INC D;2;2;8;-;-;-;-;rrcv0-;2;SY;30
DD;15;DEC D;2;2;8;-;-;-;-;rrcv1-;2;SY;30
DD;16;LD D,n;3;3;11;-;-;-;-;------;2;SY;26
DD;17;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;18;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;19;ADD IX,DE;2;4;15;4,4,4,3;-;-;-;--c-0c;2;-;194
DD;1A;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;1B;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;1C;INC E;2;2;8;-;-;-;-;rrcv0-;2;SY;30
DD;1D;DEC E;2;2;8;-;-;-;-;rrcv1-;2;SY;30
DD;1E;LD E,n;3;3;11;-;-;-;-;------;2;SY;26
DD;1F;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;20;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;21;LD IX,nn;4;4;14;4,4,3,3;-;-;-;------;2;-;100
DD;22;LD (nn),IX;4;6;20;4,4,3,3,3,3;-;-;-;------;2;-;110
DD;23;INC IX;2;2;10;4,6;-;-;-;------;2;-;199
DD;24;INC IXh;2;2;8;-;-;-;-;rrcv0-;2;SY;30
DD;25;DEC IXh;2;2;8;-;-;-;-;rrcv1-;2;SY;30
DD;26;LD IXh,n;3;3;11;-;-;-;-;------;2;SY;26
DD;27;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;28;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;29;ADD IX,IX;2;4;15;4,4,4,3;-;-;-;--c-0c;2;-;194
DD;2A;LD IX,(nn);4;6;20;4,4,3,3,3,3;-;-;-;------;2;-;105
DD;2B;DEC IX;2;2;10;4,6;-;-;-;------;2;-;202
DD;2C;INC IXl;2;2;8;-;-;-;-;rrcv0-;2;SY;30
DD;2D;DEC IXl;2;2;8;-;-;-;-;rrcv1-;2;SY;30
DD;2E;LD IXl,n;3;3;11;-;-;-;-;------;2;SY;26
DD;2F;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;30;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;31;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;32;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;33;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;34;INC (IX+d);3;6;23;4,4,3,5,4,3;-;-;-;rrcv0-;2;-;168
DD;35;DEC (IX+d);3;6;23;4,4,3,5,4,3;-;-;-;rrcv1-;2;-;170
DD;36;LD (IX+d),n;4;5;19;4,4,3,5,3;-;-;-;------;2;-;86
DD;37;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;38;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;39;ADD IX,SP;2;4;15;4,4,4,3;-;-;-;--c-0c;2;-;194
DD;3A;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;3B;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;3C;INC A;2;2;8;-;-;-;-;rrcv0-;2;SY;30
DD;3D;DEC A;2;2;8;-;-;-;-;rrcv1-;2;SY;30
DD;3E;LD A,n;3;3;11;-;-;-;-;------;2;SY;26
DD;3F;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;40;LD B,B;2;2;8;-;-;-;-;------;2;SY;26
DD;41;LD B,C;2;2;8;-;-;-;-;------;2;SY;26
DD;42;LD B,D;2;2;8;-;-;-;-;------;2;SY;26
DD;43;LD B,E;2;2;8;-;-;-;-;------;2;SY;26
DD;44;LD B,IXh;2;2;8;-;-;-;-;------;2;SY;26
DD;45;LD B,IXl;2;2;8;-;-;-;-;------;2;SY;26
DD;46;LD B,(IX+d);3;5;19;4,4,3,5,3;-;-;-;------;2;-;75
DD;47;LD B,A;2;2;8;-;-;-;-;------;2;SY;26
DD;48;LD C,B;2;2;8;-;-;-;-;------;2;SY;26
DD;49;LD C,C;2;2;8;-;-;-;-;------;2;SY;26
DD;4A;LD C,D;2;2;8;-;-;-;-;------;2;SY;26
DD;4B;LD C,E;2;2;8;-;-;-;-;------;2;SY;26
DD;4C;LD C,IXh;2;2;8;-;-;-;-;------;2;SY;26
DD;4D;LD C,IXl;2;2;8;-;-;-;-;------;2;SY;26
DD;4E;LD C,(IX+d);3;5;19;4,4,3,5,3;-;-;-;------;2;-;75
DD;4F;LD C,A;2;2;8;-;-;-;-;------;2;SY;26
DD;50;LD D,B;2;2;8;-;-;-;-;------;2;SY;26
DD;51;LD D,C;2;2;8;-;-;-;-;------;2;SY;26
DD;52;LD D,D;2;2;8;-;-;-;-;------;2;SY;26
DD;53;LD D,E;2;2;8;-;-;-;-;------;2;SY;26
DD;54;LD D,IXh;2;2;8;-;-;-;-;------;2;SY;26
DD;55;LD D,IXl;2;2;8;-;-;-;-;------;2;SY;26
DD;56;LD D,(IX+d);3;5;19;4,4,3,5,3;-;-;-;------;2;-;75
DD;57;LD D,A;2;2;8;-;-;-;-;------;2;SY;26
DD;58;LD E,B;2;2;8;-;-;-;-;------;2;SY;26
DD;59;LD E,C;2;2;8;-;-;-;-;------;2;SY;26
DD;5A;LD E,D;2;2;8;-;-;-;-;------;2;SY;26
DD;5B;LD E,E;2;2;8;-;-;-;-;------;2;SY;26
DD;5C;LD E,IXh;2;2;8;-;-;-;-;------;2;SY;26
DD;5D;LD E,IXl;2;2;8;-;-;-;-;------;2;SY;26
DD;5E;LD E,(IX+d);3;5;19;4,4,3,5,3;-;-;-;------;2;-;75
DD;5F;LD E,A;2;2;8;-;-;-;-;------;2;SY;26
DD;60;LD IXh,B;2;2;8;-;-;-;-;------;2;SY;26
DD;61;LD IXh,C;2;2;8;-;-;-;-;------;2;SY;26
DD;62;LD IXh,D;2;2;8;-;-;-;-;------;2;SY;26
DD;63;LD IXh,E;2;2;8;-;-;-;-;------;2;SY;26
DD;64;LD IXh,IXh;2;2;8;-;-;-;-;------;2;SY;26
DD;65;LD IXh,IXl;2;2;8;-;-;-;-;------;2;SY;26
DD;66;LD H,(IX+d);3;5;19;4,4,3,5,3;-;-;-;------;2;-;75
DD;67;LD IXh,A;2;2;8;-;-;-;-;------;2;SY;26
DD;68;LD IXl,B;2;2;8;-;-;-;-;------;2;SY;26
DD;69;LD IXl,C;2;2;8;-;-;-;-;------;2;SY;26
DD;6A;LD IXl,D;2;2;8;-;-;-;-;------;2;SY;26
DD;6B;LD IXl,E;2;2;8;-;-;-;-;------;2;SY;26
DD;6C;LD IXl,IXh;2;2;8;-;-;-;-;------;2;SY;26
DD;6D;LD IXl,IXl;2;2;8;-;-;-;-;------;2;SY;26
DD;6E;LD L,(IX+d);3;5;19;4,4,3,5,3;-;-;-;------;2;-;75
DD;6F;LD IXl,A;2;2;8;-;-;-;-;------;2;SY;26
DD;70;LD (IX+d),B;3;5;19;4,4,3,5,3;-;-;-;------;2;-;81
DD;71;LD (IX+d),C;3;5;19;4,4,3,5,3;-;-;-;------;2;-;81
DD;72;LD (IX+d),D;3;5;19;4,4,3,5,3;-;-;-;------;2;-;81
DD;73;LD (IX+d),E;3;5;19;4,4,3,5,3;-;-;-;------;2;-;81
DD;74;LD (IX+d),H;3;5;19;4,4,3,5,3;-;-;-;------;2;-;81
DD;75;LD (IX+d),L;3;5;19;4,4,3,5,3;-;-;-;------;2;-;81
DD;76;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;77;LD (IX+d),A;3;5;19;4,4,3,5,3;-;-;-;------;2;-;81
DD;78;LD A,B;2;2;8;-;-;-;-;------;2;SY;26
DD;79;LD A,C;2;2;8;-;-;-;-;------;2;SY;26
DD;7A;LD A,D;2;2;8;-;-;-;-;------;2;SY;26
DD;7B;LD A,E;2;2;8;-;-;-;-;------;2;SY;26
DD;7C;LD A,IXh;2;2;8;-;-;-;-;------;2;SY;26
DD;7D;LD A,IXl;2;2;8;-;-;-;-;------;2;SY;26
DD;7E;LD A,(IX+d);3;5;19;4,4,3,5,3;-;-;-;------;2;-;75
DD;7F;LD A,A;2;2;8;-;-;-;-;------;2;SY;26
DD;80;ADD A,B;2;2;8;-;-;-;-;rrcv0c;2;SY;30
DD;81;ADD A,C;2;2;8;-;-;-;-;rrcv0c;2;SY;30
DD;82;ADD A,D;2;2;8;-;-;-;-;rrcv0c;2;SY;30
DD;83;ADD A,E;2;2;8;-;-;-;-;rrcv0c;2;SY;30
DD;84;ADD A,IXh;2;2;8;-;-;-;-;rrcv0c;2;SY;30
DD;85;ADD A,IXl;2;2;8;-;-;-;-;rrcv0c;2;SY;30
DD;86;ADD A,(IX+d);3;5;19;4,4,3,5,3;-;-;-;rrcv0c;2;-;149
DD;87;ADD A,A;2;2;8;-;-;-;-;rrcv0c;2;SY;30
DD;88;ADC A,B;2;2;8;-;-;-;-;rrcv0c;2;SY;30
DD;89;ADC A,C;2;2;8;-;-;-;-;rrcv0c;2;SY;30
DD;8A;ADC A,D;2;2;8;-;-;-;-;rrcv0c;2;SY;30
DD;8B;ADC A,E;2;2;8;-;-;-;-;rrcv0c;2;SY;30
DD;8C;ADC A,IXh;2;2;8;-;-;-;-;rrcv0c;2;SY;30
DD;8D;ADC A,IXl;2;2;8;-;-;-;-;rrcv0c;2;SY;30
DD;8E;ADC A,(IX+d);3;5;19;4,4,3,5,3;-;-;-;rrcv0c;2;-;151
DD;8F;ADC A,A;2;2;8;-;-;-;-;rrcv0c;2;SY;30
DD;90;SUB B;2;2;8;-;-;-;-;rrcv1c;2;SY;30
DD;91;SUB C;2;2;8;-;-;-;-;rrcv1c;2;SY;30
DD;92;SUB D;2;2;8;-;-;-;-;rrcv1c;2;SY;30
DD;93;SUB E;2;2;8;-;-;-;-;rrcv1c;2;SY;30
DD;94;SUB IXh;2;2;8;-;-;-;-;rrcv1c;2;SY;30
DD;95;SUB IXl;2;2;8;-;-;-;-;rrcv1c;2;SY;30
DD;96;SUB (IX+d);3;5;19;4,4,3,5,3;-;-;-;rrcv1c;2;-;153
DD;97;SUB A;2;2;8;-;-;-;-;rrcv1c;2;SY;30
DD;98;SBC A,B;2;2;8;-;-;-;-;rrcv1c;2;SY;30
DD;99;SBC A,C;2;2;8;-;-;-;-;rrcv1c;2;SY;30
DD;9A;SBC A,D;2;2;8;-;-;-;-;rrcv1c;2;SY;30
DD;9B;SBC A,E;2;2;8;-;-;-;-;rrcv1c;2;SY;30
DD;9C;SBC A,IXh;2;2;8;-;-;-;-;rrcv1c;2;SY;30
DD;9D;SBC A,IXl;2;2;8;-;-;-;-;rrcv1c;2;SY;30
DD;9E;SBC A,(IX+d);3;5;19;4,4,3,5,3;-;-;-;rrcv1c;2;R4;155
DD;9F;SBC A,A;2;2;8;-;-;-;-;rrcv1c;2;SY;30
DD;A0;AND B;2;2;8;-;-;-;-;rr1p00;2;SY;30
DD;A1;AND C;2;2;8;-;-;-;-;rr1p00;2;SY;30
DD;A2;AND D;2;2;8;-;-;-;-;rr1p00;2;SY;30
DD;A3;AND E;2;2;8;-;-;-;-;rr1p00;2;SY;30
DD;A4;AND IXh;2;2;8;-;-;-;-;rr1p00;2;SY;30
DD;A5;AND IXl;2;2;8;-;-;-;-;rr1p00;2;SY;30
DD;A6;AND (IX+d);3;5;19;4,4,3,5,3;-;-;-;rr1p00;2;R4;157
DD;A7;AND A;2;2;8;-;-;-;-;rr1p00;2;SY;30
DD;A8;XOR B;2;2;8;-;-;-;-;rr0p00;2;SY;30
DD;A9;XOR C;2;2;8;-;-;-;-;rr0p00;2;SY;30
DD;AA;XOR D;2;2;8;-;-;-;-;rr0p00;2;SY;30
DD;AB;XOR E;2;2;8;-;-;-;-;rr0p00;2;SY;30
DD;AC;XOR IXh;2;2;8;-;-;-;-;rr0p00;2;SY;30
DD;AD;XOR IXl;2;2;8;-;-;-;-;rr0p00;2;SY;30
DD;AE;XOR (IX+d);3;5;19;4,4,3,5,3;-;-;-;rr0p00;2;-;161
DD;AF;XOR A;2;2;8;-;-;-;-;rr0p00;2;SY;30
DD;B0;OR B;2;2;8;-;-;-;-;rr0p00;2;SY;30
DD;B1;OR C;2;2;8;-;-;-;-;rr0p00;2;SY;30
DD;B2;OR D;2;2;8;-;-;-;-;rr0p00;2;SY;30
DD;B3;OR E;2;2;8;-;-;-;-;rr0p00;2;SY;30
DD;B4;OR IXh;2;2;8;-;-;-;-;rr0p00;2;SY;30
DD;B5;OR IXl;2;2;8;-;-;-;-;rr0p00;2;SY;30
DD;B6;OR (IX+d);3;5;19;4,4,3,5,3;-;-;-;rr0p00;2;R4;159
DD;B7;OR A;2;2;8;-;-;-;-;rr0p00;2;SY;30
DD;B8;CP B;2;2;8;-;-;-;-;rrcv1c;2;SY;30
DD;B9;CP C;2;2;8;-;-;-;-;rrcv1c;2;SY;30
DD;BA;CP D;2;2;8;-;-;-;-;rrcv1c;2;SY;30
DD;BB;CP E;2;2;8;-;-;-;-;rrcv1c;2;SY;30
DD;BC;CP IXh;2;2;8;-;-;-;-;rrcv1c;2;SY;30
DD;BD;CP IXl;2;2;8;-;-;-;-;rrcv1c;2;SY;30
DD;BE;CP (IX+d);3;5;19;4,4,3,5,3;-;-;-;rrcv1c;2;-;163
DD;BF;CP A;2;2;8;-;-;-;-;rrcv1c;2;SY;30
DD;C0;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;C1;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;C2;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;C3;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;C4;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;C5;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;C6;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;C7;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;C8;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;C9;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;CA;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;CB;SUBPREFIX_CB;-;-;-;-;-;-;-;-;-;-;217
DD;CC;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;CD;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;CE;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;CF;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;D0;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;D1;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;D2;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;D3;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;D4;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;D5;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;D6;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;D7;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;D8;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;D9;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;DA;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;DB;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;DC;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;DD;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;DE;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;DF;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;E0;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;E1;POP IX;2;4;14;4,4,3,3;-;-;-;------;2;-;121
DD;E2;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;E3;EX (SP),IX;2;6;23;4,4,3,4,3,5;-;-;-;------;2;-;128
DD;E4;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;E5;PUSH IX;2;4;15;4,5,3,3;-;-;-;------;2;-;117
DD;E6;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;E7;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;E8;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;E9;JP (IX);2;2;8;4,4;-;-;-;------;2;-;276
DD;EA;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;EB;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;EC;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;ED;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;EE;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;EF;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;F0;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;F1;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;F2;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;F3;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;F4;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;F5;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;F6;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;F7;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;F8;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;F9;LD SP,IX;2;2;10;4,6;-;-;-;------;2;-;113
DD;FA;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;FB;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;FC;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;FD;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;FE;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DD;FF;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;00;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;01;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;02;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;03;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;04;INC B;2;2;8;-;-;-;-;rrcv0-;2;SY;30
FD;05;DEC B;2;2;8;-;-;-;-;rrcv1-;2;SY;30
FD;06;LD B,n;3;3;11;-;-;-;-;------;2;SY;26
FD;07;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;08;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;09;ADD IY,BC;2;4;15;4,4,4,3;-;-;-;--c-0c;2;-;196
FD;0A;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;0B;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;0C;INC C;2;2;8;-;-;-;-;rrcv0-;2;SY;30
FD;0D;DEC C;2;2;8;-;-;-;-;rrcv1-;2;SY;30
FD;0E;LD C,n;3;3;11;-;-;-;-;------;2;SY;26
FD;0F;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;10;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;11;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;12;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;13;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;14;INC D;2;2;8;-;-;-;-;rrcv0-;2;SY;30
FD;15;DEC D;2;2;8;-;-;-;-;rrcv1-;2;SY;30
FD;16;LD D,n;3;3;11;-;-;-;-;------;2;SY;26
FD;17;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;18;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;19;ADD IY,DE;2;4;15;4,4,4,3;-;-;-;--c-0c;2;-;196
FD;1A;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;1B;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;1C;INC E;2;2;8;-;-;-;-;rrcv0-;2;SY;30
FD;1D;DEC E;2;2;8;-;-;-;-;rrcv1-;2;SY;30
FD;1E;LD E,n;3;3;11;-;-;-;-;------;2;SY;26
FD;1F;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;20;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;21;LD IY,nn;4;4;14;4,4,3,3;-;-;-;------;2;-;101
FD;22;LD (nn),IY;4;6;20;4,4,3,3,3,3;-;-;-;------;2;-;111
FD;23;INC IY;2;2;10;4,6;-;-;-;------;2;-;200
FD;24;INC IYh;2;2;8;-;-;-;-;rrcv0-;2;SY;30
FD;25;DEC IYh;2;2;8;-;-;-;-;rrcv1-;2;SY;30
FD;26;LD IYh,n;3;3;11;-;-;-;-;------;2;SY;26
FD;27;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;28;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;29;ADD IY,IY;2;4;15;4,4,4,3;-;-;-;--c-0c;2;-;196
FD;2A;LD IY,(nn);4;6;20;4,4,3,3,3,3;-;-;-;------;2;-;106
FD;2B;DEC IY;2;2;10;4,6;-;-;-;------;2;-;203
FD;2C;INC IYl;2;2;8;-;-;-;-;rrcv0-;2;SY;30
FD;2D;DEC IYl;2;2;8;-;-;-;-;rrcv1-;2;SY;30
FD;2E;LD IYl,n;3;3;11;-;-;-;-;------;2;SY;26
FD;2F;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;30;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;31;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;32;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;33;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;34;INC (IY+d);3;6;23;4,4,3,5,4,3;-;-;-;rrcv0-;2;-;169
FD;35;DEC (IY+d);3;6;23;4,4,3,5,4,3;-;-;-;rrcv1-;2;-;170
FD;36;LD (IY+d),n;4;5;19;4,4,3,5,3;-;-;-;------;2;-;87
FD;37;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;38;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;39;ADD IY,SP;2;4;15;4,4,4,3;-;-;-;--c-0c;2;-;196
FD;3A;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;3B;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;3C;INC A;2;2;8;-;-;-;-;rrcv0-;2;SY;30
FD;3D;DEC A;2;2;8;-;-;-;-;rrcv1-;2;SY;30
FD;3E;LD A,n;3;3;11;-;-;-;-;------;2;SY;26
FD;3F;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;40;LD B,B;2;2;8;-;-;-;-;------;2;SY;26
FD;41;LD B,C;2;2;8;-;-;-;-;------;2;SY;26
FD;42;LD B,D;2;2;8;-;-;-;-;------;2;SY;26
FD;43;LD B,E;2;2;8;-;-;-;-;------;2;SY;26
FD;44;LD B,IYh;2;2;8;-;-;-;-;------;2;SY;26
FD;45;LD B,IYl;2;2;8;-;-;-;-;------;2;SY;26
FD;46;LD B,(IY+d);3;5;19;4,4,3,5,3;-;-;-;------;2;-;77
FD;47;LD B,A;2;2;8;-;-;-;-;------;2;SY;26
FD;48;LD C,B;2;2;8;-;-;-;-;------;2;SY;26
FD;49;LD C,C;2;2;8;-;-;-;-;------;2;SY;26
FD;4A;LD C,D;2;2;8;-;-;-;-;------;2;SY;26
FD;4B;LD C,E;2;2;8;-;-;-;-;------;2;SY;26
FD;4C;LD C,IYh;2;2;8;-;-;-;-;------;2;SY;26
FD;4D;LD C,IYl;2;2;8;-;-;-;-;------;2;SY;26
FD;4E;LD C,(IY+d);3;5;19;4,4,3,5,3;-;-;-;------;2;-;77
FD;4F;LD C,A;2;2;8;-;-;-;-;------;2;SY;26
FD;50;LD D,B;2;2;8;-;-;-;-;------;2;SY;26
FD;51;LD D,C;2;2;8;-;-;-;-;------;2;SY;26
FD;52;LD D,D;2;2;8;-;-;-;-;------;2;SY;26
FD;53;LD D,E;2;2;8;-;-;-;-;------;2;SY;26
FD;54;LD D,IYh;2;2;8;-;-;-;-;------;2;SY;26
FD;55;LD D,IYl;2;2;8;-;-;-;-;------;2;SY;26
FD;56;LD D,(IY+d);3;5;19;4,4,3,5,3;-;-;-;------;2;-;77
FD;57;LD D,A;2;2;8;-;-;-;-;------;2;SY;26
FD;58;LD E,B;2;2;8;-;-;-;-;------;2;SY;26
FD;59;LD E,C;2;2;8;-;-;-;-;------;2;SY;26
FD;5A;LD E,D;2;2;8;-;-;-;-;------;2;SY;26
FD;5B;LD E,E;2;2;8;-;-;-;-;------;2;SY;26
FD;5C;LD E,IYh;2;2;8;-;-;-;-;------;2;SY;26
FD;5D;LD E,IYl;2;2;8;-;-;-;-;------;2;SY;26
FD;5E;LD E,(IY+d);3;5;19;4,4,3,5,3;-;-;-;------;2;-;77
FD;5F;LD E,A;2;2;8;-;-;-;-;------;2;SY;26
FD;60;LD IYh,B;2;2;8;-;-;-;-;------;2;SY;26
FD;61;LD IYh,C;2;2;8;-;-;-;-;------;2;SY;26
FD;62;LD IYh,D;2;2;8;-;-;-;-;------;2;SY;26
FD;63;LD IYh,E;2;2;8;-;-;-;-;------;2;SY;26
FD;64;LD IYh,IYh;2;2;8;-;-;-;-;------;2;SY;26
FD;65;LD IYh,IYl;2;2;8;-;-;-;-;------;2;SY;26
FD;66;LD H,(IY+d);3;5;19;4,4,3,5,3;-;-;-;------;2;-;77
FD;67;LD IYh,A;2;2;8;-;-;-;-;------;2;SY;26
FD;68;LD IYl,B;2;2;8;-;-;-;-;------;2;SY;26
FD;69;LD IYl,C;2;2;8;-;-;-;-;------;2;SY;26
FD;6A;LD IYl,D;2;2;8;-;-;-;-;------;2;SY;26
FD;6B;LD IYl,E;2;2;8;-;-;-;-;------;2;SY;26
FD;6C;LD IYl,IYh;2;2;8;-;-;-;-;------;2;SY;26
FD;6D;LD IYl,IYl;2;2;8;-;-;-;-;------;2;SY;26
FD;6E;LD L,(IY+d);3;5;19;4,4,3,5,3;-;-;-;------;2;-;77
FD;6F;LD IYl,A;2;2;8;-;-;-;-;------;2;SY;26
FD;70;LD (IY+d),B;3;5;19;4,4,3,5,3;-;-;-;------;2;-;83
FD;71;LD (IY+d),C;3;5;19;4,4,3,5,3;-;-;-;------;2;-;83
FD;72;LD (IY+d),D;3;5;19;4,4,3,5,3;-;-;-;------;2;-;83
FD;73;LD (IY+d),E;3;5;19;4,4,3,5,3;-;-;-;------;2;-;83
FD;74;LD (IY+d),H;3;5;19;4,4,3,5,3;-;-;-;------;2;-;83
FD;75;LD (IY+d),L;3;5;19;4,4,3,5,3;-;-;-;------;2;-;83
FD;76;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;77;LD (IY+d),A;3;5;19;4,4,3,5,3;-;-;-;------;2;-;83
FD;78;LD A,B;2;2;8;-;-;-;-;------;2;SY;26
FD;79;LD A,C;2;2;8;-;-;-;-;------;2;SY;26
FD;7A;LD A,D;2;2;8;-;-;-;-;------;2;SY;26
FD;7B;LD A,E;2;2;8;-;-;-;-;------;2;SY;26
FD;7C;LD A,IYh;2;2;8;-;-;-;-;------;2;SY;26
FD;7D;LD A,IYl;2;2;8;-;-;-;-;------;2;SY;26
FD;7E;LD A,(IY+d);3;5;19;4,4,3,5,3;-;-;-;------;2;-;77
FD;7F;LD A,A;2;2;8;-;-;-;-;------;2;SY;26
FD;80;ADD A,B;2;2;8;-;-;-;-;rrcv0c;2;SY;30
FD;81;ADD A,C;2;2;8;-;-;-;-;rrcv0c;2;SY;30
FD;82;ADD A,D;2;2;8;-;-;-;-;rrcv0c;2;SY;30
FD;83;ADD A,E;2;2;8;-;-;-;-;rrcv0c;2;SY;30
FD;84;ADD A,IYh;2;2;8;-;-;-;-;rrcv0c;2;SY;30
FD;85;ADD A,IYl;2;2;8;-;-;-;-;rrcv0c;2;SY;30
FD;86;ADD A,(IY+d);3;5;19;4,4,3,5,3;-;-;-;rrcv0c;2;-;150
FD;87;ADD A,A;2;2;8;-;-;-;-;rrcv0c;2;SY;30
FD;88;ADC A,B;2;2;8;-;-;-;-;rrcv0c;2;SY;30
FD;89;ADC A,C;2;2;8;-;-;-;-;rrcv0c;2;SY;30
FD;8A;ADC A,D;2;2;8;-;-;-;-;rrcv0c;2;SY;30
FD;8B;ADC A,E;2;2;8;-;-;-;-;rrcv0c;2;SY;30
FD;8C;ADC A,IYh;2;2;8;-;-;-;-;rrcv0c;2;SY;30
FD;8D;ADC A,IYl;2;2;8;-;-;-;-;rrcv0c;2;SY;30
FD;8E;ADC A,(IY+d);3;5;19;4,4,3,5,3;-;-;-;rrcv0c;2;-;151
FD;8F;ADC A,A;2;2;8;-;-;-;-;rrcv0c;2;SY;30
FD;90;SUB B;2;2;8;-;-;-;-;rrcv1c;2;SY;30
FD;91;SUB C;2;2;8;-;-;-;-;rrcv1c;2;SY;30
FD;92;SUB D;2;2;8;-;-;-;-;rrcv1c;2;SY;30
FD;93;SUB E;2;2;8;-;-;-;-;rrcv1c;2;SY;30
FD;94;SUB IYh;2;2;8;-;-;-;-;rrcv1c;2;SY;30
FD;95;SUB IYl;2;2;8;-;-;-;-;rrcv1c;2;SY;30
FD;96;SUB (IY+d);3;5;19;4,4,3,5,3;-;-;-;rrcv1c;2;-;153
FD;97;SUB A;2;2;8;-;-;-;-;rrcv1c;2;SY;30
FD;98;SBC A,B;2;2;8;-;-;-;-;rrcv1c;2;SY;30
FD;99;SBC A,C;2;2;8;-;-;-;-;rrcv1c;2;SY;30
FD;9A;SBC A,D;2;2;8;-;-;-;-;rrcv1c;2;SY;30
FD;9B;SBC A,E;2;2;8;-;-;-;-;rrcv1c;2;SY;30
FD;9C;SBC A,IYh;2;2;8;-;-;-;-;rrcv1c;2;SY;30
FD;9D;SBC A,IYl;2;2;8;-;-;-;-;rrcv1c;2;SY;30
FD;9E;SBC A,(IY+d);3;5;19;4,4,3,5,3;-;-;-;rrcv1c;2;R4;155
FD;9F;SBC A,A;2;2;8;-;-;-;-;rrcv1c;2;SY;30
FD;A0;AND B;2;2;8;-;-;-;-;rr1p00;2;SY;30
FD;A1;AND C;2;2;8;-;-;-;-;rr1p00;2;SY;30
FD;A2;AND D;2;2;8;-;-;-;-;rr1p00;2;SY;30
FD;A3;AND E;2;2;8;-;-;-;-;rr1p00;2;SY;30
FD;A4;AND IYh;2;2;8;-;-;-;-;rr1p00;2;SY;30
FD;A5;AND IYl;2;2;8;-;-;-;-;rr1p00;2;SY;30
FD;A6;AND (IY+d);3;5;19;4,4,3,5,3;-;-;-;rr1p00;2;R4;157
FD;A7;AND A;2;2;8;-;-;-;-;rr1p00;2;SY;30
FD;A8;XOR B;2;2;8;-;-;-;-;rr0p00;2;SY;30
FD;A9;XOR C;2;2;8;-;-;-;-;rr0p00;2;SY;30
FD;AA;XOR D;2;2;8;-;-;-;-;rr0p00;2;SY;30
FD;AB;XOR E;2;2;8;-;-;-;-;rr0p00;2;SY;30
FD;AC;XOR IYh;2;2;8;-;-;-;-;rr0p00;2;SY;30
FD;AD;XOR IYl;2;2;8;-;-;-;-;rr0p00;2;SY;30
FD;AE;XOR (IY+d);3;5;19;4,4,3,5,3;-;-;-;rr0p00;2;-;161
FD;AF;XOR A;2;2;8;-;-;-;-;rr0p00;2;SY;30
FD;B0;OR B;2;2;8;-;-;-;-;rr0p00;2;SY;30
FD;B1;OR C;2;2;8;-;-;-;-;rr0p00;2;SY;30
FD;B2;OR D;2;2;8;-;-;-;-;rr0p00;2;SY;30
FD;B3;OR E;2;2;8;-;-;-;-;rr0p00;2;SY;30
FD;B4;OR IYh;2;2;8;-;-;-;-;rr0p00;2;SY;30
FD;B5;OR IYl;2;2;8;-;-;-;-;rr0p00;2;SY;30
FD;B6;OR (IY+d);3;5;19;4,4,3,5,3;-;-;-;rr0p00;2;R4;159
FD;B7;OR A;2;2;8;-;-;-;-;rr0p00;2;SY;30
FD;B8;CP B;2;2;8;-;-;-;-;rrcv1c;2;SY;30
FD;B9;CP C;2;2;8;-;-;-;-;rrcv1c;2;SY;30
FD;BA;CP D;2;2;8;-;-;-;-;rrcv1c;2;SY;30
FD;BB;CP E;2;2;8;-;-;-;-;rrcv1c;2;SY;30
FD;BC;CP IYh;2;2;8;-;-;-;-;rrcv1c;2;SY;30
FD;BD;CP IYl;2;2;8;-;-;-;-;rrcv1c;2;SY;30
FD;BE;CP (IY+d);3;5;19;4,4,3,5,3;-;-;-;rrcv1c;2;-;163
FD;BF;CP A;2;2;8;-;-;-;-;rrcv1c;2;SY;30
FD;C0;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;C1;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;C2;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;C3;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;C4;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;C5;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;C6;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;C7;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;C8;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;C9;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;CA;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;CB;SUBPREFIX_CB;-;-;-;-;-;-;-;-;-;-;219
FD;CC;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;CD;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;CE;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;CF;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;D0;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;D1;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;D2;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;D3;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;D4;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;D5;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;D6;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;D7;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;D8;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;D9;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;DA;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;DB;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;DC;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;DD;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;DE;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;DF;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;E0;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;E1;POP IY;2;4;14;4,4,3,3;-;-;-;------;2;-;122
FD;E2;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;E3;EX (SP),IY;2;6;23;4,4,3,4,3,5;-;-;-;------;2;-;129
FD;E4;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;E5;PUSH IY;2;4;15;4,5,3,3;-;-;-;------;2;-;118
FD;E6;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;E7;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;E8;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;E9;JP (IY);2;2;8;4,4;-;-;-;------;2;-;277
FD;EA;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;EB;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;EC;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;ED;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;EE;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;EF;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;F0;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;F1;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;F2;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;F3;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;F4;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;F5;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;F6;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;F7;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;F8;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;F9;LD SP,IY;2;2;10;4,6;-;-;-;------;2;-;114
FD;FA;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;FB;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;FC;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;FD;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;FE;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
FD;FF;UNASSIGNED;-;-;-;-;-;-;-;-;-;-;-
DDCB;00;RLC (IX+d),B;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;01;RLC (IX+d),C;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;02;RLC (IX+d),D;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;03;RLC (IX+d),E;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;04;RLC (IX+d),H;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;05;RLC (IX+d),L;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;06;RLC (IX+d);4;6;23;4,4,3,5,4,3;-;-;-;rr0p0c;2;-;217
DDCB;07;RLC (IX+d),A;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;08;RRC (IX+d),B;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;09;RRC (IX+d),C;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;0A;RRC (IX+d),D;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;0B;RRC (IX+d),E;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;0C;RRC (IX+d),H;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;0D;RRC (IX+d),L;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;0E;RRC (IX+d);4;6;23;4,4,3,5,4,3;-;-;-;rr0p0c;2;-;224
DDCB;0F;RRC (IX+d),A;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;10;RL (IX+d),B;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;11;RL (IX+d),C;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;12;RL (IX+d),D;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;13;RL (IX+d),E;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;14;RL (IX+d),H;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;15;RL (IX+d),L;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;16;RL (IX+d);4;6;23;4,4,3,5,4,3;-;-;-;rr0p0c;2;-;221
DDCB;17;RL (IX+d),A;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;18;RR (IX+d),B;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;19;RR (IX+d),C;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;1A;RR (IX+d),D;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;1B;RR (IX+d),E;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;1C;RR (IX+d),H;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;1D;RR (IX+d),L;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;1E;RR (IX+d);4;6;23;4,4,3,5,4,3;-;-;-;rr0p0c;2;-;227
DDCB;1F;RR (IX+d),A;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;20;SLA (IX+d),B;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;21;SLA (IX+d),C;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;22;SLA (IX+d),D;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;23;SLA (IX+d),E;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;24;SLA (IX+d),H;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;25;SLA (IX+d),L;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;26;SLA (IX+d);4;6;23;4,4,3,5,4,3;-;-;-;rr0p0c;2;-;230
DDCB;27;SLA (IX+d),A;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;28;SRA (IX+d),B;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;29;SRA (IX+d),C;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;2A;SRA (IX+d),D;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;2B;SRA (IX+d),E;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;2C;SRA (IX+d),H;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;2D;SRA (IX+d),L;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;2E;SRA (IX+d);4;6;23;4,4,3,5,4,3;-;-;-;rr0p0c;2;-;233
DDCB;2F;SRA (IX+d),A;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;30;SLL (IX+d),B;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;31;SLL (IX+d),C;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;32;SLL (IX+d),D;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;33;SLL (IX+d),E;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;34;SLL (IX+d),H;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;35;SLL (IX+d),L;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;36;SLL (IX+d);4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;37;SLL (IX+d),A;4;6;23;-;-;-;-;rr0p0c;2;SY;32
DDCB;38;SRL (IX+d),B;4;6;23;-;-;-;-;0r0p0c;2;SY;32
DDCB;39;SRL (IX+d),C;4;6;23;-;-;-;-;0r0p0c;2;SY;32
DDCB;3A;SRL (IX+d),D;4;6;23;-;-;-;-;0r0p0c;2;SY;32
DDCB;3B;SRL (IX+d),E;4;6;23;-;-;-;-;0r0p0c;2;SY;32
DDCB;3C;SRL (IX+d),H;4;6;23;-;-;-;-;0r0p0c;2;SY;32
DDCB;3D;SRL (IX+d),L;4;6;23;-;-;-;-;0r0p0c;2;SY;32
DDCB;3E;SRL (IX+d);4;6;23;4,4,3,5,4,3;-;-;-;0r0p0c;2;-;236
DDCB;3F;SRL (IX+d),A;4;6;23;-;-;-;-;0r0p0c;2;SY;32
DDCB;40;BIT 0,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;41;BIT 0,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;42;BIT 0,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;43;BIT 0,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;44;BIT 0,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;45;BIT 0,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;46;BIT 0,(IX+d);4;5;20;4,4,3,5,4;-;-;-;xz1x0-;2;-;247
DDCB;47;BIT 0,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;48;BIT 1,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;49;BIT 1,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;4A;BIT 1,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;4B;BIT 1,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;4C;BIT 1,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;4D;BIT 1,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;4E;BIT 1,(IX+d);4;5;20;4,4,3,5,4;-;-;-;xz1x0-;2;-;247
DDCB;4F;BIT 1,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;50;BIT 2,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;51;BIT 2,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;52;BIT 2,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;53;BIT 2,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;54;BIT 2,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;55;BIT 2,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;56;BIT 2,(IX+d);4;5;20;4,4,3,5,4;-;-;-;xz1x0-;2;-;247
DDCB;57;BIT 2,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;58;BIT 3,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;59;BIT 3,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;5A;BIT 3,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;5B;BIT 3,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;5C;BIT 3,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;5D;BIT 3,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;5E;BIT 3,(IX+d);4;5;20;4,4,3,5,4;-;-;-;xz1x0-;2;-;247
DDCB;5F;BIT 3,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;60;BIT 4,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;61;BIT 4,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;62;BIT 4,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;63;BIT 4,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;64;BIT 4,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;65;BIT 4,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;66;BIT 4,(IX+d);4;5;20;4,4,3,5,4;-;-;-;xz1x0-;2;-;247
DDCB;67;BIT 4,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;68;BIT 5,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;69;BIT 5,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;6A;BIT 5,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;6B;BIT 5,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;6C;BIT 5,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;6D;BIT 5,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;6E;BIT 5,(IX+d);4;5;20;4,4,3,5,4;-;-;-;xz1x0-;2;-;247
DDCB;6F;BIT 5,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;70;BIT 6,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;71;BIT 6,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;72;BIT 6,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;73;BIT 6,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;74;BIT 6,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;75;BIT 6,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;76;BIT 6,(IX+d);4;5;20;4,4,3,5,4;-;-;-;xz1x0-;2;-;247
DDCB;77;BIT 6,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;78;BIT 7,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;79;BIT 7,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;7A;BIT 7,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;7B;BIT 7,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;7C;BIT 7,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;7D;BIT 7,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;7E;BIT 7,(IX+d);4;5;20;4,4,3,5,4;-;-;-;xz1x0-;2;-;247
DDCB;7F;BIT 7,(IX+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
DDCB;80;RES 0,(IX+d),B;4;6;23;-;-;-;-;------;2;SY;33
DDCB;81;RES 0,(IX+d),C;4;6;23;-;-;-;-;------;2;SY;33
DDCB;82;RES 0,(IX+d),D;4;6;23;-;-;-;-;------;2;SY;33
DDCB;83;RES 0,(IX+d),E;4;6;23;-;-;-;-;------;2;SY;33
DDCB;84;RES 0,(IX+d),H;4;6;23;-;-;-;-;------;2;SY;33
DDCB;85;RES 0,(IX+d),L;4;6;23;-;-;-;-;------;2;SY;33
DDCB;86;RES 0,(IX+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;259
DDCB;87;RES 0,(IX+d),A;4;6;23;-;-;-;-;------;2;SY;33
DDCB;88;RES 1,(IX+d),B;4;6;23;-;-;-;-;------;2;SY;33
DDCB;89;RES 1,(IX+d),C;4;6;23;-;-;-;-;------;2;SY;33
DDCB;8A;RES 1,(IX+d),D;4;6;23;-;-;-;-;------;2;SY;33
DDCB;8B;RES 1,(IX+d),E;4;6;23;-;-;-;-;------;2;SY;33
DDCB;8C;RES 1,(IX+d),H;4;6;23;-;-;-;-;------;2;SY;33
DDCB;8D;RES 1,(IX+d),L;4;6;23;-;-;-;-;------;2;SY;33
DDCB;8E;RES 1,(IX+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;259
DDCB;8F;RES 1,(IX+d),A;4;6;23;-;-;-;-;------;2;SY;33
DDCB;90;RES 2,(IX+d),B;4;6;23;-;-;-;-;------;2;SY;33
DDCB;91;RES 2,(IX+d),C;4;6;23;-;-;-;-;------;2;SY;33
DDCB;92;RES 2,(IX+d),D;4;6;23;-;-;-;-;------;2;SY;33
DDCB;93;RES 2,(IX+d),E;4;6;23;-;-;-;-;------;2;SY;33
DDCB;94;RES 2,(IX+d),H;4;6;23;-;-;-;-;------;2;SY;33
DDCB;95;RES 2,(IX+d),L;4;6;23;-;-;-;-;------;2;SY;33
DDCB;96;RES 2,(IX+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;259
DDCB;97;RES 2,(IX+d),A;4;6;23;-;-;-;-;------;2;SY;33
DDCB;98;RES 3,(IX+d),B;4;6;23;-;-;-;-;------;2;SY;33
DDCB;99;RES 3,(IX+d),C;4;6;23;-;-;-;-;------;2;SY;33
DDCB;9A;RES 3,(IX+d),D;4;6;23;-;-;-;-;------;2;SY;33
DDCB;9B;RES 3,(IX+d),E;4;6;23;-;-;-;-;------;2;SY;33
DDCB;9C;RES 3,(IX+d),H;4;6;23;-;-;-;-;------;2;SY;33
DDCB;9D;RES 3,(IX+d),L;4;6;23;-;-;-;-;------;2;SY;33
DDCB;9E;RES 3,(IX+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;259
DDCB;9F;RES 3,(IX+d),A;4;6;23;-;-;-;-;------;2;SY;33
DDCB;A0;RES 4,(IX+d),B;4;6;23;-;-;-;-;------;2;SY;33
DDCB;A1;RES 4,(IX+d),C;4;6;23;-;-;-;-;------;2;SY;33
DDCB;A2;RES 4,(IX+d),D;4;6;23;-;-;-;-;------;2;SY;33
DDCB;A3;RES 4,(IX+d),E;4;6;23;-;-;-;-;------;2;SY;33
DDCB;A4;RES 4,(IX+d),H;4;6;23;-;-;-;-;------;2;SY;33
DDCB;A5;RES 4,(IX+d),L;4;6;23;-;-;-;-;------;2;SY;33
DDCB;A6;RES 4,(IX+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;259
DDCB;A7;RES 4,(IX+d),A;4;6;23;-;-;-;-;------;2;SY;33
DDCB;A8;RES 5,(IX+d),B;4;6;23;-;-;-;-;------;2;SY;33
DDCB;A9;RES 5,(IX+d),C;4;6;23;-;-;-;-;------;2;SY;33
DDCB;AA;RES 5,(IX+d),D;4;6;23;-;-;-;-;------;2;SY;33
DDCB;AB;RES 5,(IX+d),E;4;6;23;-;-;-;-;------;2;SY;33
DDCB;AC;RES 5,(IX+d),H;4;6;23;-;-;-;-;------;2;SY;33
DDCB;AD;RES 5,(IX+d),L;4;6;23;-;-;-;-;------;2;SY;33
DDCB;AE;RES 5,(IX+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;259
DDCB;AF;RES 5,(IX+d),A;4;6;23;-;-;-;-;------;2;SY;33
DDCB;B0;RES 6,(IX+d),B;4;6;23;-;-;-;-;------;2;SY;33
DDCB;B1;RES 6,(IX+d),C;4;6;23;-;-;-;-;------;2;SY;33
DDCB;B2;RES 6,(IX+d),D;4;6;23;-;-;-;-;------;2;SY;33
DDCB;B3;RES 6,(IX+d),E;4;6;23;-;-;-;-;------;2;SY;33
DDCB;B4;RES 6,(IX+d),H;4;6;23;-;-;-;-;------;2;SY;33
DDCB;B5;RES 6,(IX+d),L;4;6;23;-;-;-;-;------;2;SY;33
DDCB;B6;RES 6,(IX+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;259
DDCB;B7;RES 6,(IX+d),A;4;6;23;-;-;-;-;------;2;SY;33
DDCB;B8;RES 7,(IX+d),B;4;6;23;-;-;-;-;------;2;SY;33
DDCB;B9;RES 7,(IX+d),C;4;6;23;-;-;-;-;------;2;SY;33
DDCB;BA;RES 7,(IX+d),D;4;6;23;-;-;-;-;------;2;SY;33
DDCB;BB;RES 7,(IX+d),E;4;6;23;-;-;-;-;------;2;SY;33
DDCB;BC;RES 7,(IX+d),H;4;6;23;-;-;-;-;------;2;SY;33
DDCB;BD;RES 7,(IX+d),L;4;6;23;-;-;-;-;------;2;SY;33
DDCB;BE;RES 7,(IX+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;259
DDCB;BF;RES 7,(IX+d),A;4;6;23;-;-;-;-;------;2;SY;33
DDCB;C0;SET 0,(IX+d),B;4;6;23;-;-;-;-;------;2;SY;33
DDCB;C1;SET 0,(IX+d),C;4;6;23;-;-;-;-;------;2;SY;33
DDCB;C2;SET 0,(IX+d),D;4;6;23;-;-;-;-;------;2;SY;33
DDCB;C3;SET 0,(IX+d),E;4;6;23;-;-;-;-;------;2;SY;33
DDCB;C4;SET 0,(IX+d),H;4;6;23;-;-;-;-;------;2;SY;33
DDCB;C5;SET 0,(IX+d),L;4;6;23;-;-;-;-;------;2;SY;33
DDCB;C6;SET 0,(IX+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;255
DDCB;C7;SET 0,(IX+d),A;4;6;23;-;-;-;-;------;2;SY;33
DDCB;C8;SET 1,(IX+d),B;4;6;23;-;-;-;-;------;2;SY;33
DDCB;C9;SET 1,(IX+d),C;4;6;23;-;-;-;-;------;2;SY;33
DDCB;CA;SET 1,(IX+d),D;4;6;23;-;-;-;-;------;2;SY;33
DDCB;CB;SET 1,(IX+d),E;4;6;23;-;-;-;-;------;2;SY;33
DDCB;CC;SET 1,(IX+d),H;4;6;23;-;-;-;-;------;2;SY;33
DDCB;CD;SET 1,(IX+d),L;4;6;23;-;-;-;-;------;2;SY;33
DDCB;CE;SET 1,(IX+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;255
DDCB;CF;SET 1,(IX+d),A;4;6;23;-;-;-;-;------;2;SY;33
DDCB;D0;SET 2,(IX+d),B;4;6;23;-;-;-;-;------;2;SY;33
DDCB;D1;SET 2,(IX+d),C;4;6;23;-;-;-;-;------;2;SY;33
DDCB;D2;SET 2,(IX+d),D;4;6;23;-;-;-;-;------;2;SY;33
DDCB;D3;SET 2,(IX+d),E;4;6;23;-;-;-;-;------;2;SY;33
DDCB;D4;SET 2,(IX+d),H;4;6;23;-;-;-;-;------;2;SY;33
DDCB;D5;SET 2,(IX+d),L;4;6;23;-;-;-;-;------;2;SY;33
DDCB;D6;SET 2,(IX+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;255
DDCB;D7;SET 2,(IX+d),A;4;6;23;-;-;-;-;------;2;SY;33
DDCB;D8;SET 3,(IX+d),B;4;6;23;-;-;-;-;------;2;SY;33
DDCB;D9;SET 3,(IX+d),C;4;6;23;-;-;-;-;------;2;SY;33
DDCB;DA;SET 3,(IX+d),D;4;6;23;-;-;-;-;------;2;SY;33
DDCB;DB;SET 3,(IX+d),E;4;6;23;-;-;-;-;------;2;SY;33
DDCB;DC;SET 3,(IX+d),H;4;6;23;-;-;-;-;------;2;SY;33
DDCB;DD;SET 3,(IX+d),L;4;6;23;-;-;-;-;------;2;SY;33
DDCB;DE;SET 3,(IX+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;255
DDCB;DF;SET 3,(IX+d),A;4;6;23;-;-;-;-;------;2;SY;33
DDCB;E0;SET 4,(IX+d),B;4;6;23;-;-;-;-;------;2;SY;33
DDCB;E1;SET 4,(IX+d),C;4;6;23;-;-;-;-;------;2;SY;33
DDCB;E2;SET 4,(IX+d),D;4;6;23;-;-;-;-;------;2;SY;33
DDCB;E3;SET 4,(IX+d),E;4;6;23;-;-;-;-;------;2;SY;33
DDCB;E4;SET 4,(IX+d),H;4;6;23;-;-;-;-;------;2;SY;33
DDCB;E5;SET 4,(IX+d),L;4;6;23;-;-;-;-;------;2;SY;33
DDCB;E6;SET 4,(IX+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;255
DDCB;E7;SET 4,(IX+d),A;4;6;23;-;-;-;-;------;2;SY;33
DDCB;E8;SET 5,(IX+d),B;4;6;23;-;-;-;-;------;2;SY;33
DDCB;E9;SET 5,(IX+d),C;4;6;23;-;-;-;-;------;2;SY;33
DDCB;EA;SET 5,(IX+d),D;4;6;23;-;-;-;-;------;2;SY;33
DDCB;EB;SET 5,(IX+d),E;4;6;23;-;-;-;-;------;2;SY;33
DDCB;EC;SET 5,(IX+d),H;4;6;23;-;-;-;-;------;2;SY;33
DDCB;ED;SET 5,(IX+d),L;4;6;23;-;-;-;-;------;2;SY;33
DDCB;EE;SET 5,(IX+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;255
DDCB;EF;SET 5,(IX+d),A;4;6;23;-;-;-;-;------;2;SY;33
DDCB;F0;SET 6,(IX+d),B;4;6;23;-;-;-;-;------;2;SY;33
DDCB;F1;SET 6,(IX+d),C;4;6;23;-;-;-;-;------;2;SY;33
DDCB;F2;SET 6,(IX+d),D;4;6;23;-;-;-;-;------;2;SY;33
DDCB;F3;SET 6,(IX+d),E;4;6;23;-;-;-;-;------;2;SY;33
DDCB;F4;SET 6,(IX+d),H;4;6;23;-;-;-;-;------;2;SY;33
DDCB;F5;SET 6,(IX+d),L;4;6;23;-;-;-;-;------;2;SY;33
DDCB;F6;SET 6,(IX+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;255
DDCB;F7;SET 6,(IX+d),A;4;6;23;-;-;-;-;------;2;SY;33
DDCB;F8;SET 7,(IX+d),B;4;6;23;-;-;-;-;------;2;SY;33
DDCB;F9;SET 7,(IX+d),C;4;6;23;-;-;-;-;------;2;SY;33
DDCB;FA;SET 7,(IX+d),D;4;6;23;-;-;-;-;------;2;SY;33
DDCB;FB;SET 7,(IX+d),E;4;6;23;-;-;-;-;------;2;SY;33
DDCB;FC;SET 7,(IX+d),H;4;6;23;-;-;-;-;------;2;SY;33
DDCB;FD;SET 7,(IX+d),L;4;6;23;-;-;-;-;------;2;SY;33
DDCB;FE;SET 7,(IX+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;255
DDCB;FF;SET 7,(IX+d),A;4;6;23;-;-;-;-;------;2;SY;33
FDCB;00;RLC (IY+d),B;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;01;RLC (IY+d),C;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;02;RLC (IY+d),D;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;03;RLC (IY+d),E;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;04;RLC (IY+d),H;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;05;RLC (IY+d),L;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;06;RLC (IY+d);4;6;23;4,4,3,5,4,3;-;-;-;rr0p0c;2;-;219
FDCB;07;RLC (IY+d),A;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;08;RRC (IY+d),B;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;09;RRC (IY+d),C;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;0A;RRC (IY+d),D;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;0B;RRC (IY+d),E;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;0C;RRC (IY+d),H;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;0D;RRC (IY+d),L;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;0E;RRC (IY+d);4;6;23;4,4,3,5,4,3;-;-;-;rr0p0c;2;-;224
FDCB;0F;RRC (IY+d),A;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;10;RL (IY+d),B;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;11;RL (IY+d),C;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;12;RL (IY+d),D;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;13;RL (IY+d),E;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;14;RL (IY+d),H;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;15;RL (IY+d),L;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;16;RL (IY+d);4;6;23;4,4,3,5,4,3;-;-;-;rr0p0c;2;-;221
FDCB;17;RL (IY+d),A;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;18;RR (IY+d),B;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;19;RR (IY+d),C;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;1A;RR (IY+d),D;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;1B;RR (IY+d),E;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;1C;RR (IY+d),H;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;1D;RR (IY+d),L;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;1E;RR (IY+d);4;6;23;4,4,3,5,4,3;-;-;-;rr0p0c;2;-;227
FDCB;1F;RR (IY+d),A;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;20;SLA (IY+d),B;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;21;SLA (IY+d),C;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;22;SLA (IY+d),D;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;23;SLA (IY+d),E;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;24;SLA (IY+d),H;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;25;SLA (IY+d),L;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;26;SLA (IY+d);4;6;23;4,4,3,5,4,3;-;-;-;rr0p0c;2;-;230
FDCB;27;SLA (IY+d),A;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;28;SRA (IY+d),B;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;29;SRA (IY+d),C;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;2A;SRA (IY+d),D;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;2B;SRA (IY+d),E;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;2C;SRA (IY+d),H;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;2D;SRA (IY+d),L;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;2E;SRA (IY+d);4;6;23;4,4,3,5,4,3;-;-;-;rr0p0c;2;-;233
FDCB;2F;SRA (IY+d),A;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;30;SLL (IY+d),B;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;31;SLL (IY+d),C;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;32;SLL (IY+d),D;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;33;SLL (IY+d),E;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;34;SLL (IY+d),H;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;35;SLL (IY+d),L;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;36;SLL (IY+d);4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;37;SLL (IY+d),A;4;6;23;-;-;-;-;rr0p0c;2;SY;32
FDCB;38;SRL (IY+d),B;4;6;23;-;-;-;-;0r0p0c;2;SY;32
FDCB;39;SRL (IY+d),C;4;6;23;-;-;-;-;0r0p0c;2;SY;32
FDCB;3A;SRL (IY+d),D;4;6;23;-;-;-;-;0r0p0c;2;SY;32
FDCB;3B;SRL (IY+d),E;4;6;23;-;-;-;-;0r0p0c;2;SY;32
FDCB;3C;SRL (IY+d),H;4;6;23;-;-;-;-;0r0p0c;2;SY;32
FDCB;3D;SRL (IY+d),L;4;6;23;-;-;-;-;0r0p0c;2;SY;32
FDCB;3E;SRL (IY+d);4;6;23;4,4,3,5,4,3;-;-;-;0r0p0c;2;-;236
FDCB;3F;SRL (IY+d),A;4;6;23;-;-;-;-;0r0p0c;2;SY;32
FDCB;40;BIT 0,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;41;BIT 0,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;42;BIT 0,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;43;BIT 0,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;44;BIT 0,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;45;BIT 0,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;46;BIT 0,(IY+d);4;5;20;4,4,3,5,4;-;-;-;xz1x0-;2;R6;249
FDCB;47;BIT 0,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;48;BIT 1,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;49;BIT 1,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;4A;BIT 1,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;4B;BIT 1,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;4C;BIT 1,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;4D;BIT 1,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;4E;BIT 1,(IY+d);4;5;20;4,4,3,5,4;-;-;-;xz1x0-;2;R6;249
FDCB;4F;BIT 1,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;50;BIT 2,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;51;BIT 2,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;52;BIT 2,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;53;BIT 2,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;54;BIT 2,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;55;BIT 2,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;56;BIT 2,(IY+d);4;5;20;4,4,3,5,4;-;-;-;xz1x0-;2;R6;249
FDCB;57;BIT 2,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;58;BIT 3,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;59;BIT 3,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;5A;BIT 3,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;5B;BIT 3,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;5C;BIT 3,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;5D;BIT 3,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;5E;BIT 3,(IY+d);4;5;20;4,4,3,5,4;-;-;-;xz1x0-;2;R6;249
FDCB;5F;BIT 3,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;60;BIT 4,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;61;BIT 4,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;62;BIT 4,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;63;BIT 4,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;64;BIT 4,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;65;BIT 4,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;66;BIT 4,(IY+d);4;5;20;4,4,3,5,4;-;-;-;xz1x0-;2;R6;249
FDCB;67;BIT 4,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;68;BIT 5,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;69;BIT 5,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;6A;BIT 5,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;6B;BIT 5,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;6C;BIT 5,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;6D;BIT 5,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;6E;BIT 5,(IY+d);4;5;20;4,4,3,5,4;-;-;-;xz1x0-;2;R6;249
FDCB;6F;BIT 5,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;70;BIT 6,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;71;BIT 6,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;72;BIT 6,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;73;BIT 6,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;74;BIT 6,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;75;BIT 6,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;76;BIT 6,(IY+d);4;5;20;4,4,3,5,4;-;-;-;xz1x0-;2;R6;249
FDCB;77;BIT 6,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;78;BIT 7,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;79;BIT 7,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;7A;BIT 7,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;7B;BIT 7,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;7C;BIT 7,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;7D;BIT 7,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;7E;BIT 7,(IY+d);4;5;20;4,4,3,5,4;-;-;-;xz1x0-;2;R6;249
FDCB;7F;BIT 7,(IY+d);4;5;20;-;-;-;-;xz1x0-;2;SY;33
FDCB;80;RES 0,(IY+d),B;4;6;23;-;-;-;-;------;2;SY;33
FDCB;81;RES 0,(IY+d),C;4;6;23;-;-;-;-;------;2;SY;33
FDCB;82;RES 0,(IY+d),D;4;6;23;-;-;-;-;------;2;SY;33
FDCB;83;RES 0,(IY+d),E;4;6;23;-;-;-;-;------;2;SY;33
FDCB;84;RES 0,(IY+d),H;4;6;23;-;-;-;-;------;2;SY;33
FDCB;85;RES 0,(IY+d),L;4;6;23;-;-;-;-;------;2;SY;33
FDCB;86;RES 0,(IY+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;259
FDCB;87;RES 0,(IY+d),A;4;6;23;-;-;-;-;------;2;SY;33
FDCB;88;RES 1,(IY+d),B;4;6;23;-;-;-;-;------;2;SY;33
FDCB;89;RES 1,(IY+d),C;4;6;23;-;-;-;-;------;2;SY;33
FDCB;8A;RES 1,(IY+d),D;4;6;23;-;-;-;-;------;2;SY;33
FDCB;8B;RES 1,(IY+d),E;4;6;23;-;-;-;-;------;2;SY;33
FDCB;8C;RES 1,(IY+d),H;4;6;23;-;-;-;-;------;2;SY;33
FDCB;8D;RES 1,(IY+d),L;4;6;23;-;-;-;-;------;2;SY;33
FDCB;8E;RES 1,(IY+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;259
FDCB;8F;RES 1,(IY+d),A;4;6;23;-;-;-;-;------;2;SY;33
FDCB;90;RES 2,(IY+d),B;4;6;23;-;-;-;-;------;2;SY;33
FDCB;91;RES 2,(IY+d),C;4;6;23;-;-;-;-;------;2;SY;33
FDCB;92;RES 2,(IY+d),D;4;6;23;-;-;-;-;------;2;SY;33
FDCB;93;RES 2,(IY+d),E;4;6;23;-;-;-;-;------;2;SY;33
FDCB;94;RES 2,(IY+d),H;4;6;23;-;-;-;-;------;2;SY;33
FDCB;95;RES 2,(IY+d),L;4;6;23;-;-;-;-;------;2;SY;33
FDCB;96;RES 2,(IY+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;259
FDCB;97;RES 2,(IY+d),A;4;6;23;-;-;-;-;------;2;SY;33
FDCB;98;RES 3,(IY+d),B;4;6;23;-;-;-;-;------;2;SY;33
FDCB;99;RES 3,(IY+d),C;4;6;23;-;-;-;-;------;2;SY;33
FDCB;9A;RES 3,(IY+d),D;4;6;23;-;-;-;-;------;2;SY;33
FDCB;9B;RES 3,(IY+d),E;4;6;23;-;-;-;-;------;2;SY;33
FDCB;9C;RES 3,(IY+d),H;4;6;23;-;-;-;-;------;2;SY;33
FDCB;9D;RES 3,(IY+d),L;4;6;23;-;-;-;-;------;2;SY;33
FDCB;9E;RES 3,(IY+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;259
FDCB;9F;RES 3,(IY+d),A;4;6;23;-;-;-;-;------;2;SY;33
FDCB;A0;RES 4,(IY+d),B;4;6;23;-;-;-;-;------;2;SY;33
FDCB;A1;RES 4,(IY+d),C;4;6;23;-;-;-;-;------;2;SY;33
FDCB;A2;RES 4,(IY+d),D;4;6;23;-;-;-;-;------;2;SY;33
FDCB;A3;RES 4,(IY+d),E;4;6;23;-;-;-;-;------;2;SY;33
FDCB;A4;RES 4,(IY+d),H;4;6;23;-;-;-;-;------;2;SY;33
FDCB;A5;RES 4,(IY+d),L;4;6;23;-;-;-;-;------;2;SY;33
FDCB;A6;RES 4,(IY+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;259
FDCB;A7;RES 4,(IY+d),A;4;6;23;-;-;-;-;------;2;SY;33
FDCB;A8;RES 5,(IY+d),B;4;6;23;-;-;-;-;------;2;SY;33
FDCB;A9;RES 5,(IY+d),C;4;6;23;-;-;-;-;------;2;SY;33
FDCB;AA;RES 5,(IY+d),D;4;6;23;-;-;-;-;------;2;SY;33
FDCB;AB;RES 5,(IY+d),E;4;6;23;-;-;-;-;------;2;SY;33
FDCB;AC;RES 5,(IY+d),H;4;6;23;-;-;-;-;------;2;SY;33
FDCB;AD;RES 5,(IY+d),L;4;6;23;-;-;-;-;------;2;SY;33
FDCB;AE;RES 5,(IY+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;259
FDCB;AF;RES 5,(IY+d),A;4;6;23;-;-;-;-;------;2;SY;33
FDCB;B0;RES 6,(IY+d),B;4;6;23;-;-;-;-;------;2;SY;33
FDCB;B1;RES 6,(IY+d),C;4;6;23;-;-;-;-;------;2;SY;33
FDCB;B2;RES 6,(IY+d),D;4;6;23;-;-;-;-;------;2;SY;33
FDCB;B3;RES 6,(IY+d),E;4;6;23;-;-;-;-;------;2;SY;33
FDCB;B4;RES 6,(IY+d),H;4;6;23;-;-;-;-;------;2;SY;33
FDCB;B5;RES 6,(IY+d),L;4;6;23;-;-;-;-;------;2;SY;33
FDCB;B6;RES 6,(IY+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;259
FDCB;B7;RES 6,(IY+d),A;4;6;23;-;-;-;-;------;2;SY;33
FDCB;B8;RES 7,(IY+d),B;4;6;23;-;-;-;-;------;2;SY;33
FDCB;B9;RES 7,(IY+d),C;4;6;23;-;-;-;-;------;2;SY;33
FDCB;BA;RES 7,(IY+d),D;4;6;23;-;-;-;-;------;2;SY;33
FDCB;BB;RES 7,(IY+d),E;4;6;23;-;-;-;-;------;2;SY;33
FDCB;BC;RES 7,(IY+d),H;4;6;23;-;-;-;-;------;2;SY;33
FDCB;BD;RES 7,(IY+d),L;4;6;23;-;-;-;-;------;2;SY;33
FDCB;BE;RES 7,(IY+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;259
FDCB;BF;RES 7,(IY+d),A;4;6;23;-;-;-;-;------;2;SY;33
FDCB;C0;SET 0,(IY+d),B;4;6;23;-;-;-;-;------;2;SY;33
FDCB;C1;SET 0,(IY+d),C;4;6;23;-;-;-;-;------;2;SY;33
FDCB;C2;SET 0,(IY+d),D;4;6;23;-;-;-;-;------;2;SY;33
FDCB;C3;SET 0,(IY+d),E;4;6;23;-;-;-;-;------;2;SY;33
FDCB;C4;SET 0,(IY+d),H;4;6;23;-;-;-;-;------;2;SY;33
FDCB;C5;SET 0,(IY+d),L;4;6;23;-;-;-;-;------;2;SY;33
FDCB;C6;SET 0,(IY+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;257
FDCB;C7;SET 0,(IY+d),A;4;6;23;-;-;-;-;------;2;SY;33
FDCB;C8;SET 1,(IY+d),B;4;6;23;-;-;-;-;------;2;SY;33
FDCB;C9;SET 1,(IY+d),C;4;6;23;-;-;-;-;------;2;SY;33
FDCB;CA;SET 1,(IY+d),D;4;6;23;-;-;-;-;------;2;SY;33
FDCB;CB;SET 1,(IY+d),E;4;6;23;-;-;-;-;------;2;SY;33
FDCB;CC;SET 1,(IY+d),H;4;6;23;-;-;-;-;------;2;SY;33
FDCB;CD;SET 1,(IY+d),L;4;6;23;-;-;-;-;------;2;SY;33
FDCB;CE;SET 1,(IY+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;257
FDCB;CF;SET 1,(IY+d),A;4;6;23;-;-;-;-;------;2;SY;33
FDCB;D0;SET 2,(IY+d),B;4;6;23;-;-;-;-;------;2;SY;33
FDCB;D1;SET 2,(IY+d),C;4;6;23;-;-;-;-;------;2;SY;33
FDCB;D2;SET 2,(IY+d),D;4;6;23;-;-;-;-;------;2;SY;33
FDCB;D3;SET 2,(IY+d),E;4;6;23;-;-;-;-;------;2;SY;33
FDCB;D4;SET 2,(IY+d),H;4;6;23;-;-;-;-;------;2;SY;33
FDCB;D5;SET 2,(IY+d),L;4;6;23;-;-;-;-;------;2;SY;33
FDCB;D6;SET 2,(IY+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;257
FDCB;D7;SET 2,(IY+d),A;4;6;23;-;-;-;-;------;2;SY;33
FDCB;D8;SET 3,(IY+d),B;4;6;23;-;-;-;-;------;2;SY;33
FDCB;D9;SET 3,(IY+d),C;4;6;23;-;-;-;-;------;2;SY;33
FDCB;DA;SET 3,(IY+d),D;4;6;23;-;-;-;-;------;2;SY;33
FDCB;DB;SET 3,(IY+d),E;4;6;23;-;-;-;-;------;2;SY;33
FDCB;DC;SET 3,(IY+d),H;4;6;23;-;-;-;-;------;2;SY;33
FDCB;DD;SET 3,(IY+d),L;4;6;23;-;-;-;-;------;2;SY;33
FDCB;DE;SET 3,(IY+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;257
FDCB;DF;SET 3,(IY+d),A;4;6;23;-;-;-;-;------;2;SY;33
FDCB;E0;SET 4,(IY+d),B;4;6;23;-;-;-;-;------;2;SY;33
FDCB;E1;SET 4,(IY+d),C;4;6;23;-;-;-;-;------;2;SY;33
FDCB;E2;SET 4,(IY+d),D;4;6;23;-;-;-;-;------;2;SY;33
FDCB;E3;SET 4,(IY+d),E;4;6;23;-;-;-;-;------;2;SY;33
FDCB;E4;SET 4,(IY+d),H;4;6;23;-;-;-;-;------;2;SY;33
FDCB;E5;SET 4,(IY+d),L;4;6;23;-;-;-;-;------;2;SY;33
FDCB;E6;SET 4,(IY+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;257
FDCB;E7;SET 4,(IY+d),A;4;6;23;-;-;-;-;------;2;SY;33
FDCB;E8;SET 5,(IY+d),B;4;6;23;-;-;-;-;------;2;SY;33
FDCB;E9;SET 5,(IY+d),C;4;6;23;-;-;-;-;------;2;SY;33
FDCB;EA;SET 5,(IY+d),D;4;6;23;-;-;-;-;------;2;SY;33
FDCB;EB;SET 5,(IY+d),E;4;6;23;-;-;-;-;------;2;SY;33
FDCB;EC;SET 5,(IY+d),H;4;6;23;-;-;-;-;------;2;SY;33
FDCB;ED;SET 5,(IY+d),L;4;6;23;-;-;-;-;------;2;SY;33
FDCB;EE;SET 5,(IY+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;257
FDCB;EF;SET 5,(IY+d),A;4;6;23;-;-;-;-;------;2;SY;33
FDCB;F0;SET 6,(IY+d),B;4;6;23;-;-;-;-;------;2;SY;33
FDCB;F1;SET 6,(IY+d),C;4;6;23;-;-;-;-;------;2;SY;33
FDCB;F2;SET 6,(IY+d),D;4;6;23;-;-;-;-;------;2;SY;33
FDCB;F3;SET 6,(IY+d),E;4;6;23;-;-;-;-;------;2;SY;33
FDCB;F4;SET 6,(IY+d),H;4;6;23;-;-;-;-;------;2;SY;33
FDCB;F5;SET 6,(IY+d),L;4;6;23;-;-;-;-;------;2;SY;33
FDCB;F6;SET 6,(IY+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;257
FDCB;F7;SET 6,(IY+d),A;4;6;23;-;-;-;-;------;2;SY;33
FDCB;F8;SET 7,(IY+d),B;4;6;23;-;-;-;-;------;2;SY;33
FDCB;F9;SET 7,(IY+d),C;4;6;23;-;-;-;-;------;2;SY;33
FDCB;FA;SET 7,(IY+d),D;4;6;23;-;-;-;-;------;2;SY;33
FDCB;FB;SET 7,(IY+d),E;4;6;23;-;-;-;-;------;2;SY;33
FDCB;FC;SET 7,(IY+d),H;4;6;23;-;-;-;-;------;2;SY;33
FDCB;FD;SET 7,(IY+d),L;4;6;23;-;-;-;-;------;2;SY;33
FDCB;FE;SET 7,(IY+d);4;6;23;4,4,3,5,4,3;-;-;-;------;2;-;257
FDCB;FF;SET 7,(IY+d),A;4;6;23;-;-;-;-;------;2;SY;33
```
