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
// tb_rmZ80_regfile — self-checking bench for rtl/rmZ80_regfile.sv.
// exit 0 = PASS.
//
// Classes:
//  a) R: 256 values x {LD R,A, increment} — bit 7 kept, 7 bits modulo 128
//  b) 8/16-bit aliases: 65,536 per pair, in both directions (BC DE HL IX IY SP PC
//     as required, plus AF, declared)
//  c) bank swaps: all 4 combinations of the 2 selectors, distinct patterns;
//     EXX does not touch AF, EX AF,AF' does not touch BC/DE/HL
//  f) dedicated outputs A/F/I/R: A and F follow the active bank
//  d) COMPLETENESS OF THE SAVESTATE: for EACH state element, alone, all ones ->
//     parallel read -> load of zero (checked) -> restore -> functional check.
//     Also: popcount of the dump = width of the field, and the positions of the
//     fields in the dump are DISJOINT with union = the whole savestate (210).
//  e) round trip of a full state, pseudo-random with a fixed seed, DECLARED not
//     exhaustive (2^210): 1000 iterations — every bit of the savestate sees both
//     polarities hundreds of times and any misalignment shows up; deterministic
//     completeness is already given by (d).
//  Structure: PARALLEL savestate of 210 bits, walking one over every position
//  (it must be 210 = 208 architectural bits + 2 selectors, docs/01 §5).
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "Vrf.h"
#include "verilated.h"

double sc_time_stamp() { return 0; }

// selectors — they must match the localparams of the RTL
enum : uint8_t { R8_B = 0, R8_C, R8_D, R8_E, R8_H, R8_L, R8_F, R8_A,
                 R8_IXH, R8_IXL, R8_IYH, R8_IYL, R8_SPH, R8_SPL,
                 R8_PCH, R8_PCL, R8_I, R8_R };
enum : uint8_t { R16_BC = 0, R16_DE, R16_HL, R16_SP, R16_IX, R16_IY,
                 R16_PC, R16_AF };

static Vrf* rf;
static long n_err = 0, n_prt = 0;

#define CK(cond, ...) do { if (!(cond)) { n_err++; \
  if (n_prt < 20) { n_prt++; std::printf("FAIL "); std::printf(__VA_ARGS__); \
    std::printf("\n"); } } } while (0)

static void idle() {
  rf->wr8_en = 0; rf->wr16_en = 0; rf->r_inc = 0;
  rf->swap_af = 0; rf->swap_gp = 0; rf->rd8_ix = 0; rf->rd8_fd = 0; rf->rd8_xhl = 0; rf->ss_load = 0;
}
static void tick() { rf->clk = 0; rf->eval(); rf->clk = 1; rf->eval(); }
static void wr8(uint8_t sel, uint8_t v) {
  idle(); rf->wr8_en = 1; rf->wr8_sel = sel; rf->wr8_data = v; rf->wr8_f = v; tick(); idle();
}
static void wr16(uint8_t sel, uint16_t v) {
  idle(); rf->wr16_en = 1; rf->wr16_sel = sel; rf->wr16_data = v; rf->wr16_af = v; tick(); idle();
}
// IXh/IXl/IYh/IYl are READ as in the core: H/L codes with the substitution on the
// data side (rd8_ix, rd8_fd); codes 8-11 do not exist on the read port
static uint8_t rd8(uint8_t sel) {
  bool ixy = (sel == R8_IXH || sel == R8_IXL || sel == R8_IYH || sel == R8_IYL);
  rf->rd8_ix = ixy; rf->rd8_fd = (sel == R8_IYH || sel == R8_IYL);
  rf->rd8_sel = ixy ? ((sel == R8_IXH || sel == R8_IYH) ? R8_H : R8_L) : sel;
  rf->eval();
  uint8_t d = rf->rd8_data;
  rf->rd8_ix = 0; rf->rd8_fd = 0; rf->eval();
  return d;
}
// 16-bit port: the register file receives the RAW code of the cw (logical ^ 6) and the substitutions separately
static uint16_t rd16(uint8_t sel) { rf->rd16_sel = sel ^ 6; rf->rd16_ix = 0; rf->rd16_fd = 0; rf->rd16_xhl = 0; rf->eval(); return rf->rd16_data; }
static void swap(bool af, bool gp) {
  idle(); rf->swap_af = af; rf->swap_gp = gp; tick(); idle();
}
static void rinc() { idle(); rf->r_inc = 1; tick(); idle(); }
static const int SS_EXP = 210;    // expected: docs/01 §5 (208 + 2)

// parallel savestate: MSB-first view d[t] = ss_dout[209-t] (order of the fields of the struct)
static void ss_dump(uint8_t* d) {
  for (int t = 0; t < SS_EXP; t++) { int k = SS_EXP - 1 - t; d[t] = (uint8_t)((rf->ss_dout[k >> 5] >> (k & 31)) & 1u); }
}
static void ss_load(const uint8_t* d) {          // load in ONE clock (cen = 0: in this bench cen is always 1, here it is lowered)
  for (int w = 0; w < 7; w++) rf->ss_din[w] = 0;
  for (int t = 0; t < SS_EXP; t++) if (d[t]) { int k = SS_EXP - 1 - t; rf->ss_din[k >> 5] |= 1u << (k & 31); }
  idle(); int cen0 = rf->cen; rf->cen = 0; rf->ss_load = 1; tick(); rf->ss_load = 0; rf->cen = (CData)cen0; idle();
}
static void ss_zero() { uint8_t z[SS_EXP]; std::memset(z, 0, sizeof z); ss_load(z); }

// ---- mirror for the full round trip -----------------------------------------
struct Mirror {
  uint8_t a[2], f[2], b[2], c[2], d[2], e[2], h[2], l[2];
  uint16_t ix, iy, sp, pc;
  uint8_t i, r;
  int sa, sg;   // current selectors
};

// brings the selectors of the DUT to the wanted state, tracking the mirror
static void set_sel(Mirror& m, int sa, int sg) {
  if (m.sa != sa) { swap(true, false); m.sa = sa; }
  if (m.sg != sg) { swap(false, true); m.sg = sg; }
}

static void check_full(Mirror& m, const char* tag) {
  // current AF bank, then the other one, then back
  for (int k = 0; k < 2; k++) {
    int bank = m.sa;
    CK(rd8(R8_A) == m.a[bank], "%s A[%d]", tag, bank);
    CK(rd8(R8_F) == m.f[bank], "%s F[%d]", tag, bank);
    // dedicated outputs (docs/06 §2.2): they must follow the ACTIVE bank
    CK(rf->a_out == m.a[bank] && rf->f_out == m.f[bank],
       "%s a_out/f_out bank %d: %02X/%02X expected %02X/%02X", tag, bank,
       rf->a_out, rf->f_out, m.a[bank], m.f[bank]);
    swap(true, false); m.sa ^= 1;
  }
  for (int k = 0; k < 2; k++) {
    int bank = m.sg;
    CK(rd8(R8_B) == m.b[bank], "%s B[%d]", tag, bank);
    CK(rd8(R8_C) == m.c[bank], "%s C[%d]", tag, bank);
    CK(rd8(R8_D) == m.d[bank], "%s D[%d]", tag, bank);
    CK(rd8(R8_E) == m.e[bank], "%s E[%d]", tag, bank);
    CK(rd8(R8_H) == m.h[bank], "%s H[%d]", tag, bank);
    CK(rd8(R8_L) == m.l[bank], "%s L[%d]", tag, bank);
    swap(false, true); m.sg ^= 1;
  }
  CK(rd16(R16_IX) == m.ix, "%s IX", tag);
  CK(rd16(R16_IY) == m.iy, "%s IY", tag);
  CK(rd16(R16_SP) == m.sp, "%s SP", tag);
  CK(rd16(R16_PC) == m.pc, "%s PC", tag);
  CK(rd8(R8_I) == m.i, "%s I", tag);
  CK(rd8(R8_R) == m.r, "%s R", tag);
  CK(rf->i_out == m.i && rf->r_out == m.r, "%s i_out/r_out", tag);
}

static uint32_t rng_state = 0xC0FFEE01u;   // fixed seed, declared
static uint32_t rnd() {
  rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5; return rng_state;
}

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  rf = new Vrf;
  idle(); rf->cen = 1; rf->rst = 0;

  long c_r = 0, c_alias = 0, c_bank = 0, c_chain = 0, c_rt = 0;

  // ---- reset: ONLY what the specification fixes -----------------------------
  rf->rst = 1; tick(); rf->rst = 0;
  CK(rd8(R8_I) == 0, "reset I");        // docs/04 §7
  CK(rd8(R8_R) == 0, "reset R");        // docs/03 §3.2 [SY]
  // the rest is NOT fixed: not checked (declared)

  // ---- a) R: 256 x {LD R,A ; increment} -------------------------------------
  for (int v = 0; v < 256; v++) {
    wr8(R8_R, (uint8_t)v);
    CK(rd8(R8_R) == v, "LD R,%02X", v);
    c_r++;
    rinc();
    uint8_t exp = (uint8_t)((v & 0x80) | ((v + 1) & 0x7F));
    CK(rd8(R8_R) == exp, "R inc from %02X: %02X/%02X", v, rd8(R8_R), exp);
    c_r++;
  }

  // ---- b) 8/16-bit aliases: 65,536 per pair, both directions -----------------
  struct Pair { uint8_t p16, hi, lo; const char* nm; };
  static const Pair pairs[8] = {
    {R16_BC, R8_B, R8_C, "BC"}, {R16_DE, R8_D, R8_E, "DE"},
    {R16_HL, R8_H, R8_L, "HL"}, {R16_IX, R8_IXH, R8_IXL, "IX"},
    {R16_IY, R8_IYH, R8_IYL, "IY"}, {R16_SP, R8_SPH, R8_SPL, "SP"},
    {R16_PC, R8_PCH, R8_PCL, "PC"}, {R16_AF, R8_A, R8_F, "AF"},  // AF: in addition
  };
  for (int p = 0; p < 8; p++)
    for (int v = 0; v < 65536; v++) {
      wr16(pairs[p].p16, (uint16_t)v);
      CK(rd8(pairs[p].hi) == (v >> 8) && rd8(pairs[p].lo) == (v & 0xFF),
         "alias %s 16->8 v=%04X", pairs[p].nm, v);
      uint16_t u = (uint16_t)(v ^ 0xFFFF);
      wr8(pairs[p].hi, (uint8_t)(u >> 8));
      wr8(pairs[p].lo, (uint8_t)(u & 0xFF));
      CK(rd16(pairs[p].p16) == u, "alias %s 8->16 v=%04X", pairs[p].nm, u);
      c_alias++;
    }

  // ---- c) bank swaps: 4 combinations, distinct patterns -----------------------
  // selectors here: (0,0) — never touched so far
  wr8(R8_A, 0x11); wr8(R8_F, 0x22);
  wr8(R8_B, 0x31); wr8(R8_C, 0x32); wr8(R8_D, 0x33);
  wr8(R8_E, 0x34); wr8(R8_H, 0x35); wr8(R8_L, 0x36);
  swap(false, true);   // EXX -> (0,1)
  CK(rd8(R8_A) == 0x11 && rd8(R8_F) == 0x22, "EXX must not touch AF");
  c_bank++;
  wr8(R8_B, 0x41); wr8(R8_C, 0x42); wr8(R8_D, 0x43);
  wr8(R8_E, 0x44); wr8(R8_H, 0x45); wr8(R8_L, 0x46);
  swap(true, false);   // EX AF,AF' -> (1,1)
  CK(rd8(R8_B) == 0x41 && rd8(R8_C) == 0x42 && rd8(R8_D) == 0x43 &&
     rd8(R8_E) == 0x44 && rd8(R8_H) == 0x45 && rd8(R8_L) == 0x46,
     "EX AF,AF' must not touch BC/DE/HL");
  c_bank++;
  wr8(R8_A, 0x51); wr8(R8_F, 0x52);
  swap(false, true);   // -> (1,0)
  CK(rd8(R8_A) == 0x51 && rd8(R8_F) == 0x52, "EXX does not touch AF (bank 1)");
  CK(rd8(R8_B) == 0x31 && rd8(R8_L) == 0x36, "back to gp bank 0");
  c_bank += 2;
  swap(true, false);   // -> (0,0)
  CK(rd8(R8_A) == 0x11 && rd8(R8_F) == 0x22, "back to af bank 0");
  CK(rd8(R8_B) == 0x31 && rd8(R8_C) == 0x32 && rd8(R8_H) == 0x35,
     "EX AF,AF' does not touch BC/DE/HL (back)");
  c_bank += 2;

  // ---- f) dedicated outputs A/F/I/R (docs/06 §2.2) ----------------------------
  // A and F follow the ACTIVE bank: distinct values in the two banks, EX AF,AF'
  // in between, EXX that must not touch them; I and R follow the registers
  // (LD I,A / LD R,A / increment). Selectors here: (0,0) from c).
  long c_ded = 0;
  for (int v = 0; v < 256; v++) {
    uint8_t a0 = (uint8_t)v, f0 = (uint8_t)~v, a1 = (uint8_t)(v ^ 0x5A), f1 = (uint8_t)(v ^ 0xA5);
    wr8(R8_A, a0); wr8(R8_F, f0);
    CK(rf->a_out == a0 && rf->f_out == f0, "ded bank0 v=%02X: %02X/%02X", v, rf->a_out, rf->f_out);
    swap(true, false);                                   // EX AF,AF' -> bank 1
    wr8(R8_A, a1); wr8(R8_F, f1);
    CK(rf->a_out == a1 && rf->f_out == f1, "ded bank1 v=%02X: %02X/%02X", v, rf->a_out, rf->f_out);
    swap(false, true);                                   // EXX: A/F do not change
    CK(rf->a_out == a1 && rf->f_out == f1, "ded EXX v=%02X", v);
    swap(false, true);
    swap(true, false);                                   // back to bank 0
    CK(rf->a_out == a0 && rf->f_out == f0, "ded back v=%02X: %02X/%02X", v, rf->a_out, rf->f_out);
    wr8(R8_I, (uint8_t)v);
    CK(rf->i_out == (uint8_t)v && rf->i_out == rd8(R8_I), "ded I v=%02X", v);
    wr8(R8_R, (uint8_t)v);
    CK(rf->r_out == (uint8_t)v, "ded R v=%02X", v);
    rinc();
    CK(rf->r_out == (uint8_t)((v & 0x80) | ((v + 1) & 0x7F)) && rf->r_out == rd8(R8_R),
       "ded R inc v=%02X", v);
    c_ded += 7;
  }

  // ---- parallel savestate: 210 bits, walking one over every position (load -> read back) ----
  int len = SS_EXP;                             // width of the port, fixed by the type
  ss_zero();
  { uint8_t z[SS_EXP]; ss_dump(z); for (int t = 0; t < SS_EXP; t++) CK(z[t] == 0, "state not cleared: bit %d", t); }
  for (int k = 0; k < SS_EXP; k++) {
    uint8_t pat[SS_EXP], back[SS_EXP];
    for (int t = 0; t < SS_EXP; t++) pat[t] = (uint8_t)(t == k);
    ss_load(pat); ss_dump(back);
    CK(!std::memcmp(pat, back, SS_EXP), "walking one %d: read back differs", k);
  }
  ss_zero();

  // ---- d) completeness of the savestate, element by element ------------------
  struct El { const char* nm; int kind; uint8_t sel; int bank; int width; };
  enum { K8BK_AF, K8BK_GP, K16, K8, KSEL_AF, KSEL_GP };
  static const El els[24] = {
    {"A0", K8BK_AF, R8_A, 0, 8}, {"F0", K8BK_AF, R8_F, 0, 8},
    {"A1", K8BK_AF, R8_A, 1, 8}, {"F1", K8BK_AF, R8_F, 1, 8},
    {"B0", K8BK_GP, R8_B, 0, 8}, {"C0", K8BK_GP, R8_C, 0, 8},
    {"D0", K8BK_GP, R8_D, 0, 8}, {"E0", K8BK_GP, R8_E, 0, 8},
    {"H0", K8BK_GP, R8_H, 0, 8}, {"L0", K8BK_GP, R8_L, 0, 8},
    {"B1", K8BK_GP, R8_B, 1, 8}, {"C1", K8BK_GP, R8_C, 1, 8},
    {"D1", K8BK_GP, R8_D, 1, 8}, {"E1", K8BK_GP, R8_E, 1, 8},
    {"H1", K8BK_GP, R8_H, 1, 8}, {"L1", K8BK_GP, R8_L, 1, 8},
    {"IX", K16, R16_IX, 0, 16}, {"IY", K16, R16_IY, 0, 16},
    {"SP", K16, R16_SP, 0, 16}, {"PC", K16, R16_PC, 0, 16},
    {"I",  K8,  R8_I, 0, 8},    {"R",  K8,  R8_R, 0, 8},
    {"SEL_AF", KSEL_AF, 0, 0, 1}, {"SEL_GP", KSEL_GP, 0, 0, 1},
  };
  uint8_t dump[SS_EXP], covered[SS_EXP];
  std::memset(covered, 0, sizeof covered);
  int width_sum = 0;
  for (int e = 0; e < 24; e++) {
    const El& el = els[e];
    // state all zero (through the savestate), selectors included
    ss_zero();
    // write ALL ONES into the element only
    switch (el.kind) {
      case K8BK_AF: if (el.bank) swap(true, false);
                    wr8(el.sel, 0xFF);
                    if (el.bank) swap(true, false); break;
      case K8BK_GP: if (el.bank) swap(false, true);
                    wr8(el.sel, 0xFF);
                    if (el.bank) swap(false, true); break;
      case K16:     wr16(el.sel, 0xFFFF); break;
      case K8:      wr8(el.sel, 0xFF); break;
      case KSEL_AF: swap(true, false); break;
      case KSEL_GP: swap(false, true); break;
    }
    // dump, clear (load of zero), functional check of zero, restore
    ss_dump(dump);
    ss_zero();
    Mirror z; std::memset(&z, 0, sizeof z);
    check_full(z, el.nm);                 // after the load of zero: everything zero
    ss_load(dump);
    // popcount = width of the field; positions disjoint from the other fields
    int pop = 0;
    for (int t = 0; t < SS_EXP; t++) {
      if (dump[t]) {
        pop++;
        CK(covered[t] == 0, "%s: bit %d already taken by another field", el.nm, t);
        covered[t] = 1;
      }
    }
    CK(pop == el.width, "%s: popcount %d, width %d", el.nm, pop, el.width);
    width_sum += el.width;
    // functional check of the restore
    switch (el.kind) {
      case K8BK_AF: if (el.bank) swap(true, false);
                    CK(rd8(el.sel) == 0xFF, "%s restore", el.nm);
                    if (el.bank) swap(true, false); break;
      case K8BK_GP: if (el.bank) swap(false, true);
                    CK(rd8(el.sel) == 0xFF, "%s restore", el.nm);
                    if (el.bank) swap(false, true); break;
      case K16:     CK(rd16(el.sel) == 0xFFFF, "%s restore", el.nm); break;
      case K8:      CK(rd8(el.sel) == 0xFF, "%s restore", el.nm); break;
      case KSEL_AF: // sel_af restored to 1: A written now goes into bank 1
                    wr8(R8_A, 0x5A); swap(true, false);
                    CK(rd8(R8_A) == 0x00, "SEL_AF restore (bank 0 untouched)");
                    swap(true, false);
                    CK(rd8(R8_A) == 0x5A, "SEL_AF restore (bank 1 written)");
                    break;
      case KSEL_GP: wr8(R8_B, 0x5A); swap(false, true);
                    CK(rd8(R8_B) == 0x00, "SEL_GP restore (bank 0 untouched)");
                    swap(false, true);
                    CK(rd8(R8_B) == 0x5A, "SEL_GP restore (bank 1 written)");
                    break;
    }
    c_chain++;
  }
  int uni = 0;
  for (int t = 0; t < SS_EXP; t++) uni += covered[t];
  CK(width_sum == SS_EXP, "sum of the field widths = %d (expected %d)", width_sum, SS_EXP);
  CK(uni == SS_EXP, "union of the field positions = %d (expected %d)", uni, SS_EXP);

  // ---- e) round trip of a full state, pseudo-random (NOT exhaustive) -----------
  // 1000 iterations with a fixed seed: every bit of the savestate sees both
  // polarities ~500 times; deterministic completeness is already in (d).
  ss_zero();
  Mirror m; std::memset(&m, 0, sizeof m);
  for (int it = 0; it < 1000; it++) {
    set_sel(m, 0, 0);
    m.a[0] = (uint8_t)rnd(); wr8(R8_A, m.a[0]);
    m.f[0] = (uint8_t)rnd(); wr8(R8_F, m.f[0]);
    m.b[0] = (uint8_t)rnd(); wr8(R8_B, m.b[0]);
    m.c[0] = (uint8_t)rnd(); wr8(R8_C, m.c[0]);
    m.d[0] = (uint8_t)rnd(); wr8(R8_D, m.d[0]);
    m.e[0] = (uint8_t)rnd(); wr8(R8_E, m.e[0]);
    m.h[0] = (uint8_t)rnd(); wr8(R8_H, m.h[0]);
    m.l[0] = (uint8_t)rnd(); wr8(R8_L, m.l[0]);
    set_sel(m, 1, 1);
    m.a[1] = (uint8_t)rnd(); wr8(R8_A, m.a[1]);
    m.f[1] = (uint8_t)rnd(); wr8(R8_F, m.f[1]);
    m.b[1] = (uint8_t)rnd(); wr8(R8_B, m.b[1]);
    m.c[1] = (uint8_t)rnd(); wr8(R8_C, m.c[1]);
    m.d[1] = (uint8_t)rnd(); wr8(R8_D, m.d[1]);
    m.e[1] = (uint8_t)rnd(); wr8(R8_E, m.e[1]);
    m.h[1] = (uint8_t)rnd(); wr8(R8_H, m.h[1]);
    m.l[1] = (uint8_t)rnd(); wr8(R8_L, m.l[1]);
    m.ix = (uint16_t)rnd(); wr16(R16_IX, m.ix);
    m.iy = (uint16_t)rnd(); wr16(R16_IY, m.iy);
    m.sp = (uint16_t)rnd(); wr16(R16_SP, m.sp);
    m.pc = (uint16_t)rnd(); wr16(R16_PC, m.pc);
    m.i = (uint8_t)rnd(); wr8(R8_I, m.i);
    m.r = (uint8_t)rnd(); wr8(R8_R, m.r);
    set_sel(m, (int)(rnd() & 1), (int)(rnd() & 1));
    ss_dump(dump);          // read
    ss_zero();              // state -> 0
    ss_load(dump);          // restore
    check_full(m, "roundtrip");
    c_rt++;
  }

  // ---- report --------------------------------------------------------------
  std::printf("parallel savestate: %d bits (expected %d), walking one %d/%d\n", len, SS_EXP, SS_EXP, SS_EXP);
  std::printf("  R (LD + inc)      : %ld\n", c_r);
  std::printf("  8/16 aliases      : %ld (8 pairs x 65536, both directions)\n", c_alias);
  std::printf("  bank swaps        : %ld checks\n", c_bank);
  std::printf("  dedicated outputs : %ld checks (A/F per bank with EX AF,AF' and EXX; I; R with inc)\n", c_ded);
  std::printf("  completeness      : %ld elements = %d bits, exact union\n",
              c_chain, width_sum);
  std::printf("  full round trips  : %ld (pseudo-random, fixed seed, NOT exhaustive)\n",
              c_rt);
  std::printf("errors: %ld -> %s\n", n_err, n_err ? "FAIL" : "PASS");
  delete rf;
  return n_err ? 1 : 0;
}
