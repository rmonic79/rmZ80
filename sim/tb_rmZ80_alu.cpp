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
// tb_rmZ80_alu — self-checking bench for rtl/rmZ80_alu.sv. exit 0 = PASS.
//
// Reference INDEPENDENT of the form of the RTL (same rules as the specification
// docs/05 + Flags column of docs/03, formulated differently):
//   Z  from (result == 0)                        (RTL: comparator on a wire)
//   S  from bit 7 of the result
//   P  counting the bits at 1 with a loop        (RTL: XOR reduction)
//   V  from signed arithmetic in int, outside [-128,127] / [-32768,32767]
//                                                (RTL: cin(b7) XOR cout(b7))
//   H  from the comparison ((a&0xF)+(b&0xF)+c) > 0xF, or < 0 in subtraction
//                                                (RTL: carry into bit 4 of the 9-bit sum)
//   C  from sum/difference in int, > 0xFF(FFFF) or < 0
//
// COMPLETE ENUMERATION (never sampling) for the 8-bit classes.
// 16 bits: NOT exhaustive (2^33 per op) — COMPOSITIONAL coverage declared in
// main, phases A and B.
//
// XF/YF — THREE distinct treatments (a bench written by the same author as the
// RTL checks the transcription, not the understanding; checking what the source
// does not fix produces false greens):
//   a) CITED op (explicit per-instruction source): CHECKED, citation at the
//      point of the comparison — CP (operand, [SY] §8.4 note 1 p.30), DAA
//      (result, §4.7 p.18), SCF/CCF (variant, §8.5 note 1 p.30),
//      BIT (operand: the register for BIT b,r — z80test, SingleStepTests; the high
//      byte of IX+d for BIT b,(IX+d) — §4.1 p.15), RES/SET (F unchanged, [UM]),
//      16-bit ADD/ADC/SBC (high step, §4.6 p.17 + §8.6 note 2 p.31);
//   b) DEDUCED op (general rule [SY] §2.3 only): XF/YF MASKED and declared
//      NOT checked — ADD ADC SUB SBC AND XOR OR, INC DEC NEG CPL, the 8 CB
//      shifts/rotates, the 4 A rotates (23 ops);
//   c) the count checked/not checked is printed at the end of the run.
//
// EXCLUDED from the comparison because the specification does not fix them (mask):
//   BIT: S and P/V ("unknown" in docs/03; [SY] §4.1 fixes them and the RTL
//   follows it, but here they stay masked)
//
// DAA: reference = the THREE tables of docs/05 §8.2 (diff/C'/H', total rule
// validated by [SY]). FULL comparison, H' included. Structural proof over the
// 2048 combinations (A,C,H,N): each table must give EXACTLY one row
// (uncovered and duplicate expected = 0; otherwise the bench FAILS and reports).
// NON-REGRESSION: on the combinations covered by the [UM] table of §8 (684),
// A' and C' of the total rule must match those of [UM]: differences expected
// = 0, counted explicitly.
//
// Compiled variant: -DTB_VARIANT_NMOS=1 (full suite) or =0 (SCF/CCF only).
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include "Valu.h"
#include "verilated.h"

// required by the Verilator runtime of this environment (legacy)
double sc_time_stamp() { return 0; }

// operation codes — they must match the OP_* localparams of the RTL (docs/06 §2.2, docs/05 §10)
enum : uint8_t {
  OP_ADD = 0x00, OP_ADC, OP_SUB, OP_SBC, OP_AND, OP_XOR, OP_OR, OP_CP,
  OP_INC = 0x08, OP_DEC, OP_NEG,
  OP_PASS = 0x0C, OP_TST = 0x0D, OP_RLDM = 0x0E, OP_RRDM = 0x0F,
  OP_RLC = 0x10, OP_RRC, OP_RL, OP_RR, OP_SLA, OP_SRA, OP_SLL, OP_SRL,
  OP_RLCA = 0x18, OP_RRCA, OP_RLA, OP_RRA, OP_DAA, OP_CPL, OP_SCF, OP_CCF,
  OP_BIT = 0x20, OP_RES, OP_SET, OP_LDIF, OP_CPIF, OP_LDAI, OP_BITX, OP_RLDA = 0x27, OP_INIF = 0x2D, OP_OUTIF = 0x2E, OP_YXB = 0x2F, OP_IORF = 0x30,
  OP_SBC_R = 0x31, OP_SBC16H_R = 0x32,
  OP_ADDLO = 0x28, OP_ADD16H, OP_ADC16H, OP_RRDA, OP_SBC16H
};

// bits of F: S Z Y H X P N C
enum : uint8_t { FC = 0x01, FN = 0x02, FP = 0x04, FX = 0x08,
                 FH = 0x10, FY = 0x20, FZ = 0x40, FS = 0x80 };

static Valu*    dut;
static long     n_err  = 0;
static long     n_prt  = 0;

// parity: count of the bits at 1 (formulation independent of the XOR reduction)
static uint8_t par(uint8_t v) {
  int n = 0;
  for (int i = 0; i < 8; i++) n += (v >> i) & 1;
  return (n % 2 == 0) ? 1 : 0;
}
static uint8_t szyx(uint8_t r) {  // S,Z,YF,XF from the result
  return (uint8_t)(((r & 0x80) ? FS : 0) | (r == 0 ? FZ : 0) |
                   ((r & 0x20) ? FY : 0) | ((r & 0x08) ? FX : 0));
}

static void drive(uint8_t op, uint8_t a, uint8_t b, uint8_t f, uint8_t q,
                  uint8_t bs, uint16_t a16, uint16_t b16) {
  dut->op = op; dut->a = a; dut->b = b; dut->f_in = f; dut->q_in = q;
  dut->bit_sel = bs; dut->a16 = a16; dut->pv_in = (uint8_t)(b16 & 1);   // b16: no such port any more; its bit 0 drives pv_in
  dut->eval();
}

static void chk(const char* cls, uint8_t exp_res, uint8_t exp_f, uint8_t mask,
                bool check_res) {
  bool bad = ((dut->f_out ^ exp_f) & mask) != 0;
  if (check_res && dut->res != exp_res) bad = true;
  if (bad) {
    n_err++;
    if (n_prt < 20) {
      n_prt++;
      std::printf("FAIL %s op=%02X a=%02X b=%02X f_in=%02X bs=%u: "
                  "res=%02X/%02X f=%02X/%02X mask=%02X\n",
                  cls, dut->op, dut->a, dut->b, dut->f_in, dut->bit_sel,
                  dut->res, exp_res, dut->f_out, exp_f, mask);
    }
  }
}

// 16 bits in TWO 8-bit STEPS: low step (ADDLO for ADD, ADC for ADC, SBC for SBC),
// then high step (ADD16H/ADC16H/SBC16H) with the F of the low step as input. The
// composed result and the final F are compared with a 16-bit reference: same
// enumeration, same counts.
static void run16(int op16, uint16_t x, uint16_t y, uint8_t f,
                  uint16_t exp_r16, uint8_t exp_f, const char* cls) {
  uint8_t lo_op = (op16 == OP_ADD16H) ? (uint8_t)OP_ADDLO
                : (op16 == OP_ADC16H) ? (uint8_t)OP_ADC : (uint8_t)OP_SBC;
  drive(lo_op, (uint8_t)x, (uint8_t)y, f, 0, 0, 0, 0);
  uint8_t r_lo = dut->res, f1 = dut->f_out;
  if (lo_op == OP_ADDLO && ((f1 ^ f) & 0xFE) != 0) {     // ADDLO: ONLY C changes in F
    n_err++;
    if (n_prt < 20) { n_prt++; std::printf("FAIL %s ADDLO touches F beyond C: f_in=%02X f1=%02X\n", cls, f, f1); }
  }
  drive((uint8_t)op16, (uint8_t)(x >> 8), (uint8_t)(y >> 8), f1, 0, 0, 0, 0);
  uint16_t r16 = (uint16_t)(((unsigned)dut->res << 8) | r_lo);
  bool bad = (dut->f_out != exp_f) || (r16 != exp_r16);
  if (bad) {
    n_err++;
    if (n_prt < 20) {
      n_prt++;
      std::printf("FAIL %s op=%02X x=%04X y=%04X f_in=%02X: r16=%04X/%04X f=%02X/%02X\n",
                  cls, op16, x, y, f, r16, exp_r16, dut->f_out, exp_f);
    }
  }
}

// ---- DAA, TOTAL rule of docs/05 §8.2, as DATA ----
// diff table: (cf, hi_lo, hi_hi, hf(-1 = any), lo_lo, lo_hi, diff)
struct SyDiff { int8_t cf; uint8_t hi_lo, hi_hi; int8_t hf; uint8_t lo_lo, lo_hi, diff; };
static const SyDiff sy_diff[9] = {
  {0, 0x0,0x9,  0, 0x0,0x9, 0x00},
  {0, 0x0,0x9,  1, 0x0,0x9, 0x06},
  {0, 0x0,0x8, -1, 0xA,0xF, 0x06},
  {0, 0xA,0xF,  0, 0x0,0x9, 0x60},
  {1, 0x0,0xF,  0, 0x0,0x9, 0x60},
  {1, 0x0,0xF,  1, 0x0,0x9, 0x66},
  {1, 0x0,0xF, -1, 0xA,0xF, 0x66},
  {0, 0x9,0xF, -1, 0xA,0xF, 0x66},
  {0, 0xA,0xF,  1, 0x0,0x9, 0x66},
};
// C' table: (cf, hi_lo, hi_hi, lo_lo, lo_hi, cf_out)
struct SyCf { uint8_t cf, hi_lo, hi_hi, lo_lo, lo_hi, cf_out; };
static const SyCf sy_cf[5] = {
  {0, 0x0,0x9, 0x0,0x9, 0},
  {0, 0x0,0x8, 0xA,0xF, 0},
  {0, 0x9,0xF, 0xA,0xF, 1},
  {0, 0xA,0xF, 0x0,0x9, 1},
  {1, 0x0,0xF, 0x0,0xF, 1},
};
// H' table: (nf, hf(-1 = any), lo_lo, lo_hi, hf_out)
struct SyHf { uint8_t nf; int8_t hf; uint8_t lo_lo, lo_hi, hf_out; };
static const SyHf sy_hf[5] = {
  {0, -1, 0x0,0x9, 0},
  {0, -1, 0xA,0xF, 1},
  {1,  0, 0x0,0xF, 0},
  {1,  1, 0x6,0xF, 0},
  {1,  1, 0x0,0x5, 1},
};
// ---- [UM] table p.173 (docs/05 §8) — ONLY for the non-regression check ----
struct UmRow { uint8_t n, c, hi_lo, hi_hi, h, lo_lo, lo_hi, add, c_after; };
static const UmRow um_rows[13] = {
  {0,0, 0x0,0x9, 0, 0x0,0x9, 0x00, 0},   // row 1 (misprint "9-0" -> 0-9, [05] §8)
  {0,0, 0x0,0x8, 0, 0xA,0xF, 0x06, 0},
  {0,0, 0x0,0x9, 1, 0x0,0x3, 0x06, 0},
  {0,0, 0xA,0xF, 0, 0x0,0x9, 0x60, 1},
  {0,0, 0x9,0xF, 0, 0xA,0xF, 0x66, 1},
  {0,0, 0xA,0xF, 1, 0x0,0x3, 0x66, 1},
  {0,1, 0x0,0x2, 0, 0x0,0x9, 0x60, 1},
  {0,1, 0x0,0x2, 0, 0xA,0xF, 0x66, 1},
  {0,1, 0x0,0x3, 1, 0x0,0x3, 0x66, 1},
  {1,0, 0x0,0x9, 0, 0x0,0x9, 0x00, 0},
  {1,0, 0x0,0x8, 1, 0x6,0xF, 0xFA, 0},
  {1,1, 0x7,0xF, 0, 0x0,0x9, 0xA0, 1},
  {1,1, 0x6,0x7, 1, 0x6,0xF, 0x9A, 1},
};

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  dut = new Valu;

  long c_twoop = 0, c_unary = 0, c_shift = 0, c_arot = 0, c_daa = 0;
  long c_bit = 0, c_ressetd = 0, c_scfccf = 0, c_16 = 0;
  long daa_unc = 0, daa_dup = 0, um_cov = 0, um_diff = 0;
  long c_pass = 0, c_tst = 0, c_ldif = 0, c_cpif = 0;   // class 7
  long c_rld = 0;                                                  // class 8

#if TB_VARIANT_NMOS
  // ---- class 1: 8 two-operand ops x 256 x 256 x 2 carry-in ------------------
  for (int op = 0; op < 8; op++)
    for (int a = 0; a < 256; a++)
      for (int b = 0; b < 256; b++)
        for (int ci = 0; ci < 2; ci++) {
          uint8_t f = ci ? 0xFF : 0x00;
          int c = (op == OP_ADC || op == OP_SBC) ? ci : 0;
          uint8_t r; uint8_t ef;
          if (op <= OP_SBC || op == OP_CP) {
            bool sub = (op >= OP_SUB);
            int  wide = sub ? (a - b - c) : (a + b + c);
            r = (uint8_t)wide;
            int  sv = sub ? ((int8_t)a - (int8_t)b - c)
                          : ((int8_t)a + (int8_t)b + c);
            bool vf = (sv < -128 || sv > 127);
            bool hf = sub ? (((a & 0xF) - (b & 0xF) - c) < 0)
                          : (((a & 0xF) + (b & 0xF) + c) > 0xF);
            bool cf = sub ? (wide < 0) : (wide > 0xFF);
            ef = (uint8_t)(szyx(r) | (hf ? FH : 0) | (vf ? FP : 0) |
                           (sub ? FN : 0) | (cf ? FC : 0));
          } else {
            r = (op == OP_AND) ? (a & b) : (op == OP_XOR) ? (a ^ b) : (a | b);
            ef = (uint8_t)(szyx(r) | (op == OP_AND ? FH : 0) | (par(r) ? FP : 0));
          }
          uint8_t mask;
          if (op == OP_CP) {
            // CITED: XF/YF from the OPERAND — "YF and XF flags are copied from
            // the operand s, not the result A-s" ([SY] §8.4 note 1, p.30)
            ef = (uint8_t)((ef & (uint8_t)~(FY | FX)) |
                           ((b & 0x20) ? FY : 0) | ((b & 0x08) ? FX : 0));
            mask = 0xFF;
          } else {
            // DEDUCED (general rule [SY] §2.3 only): XF/YF NOT checked
            mask = (uint8_t)(0xFF & ~(FY | FX));
          }
          drive((uint8_t)op, (uint8_t)a, (uint8_t)b, f, 0, 0, 0, 0);
          chk("2op", r, ef, mask, true);
          c_twoop++;
        }

  // ---- class 2: INC DEC NEG CPL x 256 x 2 -----------------------------------
  // (CPL is not contiguous: 0x1D)
  static const uint8_t un_ops[4] = {OP_INC, OP_DEC, OP_NEG, OP_CPL};
  for (int ui = 0; ui < 4; ui++) { const int op = un_ops[ui];
    for (int a = 0; a < 256; a++)
      for (int ci = 0; ci < 2; ci++) {
        uint8_t f = ci ? 0xFF : 0x00;
        uint8_t r; uint8_t ef;
        switch (op) {
          case OP_INC: {
            r = (uint8_t)(a + 1);
            ef = (uint8_t)(szyx(r) | (((a & 0xF) + 1 > 0xF) ? FH : 0) |
                           ((int8_t)a == 127 ? FP : 0) | (ci ? FC : 0));
            break;
          }
          case OP_DEC: {
            r = (uint8_t)(a - 1);
            ef = (uint8_t)(szyx(r) | ((((a & 0xF) - 1) < 0) ? FH : 0) |
                           ((int8_t)a == -128 ? FP : 0) | FN | (ci ? FC : 0));
            break;
          }
          case OP_NEG: {
            r = (uint8_t)(0 - a);
            ef = (uint8_t)(szyx(r) | (((0 - (a & 0xF)) < 0) ? FH : 0) |
                           (a == 0x80 ? FP : 0) | FN | (a != 0 ? FC : 0));
            break;
          }
          default: {  // CPL: H=1 N=1, S Z P C unchanged
            r = (uint8_t)(~a);
            uint8_t keep = (uint8_t)(f & (FS | FZ | FP | FC));
            ef = (uint8_t)(keep | ((r & 0x20) ? FY : 0) | ((r & 0x08) ? FX : 0) |
                           FH | FN);
            break;
          }
        }
        drive((uint8_t)op, (uint8_t)a, 0, f, 0, 0, 0, 0);
        // XF/YF DEDUCED ([SY] §2.3): masked, NOT checked
        chk("unary", r, ef, (uint8_t)(0xFF & ~(FY | FX)), true);
        c_unary++;
      }
  }

  // ---- class 3: 8 shifts/rotates, CB form x 256 x 2 --------------------------
  for (int op = OP_RLC; op <= OP_SRL; op++)
    for (int a = 0; a < 256; a++)
      for (int ci = 0; ci < 2; ci++) {
        uint8_t f = ci ? 0xFF : 0x00;
        uint8_t r; int cf;
        switch (op) {
          case OP_RLC: r = (uint8_t)((a << 1) | (a >> 7));  cf = a >> 7;   break;
          case OP_RRC: r = (uint8_t)((a >> 1) | (a << 7));  cf = a & 1;    break;
          case OP_RL:  r = (uint8_t)((a << 1) | ci);        cf = a >> 7;   break;
          case OP_RR:  r = (uint8_t)((a >> 1) | (ci << 7)); cf = a & 1;    break;
          case OP_SLA: r = (uint8_t)(a << 1);               cf = a >> 7;   break;
          case OP_SRA: r = (uint8_t)((a >> 1) | (a & 0x80)); cf = a & 1;   break;
          case OP_SLL: r = (uint8_t)((a << 1) | 1);         cf = a >> 7;   break;
          default:     r = (uint8_t)(a >> 1);               cf = a & 1;    break;
        }
        cf &= 1;
        uint8_t ef = (uint8_t)(szyx(r) | (par(r) ? FP : 0) | (cf ? FC : 0));
        drive((uint8_t)op, (uint8_t)a, 0, f, 0, 0, 0, 0);
        // XF/YF DEDUCED ([SY] §2.3): masked, NOT checked
        chk("shiftCB", r, ef, (uint8_t)(0xFF & ~(FY | FX)), true);
        c_shift++;
      }

  // ---- class 4: RLCA RRCA RLA RRA x 256 A x 256 F ----------------------------
  for (int op = OP_RLCA; op <= OP_RRA; op++)
    for (int a = 0; a < 256; a++)
      for (int f = 0; f < 256; f++) {
        int ci = f & 1;
        uint8_t r; int cf;
        switch (op) {
          case OP_RLCA: r = (uint8_t)((a << 1) | (a >> 7));  cf = a >> 7; break;
          case OP_RRCA: r = (uint8_t)((a >> 1) | (a << 7));  cf = a & 1;  break;
          case OP_RLA:  r = (uint8_t)((a << 1) | ci);        cf = a >> 7; break;
          default:      r = (uint8_t)((a >> 1) | (ci << 7)); cf = a & 1;  break;
        }
        cf &= 1;
        uint8_t keep = (uint8_t)(f & (FS | FZ | FP));   // S,Z,P/V not touched
        uint8_t ef = (uint8_t)(keep | ((r & 0x20) ? FY : 0) | ((r & 0x08) ? FX : 0) |
                               (cf ? FC : 0));
        drive((uint8_t)op, (uint8_t)a, 0, (uint8_t)f, 0, 0, 0, 0);
        // XF/YF DEDUCED ([SY] §2.3): masked, NOT checked
        chk("rotA", r, ef, (uint8_t)(0xFF & ~(FY | FX)), true);
        c_arot++;
      }

  // ---- class 5: DAA x 256 A x C x H x N — total rule [05] §8.2 ---------------
  // structural: each table must give EXACTLY one row (0 uncovered,
  // 0 duplicates); FULL comparison, H' included; non-regression against [UM].
  for (int a = 0; a < 256; a++)
    for (int c = 0; c < 2; c++)
      for (int h = 0; h < 2; h++)
        for (int n = 0; n < 2; n++) {
          uint8_t f = (uint8_t)((c ? FC : 0) | (h ? FH : 0) | (n ? FN : 0));
          int hi = a >> 4, lo = a & 0xF;
          int nd = 0, id = -1, nc = 0, ic = -1, nh = 0, ih = -1;
          for (int i = 0; i < 9; i++) {
            const SyDiff& r = sy_diff[i];
            if (r.cf == c && hi >= r.hi_lo && hi <= r.hi_hi &&
                (r.hf < 0 || r.hf == h) && lo >= r.lo_lo && lo <= r.lo_hi)
              { nd++; id = i; }
          }
          for (int i = 0; i < 5; i++) {
            const SyCf& r = sy_cf[i];
            if (r.cf == c && hi >= r.hi_lo && hi <= r.hi_hi &&
                lo >= r.lo_lo && lo <= r.lo_hi) { nc++; ic = i; }
          }
          for (int i = 0; i < 5; i++) {
            const SyHf& r = sy_hf[i];
            if (r.nf == n && (r.hf < 0 || r.hf == h) &&
                lo >= r.lo_lo && lo <= r.lo_hi) { nh++; ih = i; }
          }
          drive(OP_DAA, (uint8_t)a, 0, f, 0, 0, 0, 0);
          if (nd == 1 && nc == 1 && nh == 1) {
            uint8_t diff = sy_diff[id].diff;
            uint8_t r = (uint8_t)(n ? (a - diff) : (a + diff));
            uint8_t ef = (uint8_t)(szyx(r) | (par(r) ? FP : 0) | (n ? FN : 0) |
                                   (sy_hf[ih].hf_out ? FH : 0) |
                                   (sy_cf[ic].cf_out ? FC : 0));
            chk("DAA", r, ef, 0xFF, true);     // full mask, H' included
            // non-regression: where [UM] gives a value, A' and C' must match
            int nu = 0, iu = -1;
            for (int i = 0; i < 13; i++) {
              const UmRow& u = um_rows[i];
              if (u.n == n && u.c == c && u.h == h &&
                  hi >= u.hi_lo && hi <= u.hi_hi &&
                  lo >= u.lo_lo && lo <= u.lo_hi) { nu++; iu = i; }
            }
            if (nu == 1) {
              um_cov++;
              uint8_t r_um = (uint8_t)(a + um_rows[iu].add);
              if (r_um != r || um_rows[iu].c_after != sy_cf[ic].cf_out) {
                um_diff++;
                if (n_prt < 20) {
                  n_prt++;
                  std::printf("UM-DIFF A=%02X C=%d H=%d N=%d: UM %02X/C%d, "
                              "total %02X/C%d\n", a, c, h, n, r_um,
                              um_rows[iu].c_after, r, sy_cf[ic].cf_out);
                }
              }
            }
          } else if (nd == 0 || nc == 0 || nh == 0) daa_unc++;
          else                                      daa_dup++;
          c_daa++;
        }

  // ---- class 6: BIT x 256 x 8 ; RES/SET x 256 x 8 x 2 -----------------------
  for (int a = 0; a < 256; a++)
    for (int bs = 0; bs < 8; bs++) {
      uint8_t f = (uint8_t)(a ^ 0xA5);       // C-in varies between cases (declared)
      uint8_t t = (uint8_t)(a & (1 << bs));
      // S and P/V excluded ("unknown"); Z = complemented bit, H=1, N=0, C unchanged,
      // XF/YF from the OPERAND (b = the register itself: BIT b,r; measured, OP_BITX)
      uint8_t ef = (uint8_t)((t == 0 ? FZ : 0) | ((a & 0x20) ? FY : 0) |
                             ((a & 0x08) ? FX : 0) | FH | (f & FC));
      drive(OP_BITX, (uint8_t)a, (uint8_t)a, f, 0, (uint8_t)bs, 0, 0);
      chk("BIT", t, ef, (uint8_t)(0xFF & ~(FS | FP)), true);
      c_bit++;
      for (int s = 0; s < 2; s++) {          // RES then SET
        uint8_t r = s ? (uint8_t)(a | (1 << bs)) : (uint8_t)(a & ~(1 << bs));
        drive(s ? OP_SET : OP_RES, (uint8_t)a, 0, (uint8_t)a, 0, (uint8_t)bs, 0, 0);
        chk(s ? "SET" : "RESb", r, (uint8_t)a, 0xFF, true);  // F unchanged
        c_ressetd++;
      }
    }

  // ---- class 7: service ops and flags of the block instructions ---------------
  // Source of every bit in the comment of the comparison: CITED = checked,
  // DEDUCED / NOT IN THE SOURCE = masked and declared.
  // PASS — project definition (docs/06 §2.1 rule 1): res=a, F unchanged.
  for (int a = 0; a < 256; a++)
    for (int f = 0; f < 256; f++) {
      drive(OP_PASS, (uint8_t)a, (uint8_t)(a ^ 0x3C), (uint8_t)f, 0, 0, 0xBEEF, 0);
      chk("PASS", (uint8_t)a, (uint8_t)f, 0xFF, true);
      c_pass++;
    }
  // TST (IN r,(C)) — CITED: S,Z from the data, H=0, P parity, N=0, C unchanged
  // ([03] p.296 `rr0p0-`; [05] §4 "parity of the byte read" [UM] p.68; [05] §6
  // "the byte read is 0"). XF/YF: general rule [SY] §2.3 only -> DEDUCED,
  // masked.
  for (int a = 0; a < 256; a++)
    for (int f = 0; f < 256; f++) {
      uint8_t ef = (uint8_t)(((a & 0x80) ? FS : 0) | (a == 0 ? FZ : 0) |
                             (par((uint8_t)a) ? FP : 0) | (f & FC));
      drive(OP_TST, (uint8_t)a, 0, (uint8_t)f, 0, 0, 0, 0);
      chk("TST", (uint8_t)a, ef, (uint8_t)(0xFF & ~(FY | FX)), true);
      c_tst++;
    }
  // LDI-F / CPI-F — a = A, b = byte, a16 = BC ALREADY decremented (convention
  // of the ALU, docs/06 §2.2). All 8 bits CITED:
  //   LDI: S,Z,C unchanged, H=0, N=0 ([03] p.130 `--0b0-`); P/V = BC' != 0 ([05] §4,
  //        [UM] p.68); YF/XF from n = A+(HL): bits 1 and 3 ([05] §9, [SY] §4.2 p.16)
  //   CPI: S,Z,H from the comparison, N=1, C unchanged ([03] p.138 `rrcb1-`); P/V = BC' != 0;
  //        YF/XF from n = A-(HL)-H' ([05] §9, [SY] §4.2 p.16)
  static const uint16_t bcv[4] = {0x0000, 0x0001, 0x1234, 0xFFFF};
  for (int a = 0; a < 256; a++)
    for (int b = 0; b < 256; b++)
      for (int k = 0; k < 4; k++)
        for (int ci = 0; ci < 2; ci++) {
          uint8_t f = ci ? 0xFF : 0x00;
          uint8_t nl = (uint8_t)(a + b);
          uint8_t ef = (uint8_t)((f & (FS | FZ | FC)) | ((nl & 0x02) ? FY : 0) |
                                 ((nl & 0x08) ? FX : 0) | (bcv[k] ? FP : 0));
          drive(OP_LDIF, (uint8_t)a, (uint8_t)b, f, 0, 0, bcv[k], 0);
          chk("LDI-F", (uint8_t)a, ef, 0xFF, true);
          c_ldif++;
          uint8_t cmp = (uint8_t)(a - b);
          bool    hf  = (((a & 0xF) - (b & 0xF)) < 0);
          uint8_t nc  = (uint8_t)(cmp - (hf ? 1 : 0));
          ef = (uint8_t)(((cmp & 0x80) ? FS : 0) | (cmp == 0 ? FZ : 0) | (hf ? FH : 0) |
                         ((nc & 0x02) ? FY : 0) | ((nc & 0x08) ? FX : 0) |
                         (bcv[k] ? FP : 0) | FN | (f & FC));
          drive(OP_CPIF, (uint8_t)a, (uint8_t)b, f, 0, 0, bcv[k], 0);
          // res of CPI-F = A-(HL) (S/Z come from res, computed once);
          // the sequencer discards it ([03] p.138): value NOT observable, declared
          chk("CPI-F", (uint8_t)(a - b), ef, 0xFF, true);
          c_cpif++;
        }

  // ---- block I/O: OP_INIF / OP_OUTIF (a16[15:8] = B' already decremented in both: the write of B
  //   falls in the same tick). [SY] §4.3: S/Z/YF/XF from B'; k = data + operand
  //   ((C+-1)&FF for INI/IND, L after HL+-1 for OUTI/OUTD); H = C = (k > 255); P = even parity of ((k & 7) ^ B');
  //   N = bit 7 of the data. ALL 8 bits checked.
  long c_iof = 0;
  for (int a = 0; a < 256; a++)
    for (int bi = 0; bi < 256; bi += 5)
      for (int bb = 0; bb < 256; bb += 17)
        for (int isout = 0; isout < 2; isout++) {
          uint8_t f  = (uint8_t)(a ^ bi ^ 0x3C);
          uint8_t bp = (uint8_t)bb;                       // B' already decremented in a16 for both forms
          int     k  = a + bi;
          uint8_t px = (uint8_t)(((k & 7) ^ bp) & 0xFF);
          int ones = 0; for (int i = 0; i < 8; i++) ones += (px >> i) & 1;
          uint8_t ef = (uint8_t)(((bp & 0x80) ? FS : 0) | (bp == 0 ? FZ : 0) | ((bp & 0x20) ? FY : 0) | ((bp & 0x08) ? FX : 0) |
                                 ((k > 255) ? (FH | FC) : 0) | ((a & 0x80) ? FN : 0) | ((ones & 1) ? 0 : FP));
          drive(isout ? OP_OUTIF : OP_INIF, (uint8_t)a, (uint8_t)bi, f, 0, 0, (uint16_t)((bb << 8) | 0x40), 0);
          chk(isout ? "OUTI-F" : "INI-F", (uint8_t)a, ef, 0xFF, true);
          c_iof++;
        }
  std::printf("  block I/O (OP_INIF/OP_OUTIF): %ld cases = 256 data x 52 operand x 16 B x 2; 8/8 bits CITED ([SY] §4.3)\n", c_iof);

  // ---- reversed subtraction: OP_SBC_R(a,b,f) == OP_SBC(b,a,f) and OP_SBC16H_R(a,b,f) == OP_SBC16H(b,a,f), exhaustive
  long c_sbr = 0;
  for (int a = 0; a < 256; a++)
    for (int b = 0; b < 256; b++) {
      uint8_t f = (uint8_t)(a ^ b ^ 0x91);
      drive(OP_SBC, (uint8_t)b, (uint8_t)a, f, 0, 0, 0, 0); uint8_t r1 = dut->res, f1 = dut->f_out;
      drive(OP_SBC_R, (uint8_t)a, (uint8_t)b, f, 0, 0, 0, 0); chk("SBC_R", r1, f1, 0xFF, true);
      drive(OP_SBC16H, (uint8_t)b, (uint8_t)a, f, 0, 0, 0, 0); uint8_t r2 = dut->res, f2 = dut->f_out;
      drive(OP_SBC16H_R, (uint8_t)a, (uint8_t)b, f, 0, 0, 0, 0); chk("SBC16H_R", r2, f2, 0xFF, true);
      c_sbr += 2;
    }
  std::printf("  reversed SBC (OP_SBC_R/OP_SBC16H_R): %ld cases identical to SBC with the operands swapped\n", c_sbr);

  // ---- repeating forms that repeat (SingleStepTests): OP_YXB (YF/XF from b, the rest unchanged) and OP_IORF
  //   (a = B': C=1 -> bq = B'-1 if N=1 otherwise B'+1, H = B'[3:0]==0 / ==F; C=0 -> bq = B', H unchanged; P ^= xor(bq[2:0]))
  long c_rep = 0;
  for (int a = 0; a < 256; a++)
    for (int f = 0; f < 256; f += 3) {
      drive(OP_YXB, 0x5A, (uint8_t)a, (uint8_t)f, 0, 0, 0, 0);
      chk("YXB", 0x5A, (uint8_t)((f & ~(FY | FX)) | (a & (FY | FX))), 0xFF, true);
      int c = f & FC, n = f & FN;
      int bq = c ? (n ? (a - 1) : (a + 1)) : a;
      int hn = c ? (n ? ((a & 0xF) == 0) : ((a & 0xF) == 0xF)) : ((f & FH) != 0);
      int px = ((f & FP) != 0) ^ (((bq & 1) ^ ((bq >> 1) & 1) ^ ((bq >> 2) & 1)) & 1);
      uint8_t ef = (uint8_t)((f & ~(FH | FP)) | (hn ? FH : 0) | (px ? FP : 0));
      drive(OP_IORF, (uint8_t)a, 0, (uint8_t)f, 0, 0, 0, 0);
      chk("IORF", (uint8_t)a, ef, 0xFF, true);
      c_rep += 2;
    }
  std::printf("  repeating forms (OP_YXB/OP_IORF): %ld cases; 8/8 bits against the measured rule\n", c_rep);

  // ---- BIT b,(IX+d): OP_BITX = BIT with XF/YF from the operand b (high byte of IX+d, [SY] §4.1, docs/05 §9).
  //   Z from the bit, H=1, N=0, C unchanged CITED; S and P/V excluded as for BIT. Exhaustive a x bit x 4 values of b.
  long c_bitx = 0;
  for (int a = 0; a < 256; a++)
    for (int bs = 0; bs < 8; bs++)
      for (int bi = 0; bi < 4; bi++) {
        static const uint8_t bv[4] = { 0x00, 0x28, 0xD7, 0xFF };
        uint8_t f = (uint8_t)(a ^ 0x5A);
        uint8_t t = (uint8_t)(a & (1 << bs));
        uint8_t ef = (uint8_t)((t == 0 ? FZ : 0) | ((bv[bi] & 0x20) ? FY : 0) | ((bv[bi] & 0x08) ? FX : 0) | FH | (f & FC));
        drive(OP_BITX, (uint8_t)a, bv[bi], f, 0, (uint8_t)bs, 0, 0);
        chk("BITX", t, ef, (uint8_t)(0xFF & ~(FS | FP)), true);
        c_bitx++;
      }
  std::printf("  BIT b,(IX+d) (OP_BITX): %ld cases = 256 A x 8 bits x 4 high bytes; Z/H/N/C and XF/YF CITED\n", c_bitx);

  // ---- LD A,I / LD A,R: OP_LDAI, res = a; S/Z from the data, H=0, P/V = pv_in (IFF2), N=0, C unchanged.
  //   [03] p.94/95 `rr0i0-`: S Z H P N C CITED; XF/YF DEDUCED (general rule [SY] §2.3) -> masked.
  long c_ldai = 0;
  for (int a = 0; a < 256; a++)
    for (int fi = 0; fi < 2; fi++)
      for (int pv = 0; pv < 2; pv++) {
        uint8_t f  = fi ? 0xFF : 0x00;
        uint8_t ef = (uint8_t)(((a & 0x80) ? FS : 0) | (a == 0 ? FZ : 0) | (pv ? FP : 0) | (f & FC));
        drive(OP_LDAI, (uint8_t)a, 0, f, 0, 0, 0, (uint16_t)pv);
        chk("LDAI", (uint8_t)a, ef, (uint8_t)(0xFF & ~(FY | FX)), true);
        c_ldai++;
      }
  std::printf("  LD A,I/R (OP_LDAI): %ld cases = 256 A x 2 F x 2 IFF2; S/Z/H/P/N/C CITED, XF/YF masked\n", c_ldai);

  // ---- class 8: RLD / RRD, docs/05 §10 ------------------------------------------
  // a = A, b = m = (HL). Rules [UM] pp.238/240 transcribed in [05] §10.1/10.2:
  //   RLD: (HL)' = {m[3:0], A[3:0]}   A' = {A[7:4], m[7:4]}
  //   RRD: (HL)' = {A[3:0], m[7:4]}   A' = {A[7:4], m[3:0]}
  // Flags ([05] §10.3, [UM] pp.239/241): S, Z, P/V from A'; H=0; N=0; C unchanged —
  // CITED and checked. XF/YF: DEDUCED (general rule) -> masked.
  // The "-M" ops do not touch F (F unchanged, full mask). Exhaustive A x m for
  // each of the 4 ops (65,536), with f_in varied case by case.
  for (int a = 0; a < 256; a++)
    for (int b = 0; b < 256; b++) {
      uint8_t f   = (uint8_t)(a ^ b ^ 0x5A);
      uint8_t rlm = (uint8_t)(((b & 0x0F) << 4) | (a & 0x0F));
      uint8_t rrm = (uint8_t)(((a & 0x0F) << 4) | (b >> 4));
      uint8_t rla = (uint8_t)((a & 0xF0) | (b >> 4));
      uint8_t rra = (uint8_t)((a & 0xF0) | (b & 0x0F));
      drive(OP_RLDM, (uint8_t)a, (uint8_t)b, f, 0, 0, 0, 0);
      chk("RLD-M", rlm, f, 0xFF, true);
      drive(OP_RRDM, (uint8_t)a, (uint8_t)b, f, 0, 0, 0, 0);
      chk("RRD-M", rrm, f, 0xFF, true);
      uint8_t efa = (uint8_t)(((rla & 0x80) ? FS : 0) | (rla == 0 ? FZ : 0) |
                              (par(rla) ? FP : 0) | (f & FC));
      drive(OP_RLDA, (uint8_t)a, (uint8_t)b, f, 0, 0, 0, 0);
      chk("RLD-A", rla, efa, (uint8_t)(0xFF & ~(FY | FX)), true);
      efa = (uint8_t)(((rra & 0x80) ? FS : 0) | (rra == 0 ? FZ : 0) |
                      (par(rra) ? FP : 0) | (f & FC));
      drive(OP_RRDA, (uint8_t)a, (uint8_t)b, f, 0, 0, 0, 0);
      chk("RRD-A", rra, efa, (uint8_t)(0xFF & ~(FY | FX)), true);
      c_rld += 4;
    }
  // the two examples of the manual as NAMED cases ([UM] p.239 and p.241, [05] §10.4)
  {
    drive(OP_RLDM, 0x7A, 0x31, 0x00, 0, 0, 0, 0);
    bool ok1 = (dut->res == 0x1A);
    drive(OP_RLDA, 0x7A, 0x31, 0x00, 0, 0, 0, 0);
    bool ok2 = (dut->res == 0x73);
    drive(OP_RRDM, 0x84, 0x20, 0x00, 0, 0, 0, 0);
    bool ok3 = (dut->res == 0x42);
    drive(OP_RRDA, 0x84, 0x20, 0x00, 0, 0, 0, 0);
    bool ok4 = (dut->res == 0x80);
    if (!(ok1 && ok2)) { n_err++; std::printf("FAIL example [UM] p.239 RLD A=7A (HL)=31 -> expected A'=73 (HL)'=1A\n"); }
    if (!(ok3 && ok4)) { n_err++; std::printf("FAIL example [UM] p.241 RRD A=84 (HL)=20 -> expected A'=80 (HL)'=42\n"); }
    std::printf("  [UM] examples: RLD 7A/31 -> A' 73 (HL)' 1A %s; RRD 84/20 -> A' 80 (HL)' 42 %s\n",
                (ok1 && ok2) ? "OK" : "** FAIL **", (ok3 && ok4) ? "OK" : "** FAIL **");
  }

  // ---- 16 bits: declared COMPOSITIONAL coverage --------------------------------
  // NOT exhaustive (2^33 per op). Argument: the 16-bit sum is two 8-bit sums in
  // cascade. Phase A: pairs of low bytes EXHAUSTIVE (256x256) with two pairs of
  // high bytes that make both H/YF/XF and C observable as the low carry varies.
  // Phase B: pairs of high bytes EXHAUSTIVE (256x256) with the carry from the
  // low byte forced to 0 and to 1. Each half adder is therefore enumerated with
  // both carries; only the arbitrary low x high cross combinations are not
  // covered, and they are redundant under composition.
  static const int ops16[3] = {OP_ADD16H, OP_ADC16H, OP_SBC16H};
  for (int oi = 0; oi < 3; oi++)
    for (int ci = 0; ci < 2; ci++) {
      int op16 = ops16[oi];
      uint8_t f = ci ? 0xFF : 0x00;
      bool sub = (op16 == OP_SBC16H);
      int  c   = (op16 == OP_ADD16H) ? 0 : ci;
      auto expect = [&](uint16_t x, uint16_t y, uint16_t& r16, uint8_t& ef) {
        long wide = sub ? ((long)x - y - c) : ((long)x + y + c);
        r16 = (uint16_t)wide;
        bool cf = sub ? (wide < 0) : (wide > 0xFFFF);
        bool hf = sub ? ((((long)(x & 0xFFF)) - (y & 0xFFF) - c) < 0)
                      : (((x & 0xFFF) + (y & 0xFFF) + c) > 0xFFF);
        long sv = sub ? ((long)(int16_t)x - (int16_t)y - c)
                      : ((long)(int16_t)x + (int16_t)y + c);
        bool vf = (sv < -32768 || sv > 32767);
        // CITED: XF/YF (and S,H) from the HIGH STEP — "The SF, YF, HF, XF flags
        // are affected as by the second (high) 8 bit addition" ([SY] §4.6
        // p.17; §8.6 note 2 p.31). Checked.
        uint8_t yx = (uint8_t)(((r16 & 0x2000) ? FY : 0) | ((r16 & 0x0800) ? FX : 0));
        if (op16 == OP_ADD16H)
          ef = (uint8_t)((f & (FS | FZ | FP)) | yx | (hf ? FH : 0) | (cf ? FC : 0));
        else
          ef = (uint8_t)(((r16 & 0x8000) ? FS : 0) | (r16 == 0 ? FZ : 0) | yx |
                         (hf ? FH : 0) | (vf ? FP : 0) | (sub ? FN : 0) |
                         (cf ? FC : 0));
      };
      // phase A: low bytes exhaustive
      static const uint8_t hp[2][2] = {{0x0F, 0x00}, {0xFF, 0x00}};
      for (int k = 0; k < 2; k++)
        for (int al = 0; al < 256; al++)
          for (int bl = 0; bl < 256; bl++) {
            uint16_t x = (uint16_t)((hp[k][0] << 8) | al);
            uint16_t y = (uint16_t)((hp[k][1] << 8) | bl);
            uint16_t er; uint8_t ef;
            expect(x, y, er, ef);
            run16(op16, x, y, f, er, ef, "16A");
            c_16++;
          }
      // phase B: high bytes exhaustive, low carry forced k=0 and k=1
      for (int k = 0; k < 2; k++)
        for (int ah = 0; ah < 256; ah++)
          for (int bh = 0; bh < 256; bh++) {
            uint8_t al = sub ? (k ? 0x00 : 0xFF) : (k ? 0xFF : 0x00);
            uint8_t bl = sub ? (k ? 0xFF : 0x00) : (k ? 0xFF : 0x00);
            uint16_t x = (uint16_t)((ah << 8) | al);
            uint16_t y = (uint16_t)((bh << 8) | bl);
            uint16_t er; uint8_t ef;
            expect(x, y, er, ef);
            run16(op16, x, y, f, er, ef, "16B");
            c_16++;
          }
    }
#endif  // TB_VARIANT_NMOS

  // ---- SCF/CCF x 256 A x 256 F x 6 Q (in BOTH variants) ------------------------
  // Q: exhaustive on bits 5 and 3 (the only ones in the rule) + two patterns with
  // the other bits at 1 to check that they do not matter.
  static const uint8_t qv[6] = {0x00, 0x08, 0x20, 0x28, 0xD7, 0xFF};
  for (int isccf = 0; isccf < 2; isccf++)
    for (int a = 0; a < 256; a++)
      for (int f = 0; f < 256; f++)
        for (int qi = 0; qi < 6; qi++) {
          uint8_t q = qv[qi];
#if TB_VARIANT_NMOS
          uint8_t y = (uint8_t)(((q | a) & 0x20) ? FY : 0);
          uint8_t x = (uint8_t)(((q | a) & 0x08) ? FX : 0);
#else
          uint8_t y = (uint8_t)((a & 0x20) ? FY : 0);
          uint8_t x = (uint8_t)((a & 0x08) ? FX : 0);
#endif
          uint8_t keep = (uint8_t)(f & (FS | FZ | FP));
          uint8_t ef;
          if (!isccf) ef = (uint8_t)(keep | y | x | FC);                // SCF
          else        ef = (uint8_t)(keep | y | x | ((f & FC) ? FH : 0) |
                                     ((f & FC) ? 0 : FC));             // CCF
          drive(isccf ? OP_CCF : OP_SCF, (uint8_t)a, 0, (uint8_t)f, q, 0, 0, 0);
          chk("SCF/CCF", (uint8_t)a, ef, 0xFF, true);
          c_scfccf++;
        }

  // ---- report --------------------------------------------------------------
  long req = c_twoop + c_unary + c_shift + c_arot + c_daa + c_bit + c_ressetd;
#if TB_VARIANT_NMOS
  std::printf("variant: NMOS (default)\n");
  std::printf("  two operands: %ld\n  unary       : %ld\n  CB shifts   : %ld\n",
              c_twoop, c_unary, c_shift);
  std::printf("  A rotates   : %ld\n  DAA         : %ld\n  BIT         : %ld\n",
              c_arot, c_daa, c_bit);
  std::printf("  RES/SET     : %ld\n", c_ressetd);
  std::printf("  total of the required classes: %ld (expected 1325056) %s\n", req,
              req == 1325056 ? "OK" : "** DOES NOT MATCH **");
  std::printf("  DAA structural (total rule): uncovered=%ld duplicates=%ld out of 2048"
              " (expected 0/0) %s\n", daa_unc, daa_dup,
              (daa_unc == 0 && daa_dup == 0) ? "OK" : "** FAILED **");
  std::printf("  DAA non-regression [UM]: covered=%ld (expected 684), "
              "differences A'/C'=%ld (expected 0) %s\n", um_cov, um_diff,
              (um_cov == 684 && um_diff == 0) ? "OK" : "** FAILED **");
  std::printf("  extra 16 bits (compositional, NOT exhaustive; in TWO 8-bit STEPS): %ld\n", c_16);
  std::printf("  service ops: PASS %ld (exhaustive a x F), TST %ld (exhaustive a x F), "
              "LDI-F %ld, CPI-F %ld (a x b x 4 BC' x 2 F)\n",
              c_pass, c_tst, c_ldif, c_cpif);
  std::printf("  service ops — CITED and checked: PASS (definition [06] §2.1), TST S/Z/H/P/N/C, "
              "LDI-F 8/8 bits, CPI-F 8/8 bits\n");
  std::printf("  service ops — MASKED: TST XF/YF (DEDUCED [SY] §2.3)\n");
  std::printf("  RLD/RRD (docs/05 §10): %ld cases = 4 ops x 65536 (A x m); S/Z/P/H/N/C CITED and "
              "checked, XF/YF DEDUCED masked; F unchanged on the two -M ops\n", c_rld);
#else
  std::printf("variant: CMOS\n");
#endif
  std::printf("  extra SCF/CCF: %ld\n", c_scfccf);
#if TB_VARIANT_NMOS
  std::printf("XF/YF CHECKED (CITED, 10 ops): CP DAA SCF CCF BIT RES SET "
              "ADD16 ADC16 SBC16\n");
  std::printf("XF/YF NOT CHECKED (DEDUCED from [SY] §2.3, masked, 23 ops): "
              "ADD ADC SUB SBC AND XOR OR INC DEC NEG CPL RLC RRC RL RR SLA "
              "SRA SLL SRL RLCA RRCA RLA RRA\n");
#else
  std::printf("XF/YF checked in this variant: SCF CCF (CITED)\n");
#endif
  std::printf("errors: %ld -> %s\n", n_err, n_err ? "FAIL" : "PASS");

  bool count_bad = false;
#if TB_VARIANT_NMOS
  count_bad = (req != 1325056) || daa_unc != 0 || daa_dup != 0 ||
              um_cov != 684 || um_diff != 0;
#endif
  delete dut;
  return (n_err != 0 || count_bad) ? 1 : 0;
}
