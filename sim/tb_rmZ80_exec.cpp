// This file is part of rmZ80, a Zilog Z80 core in SystemVerilog.
// Copyright (c) 2026 Umberto Parisi (rmonic79)
//
// rmZ80 is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
// License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any
// later version. It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the
// implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
// more details: LICENSE in the root of the repository, or <https://www.gnu.org/licenses/>.
//
// A commercial license, for use without the obligations of the GPL (for example in closed-source products), is
// available from the author, who can also be contacted for other licensing needs.

// =============================================================================
// tb_rmZ80_exec — execution on rmZ80 (top module). exit 0 = PASS.
//
// PROGRAMS, all with the same gates; among them:
//   A) a regression program: 44 instructions, 286 T;
//   B) the extended families: absolute jumps, CALL/RET (also conditional, taken
//      and not), PUSH/POP, RST, LD with an extended address, EX (SP),HL, EX DE,HL,
//      ADD HL, INC/DEC r/(HL), ALU A,(HL), ADC/SBC n, LD (HL),n, RLCA, DAA, CPL;
//   C) the CB space: prefix + rotate/shift, BIT, RES, SET on r and (HL).
// GATE a) FINAL STATE and MEMORY worked out BY HAND from the documents (docs/05 for
//         the flags, docs/03 for the semantics), as constants with the reasoning.
// GATE b) T-STATES: measured total = sum of the T columns (or T2 for the conditional
//         instructions not taken: BYTES and T2 read from the file) of Appendix A,
//         READ FROM THE FILE docs/03-decoding.md.
// GATE c) savestate 274, walking one, save/restore in the MIDDLE (tick 151) and at EVERY
//         TICK of every program, with an identical resumed run (T, trace, savestate, memory, ports).
// GATE d) invisible pipeline: everything twice, cen 1/12 and 1/2, identical.
// GATE e) refresh ([02] §3.1): {I, R} stable over the 4 half T-states, MREQ T3B-T4A, no RD.
// GATE f) random WAIT (1/3 per half T-state) and random BUSREQ (1/4, programs without
//         interrupts), also with save/restore at every tick: identical results.
// GATE g) M1 low from the start of T1 on every fetch and IACK.
// Plus a directed BUSREQ/BUSACK timing test.
// MASKED/DECLARED: initial PC=0 through the savestate; I/R set through the savestate
// after the reset (5Ch / 80h); MREQ in the IACK refresh checked as a declared contract
// (inactive); the order of the write cycles and the bus in the internal T-states are
// not observed.
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include "Vcpu.h"
#include "verilated.h"

double sc_time_stamp() { return 0; }

static Vcpu* cpu;
static long n_err = 0, n_prt = 0;
#define CK(cond, ...) do { if (!(cond)) { n_err++; \
  if (n_prt < 40) { n_prt++; std::printf("FAIL "); std::printf(__VA_ARGS__); \
    std::printf("\n"); } } } while (0)

static const int SS_EXP = 274;
static uint8_t mem[65536];

// ---- clock, cen and spacing contract (docs/06 §9.2) ----------------------------
static long clk_idx = 0, last_cen_clk = -100;
static int  N_CEN = 12;
static long min_spacing_seen = 1L << 30;
static bool g_chk = true;                  // contract/refresh checks (the reset study turns them off: defined below)
static void tick() {
  if (cpu->cen) {
    long d = clk_idx - last_cen_clk;
    if (d < min_spacing_seen) min_spacing_seen = d;
    if (g_chk) CK(d >= 2, "contract: two cen %ld clocks apart (< 2)", d);   // the reset study breaks the contract on purpose
    last_cen_clk = clk_idx;
  }
  cpu->clk = 0; cpu->eval(); cpu->clk = 1; cpu->eval();
  clk_idx++;
}
static void quiet(int k) { cpu->cen = 0; for (int i = 0; i < k; i++) tick(); }
static void cen_tick() { cpu->cen = 1; tick(); cpu->cen = 0; for (int i = 1; i < N_CEN; i++) tick(); }
// PARALLEL savestate: MSB-first view d[0..273] (d[i] = ss_dout[273-i], seq [273:210] then
// regfile [209:0]), so that the field offsets used below (RF + ..., d[54], ...) are those
// of docs/06 §5 and docs/01 §5.
static int ss_bit(int i) { int k = SS_EXP - 1 - i; return (int)((cpu->ss_dout[k >> 5] >> (k & 31)) & 1u); }
static void ss_dump(uint8_t* d) { for (int i = 0; i < SS_EXP; i++) d[i] = (uint8_t)ss_bit(i); }   // read: no clock
static void ss_load(const uint8_t* d) {                                      // load: ONE clock with cen = 0
  for (int w = 0; w < 9; w++) cpu->ss_din[w] = 0;
  for (int i = 0; i < SS_EXP; i++) if (d[i]) { int k = SS_EXP - 1 - i; cpu->ss_din[k >> 5] |= 1u << (k & 31); }
  cpu->cen = 0; cpu->ss_load = 1; tick(); cpu->ss_load = 0;
}
static void ss_zero() { uint8_t z[SS_EXP]; std::memset(z, 0, sizeof z); ss_load(z); }

// ---- savestate, MSB-first view: fields: [0,64) sequencer, [64,274) regfile ----------
static uint32_t fld(const uint8_t* d, int off, int n) {
  uint32_t v = 0;
  for (int i = 0; i < n; i++) v = (v << 1) | d[off + i];
  return v;
}
#define RF 64
#define R_A0(d)  fld(d, RF+8, 8)
#define R_F0(d)  fld(d, RF+24, 8)
#define R_B0(d)  fld(d, RF+40, 8)
#define R_C0(d)  fld(d, RF+56, 8)
// EX DE,HL = renaming: with bit q_reg[2] (bank 0, bit 12 of the seq = d[51]) at 1 the LOGICAL values of DE and HL
// are in the swapped physical registers
#define XHL0(d)  fld(d, 51, 1)
#define R_D0(d)  (XHL0(d) ? fld(d, RF+104, 8) : fld(d, RF+72, 8))
#define R_E0(d)  (XHL0(d) ? fld(d, RF+120, 8) : fld(d, RF+88, 8))
#define R_H0(d)  (XHL0(d) ? fld(d, RF+72, 8)  : fld(d, RF+104, 8))
#define R_L0(d)  (XHL0(d) ? fld(d, RF+88, 8)  : fld(d, RF+120, 8))
#define R_IX(d)  fld(d, RF+128, 16)
#define R_IY(d)  fld(d, RF+144, 16)
#define R_SP(d)  fld(d, RF+160, 16)
#define R_PC(d)  fld(d, RF+176, 16)
#define R_I(d)   fld(d, RF+192, 8)
#define R_R(d)   fld(d, RF+200, 8)
// microstate of the seq in the savestate (docs/06 §5, MSB first: bit k of dbg_st = d[63-k])
#define S_IFF1(d) fld(d, 54, 1)
#define S_IFF2(d) fld(d, 55, 1)
#define S_IM(d)   fld(d, 56, 2)
#define R_SELAF(d) fld(d, RF+208, 1)
#define R_SELGP(d) fld(d, RF+209, 1)

static const uint8_t I_PRESET = 0x5C, R_PRESET = 0x80;

// ---- description of a test program ---------------------------------------------
struct Exp { int A, F, B, C, D, E, H, L, SP, PC, I, R; int IM = -1, IFF1 = -1, IFF2 = -1; int IX = -1, IY = -1; };  // -1 = not checked
struct MemW { uint16_t a; uint8_t v; };
// interrupt events in the trace: idx = index of the fetch (0-based) the event refers to;
// kind: 1 NMI (11 T, fetch idx is the ignored M1), 2 IM1 (13 T), 3 IM2 (19 T),
// 4 IM0 with RST (13 T) — for 2..4 fetch idx is the first one of the handler; 5 NOP forced in HALT (4 T)
struct Ev { int idx, kind; };
struct Prog {
  const char*    name;
  uint16_t       org;            // where it is loaded
  const uint8_t* bytes; int len;
  uint16_t       stop_addr;      // first fetch of this address = end
  const uint16_t* trace; int n_trace;
  Exp            exp;
  const MemW*    writes; int n_writes;
  int iff1_pre = 0, iff2_pre = 0;            // IFF preset through the savestate
  int m1_i_set = -1, i_new = 0;              // M1 (1-based) that writes I: refresh not checked there, then I = i_new
  int m1_r_set = -1, r_new = 0;              // same for R (r_new = value AFTER the increment of that M1)
  int nmi_at_fetch = -1;                     // NMI low for 2 ticks from fetch n (1-based)
  int int_at_fetch = -1;                     // INT low from fetch n up to the IACK (the device is served)
  int iack_byte = 0;                         // byte put on the bus by the device during the IACK
  const Ev* evs = nullptr; int n_evs = 0;
  int halt_ticks = -1;                       // ticks with halt_n low expected (-1 = not checked)
  const MemW* iow = nullptr; int n_iow = 0;  // expected port writes {16-bit address, data}, in order
  int nmi2_at_fetch = -1;                    // second NMI (nested NMI, program Q)
  const uint8_t* iack_more = nullptr; int n_iack_more = 0;   // IM0: bytes 2.. of the instruction, in the reads after the IACK
};

// =============================================================================
// PROGRAM A — regression.
// addr  bytes      instruction       effect (by hand)
// 0000  3E 00      LD A,00           A=00
// 0002  06 04      LD B,04           B=04
// 0004  0E 07      LD C,07           C=07
// 0006  81         ADD A,C           A+=7 (loop x4)
// 0007  10 FD      DJNZ 0006         B--, 3 taken + 1 not -> A=1C B=00 F=08
// 0009  26 40      LD H,40
// 000B  2E 10      LD L,10           HL=4010
// 000D  77         LD (HL),A         mem[4010]=1C
// 000E  23         INC HL            HL=4011
// 000F  70         LD (HL),B         mem[4011]=00
// 0010  2B         DEC HL            HL=4010
// 0011  46         LD B,(HL)         B=1C
// 0012  90         SUB B             A=00 F=42
// 0013  28 02      JR Z,0017         taken
// 0015  3E FF      LD A,FF           skipped
// 0017  EE 55      XOR 55            A=55 F=04
// 0019  30 02      JR NC,001D        taken
// 001B  3E 00      LD A,00           skipped
// 001D  FE 55      CP 55             F=42 (XF/YF from the operand 55), A stays 55
// 001F  38 02      JR C,0023         NOT taken
// 0021  4F 57 5F   LD C,A / LD D,A / LD E,A   C=D=E=55
// 0024  3E F0      LD A,F0
// 0026  C6 20      ADD A,20          A=10 F=01
// 0028  8F         ADC A,A           A=21 F=20
// 0029  08 08      EX AF,AF' x2      unchanged
// 002B  D9 D9      EXX x2            unchanged
// 002D  03         INC BC            BC=1C56
// 002E  1B         DEC DE            DE=5554
// 002F  F9         LD SP,HL          SP=4010
// 0030  12         LD (DE),A         mem[5554]=21
// 0031  0A         LD A,(BC)         A=mem[1C56]=00
// 0032  33         INC SP            SP=4011
// 0033  26 00      LD H,00
// 0035  2E 3A      LD L,3A           HL=003A
// 0037  E9         JP (HL)           PC=003A
// 0038  3E FF      skipped
// 003A  00         NOP
// 003B  18 FE      JR $              END
// R: 80h + 44 M1 = ACh (bit 7 kept); I = 5Ch preset.
// =============================================================================
static const uint8_t progA[] = {
  0x3E,0x00, 0x06,0x04, 0x0E,0x07, 0x81, 0x10,0xFD,
  0x26,0x40, 0x2E,0x10, 0x77, 0x23, 0x70, 0x2B, 0x46, 0x90,
  0x28,0x02, 0x3E,0xFF, 0xEE,0x55, 0x30,0x02, 0x3E,0x00,
  0xFE,0x55, 0x38,0x02, 0x4F, 0x57, 0x5F, 0x3E,0xF0, 0xC6,0x20, 0x8F,
  0x08, 0x08, 0xD9, 0xD9, 0x03, 0x1B, 0xF9, 0x12, 0x0A, 0x33,
  0x26,0x00, 0x2E,0x3A, 0xE9, 0x3E,0xFF, 0x00, 0x18,0xFE
};
static const uint16_t traceA[44] = {
  0x0000,0x0002,0x0004,0x0006,0x0007,0x0006,0x0007,0x0006,0x0007,0x0006,0x0007,
  0x0009,0x000B,0x000D,0x000E,0x000F,0x0010,0x0011,0x0012,0x0013,0x0017,0x0019,
  0x001D,0x001F,0x0021,0x0022,0x0023,0x0024,0x0026,0x0028,0x0029,0x002A,0x002B,
  0x002C,0x002D,0x002E,0x002F,0x0030,0x0031,0x0032,0x0033,0x0035,0x0037,0x003A };
static const MemW writesA[3] = { {0x4010, 0x1C}, {0x4011, 0x00}, {0x5554, 0x21} };
static const Prog PROG_A = { "A (regression)", 0x0000, progA, (int)sizeof progA, 0x003B,
  traceA, 44, { 0x00, 0x20, 0x1C, 0x56, 0x55, 0x54, 0x00, 0x3A, 0x4011, 0x003B, 0x5C, 0xAC },
  writesA, 3 };

// =============================================================================
// PROGRAM B — extended families. Initial state: registers 0 (savestate), SP=0,
// F=00, I=5Ch, R=80h. Stack at 8000h. Each row: effect worked out by hand.
// addr  bytes      instruction       effect
// 0000  C3 00 01   JP 0100h          PC=0100
// 0038  3C         INC A             (RST 38h handler) A=AB->AC; F: S1 Z0 H0 (B+1) V0 N0,
//                                    YF=bit5(AC)=1 XF=bit3(AC)=1, C kept (1) -> F=A9
// 0039  C9         RET               PC=014C
// 0100  31 00 80   LD SP,8000h       SP=8000
// 0103  01 34 12   LD BC,1234h
// 0106  11 78 56   LD DE,5678h
// 0109  21 00 90   LD HL,9000h
// 010C  C5         PUSH BC           [7FFF]=12 [7FFE]=34 SP=7FFE
// 010D  D5         PUSH DE           [7FFD]=56 [7FFC]=78 SP=7FFC
// 010E  E1         POP HL            HL=5678 SP=7FFE
// 010F  D1         POP DE            DE=1234 SP=8000
// 0110  EB         EX DE,HL          DE=5678 HL=1234
// 0111  22 00 A0   LD (A000h),HL     [A000]=34 [A001]=12
// 0114  2A 01 A0   LD HL,(A001h)     L=[A001]=12 H=[A002]=00 -> HL=0012
// 0117  3E 55      LD A,55h
// 0119  32 02 A0   LD (A002h),A      [A002]=55
// 011C  3A 00 A0   LD A,(A000h)      A=34
// 011F  09         ADD HL,BC         HL=0012+1234=1246; H0 (012+234<1000h) N0 C0,
//                                    YF=bit13(1246)=0 XF=bit11=0; S/Z/P kept (0) -> F=00
// 0120  36 77      LD (HL),77h       [1246]=77
// 0122  34         INC (HL)          [1246]=78; S0 Z0 H0 (7+1) V0 N0 YF=1 XF=1 C=0 -> F=28
// 0123  35         DEC (HL)          [1246]=77; S0 Z0 H0 (8-1) V0 N1 YF=1 (bit5 of 77) XF=0 -> F=22
// 0124  86         ADD A,(HL)        A=34+77=AB; S1 Z0 H0 (4+7=B) V1 (52+119=171>127) N0 C0,
//                                    YF=1 XF=1 -> F=AC
// 0125  CE 01      ADC A,01h         A=AB+1+0=AC; S1 Z0 H0 V0 N0 C0 YF=1 XF=1 -> F=A8
// 0127  DE 0D      SBC A,0Dh         A=AC-0D=9F; S1 Z0 H1 (C-D borrow) V0 N1 C0, YF=0 XF=1 -> F=9A
// 0129  3C         INC A             A=A0; S1 Z0 H1 (F+1) V0 N0 YF=1 XF=0 C=0 -> F=B0
// 012A  05         DEC B             B=12-1=11; S0 Z0 H0 V0 N1 YF=0 XF=0 C=0 -> F=02
// 012B  07         RLCA              A=A0 -> 41, C=1; S/Z/P kept (0), H0 N0, YF/XF of 41 = 0 -> F=01
// 012C  2F         CPL               A=BE; H1 N1, S/Z/P/C kept (0/0/0/1), YF=1 XF=1 -> F=3B
// 012D  27         DAA               C=1 H=1 N=1 A=BE: diff=66 (row C, low digit >= A), subtracted:
//                                    A=58; C'=1; H'=0 (N=1, H=1, low digit E >= 6); P=parity(58)=0
//                                    (3 bits at 1); S0 Z0; YF=bit5(58)=0 XF=bit3(58)=1; N kept 1 -> F=0B
// 012E  F5         PUSH AF           [7FFF]=58 [7FFE]=0B SP=7FFE
// 012F  C1         POP BC            BC=580B SP=8000
// 0130  E5         PUSH HL           [7FFF]=12 [7FFE]=46 SP=7FFE
// 0131  21 CD AB   LD HL,ABCDh
// 0134  E3         EX (SP),HL        HL=1246; [7FFE]=CD [7FFF]=AB; SP=7FFE
// 0135  F1         POP AF            F=CD A=AB SP=8000   (F=CD: S1 Z1 Y0 H0 X1 P1 N0 C1)
// 0136  CD 00 02   CALL 0200h        [7FFF]=01 [7FFE]=39 SP=7FFE PC=0200
// 0139  C4 10 02   CALL NZ,0210h     Z=1 -> NOT taken (10 T)
// 013C  CC 10 02   CALL Z,0210h      taken: [7FFF]=01 [7FFE]=3F SP=7FFE PC=0210
// 013F  DA 47 01   JP C,0147h        C=1 -> taken
// 0142  00 x5      (never executed)
// 0147  D2 60 01   JP NC,0160h       NOT taken
// 014A  C8         RET Z             Z=0 -> NOT taken (5 T)
// 014B  FF         RST 38h           [7FFF]=01 [7FFE]=4C SP=7FFE PC=0038
// 014C  0B         DEC BC            BC=AC0B-1=AC0A
// 014D  18 FE      JR $              END
// 0200  47         LD B,A            B=AB (F unchanged)
// 0201  C9         RET               PC=0139 SP=8000
// 0210  04         INC B             B=AB->AC; S1 Z0 H0 (B+1) V0 N0 YF=1 XF=1, C kept 1 -> F=A9
// 0211  C0         RET NZ            Z=0 -> taken (11 T): PC=013F SP=8000
// FINAL: A=AC F=A9 B=AC C=0A D=56 E=78 H=12 L=46 SP=8000 PC=014D I=5C,
//        R=80h + 47 M1 = AFh. Memory: program + [7FFF]=01 [7FFE]=4C
//        [7FFD]=56 [7FFC]=78 [A000]=34 [A001]=12 [A002]=55 [1246]=77.
// =============================================================================
static const uint8_t progB_0000[] = { 0xC3, 0x00, 0x01 };
static const uint8_t progB_0038[] = { 0x3C, 0xC9 };
static const uint8_t progB_0100[] = {
  0x31,0x00,0x80, 0x01,0x34,0x12, 0x11,0x78,0x56, 0x21,0x00,0x90,
  0xC5, 0xD5, 0xE1, 0xD1, 0xEB, 0x22,0x00,0xA0, 0x2A,0x01,0xA0,
  0x3E,0x55, 0x32,0x02,0xA0, 0x3A,0x00,0xA0, 0x09, 0x36,0x77, 0x34, 0x35,
  0x86, 0xCE,0x01, 0xDE,0x0D, 0x3C, 0x05, 0x07, 0x2F, 0x27,
  0xF5, 0xC1, 0xE5, 0x21,0xCD,0xAB, 0xE3, 0xF1,
  0xCD,0x00,0x02, 0xC4,0x10,0x02, 0xCC,0x10,0x02, 0xDA,0x47,0x01,
  0x00,0x00,0x00,0x00,0x00, 0xD2,0x60,0x01, 0xC8, 0xFF, 0x0B, 0x18,0xFE
};
static const uint8_t progB_0200[] = { 0x47, 0xC9 };
static const uint8_t progB_0210[] = { 0x04, 0xC0 };
static const uint16_t traceB[47] = {
  0x0000,0x0100,0x0103,0x0106,0x0109,0x010C,0x010D,0x010E,0x010F,0x0110,0x0111,
  0x0114,0x0117,0x0119,0x011C,0x011F,0x0120,0x0122,0x0123,0x0124,0x0125,0x0127,
  0x0129,0x012A,0x012B,0x012C,0x012D,0x012E,0x012F,0x0130,0x0131,0x0134,0x0135,
  0x0136,0x0200,0x0201,0x0139,0x013C,0x0210,0x0211,0x013F,0x0147,0x014A,0x014B,
  0x0038,0x0039,0x014C };
static const MemW writesB[8] = { {0x7FFF, 0x01}, {0x7FFE, 0x4C}, {0x7FFD, 0x56}, {0x7FFC, 0x78},
                                 {0xA000, 0x34}, {0xA001, 0x12}, {0xA002, 0x55}, {0x1246, 0x77} };
static const Prog PROG_B = { "B (extended families)", 0x0100, progB_0100, (int)sizeof progB_0100, 0x014D,
  traceB, 47, { 0xAC, 0xA9, 0xAC, 0x0A, 0x56, 0x78, 0x12, 0x46, 0x8000, 0x014D, 0x5C, 0xAF },
  writesB, 8 };

// =============================================================================
// PROGRAM C — CB space.
// Each CB instruction = two M1 cycles (prefix + opcode): two fetches in the trace, T from
// the CB row of the Appendix. Initial state: registers 0, F=00, I=5Ch, R=80h.
// addr  bytes    instruction    effect (by hand, docs/03 pp. 205-259, docs/05)
// 0000  3E 81    LD A,81h       A=81
// 0002  06 03    LD B,03h       B=03
// 0004  0E F0    LD C,F0h       C=F0
// 0006  16 55    LD D,55h       D=55
// 0008  21 00 40 LD HL,4000h    HL=4000
// 000B  36 96    LD (HL),96h    [4000]=96
// 000D  CB 07    RLC A          A=03, C=1 (bit 7 out)
// 000F  CB 08    RRC B          B=81, C=1 (bit 0 out)
// 0011  CB 11    RL C           C=F0 -> {1110000,C=1} = E1, C=1
// 0013  CB 1A    RR D           D=55 -> {C=1,0101010} = AA, C=1
// 0015  CB 26    SLA (HL)       [4000]=96 -> 2C, C=1
// 0017  CB 7E    BIT 7,(HL)     bit 7 of 2C = 0 -> Z=1 (F overwritten later)
// 0019  CB C8    SET 1,B        B=83
// 001B  CB 8F    RES 1,A        A=01
// 001D  CB F6    SET 6,(HL)     [4000]=6C
// 001F  CB 96    RES 2,(HL)     [4000]=68
// 0021  CB 47    BIT 0,A        bit 0 of 01 = 1 -> Z=0 (F overwritten later)
// 0023  CB 3F    SRL A          A=01 -> 00, C=1; S0 Z1 H0 P=1 (parity of 00) N0
//                               YF/XF (bits 5/3 of 00) = 0 -> F=45
// 0025  18 FE    JR $           END
// FINAL: A=00 F=45 B=83 C=E1 D=AA E=00 H=40 L=00 SP=0000 PC=0025 I=5C,
//        R = 80h + 30 M1 (6 + 12x2) = 9Eh. Memory: program + [4000]=68.
// =============================================================================
static const uint8_t progC[] = {
  0x3E,0x81, 0x06,0x03, 0x0E,0xF0, 0x16,0x55, 0x21,0x00,0x40, 0x36,0x96,
  0xCB,0x07, 0xCB,0x08, 0xCB,0x11, 0xCB,0x1A, 0xCB,0x26, 0xCB,0x7E,
  0xCB,0xC8, 0xCB,0x8F, 0xCB,0xF6, 0xCB,0x96, 0xCB,0x47, 0xCB,0x3F, 0x18,0xFE
};
static const uint16_t traceC[30] = {
  0x0000,0x0002,0x0004,0x0006,0x0008,0x000B,
  0x000D,0x000E, 0x000F,0x0010, 0x0011,0x0012, 0x0013,0x0014, 0x0015,0x0016, 0x0017,0x0018,
  0x0019,0x001A, 0x001B,0x001C, 0x001D,0x001E, 0x001F,0x0020, 0x0021,0x0022, 0x0023,0x0024 };
static const MemW writesC[1] = { {0x4000, 0x68} };
static const Prog PROG_C = { "C (CB space)", 0x0000, progC, (int)sizeof progC, 0x0025,
  traceC, 30, { 0x00, 0x45, 0x83, 0xE1, 0xAA, 0x00, 0x40, 0x00, 0x0000, 0x0025, 0x5C, 0x9E },
  writesC, 1 };

// =============================================================================
// PROGRAM D — executable ED space. Initial state:
// registers 0, F=00, I=5Ch, R=80h, IFF1=0, IFF2=1 (preset through the savestate), IM=0.
// addr  bytes        instruction     effect (by hand: docs/03 pp. 96-192, docs/05 §9)
// 0000  31 00 90     LD SP,9000h     SP=9000
// 0003  3E 12        LD A,12h        A=12
// 0005  ED 47        LD I,A          I=12 (written in T5A of the 2nd M1: that refresh is not checked)
// 0007  3E 34        LD A,34h        A=34
// 0009  ED 4F        LD R,A          R=34 written in T5A, AFTER the refresh +1 of T4B -> R=34 (as on the silicon)
// 000B  ED 5E        IM 2            IM=2
// 000D  01 34 12     LD BC,1234h
// 0010  11 78 56     LD DE,5678h
// 0013  21 CD AB     LD HL,ABCDh
// 0016  ED 43 00 A0  LD (A000h),BC   [A000]=34 [A001]=12
// 001A  ED 53 02 A0  LD (A002h),DE   [A002]=78 [A003]=56
// 001E  ED 4B 02 A0  LD BC,(A002h)   BC=5678
// 0022  ED 4A        ADC HL,BC       ABCD+5678+0 = 1_0245: HL=0245; S0 Z0 H1 (B+6+1 in the high step) V0 N0 C1;
//                                    YF/XF from the high byte 02 -> F=11
// 0024  ED 42        SBC HL,BC       0245-5678-1 = ABCC: HL=ABCC; S1 Z0 H1 V0 N1 C1; YF/XF from AB -> F=BB
// 0026  ED 44        NEG             A=00-34=CC; S1 Z0 H1 V0 N1 C1; YF/XF from CC (Y0 X1) -> F=9B
// 0028  ED 56        IM 1            IM=1
// 002A  21 00 B0     LD HL,B000h     ([B000..B002] = 11 22 33: data)
// 002D  11 00 B1     LD DE,B100h
// 0030  01 03 00     LD BC,0003h
// 0033  ED B0        LDIR            3 iterations (21+21+16 T): [B100..B102]=11 22 33; HL=B003 DE=B103 BC=0000
//                                    F: S,Z,C unchanged (1,0,1), H0 N0 P0 (BC=0); YF/XF from n=A+(HL)=CC+33=FF -> F=A9
// 0035  21 00 B1     LD HL,B100h
// 0038  01 03 00     LD BC,0003h
// 003B  3E 22        LD A,22h
// 003D  ED B1        CPIR            [B100]=11 != 22: BC=2, repeats (21 T); [B101]=22: Z=1, end (16 T)
//                                    HL=B102 BC=0001; S0 Z1 H0 P1 (BC!=0) N1 C1 unchanged; n=0 -> F=47
// 003F  21 47 00     LD HL,0047h
// 0042  E5           PUSH HL         [8FFF]=00 [8FFE]=47 SP=8FFE
// 0043  ED 45        RETN            PC=0047 SP=9000; IFF1 <- IFF2 = 1
// 0045  00 00        (not executed)
// 0047  21 4D 00     LD HL,004Dh
// 004A  E5           PUSH HL         [8FFF]=00 [8FFE]=4D
// 004B  ED 4D        RETI            PC=004D SP=9000; IFF1 <- IFF2 (both already 1)
// 004D  ED 7B 00 A0  LD SP,(A000h)   SP=1234
// 0051  18 FE        JR $            END
// FINAL: A=22 F=47 B=00 C=01 D=B1 E=03 H=00 L=4D SP=1234 PC=0051 I=12,
//        R = 34h + 45 M1 = 61h; IM=1 IFF1=1 IFF2=1. 52 fetches (7 up to and including LD R,A).
// =============================================================================
static const uint8_t progD[] = {
  0x31,0x00,0x90, 0x3E,0x12, 0xED,0x47, 0x3E,0x34, 0xED,0x4F, 0xED,0x5E,
  0x01,0x34,0x12, 0x11,0x78,0x56, 0x21,0xCD,0xAB,
  0xED,0x43,0x00,0xA0, 0xED,0x53,0x02,0xA0, 0xED,0x4B,0x02,0xA0,
  0xED,0x4A, 0xED,0x42, 0xED,0x44, 0xED,0x56,
  0x21,0x00,0xB0, 0x11,0x00,0xB1, 0x01,0x03,0x00, 0xED,0xB0,
  0x21,0x00,0xB1, 0x01,0x03,0x00, 0x3E,0x22, 0xED,0xB1,
  0x21,0x47,0x00, 0xE5, 0xED,0x45, 0x00,0x00,
  0x21,0x4D,0x00, 0xE5, 0xED,0x4D, 0xED,0x7B,0x00,0xA0, 0x18,0xFE
};
static const uint8_t progD_B000[3] = { 0x11, 0x22, 0x33 };
static const uint16_t traceD[52] = {
  0x0000, 0x0003, 0x0005,0x0006, 0x0007, 0x0009,0x000A, 0x000B,0x000C,
  0x000D, 0x0010, 0x0013, 0x0016,0x0017, 0x001A,0x001B, 0x001E,0x001F,
  0x0022,0x0023, 0x0024,0x0025, 0x0026,0x0027, 0x0028,0x0029,
  0x002A, 0x002D, 0x0030, 0x0033,0x0034, 0x0033,0x0034, 0x0033,0x0034,
  0x0035, 0x0038, 0x003B, 0x003D,0x003E, 0x003D,0x003E,
  0x003F, 0x0042, 0x0043,0x0044, 0x0047, 0x004A, 0x004B,0x004C, 0x004D,0x004E };
static const MemW writesD[9] = { {0xA000, 0x34}, {0xA001, 0x12}, {0xA002, 0x78}, {0xA003, 0x56},
                                 {0xB100, 0x11}, {0xB101, 0x22}, {0xB102, 0x33},
                                 {0x8FFE, 0x4D}, {0x8FFF, 0x00} };
static const Prog PROG_D = { "D (ED space)", 0x0000, progD, (int)sizeof progD, 0x0051,
  traceD, 52, { 0x22, 0x47, 0x00, 0x01, 0xB1, 0x03, 0x00, 0x4D, 0x1234, 0x0051, 0x12, 0x61, 1, 1, 1 },
  writesD, 9, 0, 1, 4, 0x12, 7, 0x34 };

// =============================================================================
// PROGRAM E — DI and IM with IFF1=IFF2=1 preset: IM 2; DI; JR $.
// FINAL: IM=2, IFF1=0, IFF2=0; registers 0; PC=0003; R=80h+3=83h; 3 fetches.
// =============================================================================
static const uint8_t progE[] = { 0xED,0x5E, 0xF3, 0x18,0xFE };
static const uint16_t traceE[3] = { 0x0000, 0x0001, 0x0002 };
static const Prog PROG_E = { "E (DI, IM)", 0x0000, progE, (int)sizeof progE, 0x0003,
  traceE, 3, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000, 0x0003, 0x5C, 0x83, 2, 0, 0 },
  nullptr, 0, 1, 1 };

// =============================================================================
// PROGRAM G — HALT, NMI, RETN. IFF preset 0,0; IM 0.
// 0000  31 00 90  LD SP,9000h
// 0003  FB        EI              IFF1=IFF2=1 (window closed at its end)
// 0004  76        HALT            PC=0005; then M1 of NOP at 0005: byte read, IR forced 00, PC held, refresh alive
// 0005  C3 10 00  JP 0010h        (executed only after the RETN)
// 0010  18 FE     JR $            END
// 0066  ED 45     RETN            PC <- 0005, IFF1 <- IFF2
// NMI low for 2 ticks from the 5th fetch (2nd NOP): accepted at the end of the NOP -> M1 at 0005 IGNORED (5 T),
// push 0005 ([8FFF]=00 [8FFE]=05), PC <- 0066, IFF1 <- 0 (IFF2 stays 1), leaves HALT.
// Trace (9 fetches): 0000 0003 0004 0005(NOP) 0005(NOP) 0005(NMI) 0066 0067 0005(JP).
// T: 10+4+4+4+4+11+14+10 = 61. FINAL: SP=9000 PC=0010 IM=0 IFF1=1 IFF2=1; R = 80h+9 = 89h;
// halt_n low: T4B of HALT + 2 NOP x 8 = 17 ticks.
// =============================================================================
static const uint8_t progG[] = { 0x31,0x00,0x90, 0xFB, 0x76, 0xC3,0x10,0x00 };
static const uint8_t progG_0010[2] = { 0x18, 0xFE };
static const uint8_t progG_0066[2] = { 0xED, 0x45 };
static const uint16_t traceG[9] = { 0x0000, 0x0003, 0x0004, 0x0005, 0x0005, 0x0005, 0x0066, 0x0067, 0x0005 };
static const Ev evG[3] = { {3, 5}, {4, 5}, {5, 1} };
static const MemW writesG[2] = { {0x8FFE, 0x05}, {0x8FFF, 0x00} };
static const Prog PROG_G = { "G (HALT, NMI, RETN)", 0x0000, progG, (int)sizeof progG, 0x0010,
  traceG, 9, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9000, 0x0010, 0x5C, 0x89, 0, 1, 1 },
  writesG, 2, 0, 0, -1, 0, -1, 0, 5, -1, 0, evG, 3, 17 };

// =============================================================================
// PROGRAM F — INT mode 1 and the EI window. IFF preset 0,0.
// 0000  31 00 90  LD SP,9000h | 0003  ED 56  IM 1 | 0005  FB  EI | 0006  3E 11  LD A,11h | 0008  18 FE  JR $
// 0038  3C  INC A | 0039  ED 4D  RETI
// INT low from the 4th fetch (EI): NOT accepted at the end of EI (docs/04 §3), accepted at the end of
// LD A,11h: IACK 7 T (IORQ, 2 waits; the byte is ignored), push 0008, PC <- 0038 (13 T); IFF1=IFF2=0;
// handler: INC A (A=12, F=00), RETI -> 0008 (end). Trace (8): 0000 0003 0004 0005 0006 0038 0039 003A.
// T: 10+8+4+7+13+4+14 = 60. FINAL: A=12 F=00 SP=9000 PC=0008 IM=1 IFF 0,0; R = 80h + 8 M1 + 1 IACK = 89h.
// =============================================================================
static const uint8_t progF[] = { 0x31,0x00,0x90, 0xED,0x56, 0xFB, 0x3E,0x11, 0x18,0xFE };
static const uint8_t progF_0038[3] = { 0x3C, 0xED, 0x4D };
static const uint16_t traceF[8] = { 0x0000, 0x0003, 0x0004, 0x0005, 0x0006, 0x0038, 0x0039, 0x003A };
static const Ev evF[1] = { {5, 2} };
static const MemW writesF[2] = { {0x8FFE, 0x08}, {0x8FFF, 0x00} };
static const Prog PROG_F = { "F (INT IM1, EI window)", 0x0000, progF, (int)sizeof progF, 0x0008,
  traceF, 8, { 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9000, 0x0008, 0x5C, 0x89, 1, 0, 0 },
  writesF, 2, 0, 0, -1, 0, -1, 0, -1, 4, 0x00, evF, 1, -1 };

// =============================================================================
// PROGRAM H — INT mode 2. IFF preset 0,0.
// 0000  31 00 90  LD SP,9000h | 0003  3E 20  LD A,20h | 0005  ED 47  LD I,A (I=20) | 0007  ED 5E  IM 2
// 0009  FB  EI | 000A  00  NOP | 000B  18 FE  JR $ | table 2040: FF 00 30 | 3000  ED 4D  RETI
// INT low from the 7th fetch (EI): accepted at the end of the NOP. IACK: the device puts 41h and the CPU uses
// ALL 8 bits ([SY] p.20 and p.24, measured): pointer {I,41} = 2041 -> PC = 3000. A CPU that forced bit 0
// to 0 would read 2040 -> PC = 00FF: the bench sees it.
// push 000B, 19 T; RETI -> 000B (end). Trace (10): 0000 0003 0005 0006 0007 0008 0009 000A 3000 3001.
// T: 10+7+9+8+4+4+19+14 = 75. FINAL: A=20 F=00 I=20 SP=9000 PC=000B IM=2 IFF 0,0;
// R = 80h + 10 M1 + 1 IACK = 8Bh; LD I,A at the 4th M1 (refresh not checked there, then I = 20).
// =============================================================================
static const uint8_t progH[] = { 0x31,0x00,0x90, 0x3E,0x20, 0xED,0x47, 0xED,0x5E, 0xFB, 0x00, 0x18,0xFE };
static const uint8_t progH_2040[3] = { 0xFF, 0x00, 0x30 };   // 2041 = 3000h (right), 2040 = 00FFh (bit 0 forced)
static const uint8_t progH_3000[2] = { 0xED, 0x4D };
static const uint16_t traceH[10] = { 0x0000, 0x0003, 0x0005, 0x0006, 0x0007, 0x0008, 0x0009, 0x000A, 0x3000, 0x3001 };
static const Ev evH[1] = { {8, 3} };
static const MemW writesH[2] = { {0x8FFE, 0x0B}, {0x8FFF, 0x00} };
static const Prog PROG_H = { "H (INT IM2)", 0x0000, progH, (int)sizeof progH, 0x000B,
  traceH, 10, { 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9000, 0x000B, 0x20, 0x8B, 2, 0, 0 },
  writesH, 2, 0, 0, 4, 0x20, -1, 0, -1, 7, 0x41, evH, 1, -1 };

// =============================================================================
// PROGRAM I — INT mode 0: the device puts RST 10h (D7) on the bus in the IACK.
// 0000  31 00 90  LD SP,9000h | 0003  FB  EI | 0004  00  NOP | 0005  18 FE  JR $ | 0010  ED 4D  RETI
// INT low from the 2nd fetch (EI): accepted at the end of the NOP; the M1 of the instruction is the IACK (5 + 2 = 7 T,
// IORQ, IR <- D7, PC held), then push 0005, PC <- 0010 (13 T); RETI -> 0005 (end).
// Trace (5): 0000 0003 0004 0010 0011. T: 10+4+4+13+14 = 45. FINAL: SP=9000 PC=0005 IM=0 IFF 0,0;
// [8FFE]=05 [8FFF]=00; R = 80h + 5 M1 + 1 IACK = 86h.
// =============================================================================
static const uint8_t progI[] = { 0x31,0x00,0x90, 0xFB, 0x00, 0x18,0xFE };
static const uint8_t progI_0010[2] = { 0xED, 0x4D };
static const uint16_t traceI[5] = { 0x0000, 0x0003, 0x0004, 0x0010, 0x0011 };
static const Ev evI[1] = { {3, 4} };
static const MemW writesI[2] = { {0x8FFE, 0x05}, {0x8FFF, 0x00} };
static const Prog PROG_I = { "I (INT IM0, RST 10h from the bus)", 0x0000, progI, (int)sizeof progI, 0x0005,
  traceI, 5, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9000, 0x0005, 0x5C, 0x86, 0, 0, 0 },
  writesI, 2, 0, 0, -1, 0, -1, 0, -1, 2, 0xD7, evI, 1, -1 };

// =============================================================================
// PROGRAM J — DD/FD space without displacement. Registers 0, F=00, I=5C, R=80.
// 0000  31 00 90     LD SP,9000h     | 0003  DD 21 34 12  LD IX,1234h  | 0007  FD 21 78 56  LD IY,5678h
// 000B  DD 26 AB     LD IXh,ABh      | 000E  DD 2E CD     LD IXl,CDh   -> IX=ABCD ([SY] §5.2)
// 0011  DD 7C        LD A,IXh        A=AB
// 0013  DD 85        ADD A,IXl       AB+CD=1_78: A=78; S0 Z0 H1 V1 (-85-51<-128) N0 C1; YF/XF from 78 -> F=3D
// 0015  DD 23        INC IX          IX=ABCE   | 0017  FD 2B  DEC IY  IY=5677
// 0019  DD 09        ADD IX,BC       BC=0: IX=ABCE; H0 C0 N0, YF/XF from AB; S/Z/P unchanged -> F=2C
// 001B  DD 29        ADD IX,IX       ABCE+ABCE=1_579C: IX=579C; H1 (B+B+1) C1 N0; YF/XF from 57 -> F=15
// 001D  DD E5        PUSH IX         [8FFF]=57 [8FFE]=9C SP=8FFE
// 001F  FD E1        POP IY          IY=579C SP=9000
// 0021  DD E9        JP (IX)         PC=579C
// 579C  DD F9        LD SP,IX        SP=579C  | 579E  21 00 40  LD HL,4000h | 57A1  31 00 90  LD SP,9000h
// 57A4  DD 22 00 A0  LD (A000h),IX   [A000]=9C [A001]=57
// 57A8  FD 2A 00 A0  LD IY,(A000h)   IY=579C
// 57AC  DD 00        NOP with a prefix that "does not touch" it: 4+4 T
// 57AE  DD DD 21 11 22  LD IX,2211h: the first DD is a stray prefix (+4 T), the last one counts (docs/03 §3.3)
// 57B3  DD EB        EX DE,HL: NOT touched by the prefix (docs/03 §3.4): DE=4000 HL=0000
// 57B5  DD E3        EX (SP),IX      IX <- [9000..9001] = 0000; [9000]=11 [9001]=22
// 57B7  18 FE        JR $            END
// FINAL: A=78 F=15 D=40 E=00 HL=0000 SP=9000 PC=57B7 IX=0000 IY=579C I=5C; 44 fetches -> R=ACh.
// =============================================================================
static const uint8_t progJ[] = {
  0x31,0x00,0x90, 0xDD,0x21,0x34,0x12, 0xFD,0x21,0x78,0x56, 0xDD,0x26,0xAB, 0xDD,0x2E,0xCD,
  0xDD,0x7C, 0xDD,0x85, 0xDD,0x23, 0xFD,0x2B, 0xDD,0x09, 0xDD,0x29, 0xDD,0xE5, 0xFD,0xE1, 0xDD,0xE9 };
static const uint8_t progJ_579C[] = {
  0xDD,0xF9, 0x21,0x00,0x40, 0x31,0x00,0x90, 0xDD,0x22,0x00,0xA0, 0xFD,0x2A,0x00,0xA0,
  0xDD,0x00, 0xDD,0xDD,0x21,0x11,0x22, 0xDD,0xEB, 0xDD,0xE3, 0x18,0xFE };
static const uint16_t traceJ[44] = {
  0x0000, 0x0003,0x0004, 0x0007,0x0008, 0x000B,0x000C, 0x000E,0x000F, 0x0011,0x0012, 0x0013,0x0014,
  0x0015,0x0016, 0x0017,0x0018, 0x0019,0x001A, 0x001B,0x001C, 0x001D,0x001E, 0x001F,0x0020, 0x0021,0x0022,
  0x579C,0x579D, 0x579E, 0x57A1, 0x57A4,0x57A5, 0x57A8,0x57A9, 0x57AC,0x57AD, 0x57AE,0x57AF,0x57B0,
  0x57B3,0x57B4, 0x57B5,0x57B6 };
static const MemW writesJ[6] = { {0x8FFF, 0x57}, {0x8FFE, 0x9C}, {0xA000, 0x9C}, {0xA001, 0x57}, {0x9000, 0x11}, {0x9001, 0x22} };
static const Prog PROG_J = { "J (DD/FD without displacement)", 0x0000, progJ, (int)sizeof progJ, 0x57B7,
  traceJ, 44, { 0x78, 0x15, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x9000, 0x57B7, 0x5C, 0xAC, -1, -1, -1, 0x0000, 0x579C },
  writesJ, 6 };

// =============================================================================
// PROGRAM K — (IX+d)/(IY+d) forms. Registers 0, I=5C, R=80.
// 0000  DD 21 00 40  LD IX,4000h   | 0004  FD 21 10 40  LD IY,4010h
// 0008  DD 36 05 3C  LD (IX+5),3Ch  [4005]=3C  | 000C  FD 36 FB C3  LD (IY-5),C3h  [400B]=C3
// 0010  DD 7E 05     LD A,(IX+5)   A=3C        | 0013  FD 46 FB     LD B,(IY-5)    B=C3
// 0016  DD 86 05     ADD A,(IX+5)  3C+3C=78: H1 (C+C), V0, YF/XF from 78 -> F=38
// 0019  FD 96 FB     SUB (IY-5)    78-C3=B5 borrow: S1 H0 V1 (pos-neg=neg) N1 C1, YF/XF from B5 -> F=A7
// 001C  DD 34 05     INC (IX+5)    [4005]=3D: S0 Z0 H0 V0 N0, YF/XF from 3D, C kept -> F=29
// 001F  FD 35 FB     DEC (IY-5)    [400B]=C2: S1 H0 V0 N1, YF/XF from C2, C kept -> F=83
// 0022  DD 70 05     LD (IX+5),B   [4005]=C3   | 0025  DD 66 05  LD H,(IX+5)  H=C3 (IX stays 4000: H, not IXh)
// 0028  FD 74 FA     LD (IY-6),H   [400A]=C3   | 002B  DD 4E 05  LD C,(IX+5)  C=C3
// 002E  DD BE 05     CP (IX+5)     B5-C3 borrow: S1 H0 V0 N1 C1, YF/XF from the OPERAND C3 -> F=83
// 0031  18 FE        JR $          END
// FINAL: A=B5 F=83 B=C3 C=C3 H=C3 L=00 SP=0000 PC=0031 IX=4000 IY=4010 I=5C; 30 fetches -> R=9Eh.
// =============================================================================
static const uint8_t progK[] = {
  0xDD,0x21,0x00,0x40, 0xFD,0x21,0x10,0x40, 0xDD,0x36,0x05,0x3C, 0xFD,0x36,0xFB,0xC3,
  0xDD,0x7E,0x05, 0xFD,0x46,0xFB, 0xDD,0x86,0x05, 0xFD,0x96,0xFB, 0xDD,0x34,0x05, 0xFD,0x35,0xFB,
  0xDD,0x70,0x05, 0xDD,0x66,0x05, 0xFD,0x74,0xFA, 0xDD,0x4E,0x05, 0xDD,0xBE,0x05, 0x18,0xFE };
static const uint16_t traceK[30] = {
  0x0000,0x0001, 0x0004,0x0005, 0x0008,0x0009, 0x000C,0x000D, 0x0010,0x0011, 0x0013,0x0014,
  0x0016,0x0017, 0x0019,0x001A, 0x001C,0x001D, 0x001F,0x0020, 0x0022,0x0023, 0x0025,0x0026,
  0x0028,0x0029, 0x002B,0x002C, 0x002E,0x002F };
static const MemW writesK[3] = { {0x4005, 0xC3}, {0x400A, 0xC3}, {0x400B, 0xC2} };
static const Prog PROG_K = { "K ((IX+d) forms)", 0x0000, progK, (int)sizeof progK, 0x0031,
  traceK, 30, { 0xB5, 0x83, 0xC3, 0xC3, 0x00, 0x00, 0xC3, 0x00, 0x0000, 0x0031, 0x5C, 0x9E, -1, -1, -1, 0x4000, 0x4010 },
  writesK, 3 };

// =============================================================================
// PROGRAM L — DDCB/FDCB spaces with the double effect. Registers 0, I=5C, R=80.
// 0000  DD 21 00 50   LD IX,5000h    | 0004  FD 21 10 50  LD IY,5010h
// 0008  DD 36 02 81   LD (IX+2),81h  [5002]=81
// 000C  DD CB 02 06   RLC (IX+2)     81->03 C1: P1 (2 bits) -> F=05; [5002]=03
// 0010  DD CB 02 00   RLC (IX+2),B   03->06 C0: F=04; [5002]=06; B=06 (double effect)
// 0014  DD CB 02 46   BIT 0,(IX+2)   bit0 of 06 = 0: Z1 H1 (F redefined later by RR)
// 0018  FD 36 F8 55   LD (IY-8),55h  [5008]=55
// 001C  FD CB F8 C7   SET 0,(IY-8),A 55 (bit already 1): [5008]=55, A=55; F unchanged
// 0020  FD CB F8 97   RES 2,(IY-8),A 55->51: [5008]=51, A=51; F unchanged
// 0024  DD CB 02 1E   RR (IX+2)      06 with C0 -> 03, C0: P1 -> F=04; [5002]=03
// 0028  FD CB F8 3D   SRL (IY-8),L   51->28 C1: P1 (2 bits), YF/XF from 28 -> F=2D; [5008]=28; L=28
// 002C  18 FE         JR $           END
// FINAL: A=51 F=2D B=06 C=00 H=00 L=28 SP=0000 PC=002C IX=5000 IY=5010 I=5C; 22 fetches -> R=96h.
// =============================================================================
static const uint8_t progL[] = {
  0xDD,0x21,0x00,0x50, 0xFD,0x21,0x10,0x50, 0xDD,0x36,0x02,0x81, 0xDD,0xCB,0x02,0x06,
  0xDD,0xCB,0x02,0x00, 0xDD,0xCB,0x02,0x46, 0xFD,0x36,0xF8,0x55, 0xFD,0xCB,0xF8,0xC7,
  0xFD,0xCB,0xF8,0x97, 0xDD,0xCB,0x02,0x1E, 0xFD,0xCB,0xF8,0x3D, 0x18,0xFE };
static const uint16_t traceL[22] = {
  0x0000,0x0001, 0x0004,0x0005, 0x0008,0x0009, 0x000C,0x000D, 0x0010,0x0011, 0x0014,0x0015,
  0x0018,0x0019, 0x001C,0x001D, 0x0020,0x0021, 0x0024,0x0025, 0x0028,0x0029 };
static const MemW writesL[2] = { {0x5002, 0x03}, {0x5008, 0x28} };
static const Prog PROG_L = { "L (DDCB/FDCB, double effect)", 0x0000, progL, (int)sizeof progL, 0x002C,
  traceL, 22, { 0x51, 0x2D, 0x06, 0x00, 0x00, 0x00, 0x00, 0x28, 0x0000, 0x002C, 0x5C, 0x96, -1, -1, -1, 0x5000, 0x5010 },
  writesL, 2 };

// =============================================================================
// PROGRAM M — INT accepted at the end of an instruction WITH A PREFIX. IFF preset 0,0.
// 0000  31 00 90     LD SP,9000h | 0003  ED 56  IM 1 | 0005  FB  EI | 0006  DD 21 34 12  LD IX,1234h
// 000A  18 FE        JR $ | 0038  3C  INC A | 0039  ED 4D  RETI
// INT low from the 4th fetch (EI): not accepted at the end of EI, accepted at the end of LD IX,nn (DD space:
// the window opens at the end of EVERY instruction, docs/04 §3): IACK 7 T, push 000A, PC <- 0038 (13 T);
// handler INC A (A=01), RETI -> 000A (end). Trace (9): 0000 0003 0004 0005 0006 0007 0038 0039 003A.
// T: 10+8+4+14+13+4+14 = 67. FINAL: A=01 F=00 SP=9000 PC=000A IX=1234 IM=1 IFF 0,0; R = 80h + 9 M1 + 1 IACK = 8Ah.
// =============================================================================
static const uint8_t progM[] = { 0x31,0x00,0x90, 0xED,0x56, 0xFB, 0xDD,0x21,0x34,0x12, 0x18,0xFE };
static const uint8_t progM_0038[3] = { 0x3C, 0xED, 0x4D };
static const uint16_t traceM[9] = { 0x0000, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007, 0x0038, 0x0039, 0x003A };
static const Ev evM[1] = { {6, 2} };
static const MemW writesM[2] = { {0x8FFE, 0x0A}, {0x8FFF, 0x00} };
static const Prog PROG_M = { "M (INT after an instruction with a prefix)", 0x0000, progM, (int)sizeof progM, 0x000A,
  traceM, 9, { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9000, 0x000A, 0x5C, 0x8A, 1, 0, 0, 0x1234, -1 },
  writesM, 2, 0, 0, -1, 0, -1, 0, -1, 4, 0x00, evM, 1, -1 };

// =============================================================================
// PROGRAM N — complete I/O. Registers 0, I=5C, R=80. Input port: data =
// hi ^ lo ^ 5A of the 16-bit address; the port writes are checked with address and data.
// Flags of the block I/O instructions: [SY] §4.3 (docs/05 §9).
// 0000  3E 12     LD A,12h    | 0002  DB 34  IN A,(34h)   port 1234 -> 7C: A=7C (F unchanged)
// 0004  D3 56     OUT (56h),A port 7C56 <- 7C
// 0006  01 40 03  LD BC,0340h -> B=03 C=40
// 0009  ED 40     IN B,(C)    port 0340 -> 19: B=19; F: P0 (3 bits), XF=1 -> F=08
// 000B  ED 70     IN F,(C)    port 1940 -> 03: F: P1 -> F=04 (B stays 19)
// 000D  ED 79     OUT (C),A   port 1940 <- 7C
// 000F  ED 71     OUT (C),0   port 1940 <- 00 (NMOS)
// 0011  21 00 60  LD HL,6000h | 0014  06 02  LD B,02h
// 0016  ED A2     INI         port 0240 (B before the decrement) -> 18: [6000]=18, HL=6001, B=01, F=04
// 0018  ED AA     IND         port 0140 -> 1B: [6001]=1B, HL=6000, B=00, F=40
// 001A  06 02     LD B,02h
// 001C  ED B2     INIR        2 iterations (21+16 T): [6000]=18, [6001]=1B, HL=6002, B=00, F=40
// 001E  36 AB     LD (HL),ABh [6002]=AB | 0020  06 02  LD B,02h
// 0022  ED A3     OUTI        B=01 then port 0140 <- [6002]=AB, HL=6003, F=02
// 0024  2B        DEC HL      HL=6002
// 0025  ED AB     OUTD        B=00, port 0040 <- AB, HL=6001, F=42
// 0027  21 03 60  LD HL,6003h | 002A  36 CD  LD (HL),CDh  [6003]=CD | 002C  2B  DEC HL  HL=6002 | 002D  06 02  LD B,02h
// 002F  ED B3     OTIR        2 iterations: port 0140 <- AB (HL 6003), port 0040 <- CD (HL 6004), B=00, F=42
// 0031  18 FE     JR $        END
// FINAL: A=7C F=42 B=00 C=40 HL=6004 SP=0000 PC=0031 I=5C; 38 fetches (the repeated iterations redo the M1s) -> R=A6h.
// =============================================================================
static const uint8_t progN[] = {
  0x3E,0x12, 0xDB,0x34, 0xD3,0x56, 0x01,0x40,0x03, 0xED,0x40, 0xED,0x70, 0xED,0x79, 0xED,0x71,
  0x21,0x00,0x60, 0x06,0x02, 0xED,0xA2, 0xED,0xAA, 0x06,0x02, 0xED,0xB2, 0x36,0xAB, 0x06,0x02,
  0xED,0xA3, 0x2B, 0xED,0xAB, 0x21,0x03,0x60, 0x36,0xCD, 0x2B, 0x06,0x02, 0xED,0xB3, 0x18,0xFE };
static const uint16_t traceN[38] = {
  0x0000, 0x0002, 0x0004, 0x0006, 0x0009,0x000A, 0x000B,0x000C, 0x000D,0x000E, 0x000F,0x0010,
  0x0011, 0x0014, 0x0016,0x0017, 0x0018,0x0019, 0x001A, 0x001C,0x001D,0x001C,0x001D, 0x001E, 0x0020,
  0x0022,0x0023, 0x0024, 0x0025,0x0026, 0x0027, 0x002A, 0x002C, 0x002D, 0x002F,0x0030,0x002F,0x0030 };
static const MemW writesN[4] = { {0x6000, 0x18}, {0x6001, 0x1B}, {0x6002, 0xAB}, {0x6003, 0xCD} };
static const MemW iowN[7] = { {0x7C56, 0x7C}, {0x1940, 0x7C}, {0x1940, 0x00}, {0x0140, 0xAB}, {0x0040, 0xAB}, {0x0140, 0xAB}, {0x0040, 0xCD} };
static const Prog PROG_N = { "N (I/O: IN/OUT n, IN/OUT (C), INI/IND/OUTI/OUTD and ...R)", 0x0000, progN, (int)sizeof progN, 0x0031,
  traceN, 38, { 0x7C, 0x42, 0x00, 0x40, 0x00, 0x00, 0x60, 0x04, 0x0000, 0x0031, 0x5C, 0xA6, -1, -1, -1, -1, -1 },
  writesN, 4, 0, 0, -1, 0, -1, 0, -1, -1, 0x00, nullptr, 0, -1, iowN, 7 };

// =============================================================================
// PROGRAM O — RLD/RRD and LD A,I / LD A,R. Registers 0, I=5C, R=80, IFF 0,0.
// 0000  21 00 70  LD HL,7000h | 0003  36 7A  LD (HL),7Ah  [7000]=7A | 0005  3E 31  LD A,31h
// 0007  ED 6F     RLD   (HL)' = {m[3:0], A[3:0]} = A1, A' = {A[7:4], m[7:4]} = 37; S0 Z0 H0 P0 (5 bits) N0 C0, YF/XF from 37 -> F=20
// 0009  ED 67     RRD   m=A1: (HL)' = {A[3:0], m[7:4]} = 7A, A' = {A[7:4], m[3:0]} = 31; P0 (3 bits), YF/XF from 31 -> F=20
// 000B  3E 00     LD A,00h
// 000D  ED 57     LD A,I  A=5C; S0 Z0 H0 P/V=IFF2=0 N0 C0, YF/XF from 5C -> F=08 (9 T)
// 000F  FB        EI      IFF1=IFF2=1
// 0010  ED 5F     LD A,R  R after the refresh of THIS M1 (r_inc in T4B, read in T5A): 13th M1 -> 8D;
//                         S1 Z0 H0 P/V=IFF2=1 N0 C0, YF/XF from 8D -> F=8C
// 0012  18 FE     JR $    END
// FINAL: A=8D F=8C HL=7000 SP=0000 PC=0012 I=5C IFF 1,1; 13 fetches -> R=8Dh; [7000]=7A.
// =============================================================================
static const uint8_t progO[] = {
  0x21,0x00,0x70, 0x36,0x7A, 0x3E,0x31, 0xED,0x6F, 0xED,0x67, 0x3E,0x00, 0xED,0x57, 0xFB, 0xED,0x5F, 0x18,0xFE };
static const uint16_t traceO[13] = {
  0x0000, 0x0003, 0x0005, 0x0007,0x0008, 0x0009,0x000A, 0x000B, 0x000D,0x000E, 0x000F, 0x0010,0x0011 };
static const MemW writesO[1] = { {0x7000, 0x7A} };
static const Prog PROG_O = { "O (RLD/RRD, LD A,I / LD A,R)", 0x0000, progO, (int)sizeof progO, 0x0012,
  traceO, 13, { 0x8D, 0x8C, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00, 0x0000, 0x0012, 0x5C, 0x8D, -1, 1, 1 },
  writesO, 1 };

// =============================================================================
// PROGRAM P — SCF/CCF and the Q register, NMOS variant (docs/01 §3, docs/05 §9:
// YF/XF = Q | A, with Q = F at the last write of F). Registers 0, Q=0.
// 0000  3E 28  LD A,28h
// 0002  B7     OR A        A=28: S0 Z0 H0 P1 (2 bits) N0 C0, YF/XF from 28 -> F=2C; Q <- 2C
// 0003  3E 00  LD A,00h    (Q stays 2C: LD does not write F)
// 0005  37     SCF         the previous instruction (LD) did NOT write F: YF/XF = A | F = 28; C=1 H=0, S/Z/P from F_in (P1): F=2D
// 0006  3F     CCF         the previous instruction (SCF) DID write F: YF/XF = A = 00; C = ~1 = 0, H = C_in = 1, P from F_in: F=14
// 0007  18 FE  JR $        END
// FINAL: A=00 F=14; SP=0000 PC=0007 I=5C; 5 fetches -> R=85h. The rule is the one z80test checks with values
// from a real Z80 (its SCF+CCF and CCF+SCF tests). CMOS would give 05 and then 14.
// =============================================================================
static const uint8_t progP[] = { 0x3E,0x28, 0xB7, 0x3E,0x00, 0x37, 0x3F, 0x18,0xFE };
static const uint16_t traceP[5] = { 0x0000, 0x0002, 0x0003, 0x0005, 0x0006 };
static const Prog PROG_P = { "P (SCF/CCF and the Q register, NMOS)", 0x0000, progP, (int)sizeof progP, 0x0007,
  traceP, 5, { 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000, 0x0007, 0x5C, 0x85 },
  nullptr, 0 };

// =============================================================================
// PROGRAM Q — NESTED NMI. Accepting an NMI clears IFF1 and does NOT touch IFF2 ([UM] p.19
// table 1 "Accept NMI 0"; [SY] p.20-21 and p.24, measured with an NMI inside an NMI). IFF preset 0,0.
// 0000  31 00 90  LD SP,9000h | 0003  FB  EI | 0004  00  NOP | 0005  00  NOP | 0006  ED 57  LD A,I
// 0008  18 FE  JR $ (END) | 0066  00  NOP | 0067  00  NOP | 0068  ED 45  RETN
// NMI #1 from the 4th fetch (NOP at 0005): M1 at 0006 ignored, push 0006, PC <- 0066; IFF1 = 0, IFF2 = 1.
// NMI #2 from the 6th fetch (NOP at 0066): M1 at 0067 ignored, push 0067, PC <- 0066; IFF2 stays 1.
// RETN -> 0067 (IFF1 <- IFF2 = 1); NOP; RETN -> 0006 (IFF1 <- 1); LD A,I: A = I = 5Ch, P/V = IFF2 = 1.
// A CPU that copied IFF1 into IFF2 on NMI would end with IFF1 = IFF2 = 0 and P/V = 0.
// Trace (16 fetches): 0000 0003 0004 0005 0006(NMI) 0066 0067(NMI) 0066 0067 0068 0069 0067 0068 0069 0006 0007.
// T: 10+4+4+4+11+4+11+4+4+14+4+14+9 = 97. FINAL: A=5C F=0C (S0 Z0 Y0 H0 X1 P1 N0 C0) SP=9000 PC=0008
// IM=0 IFF1=1 IFF2=1; R = 80h + 16 M1 = 90h; writes: push 0006 at 8FFE, push 0067 at 8FFC.
// =============================================================================
static const uint8_t progQ[] = { 0x31,0x00,0x90, 0xFB, 0x00, 0x00, 0xED,0x57, 0x18,0xFE };
static const uint8_t progQ_0066[4] = { 0x00, 0x00, 0xED, 0x45 };
static const uint16_t traceQ[16] = { 0x0000, 0x0003, 0x0004, 0x0005, 0x0006, 0x0066, 0x0067, 0x0066, 0x0067,
                                     0x0068, 0x0069, 0x0067, 0x0068, 0x0069, 0x0006, 0x0007 };
static const Ev evQ[2] = { {4, 1}, {6, 1} };
static const MemW writesQ[4] = { {0x8FFE, 0x06}, {0x8FFF, 0x00}, {0x8FFC, 0x67}, {0x8FFD, 0x00} };
static const Prog PROG_Q = { "Q (nested NMI, IFF2 kept)", 0x0000, progQ, (int)sizeof progQ, 0x0008,
  traceQ, 16, { 0x5C, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9000, 0x0008, 0x5C, 0x90, 0, 1, 1 },
  writesQ, 4, 0, 0, -1, 0, -1, 0, 4, -1, 0, evQ, 2, -1, nullptr, 0, 6 };

// =============================================================================
// PROGRAM S — IM0 with CALL nn. [UM] p.184: "The first byte of a multi-byte instruction is read
// during the interrupt acknowledge cycle. Subsequent bytes are read in by a normal memory read sequence";
// [PS] p.7: "one interrupt acknowledge cycle followed by two memory read cycles"; length = normal + 2 ([UM]
// p.19). The PC does not advance during the supplied instruction, and the CALL saves the interrupted PC
// (deduction from [UM] p.19, docs/04 §5). IFF preset 0,0.
// 0000  31 00 90  LD SP,9000h | 0003  ED 46  IM 0 | 0005  FB  EI | 0006  00  NOP | 0007  18 FE  JR $ (END)
// 1234  ED 4D  RETI
// INT low from the 4th fetch (EI): not accepted at the end of EI, accepted at the end of the NOP. IACK: the device
// gives CDh, then 34h and 12h in the two reads: CALL 1234h, push 0007 (the interrupted PC), PC <- 1234; RETI -> 0007 (END).
// Trace (7): 0000 0003 0004 0005 0006 1234 1235. T: 10+8+4+4+19+14 = 59.
// FINAL: SP=9000 PC=0007 IM=0 IFF 0,0; R = 80h + 7 fetches + 1 IACK = 88h; writes: push 0007 at 8FFE.
// =============================================================================
static const uint8_t progS[] = { 0x31,0x00,0x90, 0xED,0x46, 0xFB, 0x00, 0x18,0xFE };
static const uint8_t progS_1234[2] = { 0xED, 0x4D };
static const uint8_t progS_more[2] = { 0x34, 0x12 };
static const uint16_t traceS[7] = { 0x0000, 0x0003, 0x0004, 0x0005, 0x0006, 0x1234, 0x1235 };
static const Ev evS[1] = { {5, 6} };
static const MemW writesS[2] = { {0x8FFE, 0x07}, {0x8FFF, 0x00} };
static const Prog PROG_S = { "S (IM0 with CALL nn)", 0x0000, progS, (int)sizeof progS, 0x0007,
  traceS, 7, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9000, 0x0007, 0x5C, 0x88, 0, 0, 0 },
  writesS, 2, 0, 0, -1, 0, -1, 0, -1, 4, 0xCD, evS, 1, -1, nullptr, 0, -1, progS_more, 2 };

// =============================================================================
// PROGRAM T — LD A,I with an interrupt. [UM] p.94: "If an interrupt occurs during execution
// of this instruction, the Parity flag contains a 0" (same sentence for LD A,R, p.95). IFF preset 0,0.
// 0000  31 00 90  LD SP,9000h | 0003  ED 56  IM 1 | 0005  FB  EI | 0006  ED 57  LD A,I | 0008  18 FE  JR $ (END)
// 0038  F5  PUSH AF | 0039  F1  POP AF | 003A  ED 4D  RETI
// INT low from the 4th fetch (EI): accepted at the end of LD A,I (the instruction after EI). Without the interrupt
// P/V would be IFF2 = 1; with the interrupt P/V = 0: A = I = 5Ch, F = 08h (S0 Z0 Y0 H0 X1 P0 N0 C0), saved by
// PUSH AF. Trace (10): 0000 0003 0004 0005 0006 0007 0038 0039 003A 003B. T: 10+8+4+9+13+11+10+14 = 79.
// FINAL: A=5C F=08 SP=9000 PC=0008 IM=1 IFF 0,0; R = 80h + 10 fetches + 1 IACK = 8Bh;
// writes: push 0008 at 8FFE, PUSH AF at 8FFC (F) and 8FFD (A).
// =============================================================================
static const uint8_t progT[] = { 0x31,0x00,0x90, 0xED,0x56, 0xFB, 0xED,0x57, 0x18,0xFE };
static const uint8_t progT_0038[4] = { 0xF5, 0xF1, 0xED, 0x4D };
static const uint16_t traceT[10] = { 0x0000, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007, 0x0038, 0x0039, 0x003A, 0x003B };
static const Ev evT[1] = { {6, 2} };
static const MemW writesT[4] = { {0x8FFE, 0x08}, {0x8FFF, 0x00}, {0x8FFC, 0x08}, {0x8FFD, 0x5C} };
static const Prog PROG_T = { "T (LD A,I with an interrupt: P/V = 0)", 0x0000, progT, (int)sizeof progT, 0x0008,
  traceT, 10, { 0x5C, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9000, 0x0008, 0x5C, 0x8B, 1, 0, 0 },
  writesT, 4, 0, 0, -1, 0, -1, 0, -1, 4, 0x00, evT, 1, -1 };

static void load_prog(const Prog& p) {
  std::memset(mem, 0, sizeof mem);
  std::memcpy(mem + p.org, p.bytes, (size_t)p.len);
  if (&p == &PROG_J) std::memcpy(mem + 0x579C, progJ_579C, sizeof progJ_579C);
  if (&p == &PROG_I) std::memcpy(mem + 0x0010, progI_0010, 2);
  if (&p == &PROG_F) std::memcpy(mem + 0x0038, progF_0038, 3);
  if (&p == &PROG_M) std::memcpy(mem + 0x0038, progM_0038, 3);
  if (&p == &PROG_H) { std::memcpy(mem + 0x2040, progH_2040, 3); std::memcpy(mem + 0x3000, progH_3000, 2); }
  if (&p == &PROG_D) std::memcpy(mem + 0xB000, progD_B000, sizeof progD_B000);
  if (&p == &PROG_G) { std::memcpy(mem + 0x0010, progG_0010, 2); std::memcpy(mem + 0x0066, progG_0066, 2); }
  if (&p == &PROG_Q) std::memcpy(mem + 0x0066, progQ_0066, 4);
  if (&p == &PROG_S) std::memcpy(mem + 0x1234, progS_1234, 2);
  if (&p == &PROG_T) std::memcpy(mem + 0x0038, progT_0038, 4);
  if (&p == &PROG_B) {           // the pieces outside the main block
    std::memcpy(mem + 0x0000, progB_0000, sizeof progB_0000);
    std::memcpy(mem + 0x0038, progB_0038, sizeof progB_0038);
    std::memcpy(mem + 0x0200, progB_0200, sizeof progB_0200);
    std::memcpy(mem + 0x0210, progB_0210, sizeof progB_0210);
  }
}
static void expected_image(const Prog& p, uint8_t* img) {
  load_prog(p); std::memcpy(img, mem, sizeof mem);
  for (int i = 0; i < p.n_writes; i++) img[p.writes[i].a] = p.writes[i].v;
}

struct Run {
  long tstates;  int n_fetch;  int trace[96];  uint8_t final_chain[SS_EXP];
  uint8_t memimg[65536];  long n_rfsh;  long n_halt;
  MemW iow[16]; int n_iow;                       // port writes seen (16-bit address, data), in order
};
// input port: data is a function of the WHOLE 16-bit address, so that the high byte
// (A for IN A,(n), B for IN r,(C) and for the block instructions) is checked through the value read
static uint8_t io_rd(uint16_t a) { return (uint8_t)((a >> 8) ^ (a & 0xFF) ^ 0x5A); }
// input data: memory, the byte of the device during the IACK (IORQ + M1), the port (IORQ without M1)
static int g_iack_k = -1;                       // IM0: index of the next byte of the device (-1 = none)
static void set_din(const Prog& p) {
  cpu->data_in = (!cpu->iorq_n && !cpu->m1_n) ? (uint8_t)p.iack_byte :
                 (!cpu->iorq_n) ? io_rd((uint16_t)cpu->addr) : mem[cpu->addr];
  // IM0 with multi-byte instructions: the device supplies bytes 2.. in the memory reads after the IACK
  if (g_iack_k >= 0 && !cpu->mreq_n && !cpu->rd_n && cpu->m1_n) cpu->data_in = p.iack_more[g_iack_k];
}

// saves and restores the same state (contract: ss_load with cen = 0, then >= 1 quiet clock; here 2)
static void dump_restore(uint8_t* d) {
  ss_dump(d);
  ss_load(d);
  quiet(2);
}
static void preset_ir(const Prog& p) {
  uint8_t d[SS_EXP];
  ss_dump(d);
  // the programs start with AF BC DE HL (both banks) IX IY SP at 0: the reset sets them to
  // FFFFh ([SY] §2.4), so the bench preloads them like I/R/IFF (stimulus of the bench, not of the core)
  for (int k = 0; k < 176; k++) d[RF + k] = 0;
  for (int k = 0; k < 8; k++) {
    d[RF + 192 + k] = (uint8_t)((I_PRESET >> (7 - k)) & 1);
    d[RF + 200 + k] = (uint8_t)((R_PRESET >> (7 - k)) & 1);
  }
  d[54] = (uint8_t)p.iff1_pre; d[55] = (uint8_t)p.iff2_pre;   // IFF1, IFF2
  ss_load(d);
  quiet(2);
}

// ---- RESET study (bench only): parameterised start-up sequence ----------------------------
// Default = the usual sequence (zero state, rst 1 clock with cen 0, 2 quiet clocks, preset I/R/IFF):
// with the defaults run_program is IDENTICAL to the plain run (invariant checked in rst_study).
static const uint8_t* g_seed = nullptr;   // state loaded BEFORE the reset (nullptr = all zero)
static int  g_rst_len = 1;                // clocks with rst = 1
static int  g_cen_rst = 0;                // cen during the reset (on the board: 1)
static int  g_quiet   = 2;                // clocks with cen = 0 after the release (on the main board: 0)
static bool g_preset  = true;             // preset I/R/IFF through the savestate after the reset
static long g_rst_at  = 0;                // > 0: a second reset (same shape) at the given tick, with the program running
static const long BUSMAX = 45000;
static uint32_t* g_bus = nullptr; static long g_nbus = 0;   // bus signature per tick: addr | controls | data written
static inline uint32_t bus_sig() {
  return (uint32_t)cpu->addr | ((uint32_t)cpu->mreq_n << 16) | ((uint32_t)cpu->iorq_n << 17) | ((uint32_t)cpu->rd_n << 18) |
         ((uint32_t)cpu->wr_n << 19) | ((uint32_t)cpu->m1_n << 20) | ((uint32_t)cpu->rfsh_n << 21) | ((uint32_t)cpu->halt_n << 22) |
         ((uint32_t)(cpu->wr_n ? 0 : cpu->data_out) << 24);
}
static void apply_reset() {                // rst for g_rst_len clocks (cen = g_cen_rst), then g_quiet clocks with cen = 0
  for (int i = 0; i < g_rst_len; i++) { cpu->rst = 1; cpu->cen = (CData)g_cen_rst; tick(); }
  cpu->rst = 0; cpu->cen = 0;
  quiet(g_quiet);
}

// random WAIT: WAIT low with probability 1/3 at every half T-state, held for the whole cen; the CPU
// repeats T2/TW (docs/02 §2). The architectural result must stay identical to the run without WAIT.
static bool g_wait = false;  static uint32_t g_wlcg = 1;
// random BUSREQ: BUSREQ low with probability 1/4 at every half T-state; the CPU releases the bus at the
// end of the current M-cycle and takes it back when BUSREQ goes high again (docs/02 §6). The architectural
// result must stay identical to the run without BUSREQ (only programs without interrupts: the release delays,
// legitimately, the acceptance of INT/NMI). Invariant at every half T-state: with BUSACK low MREQ IORQ RD WR
// RFSH M1 are high and the data bus is not driven.
static bool g_breq = false;  static uint32_t g_blcg = 1;  static long g_ack_ticks = 0, g_ack_total = 0;
static void run_program(const Prog& p, Run* r, long pause_tick) {
  load_prog(p);
  g_wlcg = 12345u;  g_blcg = 54321u;
  cpu->cen = 0; cpu->rst = 0; cpu->ss_load = 0;
  cpu->wait_n = 1; cpu->busreq_n = 1; cpu->int_n = 1; cpu->nmi_n = 1;
  cpu->data_in = 0;
  if (g_seed) ss_load(g_seed); else ss_zero();  // state all zero: PC=0 (declared); or the seed of the reset study
  apply_reset();
  if (g_preset) preset_ir(p);
  r->tstates = 0; r->n_fetch = 0; r->n_rfsh = 0; r->n_halt = 0; r->n_iow = 0;
  long ticks = 0; int prev_mreq = 1, prev_iorq = 1; bool stop_now = false, prev_iow = false;
  int m1_count = 0, rf_tick = 0, nmi_pulse = 0; bool cur_iack = false;
  g_iack_k = -1; int prev_rd_more = 1;        // IM0: supply of the bytes of the device (set_din)
  int m1_prev_s = 1; bool m1_fell = false;     // M1 pin at the previous sample (check of T1A, below)
  uint8_t emu_i = I_PRESET, emu_r = R_PRESET;   // I and R expected on the refresh bus of the current M1
  g_nbus = 0;
  while (true) {
    cpu->cen = 0;
    cpu->nmi_n = (nmi_pulse > 0) ? 0 : 1; if (nmi_pulse > 0) nmi_pulse--;   // NMI pulse
    set_din(p);
    cpu->eval();
    if (g_rst_at > 0 && ticks == g_rst_at) {   // second reset with the program running (as a user does): the bus capture restarts
      apply_reset();
      // the stimuli of the bench restart too (INT/NMI "at fetch n" count from the reset, like the device
      // on the board that is reset together with the CPU); the memory does NOT: a reset does not touch the RAM
      r->n_fetch = 0; cpu->int_n = 1; nmi_pulse = 0; cpu->nmi_n = 1;
      set_din(p); cpu->eval();
      g_nbus = 0;
    }
    if (g_bus && g_nbus < BUSMAX) g_bus[g_nbus++] = bus_sig();
    if (pause_tick == -1 || (pause_tick > 0 && ticks == pause_tick)) {   // -1: save/restore at EVERY tick
      uint8_t d[SS_EXP];
      dump_restore(d);
      set_din(p); cpu->eval();
    }
    bool fetch_t1b = (prev_mreq == 1 && cpu->mreq_n == 0 && cpu->rd_n == 0 && cpu->m1_n == 0);
    bool iack_t1b  = (prev_iorq == 1 && cpu->iorq_n == 0 && cpu->m1_n == 0);   // IACK: M1 with IORQ
    // M1 pin ([UM] p.8 and figure 5, docs/02 §3): low from the START of T1, that is already in the half T-state before
    // the one in which MREQ and RD (fetch) or IORQ (acknowledge) fall; and it never falls without one of the two
    // starting in the next half T-state
    if (fetch_t1b || iack_t1b)
      CK(m1_prev_s == 0, "%s M1 not low in T1A (half T-state before %s), tick %ld", p.name, fetch_t1b ? "MREQ/RD" : "IORQ", ticks);
    if (m1_fell)
      CK(fetch_t1b || iack_t1b, "%s M1 fell without a fetch or an acknowledge in the next half T-state, tick %ld", p.name, ticks);
    m1_fell = (m1_prev_s == 1 && cpu->m1_n == 0);  m1_prev_s = cpu->m1_n;
    prev_mreq = cpu->mreq_n; prev_iorq = cpu->iorq_n;
    if (fetch_t1b || iack_t1b) {
      if (fetch_t1b) {
        if (std::getenv("PEEK")) {
          uint8_t d[SS_EXP];
          dump_restore(d);
          std::printf("fetch %04X: A=%02X F=%02X B=%02X C=%02X D=%02X E=%02X H=%02X L=%02X SP=%04X IX=%04X IY=%04X\n",
                      cpu->addr, R_A0(d), R_F0(d), R_B0(d), R_C0(d), R_D0(d), R_E0(d),
                      R_H0(d), R_L0(d), R_SP(d), R_IX(d), R_IY(d));
          set_din(p); cpu->eval();
        }
        if (cpu->addr == p.stop_addr) { stop_now = true; }
        else {
          if (r->n_fetch < 96) r->trace[r->n_fetch] = cpu->addr; r->n_fetch++;
          if (r->n_fetch == p.nmi_at_fetch || r->n_fetch == p.nmi2_at_fetch) nmi_pulse = 2;
          if (r->n_fetch == p.int_at_fetch) cpu->int_n = 0;
        }
      } else {
        cpu->int_n = 1;                          // the device is served at the IACK
      }
      cur_iack = iack_t1b;
      if (iack_t1b && p.n_iack_more > 0) g_iack_k = 0;
      if (g_chk) CK(m1_count == 0 || rf_tick == 4, "%s refresh: window of %d ticks in M1 %d", p.name, rf_tick, m1_count);
      if (m1_count > 0) {                        // closes the previous M1: R+1 (or R written by LD R,A), I written by LD I,A
        emu_r = (m1_count == p.m1_r_set) ? (uint8_t)p.r_new
                                         : (uint8_t)((emu_r & 0x80) | ((emu_r + 1) & 0x7F));
        if (m1_count == p.m1_i_set) emu_i = (uint8_t)p.i_new;
      }
      m1_count++; rf_tick = 0;
    }
    if (stop_now) break;
    if (cpu->rfsh_n == 0 && g_chk) {
      rf_tick++;
      // MREQ active in T3B-T4A of the M1 refresh (docs/02 §3.1); in the IACK it stays inactive:
      // IORQ in place of MREQ (docs/02 §7), the refresh of the IACK is not fixed by the source (declared)
      int exp_mreq = (!cur_iack && (rf_tick == 2 || rf_tick == 3)) ? 0 : 1;
      // in the M1 that writes I or R (LD I,A / LD R,A in T5A) the byte changes inside the window: not checked
      if (m1_count != p.m1_i_set)
        CK((cpu->addr >> 8) == emu_i, "%s refresh M1 %d tick %d: high byte %02X, expected I=%02X",
           p.name, m1_count, rf_tick, cpu->addr >> 8, emu_i);
      if (m1_count != p.m1_r_set)
        CK((cpu->addr & 0xFF) == emu_r, "%s refresh M1 %d tick %d: low byte %02X, expected R=%02X stable",
           p.name, m1_count, rf_tick, cpu->addr & 0xFF, emu_r);
      CK(cpu->mreq_n == exp_mreq, "%s refresh M1 %d tick %d: MREQ %d, expected %d",
         p.name, m1_count, rf_tick, cpu->mreq_n, exp_mreq);
      CK(cpu->rd_n == 1, "%s refresh M1 %d: RD active", p.name, m1_count);
      r->n_rfsh++;
    }
    if (!cpu->wr_n && !cpu->mreq_n) mem[cpu->addr] = cpu->data_out;
    if (g_iack_k >= 0 && prev_rd_more == 0 && cpu->rd_n == 1 && cpu->m1_n) { if (++g_iack_k >= p.n_iack_more) g_iack_k = -1; }
    prev_rd_more = (cpu->m1_n && !cpu->mreq_n) ? cpu->rd_n : 1;
    bool iow_now = (!cpu->wr_n && !cpu->iorq_n);                 // port write: one entry per cycle (edge of WR)
    if (iow_now && !prev_iow) { if (r->n_iow < 16) r->iow[r->n_iow] = { (uint16_t)cpu->addr, cpu->data_out }; r->n_iow++; }
    prev_iow = iow_now;
    if (!cpu->halt_n) r->n_halt++;
    if ((ticks & 1) == 0) r->tstates++;
    if (g_wait) { g_wlcg = g_wlcg * 1103515245u + 12345u; cpu->wait_n = (CData)(((g_wlcg >> 16) % 3u) != 0u); }
    if (g_breq) {
      g_blcg = g_blcg * 1103515245u + 12345u; cpu->busreq_n = (CData)(((g_blcg >> 16) % 4u) != 0u);
      if (!cpu->busack_n) {
        g_ack_ticks++;
        CK(cpu->mreq_n && cpu->iorq_n && cpu->rd_n && cpu->wr_n && cpu->rfsh_n && cpu->m1_n && !cpu->data_oe,
           "%s BUSREQ: with BUSACK low a bus signal is active", p.name);
      }
    }
    cen_tick();
    ticks++;
    if (ticks > 40000) { if (g_chk) CK(false, "%s: program does not end", p.name); break; }
  }
  r->tstates -= 1;
  dump_restore(r->final_chain);
  std::memcpy(r->memimg, mem, sizeof mem);
}

// =============================================================================
// RESET STUDY (bench only). Hypothesis to prove or disprove: on a board the first cen arrives at the
// FIRST clock after the release of rst, against the contract (>= 2 quiet clocks: P1 is recomputed), and
// the program starts only after several resets. Scenarios, for every program and for 8 seeds of "dirty"
// state loaded BEFORE the reset (all bits random, PC forced to 0):
//   REF  : seed, rst 1 clock, 2 quiet clocks           (P1 clean by contract)
//   A    : seed, rst 1 clock, cen at the first clock   (P1 = word of the dirty state: the case of the broken contract)
//   REFL : seed, rst 8 clocks with cen = 1, 2 quiet clocks (long reset, like a button)
//   C    : seed, rst 8 clocks with cen = 1, cen at the first clock (the board)
//   CLEAN: zero state, rst 1, 2 quiet: the reference without dirty state (IR, WZ, Q, registers = 0)
//   BIS  : program running, second reset (8 clocks, cen at the first clock) at tick 150, then comparison
//          of the bus AFTER the second reset with CLEAN from tick 0 (what a user who resets again sees)
// Comparisons on the bus tick by tick (address, MREQ IORQ RD WR M1 RFSH HALT, data written), on the
// memory and on the ports; first tick of divergence printed. No refresh check (I/R are 0 after rst by
// construction) and no preset: the signatures are compared with each other, not with the appendix.
// =============================================================================
static uint32_t g_busA[BUSMAX], g_busB[BUSMAX], g_busP[BUSMAX];
static void seed_state(uint8_t* d, uint32_t seed) {
  uint32_t x = seed * 2654435761u + 0x9E3779B9u;
  for (int i = 0; i < SS_EXP; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; d[i] = (uint8_t)(x & 1); }
  for (int k = 0; k < 16; k++) d[RF + 176 + k] = 0;      // PC = 0: the program starts from 0 as in the reference
}
// comparison of two captures: returns the first different tick (-1 = identical up to the shorter one and same length)
static long first_diff(const uint32_t* a, long na, const uint32_t* b, long nb) {
  long n = na < nb ? na : nb;
  for (long t = 0; t < n; t++) if (a[t] != b[t]) return t;
  return (na == nb) ? -1 : n;
}
static void print_sig(const char* tag, uint32_t s) {
  std::printf("%s addr=%04X MREQ=%u IORQ=%u RD=%u WR=%u M1=%u RFSH=%u HALT=%u dout=%02X", tag, s & 0xFFFF,
              (s >> 16) & 1, (s >> 17) & 1, (s >> 18) & 1, (s >> 19) & 1, (s >> 20) & 1, (s >> 21) & 1, (s >> 22) & 1, (s >> 24) & 0xFF);
}
static void run_cfg(const Prog& p, Run* r, uint32_t* bus, long* nbus, const uint8_t* seed, int rst_len, int cen_rst, int quiet_n, long rst_at) {
  g_seed = seed; g_rst_len = rst_len; g_cen_rst = cen_rst; g_quiet = quiet_n; g_preset = false; g_chk = false; g_rst_at = rst_at;
  g_bus = bus; run_program(p, r, 0); *nbus = g_nbus;
  g_seed = nullptr; g_rst_len = 1; g_cen_rst = 0; g_quiet = 2; g_preset = true; g_chk = true; g_rst_at = 0; g_bus = nullptr;
}
static void rst_study(const Run* ref_exec) {
  N_CEN = 2;
  std::printf("=== RESET study: cen at the first clock after rst, dirty state, repeated resets ===\n");
  const int NP = 16;                            // NP is declared further down
  const Prog* progs[NP] = { &PROG_A, &PROG_B, &PROG_C, &PROG_D, &PROG_E, &PROG_G, &PROG_F, &PROG_H, &PROG_I, &PROG_J, &PROG_K, &PROG_L, &PROG_M, &PROG_N, &PROG_O, &PROG_P };
  static Run ra, rb, rp; long na, nb, np;
  // invariant: with the usual sequence (zero seed, rst 1, cen 0, 2 quiet, preset) the bench gives what it gave
  long inv_ok = 0;
  for (int pi = 0; pi < NP; pi++) {
    const Prog& p = *progs[pi];
    g_bus = g_busA; run_program(p, &ra, 0); g_bus = nullptr;
    bool same = ra.tstates == ref_exec[pi].tstates && ra.n_fetch == ref_exec[pi].n_fetch &&
                !std::memcmp(ra.trace, ref_exec[pi].trace, sizeof(int) * (size_t)ra.n_fetch) &&
                !std::memcmp(ra.final_chain, ref_exec[pi].final_chain, SS_EXP) && !std::memcmp(ra.memimg, ref_exec[pi].memimg, sizeof mem);
    CK(same, "%s: invariant of the reset study (usual sequence != exec bench)", p.name);
    if (same) inv_ok++;
  }
  std::printf("  invariant (usual sequence == exec bench, cen 1/2): %ld/%d\n", inv_ok, NP);
  const int NSEED = 8;
  long cntA = 0, cntC = 0, cntS = 0, cntSL = 0, cntBIS = 0, tot = 0, totBIS = 0;
  for (int pi = 0; pi < NP; pi++) {
    const Prog& p = *progs[pi];
    // CLEAN
    run_cfg(p, &rp, g_busP, &np, nullptr, 1, 0, 2, 0);
    int divA = 0, divC = 0, divS = 0, divSL = 0, divBIS = 0;
    long firstA = -2, firstC = -2, firstS = -2, firstSL = -2, firstBIS = -2; uint32_t sA[2] = {0, 0}, sS[2] = {0, 0}, sBIS[2] = {0, 0}; uint32_t seedA = 0, seedS = 0, seedBIS = 0;
    for (int s = 1; s <= NSEED; s++) {
      uint8_t seed[SS_EXP]; seed_state(seed, (uint32_t)(pi * 64 + s));
      // A against REF (rst 1 clock)
      run_cfg(p, &rb, g_busB, &nb, seed, 1, 1, 2, 0);
      run_cfg(p, &ra, g_busA, &na, seed, 1, 1, 0, 0);
      long d = first_diff(g_busA, na, g_busB, nb);
      bool memd = std::memcmp(ra.memimg, rb.memimg, sizeof mem) != 0 || ra.n_iow != rb.n_iow;
      if (d >= 0 || memd) { divA++; if (firstA == -2) { firstA = d; sA[0] = d >= 0 && d < na ? g_busA[d] : 0; sA[1] = d >= 0 && d < nb ? g_busB[d] : 0; seedA = (uint32_t)(pi * 64 + s); } }
      // REF against CLEAN: effect of the state that rst does NOT touch (IR, WZ, Q, registers), with P1 clean
      d = first_diff(g_busB, nb, g_busP, np);
      memd = std::memcmp(rb.memimg, rp.memimg, sizeof mem) != 0 || rb.n_iow != rp.n_iow;
      if (d >= 0 || memd) { divS++; if (firstS == -2) { firstS = d; sS[0] = d >= 0 && d < nb ? g_busB[d] : 0; sS[1] = d >= 0 && d < np ? g_busP[d] : 0; seedS = (uint32_t)(pi * 64 + s); } }
      // C against REFL (rst 8 clocks with cen = 1: like the board)
      run_cfg(p, &rb, g_busB, &nb, seed, 8, 1, 2, 0);
      run_cfg(p, &ra, g_busA, &na, seed, 8, 1, 0, 0);
      d = first_diff(g_busA, na, g_busB, nb);
      memd = std::memcmp(ra.memimg, rb.memimg, sizeof mem) != 0 || ra.n_iow != rb.n_iow;
      if (d >= 0 || memd) { divC++; if (firstC == -2) firstC = d; }
      d = first_diff(g_busB, nb, g_busP, np);
      memd = std::memcmp(rb.memimg, rp.memimg, sizeof mem) != 0 || rb.n_iow != rp.n_iow;
      if (d >= 0 || memd) { divSL++; if (firstSL == -2) firstSL = d; }
      // BIS: program running (zero seed, clean start), second reset at tick 150 like the board, bus after the reset against CLEAN
      run_cfg(p, &ra, g_busA, &na, nullptr, 8, 1, 0, 150);
      d = first_diff(g_busA, na, g_busP, np);
      if (d >= 0) { divBIS++; if (firstBIS == -2) { firstBIS = d; sBIS[0] = d < na ? g_busA[d] : 0; sBIS[1] = d < np ? g_busP[d] : 0; seedBIS = 0; } }
      tot++;
    }
    totBIS += NSEED;
    cntA += divA; cntC += divC; cntS += divS; cntSL += divSL; cntBIS += divBIS;
    std::printf("  %s: A(rst 1, cen at once) vs REF: %d/%d differ", p.name, divA, NSEED);
    if (divA) { std::printf(" [seed %u, first tick %ld: ", seedA, firstA); print_sig("A", sA[0]); print_sig(" | REF", sA[1]); std::printf("]"); }
    std::printf("\n      C(rst 8+cen, cen at once) vs REFL: %d/%d differ%s", divC, NSEED, divC ? "" : " (P1 clean after the long reset)");
    if (divC) std::printf(" [first tick %ld]", firstC);
    std::printf("\n      REF(dirty state, P1 clean) vs CLEAN: %d/%d differ", divS, NSEED);
    if (divS) { std::printf(" [seed %u, first tick %ld: ", seedS, firstS); print_sig("DIRTY", sS[0]); print_sig(" | CLEAN", sS[1]); std::printf("]"); }
    std::printf("\n      REFL(dirty state, rst 8) vs CLEAN: %d/%d differ", divSL, NSEED);
    if (divSL) std::printf(" [first tick %ld]", firstSL);
    std::printf("\n      BIS(second reset with the program running) vs CLEAN: %d/%d differ", divBIS, NSEED);
    if (divBIS) { std::printf(" [first tick %ld: ", firstBIS); print_sig("BIS", sBIS[0]); print_sig(" | CLEAN", sBIS[1]); std::printf("]"); }
    std::printf("\n");
  }
  std::printf("  TOTAL over %ld cases: A vs REF %ld different (contract broken, rst 1 clock); C vs REFL %ld (contract broken, rst 8 clocks);\n"
              "         REF vs CLEAN %ld and REFL vs CLEAN %ld (state not cleared by rst); BIS vs CLEAN %ld/%ld (second reset)\n",
              tot, cntA, cntC, cntS, cntSL, cntBIS, totBIS);
}

// ---- Appendix A: BYTES, T, T2 per BASE opcode -----------------------------------
static int appB[256], appT[256], appT2[256]; static bool appOk[256];
static int appT_cb[256]; static bool appOk_cb[256];   // CB space: T (2 bytes, no conditional instructions)
static int appT_ed[256], appT2_ed[256]; static bool appOk_ed[256];   // ED space: T and T2 (repeated block instructions)
static int appT_ix[2][256], appT2_ix[2][256], appB_ix[2][256]; static bool appOk_ix[2][256];   // DD (0) / FD (1)
static int appT_xcb[2][256]; static bool appOk_xcb[2][256];                                   // DDCB / FDCB

static long expected_T(const Prog& p, const Run& r) {
  long exp_T = 0;
  for (int i = 0; i < r.n_fetch && i < p.n_trace; i++) {
    int a = r.trace[i]; int op = mem[a];
    int kind = 0;
    for (int e = 0; e < p.n_evs; e++) if (p.evs[e].idx == i) kind = p.evs[e].kind;
    if (kind == 1) { exp_T += 11; continue; }    // NMI: this fetch is the ignored M1 (5 T) + push (3+3) — [SY] §5.1
    if (kind == 5) { exp_T += 4;  continue; }    // HALT: forced M1 of NOP — [02] §7
    if (kind == 2) exp_T += 13;                  // IM1: IACK 7 + push 6, then the normal fetch of the handler — [04] §5
    if (kind == 3) exp_T += 19;                  // IM2: 7 + 6 + 6 — [UM] p.20
    if (kind == 4) exp_T += 13;                  // IM0 with RST: 11 + 2 — [04] §5
    if (kind == 6) exp_T += 19;                  // IM0 with CALL nn: 17 + 2 — [UM] p.19, p.281
    if (op == 0xCB) {                            // prefix: two M1 cycles, T from the CB row
      int op2 = mem[a + 1];
      CK(appOk_cb[op2], "%s: CB opcode %02X not in the appendix", p.name, op2);
      CK(i + 1 < r.n_fetch && r.trace[i + 1] == a + 1, "%s: after the prefix at %04X the fetch is not %04X", p.name, a, a + 1);
      exp_T += appT_cb[op2];
      i++;                                       // the second M1 belongs to the same instruction
      continue;
    }
    if (op == 0xDD || op == 0xFD) {              // indexed: DD/FD is a flag, the last one counts (docs/03 §3.3)
      int x = (op == 0xFD); int op2 = mem[a + 1];
      CK(i + 1 < r.n_fetch && r.trace[i + 1] == a + 1, "%s: after the prefix at %04X the fetch is not %04X", p.name, a, a + 1);
      if (op2 == 0xDD || op2 == 0xFD || op2 == 0xED) { exp_T += 4; continue; }   // stray prefix: +4 T ([SY] §3.7)
      if (op2 == 0xCB) {                         // DD CB d op: 4 bytes, T from the DDCB row
        int op4 = mem[a + 3];
        CK(appOk_xcb[x][op4], "%s: opcode %sCB %02X not in the appendix", p.name, x ? "FD" : "DD", op4);
        exp_T += appT_xcb[x][op4]; i++; continue;
      }
      int T, T2, B;
      if (appOk_ix[x][op2]) { T = appT_ix[x][op2]; T2 = appT2_ix[x][op2]; B = appB_ix[x][op2]; }
      else {                                     // UNASSIGNED: BASE row + M1 of the prefix (docs/06 §4.2, declared contract)
        CK(appOk[op2], "%s: opcode %02X not in the BASE appendix", p.name, op2);
        T = appT[op2] + 4; T2 = (appT2[op2] >= 0) ? appT2[op2] + 4 : -1; B = appB[op2] + 1;
      }
      if (T2 >= 0) {
        int next = (i + 2 < r.n_fetch) ? r.trace[i + 2] : p.stop_addr;
        if (next == a + B) T = T2;               // conditional not taken
      }
      exp_T += T; i++; continue;
    }
    if (op == 0xED) {                            // ED prefix: T, or T2 if the block instruction does NOT repeat
      int op2 = mem[a + 1];
      CK(appOk_ed[op2], "%s: ED opcode %02X not in the appendix", p.name, op2);
      CK(i + 1 < r.n_fetch && r.trace[i + 1] == a + 1, "%s: after the prefix at %04X the fetch is not %04X", p.name, a, a + 1);
      int T = appT_ed[op2];
      if (appT2_ed[op2] >= 0) {                  // LDIR/CPIR...: repeats if the next fetch is at a again
        int next = (i + 2 < r.n_fetch) ? r.trace[i + 2] : p.stop_addr;
        T = (next == a) ? appT_ed[op2] : appT2_ed[op2];
      }
      exp_T += T;
      i++;
      continue;
    }
    CK(appOk[op], "%s: opcode %02X not in the BASE appendix", p.name, op);
    int T = appT[op];
    if (appT2[op] >= 0) {                        // conditional: T2 if not taken
      int next = (i + 1 < r.n_fetch) ? r.trace[i + 1] : p.stop_addr;
      bool taken = (next != a + appB[op]);
      T = taken ? appT[op] : appT2[op];
    }
    exp_T += T;
  }
  return exp_T;
}

static void check_final(const Prog& p, const Run& r1, long* exp_T_out, int* memdiff_out) {
  CK(r1.n_fetch == p.n_trace, "%s: fetches executed %d (expected %d)", p.name, r1.n_fetch, p.n_trace);
  for (int i = 0; i < p.n_trace && i < r1.n_fetch; i++)
    CK(r1.trace[i] == p.trace[i], "%s: trace[%d] %04X/%04X", p.name, i, r1.trace[i], p.trace[i]);
  load_prog(p);                                  // the program does not modify itself
  long exp_T = expected_T(p, r1);
  CK(r1.tstates == exp_T, "%s: T-states measured %ld, expected from the appendix %ld", p.name, r1.tstates, exp_T);
  const uint8_t* d = r1.final_chain; const Exp& E = p.exp;
  CK(R_SELAF(d) == 0 && R_SELGP(d) == 0, "%s: bank selectors", p.name);
  CK(R_A0(d) == (uint32_t)E.A, "%s A %02X/%02X", p.name, R_A0(d), E.A);
  CK(R_F0(d) == (uint32_t)E.F, "%s F %02X/%02X", p.name, R_F0(d), E.F);
  CK(R_B0(d) == (uint32_t)E.B, "%s B %02X/%02X", p.name, R_B0(d), E.B);
  CK(R_C0(d) == (uint32_t)E.C, "%s C %02X/%02X", p.name, R_C0(d), E.C);
  CK(R_D0(d) == (uint32_t)E.D, "%s D %02X/%02X", p.name, R_D0(d), E.D);
  CK(R_E0(d) == (uint32_t)E.E, "%s E %02X/%02X", p.name, R_E0(d), E.E);
  CK(R_H0(d) == (uint32_t)E.H, "%s H %02X/%02X", p.name, R_H0(d), E.H);
  CK(R_L0(d) == (uint32_t)E.L, "%s L %02X/%02X", p.name, R_L0(d), E.L);
  CK(R_SP(d) == (uint32_t)E.SP, "%s SP %04X/%04X", p.name, R_SP(d), E.SP);
  CK(R_PC(d) == (uint32_t)E.PC, "%s PC %04X/%04X", p.name, R_PC(d), E.PC);
  CK(R_I(d) == (uint32_t)E.I, "%s I %02X/%02X", p.name, R_I(d), E.I);
  CK(R_R(d) == (uint32_t)E.R, "%s R %02X/%02X", p.name, R_R(d), E.R);
  if (E.IM >= 0)   CK(S_IM(d) == (uint32_t)E.IM, "%s IM %u/%d", p.name, (unsigned)S_IM(d), E.IM);
  if (E.IFF1 >= 0) CK(S_IFF1(d) == (uint32_t)E.IFF1, "%s IFF1 %u/%d", p.name, (unsigned)S_IFF1(d), E.IFF1);
  if (E.IFF2 >= 0) CK(S_IFF2(d) == (uint32_t)E.IFF2, "%s IFF2 %u/%d", p.name, (unsigned)S_IFF2(d), E.IFF2);
  if (p.halt_ticks >= 0) CK(r1.n_halt == p.halt_ticks, "%s halt_n low for %ld ticks, expected %d", p.name, r1.n_halt, p.halt_ticks);
  if (E.IX >= 0) CK(R_IX(d) == (uint32_t)E.IX, "%s IX %04X/%04X", p.name, (unsigned)R_IX(d), E.IX);
  if (E.IY >= 0) CK(R_IY(d) == (uint32_t)E.IY, "%s IY %04X/%04X", p.name, (unsigned)R_IY(d), E.IY);
  // alternate bank (shadow, [1]): preloaded with 0 (preset_ir), no program touches it
  CK(fld(d, RF+0, 8) == 0 && fld(d, RF+16, 8) == 0 && fld(d, RF+32, 8) == 0 &&
     fld(d, RF+48, 8) == 0 && fld(d, RF+64, 8) == 0 && fld(d, RF+80, 8) == 0 &&
     fld(d, RF+96, 8) == 0 && fld(d, RF+112, 8) == 0, "%s: alternate bank touched", p.name);
  static uint8_t img[65536];
  expected_image(p, img);
  int memdiff = 0;
  for (int a = 0; a < 65536; a++) if (r1.memimg[a] != img[a]) {
    if (memdiff < 8) CK(false, "%s: mem[%04X] = %02X, expected %02X", p.name, a, r1.memimg[a], img[a]);
    memdiff++;
  }
  CK(r1.n_iow == p.n_iow, "%s: port writes %d, expected %d", p.name, r1.n_iow, p.n_iow);
  for (int i = 0; i < p.n_iow && i < r1.n_iow && i < 16; i++)
    CK(r1.iow[i].a == p.iow[i].a && r1.iow[i].v == p.iow[i].v, "%s: port #%d = %04X<-%02X, expected %04X<-%02X",
       p.name, i, r1.iow[i].a, r1.iow[i].v, p.iow[i].a, p.iow[i].v);
  *exp_T_out = exp_T; *memdiff_out = memdiff;
}

static void report_run(const Prog& p, const Run& r1, long exp_T, int memdiff, bool ok) {
  const uint8_t* d = r1.final_chain; const Exp& E = p.exp;
  std::printf("  program %s: fetches %d (expected %d) trace %s; T-states %ld (Appendix %ld)\n",
              p.name, r1.n_fetch, p.n_trace, ok ? "identical" : "see FAIL", r1.tstates, exp_T);
  std::printf("    state: A=%02X F=%02X B=%02X C=%02X D=%02X E=%02X H=%02X L=%02X SP=%04X PC=%04X I=%02X R=%02X "
              "(expected %02X %02X %02X %02X %02X %02X %02X %02X %04X %04X %02X %02X); memory %d bytes different; "
              "refresh %ld ticks\n",
              R_A0(d), R_F0(d), R_B0(d), R_C0(d), R_D0(d), R_E0(d), R_H0(d), R_L0(d), R_SP(d), R_PC(d),
              R_I(d), R_R(d), E.A, E.F, E.B, E.C, E.D, E.E, E.H, E.L, E.SP, E.PC, E.I, E.R, memdiff, r1.n_rfsh);
  if (E.IM >= 0)
    std::printf("    IM=%u IFF1=%u IFF2=%u (expected %d %d %d)\n", (unsigned)S_IM(d), (unsigned)S_IFF1(d),
                (unsigned)S_IFF2(d), E.IM, E.IFF1, E.IFF2);
}

static const int N_PROG = 19;
static void run_all(int n_cen, Run* outs) {
  N_CEN = n_cen;
  std::printf("=== run with cen 1 every %d clocks ===\n", n_cen);
  // parallel savestate: walking one over all 274 positions (load -> identical read back,
  // no other bit touched), all ones and all zeros; the load must stay still with ss_load = 0
  cpu->cen = 0; cpu->rst = 0; cpu->ss_load = 0;
  ss_zero();
  for (int i = 0; i < SS_EXP; i++) CK(ss_bit(i) == 0, "state not cleared: bit %d", i);
  uint8_t pat[SS_EXP], back[SS_EXP];
  for (int k = 0; k < SS_EXP; k++) {
    for (int j = 0; j < SS_EXP; j++) pat[j] = (uint8_t)(j == k);
    ss_load(pat);
    ss_dump(back);
    CK(!std::memcmp(pat, back, SS_EXP), "walking one %d: read back differs", k);
    quiet(1);                                    // one clock with cen = 0 and ss_load = 0: the state does not move
    ss_dump(back);
    CK(!std::memcmp(pat, back, SS_EXP), "walking one %d: state changed without cen", k);
  }
  std::memset(pat, 1, sizeof pat); ss_load(pat); ss_dump(back);
  CK(!std::memcmp(pat, back, SS_EXP), "all ones: read back differs");
  ss_zero();
  std::printf("  parallel savestate: %d bits (210 regfile + 64 sequencer), walking one %d/%d\n", SS_EXP, SS_EXP, SS_EXP);

  const Prog* progs[N_PROG] = { &PROG_A, &PROG_B, &PROG_C, &PROG_D, &PROG_E, &PROG_G, &PROG_F, &PROG_H, &PROG_I, &PROG_J, &PROG_K, &PROG_L, &PROG_M, &PROG_N, &PROG_O, &PROG_P, &PROG_Q, &PROG_S, &PROG_T };
  for (int pi = 0; pi < N_PROG; pi++) {
    const Prog& p = *progs[pi];
    long e0 = n_err;
    static Run r1, r2; long exp_T; int memdiff;
    run_program(p, &r1, 0);
    check_final(p, r1, &exp_T, &memdiff);
    run_program(p, &r2, 151);                     // save/restore in the middle
    CK(r2.tstates == r1.tstates && r2.n_fetch == r1.n_fetch, "%s resume: T/fetches differ", p.name);
    CK(!std::memcmp(r1.trace, r2.trace, sizeof(int) * (size_t)r1.n_fetch), "%s resume: trace", p.name);
    CK(!std::memcmp(r1.final_chain, r2.final_chain, SS_EXP), "%s resume: final savestate", p.name);
    CK(!std::memcmp(r1.memimg, r2.memimg, sizeof mem), "%s resume: memory", p.name);
    // save/restore at EVERY tick of the program: the 274-bit savestate is the whole state; P1 (registered
    // at every clk) is recomputed from the restored state in the 2 quiet clks before the next cen
    static Run r3;
    run_program(p, &r3, -1);
    CK(r3.tstates == r1.tstates && r3.n_fetch == r1.n_fetch, "%s resume at every tick: T/fetches differ", p.name);
    CK(!std::memcmp(r1.trace, r3.trace, sizeof(int) * (size_t)r1.n_fetch), "%s resume at every tick: trace", p.name);
    CK(!std::memcmp(r1.final_chain, r3.final_chain, SS_EXP), "%s resume at every tick: final savestate", p.name);
    CK(!std::memcmp(r1.memimg, r3.memimg, sizeof mem), "%s resume at every tick: memory", p.name);
    CK(r3.n_iow == r1.n_iow && !std::memcmp(r1.iow, r3.iow, sizeof(r1.iow[0]) * (size_t)(r1.n_iow < 16 ? r1.n_iow : 16)),
       "%s resume at every tick: port writes", p.name);
    // random WAITs (the repeated T is predicted by NX: tw decided on the falling edge), also with save/restore
    // at every tick (loads also inside a TW, with the repetition already decided): trace, savestate, memory and ports identical
    static Run r4, r5;
    g_wait = true;  run_program(p, &r4, 0);  run_program(p, &r5, -1);  g_wait = false;  cpu->wait_n = 1;
    for (const Run* rw : { &r4, &r5 }) {
      CK(rw->n_fetch == r1.n_fetch && !std::memcmp(r1.trace, rw->trace, sizeof(int) * (size_t)(r1.n_fetch < 96 ? r1.n_fetch : 96)),
         "%s WAIT: trace", p.name);
      CK(!std::memcmp(r1.final_chain, rw->final_chain, SS_EXP), "%s WAIT: final savestate", p.name);
      CK(!std::memcmp(r1.memimg, rw->memimg, sizeof mem), "%s WAIT: memory", p.name);
      CK(rw->n_iow == r1.n_iow && !std::memcmp(r1.iow, rw->iow, sizeof(r1.iow[0]) * (size_t)(r1.n_iow < 16 ? r1.n_iow : 16)),
         "%s WAIT: port writes", p.name);
    }
    std::printf("    random WAITs (%ld T against %ld), also with resume at every tick: %s\n", r4.tstates, r1.tstates,
                (n_err == e0) ? "identical (trace, savestate, memory, ports)" : "see FAIL");
    // random BUSREQs, also with save/restore at every tick (the load also happens inside a Tx)
    if (p.int_at_fetch < 0 && p.nmi_at_fetch < 0) {
      static Run r6, r7;
      g_breq = true;  g_ack_ticks = 0;  run_program(p, &r6, 0);  run_program(p, &r7, -1);  g_breq = false;  cpu->busreq_n = 1;
      for (const Run* rw : { &r6, &r7 }) {
        CK(rw->n_fetch == r1.n_fetch && !std::memcmp(r1.trace, rw->trace, sizeof(int) * (size_t)(r1.n_fetch < 96 ? r1.n_fetch : 96)),
           "%s BUSREQ: trace", p.name);
        CK(!std::memcmp(r1.final_chain, rw->final_chain, SS_EXP), "%s BUSREQ: final savestate", p.name);
        CK(!std::memcmp(r1.memimg, rw->memimg, sizeof mem), "%s BUSREQ: memory", p.name);
        CK(rw->n_iow == r1.n_iow && !std::memcmp(r1.iow, rw->iow, sizeof(r1.iow[0]) * (size_t)(r1.n_iow < 16 ? r1.n_iow : 16)),
           "%s BUSREQ: port writes", p.name);
      }
      g_ack_total += g_ack_ticks;   // the release is checked on the total: a short program may not meet one
      std::printf("    random BUSREQs (%ld T against %ld, %ld half T-states with BUSACK low), also with resume at every tick: %s\n",
                  r6.tstates, r1.tstates, g_ack_ticks, (n_err == e0) ? "identical (trace, savestate, memory, ports)" : "see FAIL");
    }
    report_run(p, r1, exp_T, memdiff, n_err == e0);
    std::printf("    save/restore in the middle (tick 151): %s\n", (n_err == e0) ? "identical resume" : "see FAIL");
    std::printf("    save/restore at EVERY tick (%ld ticks, %ld T): %s\n", 2L * (r1.tstates + 1), r1.tstates,
                (n_err == e0) ? "identical resume (T, trace, savestate, memory, ports)" : "see FAIL");
    outs[pi] = r1;
  }
  std::printf("  minimum cen spacing seen: %ld clocks (contract >= 2)\n", min_spacing_seen);
}

// =============================================================================
// BUSREQ, directed test of the timing ([PS] p.29 and fig. 10, [UM] p.11-12; docs/02 §6). Program of NOPs only.
// BUSREQ low from the T1B of the 3rd fetch: it is sampled on the edge that opens T4 (last T of the M1) and the bus
// is released on the edge that closes it, that is when the refresh ends: BUSACK must fall in the same half T-state
// in which RFSH rises. After 3 half T-states with BUSACK low BUSREQ rises: the release is sampled on the edge that
// opens the Tx, BUSACK rises half a T-state later (falling edge of the Tx) and M1 falls half a T-state after that (T1).
// =============================================================================
static void busreq_directed() {
  N_CEN = 2;
  long e0 = n_err;
  std::memset(mem, 0, sizeof mem);                                // 0000.. = NOP
  cpu->cen = 0; cpu->rst = 0; cpu->ss_load = 0; cpu->wait_n = 1; cpu->busreq_n = 1; cpu->int_n = 1; cpu->nmi_n = 1;
  ss_zero(); apply_reset();
  long tick_no = 0, t_rfsh_up = -1, t_ack_dn = -1, t_ack_up = -1, t_m1_dn = -1, t_rel = -1, ack_low = 0;
  int fetch = 0, prev_mreq = 1, prev_rfsh = 1, prev_ack = 1, prev_m1 = 1; bool armed = false, released = false;
  while (tick_no < 400 && fetch < 8) {
    cpu->cen = 0; cpu->data_in = mem[cpu->addr]; cpu->eval();
    bool fetch_t1b = (prev_mreq == 1 && cpu->mreq_n == 0 && cpu->rd_n == 0 && cpu->m1_n == 0);
    if (fetch_t1b) { fetch++; if (fetch == 3) { armed = true; cpu->busreq_n = 0; } }
    if (armed && prev_rfsh == 0 && cpu->rfsh_n == 1 && t_rfsh_up < 0) t_rfsh_up = tick_no;
    if (prev_ack == 1 && cpu->busack_n == 0 && t_ack_dn < 0) t_ack_dn = tick_no;
    if (!cpu->busack_n) {
      ack_low++;
      CK(cpu->mreq_n && cpu->iorq_n && cpu->rd_n && cpu->wr_n && cpu->rfsh_n && cpu->m1_n && !cpu->data_oe,
         "directed BUSREQ: with BUSACK low a bus signal is active (tick %ld)", tick_no);
    }
    if (prev_ack == 0 && cpu->busack_n == 1 && t_ack_up < 0) t_ack_up = tick_no;
    if (t_ack_up >= 0 && prev_m1 == 1 && cpu->m1_n == 0 && t_m1_dn < 0) t_m1_dn = tick_no;
    // after 3 half T-states with BUSACK low, BUSREQ rises before an edge that opens a Tx (BUSACK low and phase A of the Tx)
    if (!released && ack_low >= 3 && (ack_low % 2) == 1) { cpu->busreq_n = 1; released = true; t_rel = tick_no; }
    prev_mreq = cpu->mreq_n; prev_rfsh = cpu->rfsh_n; prev_ack = cpu->busack_n; prev_m1 = cpu->m1_n;
    cen_tick(); tick_no++;
  }
  CK(t_ack_dn >= 0, "directed BUSREQ: BUSACK never fell");
  CK(t_ack_dn == t_rfsh_up, "directed BUSREQ: BUSACK fell at tick %ld, end of the refresh (T4) at tick %ld", t_ack_dn, t_rfsh_up);
  // BUSREQ rises in phase A of a Tx: the release is sampled on the rising edge that opens the next Tx (t_rel+1),
  // BUSACK rises on the falling edge of that Tx (t_rel+2) and is seen at the next half T-state
  CK(t_ack_up == t_rel + 3, "directed BUSREQ: BUSACK rose at tick %ld, expected %ld", t_ack_up, t_rel + 3);
  CK(t_m1_dn == t_ack_up + 1, "directed BUSREQ: M1 fell at tick %ld, expected %ld (T1, half a T-state after BUSACK)", t_m1_dn, t_ack_up + 1);
  CK(fetch == 8, "directed BUSREQ: the program did not reach the 8th fetch (%d)", fetch);
  std::printf("=== BUSREQ directed test: BUSACK down at tick %ld (end of T4 %ld), release sampled after tick %ld, BUSACK up %ld, M1 down %ld: %s ===\n",
              t_ack_dn, t_rfsh_up, t_rel, t_ack_up, t_m1_dn, (n_err == e0) ? "as [PS] fig. 10" : "see FAIL");
}

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  cpu = new Vcpu;
  cpu->cen = 0; cpu->rst = 0; cpu->ss_load = 0; for (int w = 0; w < 9; w++) cpu->ss_din[w] = 0;
  cpu->wait_n = 1; cpu->busreq_n = 1; cpu->int_n = 1; cpu->nmi_n = 1;

  // ---- Appendix A from the file: BYTES, T and T2 per opcode of every space -----
  std::memset(appOk, 0, sizeof appOk); std::memset(appOk_cb, 0, sizeof appOk_cb);
  std::memset(appOk_ed, 0, sizeof appOk_ed);
  std::memset(appOk_ix, 0, sizeof appOk_ix); std::memset(appOk_xcb, 0, sizeof appOk_xcb);
  {
    std::FILE* f = std::fopen("../docs/03-decoding.md", "rb");
    if (!f) { std::printf("FATAL: docs/03-decoding.md not found\n"); return 1; }
    char ln[512]; int in_block = 0, nrows = 0;
    while (std::fgets(ln, sizeof ln, f)) {
      size_t L = std::strlen(ln);
      while (L && (ln[L-1] == '\n' || ln[L-1] == '\r')) ln[--L] = 0;
      if (!in_block) { if (!std::strcmp(ln, "```text")) in_block = 1; continue; }
      if (!std::strcmp(ln, "```")) break;
      char* fl[14]; int nf = 0; char* p = ln; fl[nf++] = p;
      for (; *p && nf < 14; p++) if (*p == ';') { *p = 0; fl[nf++] = p + 1; }
      if (nf != 14) continue;
      nrows++;
      if (!std::strcmp(fl[0], "CB")) {
        int opc = (int)std::strtol(fl[1], nullptr, 16);
        appT_cb[opc] = std::atoi(fl[5]); appOk_cb[opc] = true;
        continue;
      }
      if (!std::strcmp(fl[0], "ED")) {
        int opc = (int)std::strtol(fl[1], nullptr, 16);
        appT_ed[opc]  = std::atoi(fl[5]);
        appT2_ed[opc] = std::strcmp(fl[8], "-") ? std::atoi(fl[8]) : -1;
        appOk_ed[opc] = true;
        continue;
      }
      if (!std::strcmp(fl[0], "DD") || !std::strcmp(fl[0], "FD")) {          // indexed
        int x = (fl[0][0] == 'F'); int opc = (int)std::strtol(fl[1], nullptr, 16);
        if (!std::strncmp(fl[2], "UNASSIGNED", 10) || !std::strncmp(fl[2], "SUBPREFIX", 9)) continue;
        appB_ix[x][opc]  = std::atoi(fl[3]);
        appT_ix[x][opc]  = std::atoi(fl[5]);
        appT2_ix[x][opc] = std::strcmp(fl[8], "-") ? std::atoi(fl[8]) : -1;
        appOk_ix[x][opc] = true;
        continue;
      }
      if (!std::strcmp(fl[0], "DDCB") || !std::strcmp(fl[0], "FDCB")) {
        int x = (fl[0][0] == 'F'); int opc = (int)std::strtol(fl[1], nullptr, 16);
        appT_xcb[x][opc] = std::atoi(fl[5]); appOk_xcb[x][opc] = true;
        continue;
      }
      if (std::strcmp(fl[0], "BASE")) continue;
      int op = (int)std::strtol(fl[1], nullptr, 16);
      if (!std::strncmp(fl[2], "PREFIX", 6) || !std::strcmp(fl[2], "UNASSIGNED")) continue;
      appB[op]  = std::atoi(fl[3]);
      appT[op]  = std::atoi(fl[5]);
      appT2[op] = std::strcmp(fl[8], "-") ? std::atoi(fl[8]) : -1;
      appOk[op] = true;
    }
    std::fclose(f);
    CK(nrows == 1792, "appendix: %d rows", nrows);
  }

  static Run o12[N_PROG], o2[N_PROG];            // static: one 64 KB memory image per program and cen setting
  run_all(12, o12);
  run_all(2,  o2);
  static const char* pn[N_PROG] = { "A", "B", "C", "D", "E", "G", "F", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "S", "T" };
  for (int pi = 0; pi < N_PROG; pi++)
    CK(o12[pi].tstates == o2[pi].tstates && !std::memcmp(o12[pi].final_chain, o2[pi].final_chain, SS_EXP) &&
       !std::memcmp(o12[pi].memimg, o2[pi].memimg, sizeof mem), "%s: cen12/cen2 differ", pn[pi]);
  std::printf("=== comparison cen12 against cen2 (all programs): %s ===\n",
              n_err ? "see FAIL" : "identical (T, trace, savestate, memory)");
  CK(g_ack_total > 0, "random BUSREQs: the bus was never released in any program");
  busreq_directed();
  if (std::getenv("RST_STUDY")) rst_study(o2);   // reset study (informative: it does not count as an error, except for the invariant)
  std::printf("errors: %ld -> %s\n", n_err, n_err ? "FAIL" : "PASS");
  delete cpu;
  return n_err ? 1 : 0;
}
