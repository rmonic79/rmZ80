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
// tb_rmZ80_seq — self-checking bench for the sequencer (rmZ80_seq) alone.
// exit 0 = PASS. Checks TICK BY TICK (half T-state by half T-state) against the
// rules of docs/02 §2-§7, enumerating every M-cycle type x external waits 0..8.
//
// WHAT IS CHECKED AGAINST THE SPECIFICATION (citation next to the point):
//   - lengths: M1=4T, RD/WR=3T, I/O=4T (3+1 automatic, [02] §5), IACK=6T (4+2, [02]
//     §7) — COUNTED per cycle, +1 T for every external wait served ([02] §2);
//   - M1: MREQ+RD from T1B ([02] §3), off from the edge that starts T3 ([02] §3),
//     RFSH active in T3-T4, NEVER RD in the refresh, low 7 bits = R and high byte =
//     I ([02] §3);
//   - M1 refresh ([02] §3.1): MREQ ACTIVE in T3B and T4A and inactive in T3A and
//     T4B (point 4, figure 5); low byte of the address STABLE over all 4 half T-states
//     (point 5) — the bench emulates the register file: R advances at the tick after
//     r_inc, and the address must stay the one at the start of the window; r_inc in
//     the LAST half T-state of the window (T4B / T6B) — a declared choice ([02] §3.2:
//     "after T4B");
//   - WAIT sampled on the FALLING edge of T2 and of every TW, that is at the A->B tick ([UM] p.7, [02] §2): besides
//     waits held for whole T-states, WAIT low only in the A phases (counted) and only in the B phases (ignored);
//   - INT and NMI sampled on the rising edge that opens the last T of the instruction ([UM] p.12, fig. 9, p.13):
//     INT low only after that edge is not accepted, INT removed after that edge is accepted; an NMI edge after
//     that edge is accepted at the end of the next instruction, an NMI edge on that edge at the end of the same
//     instruction;
//   - data sampled on the edge that starts T3 ([02] §3) — observed on IR/DIN;
//   - WR inactive in T3B, half a T-state before the bus changes ([02] §4);
//   - IACK: MREQ NEVER active (IORQ in place of MREQ, [02] §7);
//   - R: exactly ONE r_inc pulse per M1 and per IACK ([03] §3.2).
//
// MASKED (no comparison — the specification is silent):
//   - bit 7 of the low byte of the refresh address ([02] §3: "lower seven bits";
//     the core puts bit 7 of R there, [SY]).
// CHECKED AS A DECLARED CONTRACT: MREQ inactive in the refresh of the IACK
//   (T5-T6, inherited window; [02] §7 "IORQ in place of MREQ", §3.1 speaks
//   only of M1).
//
// CHECKED ONLY AS A DECLARED CONTRACT (no citable source):
//   assertion instants of WR (T2A) and of IORQ/RD in the I/O and IACK cycles (T1B);
//   I/O strobes off at the edge of the sampling T; IACK without RD and with the
//   refresh in T5-T6; extended M1 (internal T5/T6); dbg_st / the order of the savestate.
//
// Savestate: width 64 = $bits of the microstate (docs/06 §5), proved with a walking
// one over all positions and random patterns, dump == dbg_st, restore + RESUME in
// the middle of a cycle with the waveforms still correct. The fields that these
// tests do not drive (Q: F written, DE<->HL renaming, BUSREQ sampling; BUSACK) are
// covered here ONLY as storage and position (walking one/dump), not functionally:
// declared (the execution bench and SingleStepTests check them).
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "Vseq.h"
#include "verilated.h"

double sc_time_stamp() { return 0; }

enum : uint8_t { MT_NONE = 0, MT_M1, MT_RD, MT_WR, MT_IORD, MT_IOWR, MT_IACK };
static const char* TNAME[7] = {"NONE", "M1", "RD", "WR", "IORD", "IOWR", "IACK"};

static Vseq* sq;
static long n_err = 0, n_prt = 0;
static long tick_cnt[7] = {0};

#define CK(cond, ...) do { if (!(cond)) { n_err++; \
  if (n_prt < 25) { n_prt++; std::printf("FAIL "); std::printf(__VA_ARGS__); \
    std::printf("\n"); } } } while (0)

// one tick = one edge with cen preceded by a quiet clock (contract cen >= 2 clocks: the seq registers
// the load conditions from the bus one clock earlier); the state moves only on the edge with cen
static void clk1() { sq->clk = 0; sq->eval(); sq->clk = 1; sq->eval(); }
static void tick() { CData c = sq->cen; sq->cen = 0; clk1(); sq->cen = c; clk1(); }

// offsets in the state vector (contract dbg_st = order of docs/06 §5, MSB first)
static uint64_t fld(uint64_t v, int hi, int lo) {
  return (v >> lo) & ((1ull << (hi - lo + 1)) - 1);
}
#define F_T(v)     fld(v, 62, 60)
#define F_TW(v)    fld(v, 59, 59)
#define F_M(v)     fld(v, 58, 56)
#define F_IR(v)    fld(v, 52, 45)
#define F_DIN(v)   fld(v, 41, 34)
#define F_IFF1(v)  fld(v, 9, 9)
#define F_IFF2(v)  fld(v, 8, 8)
#define F_IM(v)    fld(v, 7, 6)

static const int SS_EXP = 64;

// parallel savestate: the microstate is a 64-bit vector; MSB-first view d[j] = bit (63-j)
static void ss_dump(uint8_t* d) { for (int j = 0; j < SS_EXP; j++) d[j] = (uint8_t)((sq->ss_dout >> (SS_EXP - 1 - j)) & 1u); }
static void ss_load(const uint8_t* d) {         // load in ONE clock with cen = 0 (contract), then cen as before
  uint64_t v = 0;
  for (int j = 0; j < SS_EXP; j++) if (d[j]) v |= 1ull << (SS_EXP - 1 - j);
  int cen0 = sq->cen; sq->cen = 0; sq->ss_din = v; sq->ss_load = 1; tick(); sq->ss_load = 0; sq->cen = (CData)cen0;
}

// t3p and windows per type (rules of [02], the same as the specification)
static int t3p_of(int type) {
  if (type == MT_IORD || type == MT_IOWR) return 4;
  if (type == MT_IACK) return 5;
  return 3;
}
static bool refr(int type, int t) {
  return (type == MT_M1 && t >= 3 && t <= 4) || (type == MT_IACK && t >= 5 && t <= 6);
}

// expected waveform: active-low levels for (type, t, phase)
struct Wave { int mreq, iorq, rd, wr, rfsh; bool care_mreq; int addr_kind; };
// addr_kind: 0 = addr_val, 1 = refresh ({I,R}, bit 7 of the low byte masked)
static Wave wave(int type, int t3, int t, int phase) {
  Wave w{1, 1, 1, 1, 1, true, 0};
  bool aft1B = (t > 1) || phase;
  bool env = (t < t3) && aft1B;
  switch (type) {
    case MT_M1: case MT_RD:
      w.mreq = env ? 0 : 1; w.rd = env ? 0 : 1; break;
    case MT_WR:
      w.mreq = (env || (t == t3 && !phase)) ? 0 : 1;
      w.wr = ((t >= 2 && t < t3) || (t == t3 && !phase)) ? 0 : 1; break;
    case MT_IORD:
      w.iorq = env ? 0 : 1; w.rd = env ? 0 : 1; break;
    case MT_IOWR:
      w.iorq = (env || (t == t3 && !phase)) ? 0 : 1;
      w.wr = ((t >= 2 && t < t3) || (t == t3 && !phase)) ? 0 : 1; break;
    case MT_IACK:
      w.iorq = env ? 0 : 1; break;   // MREQ never active — [02] §7 (checked)
    default: break;
  }
  if (refr(type, t)) {
    w.rfsh = 0;                       // [02] §3
    w.addr_kind = 1;
    w.care_mreq = true;
    if (type == MT_M1)                // [02] §3.1 p.4: MREQ active T3B-T4A, figure 5
      w.mreq = ((t == 3 && phase) || (t == 4 && !phase)) ? 0 : 1;
    else                              // IACK: MREQ inactive in the refresh too (declared)
      w.mreq = 1;
  }
  return w;
}
// last half T-state of the refresh window: where the seq declares r_inc
static int rinc_t_of(int type) { return (type == MT_M1) ? 4 : 6; }

// WAIT: 0 = low for whole T-states, 1 = low only in the A phases, 2 = low only in the B phases (while waits remain to be served)
static int g_wmode = 0;

// runs and checks a complete M-cycle; returns the T-states executed
static int run_cycle(int type, int durT, int n_wait, uint8_t dinv,
                     uint16_t av, uint8_t dv, uint8_t iv, uint8_t rv) {
  int t3 = t3p_of(type);
  int t = 1, phase = 0, lows = n_wait, tstates = 0, rpulses = 0;
  bool done = false, rept = false;
  uint8_t cur_r = rv;                 // emulation of the register file: R advances at the tick after r_inc
  while (!done) {
    if (!phase) tstates++;
    sq->din = dinv; sq->addr_val = av; sq->dout_val = dv;
    sq->i_val = iv; sq->r_val = cur_r;
    // [UM] p.7: WAIT is sampled on the FALLING edge of T2 and of every TW = the tick that goes from A to B
    bool samp = (type != MT_NONE) && !phase && (t >= 2) && (t < t3);
    bool low  = (lows > 0) && (g_wmode == 0 || ((g_wmode == 1) == !phase));
    sq->wait_n = low ? 0 : 1;
    sq->eval();
    // ---- comparison of the current half T-state ----
    Wave w = wave(type, t3, t, phase);
    if (w.care_mreq)
      CK(sq->mreq_n == w.mreq, "%s t%d%c mreq %d/%d", TNAME[type], t, phase ? 'B' : 'A', sq->mreq_n, w.mreq);
    CK(sq->iorq_n == w.iorq, "%s t%d%c iorq %d/%d", TNAME[type], t, phase ? 'B' : 'A', sq->iorq_n, w.iorq);
    CK(sq->rd_n == w.rd, "%s t%d%c rd %d/%d", TNAME[type], t, phase ? 'B' : 'A', sq->rd_n, w.rd);
    CK(sq->wr_n == w.wr, "%s t%d%c wr %d/%d", TNAME[type], t, phase ? 'B' : 'A', sq->wr_n, w.wr);
    CK(sq->rfsh_n == w.rfsh, "%s t%d%c rfsh %d/%d", TNAME[type], t, phase ? 'B' : 'A', sq->rfsh_n, w.rfsh);
    if (w.addr_kind == 0)
      CK(sq->addr == av, "%s t%d%c addr %04X/%04X", TNAME[type], t, phase ? 'B' : 'A', sq->addr, av);
    else {  // refresh: low 7 bits = R at the START of the window (stable, [02] §3.1 p.5),
            // high byte = I ([02] §3); bit 7 masked
      CK((sq->addr & 0x7F) == (rv & 0x7F) && (sq->addr >> 8) == iv,
         "%s t%d%c refresh addr %04X (expected R=%02X stable, I=%02X)", TNAME[type], t,
         phase ? 'B' : 'A', sq->addr, rv, iv);
    }
    bool rinc_now = (sq->r_inc != 0);
    if (rinc_now) {
      rpulses++;
      CK(refr(type, t) && phase && t == rinc_t_of(type),
         "%s r_inc at t%d%c: expected only in the last half T-state of the refresh window",
         TNAME[type], t, phase ? 'B' : 'A');
    }
    tick_cnt[type]++;
    // ---- advance of the mirror (rule [02] §2) ----
    tick();
    if (rinc_now) cur_r = (uint8_t)((cur_r & 0x80) | ((cur_r + 1) & 0x7F));
    if (!phase) { rept = samp && low; if (rept) lows--; phase = 1; continue; }
    phase = 0;
    if (rept) { rept = false; continue; }          // TW: the T repeats
    if (t == durT) done = true; else t++;
  }
  int exp_r = (type == MT_M1 || type == MT_IACK) ? 1 : 0;
  CK(rpulses == exp_r, "%s r_inc pulses %d/%d", TNAME[type], rpulses, exp_r);
  return tstates;
}

static void set_sched(int type, int durT, int last) {
  sq->sch_type = (uint8_t)type; sq->sch_dur = (uint8_t)(durT - 3);
  sq->sch_last = (uint8_t)last;
}
static void do_rst(int type, int durT) {
  set_sched(type, durT, 1);
  sq->rst = 1; tick(); sq->rst = 0;
}

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  sq = new Vseq;
  sq->cen = 1; sq->rst = 0; sq->ss_load = 0; sq->ss_din = 0; sq->wait_n = 1; sq->busreq_n = 1;   // active low: 1 at rest
  sq->din = 0; sq->addr_val = 0; sq->dout_val = 0;
  sq->i_val = 0; sq->r_val = 0; sq->wz_we = 0; sq->wz_din = 0;
  sq->sp_set = 0; sq->sp_val = 0; set_sched(MT_NONE, 3, 1);
  sq->im_we = 0; sq->im_val = 0; sq->iff_op = 0; sq->halt_set = 0; sq->nmi_n = 1; sq->int_n = 1; sq->ir_ld = 0; sq->q_we = 0; sq->swap_dehl = 0; sq->sel_gp = 0;

  // ---- reset: [04] §7 (IFF1=IFF2=0, IM=0) -------------------------------------
  do_rst(MT_NONE, 3);
  CK(F_IFF1(sq->dbg_st) == 0 && F_IFF2(sq->dbg_st) == 0 && F_IM(sq->dbg_st) == 0,
     "reset IFF/IM ([04] §7)");

  // ---- a+b: every type x wait 0..8: waveform + COUNTED length -----------------
  struct Sc { int type, durT, base; };
  static const Sc scs[6] = {{MT_M1, 4, 4}, {MT_RD, 3, 3}, {MT_WR, 3, 3},
                            {MT_IORD, 4, 4}, {MT_IOWR, 4, 4}, {MT_IACK, 6, 6}};
  for (int wm = 0; wm < 3; wm++)
  for (int s = 0; s < 6; s++)
    for (int nw = 0; nw <= 8; nw++) {
      g_wmode = wm;
      do_rst(scs[s].type, scs[s].durT);
      uint8_t dinv = (uint8_t)(0x30 + s * 9 + nw);
      uint64_t pre = sq->dbg_st;
      int ts = run_cycle(scs[s].type, scs[s].durT, nw, dinv,
                         (uint16_t)(0x1234 + s * 0x111), (uint8_t)(0xC0 + s),
                         (uint8_t)(0x40 + s), (uint8_t)(0x80 + nw));
      int exp_w = (wm == 2) ? 0 : nw;                 // low only in phase B: never seen by the falling edge
      CK(ts == scs[s].base + exp_w, "%s WAIT mode %d: length %d T (expected %d+%d)",
         TNAME[scs[s].type], wm, ts, scs[s].base, exp_w);
      // data sampled on the edge that starts T3 ([02] §3): destination
      if (scs[s].type == MT_M1 || scs[s].type == MT_IACK)   // IACK in the BASE space = IM0: the byte is the instruction
        CK(F_IR(sq->dbg_st) == dinv, "%s IR %02X/%02X", TNAME[scs[s].type], (int)F_IR(sq->dbg_st), dinv);
      else if (scs[s].type == MT_RD || scs[s].type == MT_IORD)
        CK(F_DIN(sq->dbg_st) == dinv, "%s DIN %02X/%02X", TNAME[scs[s].type],
           (int)F_DIN(sq->dbg_st), dinv);
      else
        CK(F_IR(sq->dbg_st) == F_IR(pre) && F_DIN(sq->dbg_st) == F_DIN(pre),
           "%s IR/DIN must stay unchanged", TNAME[scs[s].type]);
    }
  g_wmode = 0;

  // ---- NONE (internal cycles, lengths 3..6): no bus activity -------------------
  for (int d = 3; d <= 6; d++) {
    do_rst(MT_NONE, d);
    int ts = run_cycle(MT_NONE, d, 0, 0x00, 0x4000, 0, 0, 0);
    CK(ts == d, "NONE length %d/%d", ts, d);
  }
  // ---- extended M1 5/6 T (declared contract: internal T5/T6) -------------------
  for (int d = 5; d <= 6; d++) {
    do_rst(MT_M1, d);
    int ts = run_cycle(MT_M1, d, 0, 0x77, 0x2000, 0, 0x55, 0x2A);
    CK(ts == d, "extended M1 length %d/%d", ts, d);
  }

  // ---- sequence of M-cycles: m_idx advances and returns to 1 on sch_last --------
  // contract: the CURRENT row stays on the inputs up to and including the last tick
  // of its M-cycle; the next row is presented right after (the type is sampled at
  // the start of the new M-cycle)
  do_rst(MT_RD, 3);
  set_sched(MT_RD, 3, 0);            // 1st M: not the last
  CK(F_M(sq->dbg_st) == 1, "initial m_idx");
  run_cycle(MT_RD, 3, 0, 0x11, 0x0100, 0, 0, 0);
  CK(F_M(sq->dbg_st) == 2, "m_idx after the 1st M=%d", (int)F_M(sq->dbg_st));
  set_sched(MT_WR, 3, 0);            // 2nd M
  run_cycle(MT_WR, 3, 0, 0x22, 0x0200, 0x99, 0, 0);
  CK(F_M(sq->dbg_st) == 3, "m_idx after the 2nd M=%d", (int)F_M(sq->dbg_st));
  set_sched(MT_M1, 4, 1);            // 3rd and last M
  run_cycle(MT_M1, 4, 0, 0x33, 0x0300, 0, 0x10, 0x20);
  CK(F_M(sq->dbg_st) == 1, "m_idx after last=%d", (int)F_M(sq->dbg_st));
  set_sched(MT_NONE, 3, 1);

  // ---- WZ: write ports from the top module (docs/06 §2.2) -------------------------
  // low byte, high byte, whole word; no write without we; reset value 0
  long c_wz = 0;
  do_rst(MT_NONE, 3);
  sq->wz_we = 3; sq->wz_din = 0x1234; tick(); sq->wz_we = 0;
  CK(fld(sq->dbg_st, 33, 18) == 0x1234, "WZ <- 1234 (we=11): %04X", (unsigned)fld(sq->dbg_st, 33, 18)); c_wz++;
  sq->wz_we = 1; sq->wz_din = 0xAB56; tick(); sq->wz_we = 0;
  CK(fld(sq->dbg_st, 33, 18) == 0x1256, "WZL <- 56 (we=01), WZH unchanged: %04X", (unsigned)fld(sq->dbg_st, 33, 18)); c_wz++;
  sq->wz_we = 2; sq->wz_din = 0x78CD; tick(); sq->wz_we = 0;
  CK(fld(sq->dbg_st, 33, 18) == 0x7856, "WZH <- 78 (we=10), WZL unchanged: %04X", (unsigned)fld(sq->dbg_st, 33, 18)); c_wz++;
  sq->wz_din = 0xFFFF; tick(); tick();
  CK(fld(sq->dbg_st, 33, 18) == 0x7856, "WZ without we must stay: %04X", (unsigned)fld(sq->dbg_st, 33, 18)); c_wz++;
  for (int v = 0; v < 256; v++) {                 // exhaustive on the two bytes, separately
    sq->wz_we = 1; sq->wz_din = (uint16_t)(0x5A00 | v); tick();
    sq->wz_we = 2; sq->wz_din = (uint16_t)((v ^ 0xFF) << 8); tick(); sq->wz_we = 0;
    CK(fld(sq->dbg_st, 33, 18) == (unsigned)(((v ^ 0xFF) << 8) | v), "WZ byte v=%02X", v); c_wz++;
  }
  do_rst(MT_NONE, 3);
  CK(fld(sq->dbg_st, 33, 18) == 0x0000, "rst: WZ = 0 (defined value): %04X", (unsigned)fld(sq->dbg_st, 33, 18)); c_wz++;
  sq->wz_din = 0;

  // ---- IM and IFF from the cw: IM <- val; DI (op 1) IFF1=IFF2=0; RETN (op 2) IFF1 <- IFF2
  long c_ii = 0;
  for (int v = 0; v < 4; v++) {
    sq->im_we = 1; sq->im_val = (uint8_t)v; tick(); sq->im_we = 0;
    CK(F_IM(sq->dbg_st) == (unsigned)v, "IM <- %d: %u", v, (unsigned)F_IM(sq->dbg_st)); c_ii++;
  }
  sq->im_val = 0; tick();
  CK(F_IM(sq->dbg_st) == 3, "IM without we must stay: %u", (unsigned)F_IM(sq->dbg_st)); c_ii++;
  {
    uint8_t d[SS_EXP];
    ss_dump(d);
    d[SS_EXP - 1 - 9] = 0; d[SS_EXP - 1 - 8] = 1;          // IFF1 = 0, IFF2 = 1 through the savestate
    ss_load(d);
    CK(F_IFF1(sq->dbg_st) == 0 && F_IFF2(sq->dbg_st) == 1, "IFF preset through the savestate"); c_ii++;
    sq->iff_op = 2; tick(); sq->iff_op = 0;
    CK(F_IFF1(sq->dbg_st) == 1 && F_IFF2(sq->dbg_st) == 1, "RETN: IFF1 <- IFF2"); c_ii++;
    tick();
    CK(F_IFF1(sq->dbg_st) == 1 && F_IFF2(sq->dbg_st) == 1, "IFF without op stay"); c_ii++;
    sq->iff_op = 1; tick(); sq->iff_op = 0;
    CK(F_IFF1(sq->dbg_st) == 0 && F_IFF2(sq->dbg_st) == 0, "DI: IFF1 = IFF2 = 0"); c_ii++;
  }

  // ---- HALT and NMI: IR forced to NOP in HALT; NMI edge stored;
  //      window at the end of the instruction: space 7, IR 66, IFF1 <- 0 and IFF2 UNCHANGED ([UM] p.19, [SY] p.20-21 and p.24), leaves HALT
  long c_hn = 0;
  do_rst(MT_M1, 4);                                  // M1 4 T last: every cycle is an instruction
  sq->din = 0x5A;
  for (int h = 0; h < 8; h++) tick();
  CK(F_IR(sq->dbg_st) == 0x5A, "normal M1: IR <- din"); c_hn++;
  sq->halt_set = 1; tick(); sq->halt_set = 0;        // HALT executed
  CK(fld(sq->dbg_st, 2, 2) == 1, "halt_q <- 1"); c_hn++;
  for (int h = 0; h < 7; h++) tick();                // completes the M1 in HALT
  CK(F_IR(sq->dbg_st) == 0x00, "HALT: IR forced to NOP: %02X", (int)F_IR(sq->dbg_st)); c_hn++;
  CK(fld(sq->dbg_st, 2, 2) == 1, "HALT stays without interrupts"); c_hn++;
  {                                                   // IFF1 = 1, IFF2 = 0 through the savestate
    uint8_t d[SS_EXP];
    ss_dump(d);
    d[SS_EXP - 1 - 9] = 1; d[SS_EXP - 1 - 8] = 0;
    ss_load(d);
  }
  sq->nmi_n = 0; tick(); tick(); sq->nmi_n = 1;      // low level for 2 ticks: ONE edge
  CK(fld(sq->dbg_st, 4, 4) == 1, "nmi_pend from the edge"); c_hn++;
  { int steps = 0; while (fld(sq->dbg_st, 44, 42) != 7 && steps < 20) { tick(); steps++; } }
  CK(fld(sq->dbg_st, 44, 42) == 7 && F_IR(sq->dbg_st) == 0x66, "NMI accepted at the end of the instruction: space %u IR %02X",
     (unsigned)fld(sq->dbg_st, 44, 42), (int)F_IR(sq->dbg_st)); c_hn++;
  CK(F_IFF1(sq->dbg_st) == 0 && F_IFF2(sq->dbg_st) == 0, "NMI: IFF1 <- 0, IFF2 unchanged (it was 0)"); c_hn++;
  CK(fld(sq->dbg_st, 4, 4) == 0 && fld(sq->dbg_st, 2, 2) == 0, "NMI: pend cleared, leaves HALT"); c_hn++;
  sq->din = 0x00;
  do_rst(MT_NONE, 3);

  // ---- INT: window with IM 1 and IFF1 = 1 -> space 7, IR 38, IFF cleared, int_lat;
  //      the response does NOT reload IR from the bus; at the end of the response BASE and int_lat cleared
  long c_in = 0;
  do_rst(MT_M1, 4);
  sq->im_we = 1; sq->im_val = 1; tick(); sq->im_we = 0;
  {
    uint8_t d[SS_EXP];
    ss_dump(d);
    d[SS_EXP - 1 - 9] = 1; d[SS_EXP - 1 - 8] = 1;          // IFF1 = IFF2 = 1
    ss_load(d);
  }
  sq->int_n = 0;
  { int steps = 0; while (fld(sq->dbg_st, 44, 42) != 7 && steps < 20) { tick(); steps++; } }
  CK(fld(sq->dbg_st, 44, 42) == 7 && F_IR(sq->dbg_st) == 0x38, "INT IM1 accepted: space %u IR %02X",
     (unsigned)fld(sq->dbg_st, 44, 42), (int)F_IR(sq->dbg_st)); c_in++;
  CK(F_IFF1(sq->dbg_st) == 0 && F_IFF2(sq->dbg_st) == 0, "INT: IFF1 = IFF2 = 0"); c_in++;
  CK(fld(sq->dbg_st, 3, 3) == 1, "INT: int_lat set"); c_in++;
  sq->int_n = 1; sq->din = 0x5A;
  for (int h = 0; h < 8; h++) tick();                // one M1 4 T last in space 7: IR is not reloaded
  CK(F_IR(sq->dbg_st) == 0x38 && fld(sq->dbg_st, 44, 42) == 0 && fld(sq->dbg_st, 3, 3) == 0,
     "response: IR unchanged (%02X), end -> BASE (%u), int_lat cleared (%u)", (int)F_IR(sq->dbg_st),
     (unsigned)fld(sq->dbg_st, 44, 42), (unsigned)fld(sq->dbg_st, 3, 3)); c_in++;
  sq->din = 0x00;
  do_rst(MT_NONE, 3);

  // ---- sampling instant of INT and NMI: the rising edge that opens the last T of the instruction ([UM] p.12
  //      "the rising edge of the final clock", fig. 9: INT low through the edge that opens the "Last T State";
  //      p.13: NMI "sampled at the same time"). Instruction = one M1 of 4 T, the last; tick k = edge taken from
  //      state k (0 T1A ... 5 T3B = edge that opens T4, 6 T4A, 7 T4B = edge at the end of the instruction).
  long c_smp = 0;
  {
    auto set_irq_state = [](int im, int iff) {
      uint8_t d[SS_EXP]; ss_dump(d);
      d[SS_EXP - 1 - 9] = (uint8_t)iff; d[SS_EXP - 1 - 8] = (uint8_t)iff;
      d[SS_EXP - 1 - 7] = (uint8_t)((im >> 1) & 1); d[SS_EXP - 1 - 6] = (uint8_t)(im & 1);
      ss_load(d);
    };
    // runs one instruction (8 ticks); lv[k] = level of the pin (int_n or nmi_n) at tick k
    auto run_instr = [](CData* pin, const int* lv) {
      for (int k = 0; k < 8; k++) { sq->din = 0x00; *pin = (CData)lv[k]; sq->eval(); tick(); }
    };
    auto space = []() { return (unsigned)fld(sq->dbg_st, 44, 42); };
    static const int HI[8]        = {1, 1, 1, 1, 1, 1, 1, 1};
    static const int LO[8]        = {0, 0, 0, 0, 0, 0, 0, 0};
    static const int LO_AFTER[8]  = {1, 1, 1, 1, 1, 1, 0, 0};   // low only after the edge that opens T4
    static const int LO_UNTIL[8]  = {0, 0, 0, 0, 0, 0, 1, 1};   // low up to and including the edge that opens T4
    static const int LO_AT[8]     = {1, 1, 1, 1, 1, 0, 0, 0};   // NMI edge exactly at the edge that opens T4

    // INT low only after the sampling: not accepted here, accepted at the end of the next instruction
    do_rst(MT_M1, 4); set_irq_state(1, 1);
    run_instr(&sq->int_n, LO_AFTER);
    CK(space() == 0, "INT low only after the edge that opens the last T: accepted (space %u)", space()); c_smp++;
    run_instr(&sq->int_n, LO);
    CK(space() == 7 && F_IR(sq->dbg_st) == 0x38, "INT held low: accepted at the end of the next instruction (space %u)", space()); c_smp++;
    sq->int_n = 1;
    // INT removed after the sampling: accepted
    do_rst(MT_M1, 4); set_irq_state(1, 1);
    run_instr(&sq->int_n, LO_UNTIL);
    CK(space() == 7 && F_IR(sq->dbg_st) == 0x38, "INT low up to the edge that opens the last T: not accepted (space %u)", space()); c_smp++;
    sq->int_n = 1;
    // NMI edge after the sampling: pending, accepted at the end of the next instruction
    do_rst(MT_M1, 4); set_irq_state(0, 0);
    run_instr(&sq->nmi_n, LO_AFTER);
    CK(space() == 0 && fld(sq->dbg_st, 4, 4) == 1, "NMI edge after the edge that opens the last T: space %u, pend %u (expected 0, 1)",
       space(), (unsigned)fld(sq->dbg_st, 4, 4)); c_smp++;
    run_instr(&sq->nmi_n, HI);
    CK(space() == 7 && F_IR(sq->dbg_st) == 0x66, "pending NMI: accepted at the end of the next instruction (space %u)", space()); c_smp++;
    // NMI edge on the edge that opens the last T: sampled with it, accepted here
    do_rst(MT_M1, 4); set_irq_state(0, 0);
    run_instr(&sq->nmi_n, LO_AT);
    CK(space() == 7 && F_IR(sq->dbg_st) == 0x66, "NMI edge at the edge that opens the last T: not accepted (space %u)", space()); c_smp++;
    sq->nmi_n = 1;
    do_rst(MT_NONE, 3);
  }

  // ---- space: sp_set at the end of a non-last M-cycle, BASE at the end of the instruction
  long c_sp = 0;
  do_rst(MT_M1, 4);
  set_sched(MT_M1, 4, 0); sq->sp_set = 1; sq->sp_val = 1;      // "CB prefix": M1 not the last
  run_cycle(MT_M1, 4, 0, 0xCB, 0x0100, 0, 0, 0);
  CK(fld(sq->dbg_st, 44, 42) == 1, "space <- 1 at the end of the M1 of the prefix: %u", (unsigned)fld(sq->dbg_st, 44, 42)); c_sp++;
  sq->sp_set = 0; set_sched(MT_M1, 4, 0);
  run_cycle(MT_M1, 4, 0, 0x06, 0x0101, 0, 0, 0);
  CK(fld(sq->dbg_st, 44, 42) == 1, "space stays 1 without sp_set: %u", (unsigned)fld(sq->dbg_st, 44, 42)); c_sp++;
  set_sched(MT_RD, 3, 1);
  run_cycle(MT_RD, 3, 0, 0x00, 0x4000, 0, 0, 0);
  CK(fld(sq->dbg_st, 44, 42) == 0, "space <- BASE at the end of the instruction: %u", (unsigned)fld(sq->dbg_st, 44, 42)); c_sp++;
  set_sched(MT_M1, 4, 1); sq->sp_set = 1; sq->sp_val = 3;      // sp_set with sch_last: BASE wins
  run_cycle(MT_M1, 4, 0, 0x00, 0x0000, 0, 0, 0); sq->sp_set = 0;
  CK(fld(sq->dbg_st, 44, 42) == 0, "sch_last wins over sp_set: %u", (unsigned)fld(sq->dbg_st, 44, 42)); c_sp++;
  set_sched(MT_M1, 4, 0); sq->sp_set = 1; sq->sp_val = 4;   // prefix after prefix: the count of the M-cycles restarts
  run_cycle(MT_M1, 4, 0, 0x00, 0x0000, 0, 0, 0); sq->sp_set = 0;
  CK(fld(sq->dbg_st, 44, 42) == 4 && F_M(sq->dbg_st) == 1, "prefix chain: space %u, m_idx %u (expected 4, 1)",
     (unsigned)fld(sq->dbg_st, 44, 42), (unsigned)F_M(sq->dbg_st)); c_sp++;
  set_sched(MT_M1, 4, 1); run_cycle(MT_M1, 4, 0, 0x00, 0x0000, 0, 0, 0);   // closes the instruction: BASE
  set_sched(MT_NONE, 3, 1);

  // ---- parallel savestate: 64 bits = $bits (docs/06 §5); ss_dout == dbg_st at every instant ----
  int len = SS_EXP;                                  // width of the port: fixed by the type
  { uint8_t z[SS_EXP]; std::memset(z, 0, sizeof z); ss_load(z); }
  CK(sq->ss_dout == 0 && sq->dbg_st == 0, "state not cleared by the load");

  // ---- walking one over all 64 positions + random patterns: load -> identical read back,
  //      and with cen = 1 without ss_load the load is not repeated (ss_din stays on the bus)
  for (int k = 0; k < SS_EXP; k++) {
    uint8_t pat[SS_EXP], back[SS_EXP];
    for (int j = 0; j < SS_EXP; j++) pat[j] = (j == k);
    ss_load(pat);
    ss_dump(back);
    CK(!std::memcmp(pat, back, SS_EXP), "walking one pos %d: read back differs", k);
    CK(sq->ss_dout == sq->dbg_st, "walking one pos %d: ss_dout != dbg_st", k);
  }
  uint32_t rng = 0x5EEDC0DEu;
  for (int rturn = 0; rturn < 10; rturn++) {
    uint8_t pat[SS_EXP], back[SS_EXP];
    for (int j = 0; j < SS_EXP; j++) {
      rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
      pat[j] = (uint8_t)(rng & 1);
    }
    ss_load(pat);
    ss_dump(back);
    CK(!std::memcmp(pat, back, SS_EXP), "pattern %d: read back differs", rturn);
  }

  // ---- dump == dbg_st; restore; RESUME in the middle of a cycle -------------------
  do_rst(MT_M1, 4);
  // advances up to and including (T2,B): 4 half ticks of the M1 cycle
  for (int h = 0; h < 4; h++) {
    sq->din = 0x5A; sq->addr_val = 0xBEEF; sq->i_val = 0x12; sq->r_val = 0x34;
    sq->wait_n = 1; sq->eval(); tick();
  }
  uint64_t snap = sq->dbg_st;
  uint8_t dump[SS_EXP];
  ss_dump(dump);
  for (int j = 0; j < SS_EXP; j++)
    CK(dump[j] == (uint8_t)((snap >> (SS_EXP - 1 - j)) & 1),
       "dump bit %d != dbg_st", j);
  { uint8_t z[SS_EXP]; std::memset(z, 0, sizeof z); ss_load(z); }   // state destroyed on purpose ...
  CK(sq->dbg_st == 0, "state not cleared by the load of zero");
  ss_load(dump);                                                     // ... and restored from the dump
  CK(sq->dbg_st == snap, "restore: dbg_st does not match");
  // resume: completes the M1 cycle (from T3A) checking the remaining half T-states
  {
    int t = 3, phase = 0;
    for (int h = 0; h < 4; h++) {   // T3A..T4B
      sq->din = 0x5A; sq->addr_val = 0xBEEF; sq->i_val = 0x12; sq->r_val = 0x34;
      sq->wait_n = 1; sq->eval();
      Wave w = wave(MT_M1, 3, t, phase);
      CK(sq->rd_n == w.rd && sq->rfsh_n == w.rfsh && sq->mreq_n == w.mreq,
         "resume M1 t%d%c (rd %d rfsh %d mreq %d)", t, phase ? 'B' : 'A', sq->rd_n, sq->rfsh_n, sq->mreq_n);
      CK((sq->addr & 0x7F) == 0x34 && (sq->addr >> 8) == 0x12, "resume refresh addr");
      tick();
      if (!phase) phase = 1; else { phase = 0; t++; }
    }
    CK(F_IR(sq->dbg_st) == 0x5A, "resume: IR from the resumed fetch");
  }

  // ---- rst after an all-ones state: [04] §7 and the defined reset values -------
  { uint8_t ones[SS_EXP]; std::memset(ones, 1, sizeof ones); ss_load(ones); }
  do_rst(MT_NONE, 3);
  CK(F_IFF1(sq->dbg_st) == 0 && F_IFF2(sq->dbg_st) == 0 && F_IM(sq->dbg_st) == 0,
     "rst: IFF/IM at 0 ([04] §7)");
  CK(F_IR(sq->dbg_st) == 0x00 && F_DIN(sq->dbg_st) == 0x00,
     "rst: IR = NOP, DIN = 0");
  CK(fld(sq->dbg_st, 33, 18) == 0x0000 && fld(sq->dbg_st, 17, 10) == 0x00,
     "rst: WZ = 0, Q and renaming at 0");
  CK(fld(sq->dbg_st, 55, 53) == 1, "rst: mtype = M1 constant: %u", (unsigned)fld(sq->dbg_st, 55, 53));

  // ---- report --------------------------------------------------------------
  std::printf("parallel savestate: %d bits (expected %d = $bits of the microstate), walking one and 10 patterns\n",
              len, SS_EXP);
  long tot = 0;
  for (int s = 0; s < 7; s++) tot += tick_cnt[s];
  std::printf("half ticks checked per type:\n");
  for (int s = 0; s < 7; s++)
    std::printf("  %-5s: %ld\n", TNAME[s], tick_cnt[s]);
  std::printf("  total: %ld\n", tot);
  std::printf("M1 refresh ([02] §3.1): MREQ active T3B-T4A and inactive T3A/T4B checked; "
              "low byte stable over the 4 half T-states (R emulated, advances at the tick after r_inc); "
              "r_inc only in T4B (IACK: T6B) — declared choice ([02] §3.2)\n");
  std::printf("WZ: %ld checks of the write ports (low/high byte/whole word, "
              "without we, exhaustive per byte, reset value)\n", c_wz);
  std::printf("space: %ld checks (set at the end of the M1 of a prefix, hold, BASE at the end of the instruction, sch_last wins)\n", c_sp);
  std::printf("IM/IFF from the cw: %ld checks (IM 0..3 and hold; RETN copies IFF2 into IFF1; DI clears)\n", c_ii);
  std::printf("HALT/NMI: %ld checks (IR forced to NOP; edge stored; window: space 7, IR 66, IFF)\n", c_hn);
  std::printf("INT: %ld checks (IM1 window: space 7, IR 38, IFF cleared, int_lat; IR unchanged in the response)\n", c_in);
  std::printf("sampling of INT/NMI at the edge that opens the last T ([UM] p.12-13): %ld checks\n", c_smp);
  std::printf("WAIT at the falling edge ([UM] p.7): every type x 0..8 waits, held for whole T-states, only in phase A, only in phase B\n");
  std::printf("microstate fields NOT driven by these tests (Q: F written, DE<->HL renaming, "
              "BUSREQ sampling; BUSACK): covered "
              "only as storage/position through the savestate, DECLARED\n");
  std::printf("errors: %ld -> %s\n", n_err, n_err ? "FAIL" : "PASS");
  delete sq;
  return n_err ? 1 : 0;
}
