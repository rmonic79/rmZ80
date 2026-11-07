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
// tb_rmZ80_sst — harness for the SINGLE-STEP tests SingleStepTests/z80.
// Black-box data (JSON, outside version control: reference/tests/SingleStepTests-z80/v1, downloaded
// by sim/get_sst.sh). For each case:
//   1. initial state loaded through the SAVESTATE (274 bits: seq 64 + regfile 210) + the RAM of the case;
//   2. execution up to the M1 of the next instruction (edge of M1 with address = expected final PC);
//   3. comparison: registers of the active and alternate bank, IX IY SP PC I R, WZ, IFF1/2, IM, Q (bit "F
//      written" against q != 0, and F == q), final RAM, port writes; T-states counted against the cycles (a difference
//      fails the case, like any other).
// Output: per file passed/1000 and the first failing cases; list of the opcodes with failures.
// Options: --dir D [--file name.json] [--max N cases per file] [--log F]
// (No std::string: the g++ used with Verilator here does not link the move constructor.)
// =============================================================================
#include "Vcpu.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <dirent.h>

double sc_time_stamp() { return 0; }

// ---- minimal JSON (only what this format needs) ------------------------------
struct J;
struct KV { char k[24]; J* v; };
struct J {
  enum T { NUL, NUM, STR, ARR, OBJ } t = NUL;
  double num = 0; char str[40] = {0}; std::vector<J*> arr; std::vector<KV> obj;
  const J* get(const char* k) const { for (auto& p : obj) if (!std::strcmp(p.k, k)) return p.v; return nullptr; }
  long   i(const char* k, long def = 0) const { const J* v = get(k); return (v && v->t == NUM) ? (long)v->num : def; }
};
static std::vector<J*> pool;
static J* nj() { J* j = new J; pool.push_back(j); return j; }
static void free_pool() { for (J* j : pool) delete j; pool.clear(); }
struct P {
  const char* s; size_t n, p = 0;
  void ws() { while (p < n && (s[p] == ' ' || s[p] == '\n' || s[p] == '\r' || s[p] == '\t')) p++; }
  J* val() {
    ws(); J* j = nj();
    if (p >= n) return j;
    char c = s[p];
    if (c == '{') { j->t = J::OBJ; p++; ws(); if (s[p] == '}') { p++; return j; }
      while (true) { ws(); J* k = val(); ws(); p++; J* v = val(); KV kv; std::strncpy(kv.k, k->str, 23); kv.k[23] = 0; kv.v = v; j->obj.push_back(kv); ws(); if (s[p] == ',') { p++; continue; } p++; break; } return j; }
    if (c == '[') { j->t = J::ARR; p++; ws(); if (s[p] == ']') { p++; return j; }
      while (true) { j->arr.push_back(val()); ws(); if (s[p] == ',') { p++; continue; } p++; break; } return j; }
    if (c == '"') { j->t = J::STR; p++; int q = 0; while (p < n && s[p] != '"') { if (s[p] == '\\') p++; if (q < 39) j->str[q++] = s[p]; p++; } p++; return j; }
    if (c == 'n') { p += 4; return j; }
    if (c == 't') { p += 4; j->t = J::NUM; j->num = 1; return j; }
    if (c == 'f') { p += 5; j->t = J::NUM; j->num = 0; return j; }
    j->t = J::NUM; char* e; j->num = std::strtod(s + p, &e); p = (size_t)(e - s); return j;
  }
};

// ---- CPU ----------------------------------------------------------------------
static Vcpu*   cpu;
static uint8_t mem[65536];
static const int N_CEN = 2, SS = 274, RF = 64;
static void tick() { cpu->clk = 0; cpu->eval(); cpu->clk = 1; cpu->eval(); }
static void cen_tick() { cpu->cen = 1; tick(); cpu->cen = 0; for (int i = 1; i < N_CEN; i++) tick(); }
// parallel savestate: MSB-first view d[i] = ss_dout[273-i] (seq [273:210], regfile [209:0])
static void ss_dump(uint8_t* d) { for (int i = 0; i < SS; i++) { int k = SS - 1 - i; d[i] = (uint8_t)((cpu->ss_dout[k >> 5] >> (k & 31)) & 1u); } }
static void ss_load(const uint8_t* d) {
  for (int w = 0; w < 9; w++) cpu->ss_din[w] = 0;
  for (int i = 0; i < SS; i++) if (d[i]) { int k = SS - 1 - i; cpu->ss_din[k >> 5] |= 1u << (k & 31); }
  cpu->cen = 0; cpu->ss_load = 1; tick(); cpu->ss_load = 0;
}
static void putf(uint8_t* d, int off, int n, uint32_t v) { for (int i = 0; i < n; i++) d[off + i] = (uint8_t)((v >> (n - 1 - i)) & 1); }
static uint32_t fld(const uint8_t* d, int off, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) v = (v << 1) | d[off + i]; return v; }
static void put_seq(uint8_t* d, int hi, int lo, uint32_t v) { putf(d, 63 - hi, hi - lo + 1, v); }   // bit k of the state = d[63-k]
static uint32_t get_seq(const uint8_t* d, int hi, int lo) { return fld(d, 63 - hi, hi - lo + 1); }

static std::vector<std::pair<int,int>> port_rd, port_wr_seen;
static void set_din() {
  if (!cpu->iorq_n && cpu->m1_n) { uint8_t v = 0xFF; for (auto& pr : port_rd) if (pr.first == (int)cpu->addr) v = (uint8_t)pr.second; cpu->data_in = v; }
  else cpu->data_in = mem[cpu->addr];
}

struct Stat { long cases = 0, pass = 0, fail = 0, tdiff = 0; };
static char err[1024];
static void app(const char* fmt, long a, long b) { char b2[96]; std::snprintf(b2, sizeof b2, fmt, a, b); if (std::strlen(err) + std::strlen(b2) < sizeof err - 1) std::strcat(err, b2); }

static int run_case(const J& c, FILE* logf, const char* fname, int idx, Stat& st, int nerr_shown) {
  const J* I = c.get("initial"); const J* F = c.get("final"); const J* cyc = c.get("cycles"); const J* ports = c.get("ports");
  std::memset(mem, 0, sizeof mem);
  if (const J* r = I->get("ram")) for (J* e : r->arr) mem[(int)e->arr[0]->num] = (uint8_t)e->arr[1]->num;
  port_rd.clear(); port_wr_seen.clear();
  if (ports) for (J* e : ports->arr) if (e->arr[2]->str[0] == 'r') port_rd.push_back({(int)e->arr[0]->num, (int)e->arr[1]->num});
  uint8_t d[SS]; std::memset(d, 0, sizeof d);
  uint32_t af_ = (uint32_t)I->i("af_"), bc_ = (uint32_t)I->i("bc_"), de_ = (uint32_t)I->i("de_"), hl_ = (uint32_t)I->i("hl_");
  putf(d, RF + 0, 8, af_ >> 8); putf(d, RF + 8, 8, (uint32_t)I->i("a")); putf(d, RF + 16, 8, af_ & 0xFF); putf(d, RF + 24, 8, (uint32_t)I->i("f"));
  putf(d, RF + 32, 8, bc_ >> 8); putf(d, RF + 40, 8, (uint32_t)I->i("b")); putf(d, RF + 48, 8, bc_ & 0xFF); putf(d, RF + 56, 8, (uint32_t)I->i("c"));
  putf(d, RF + 64, 8, de_ >> 8); putf(d, RF + 72, 8, (uint32_t)I->i("d")); putf(d, RF + 80, 8, de_ & 0xFF); putf(d, RF + 88, 8, (uint32_t)I->i("e"));
  putf(d, RF + 96, 8, hl_ >> 8); putf(d, RF + 104, 8, (uint32_t)I->i("h")); putf(d, RF + 112, 8, hl_ & 0xFF); putf(d, RF + 120, 8, (uint32_t)I->i("l"));
  putf(d, RF + 128, 16, (uint32_t)I->i("ix")); putf(d, RF + 144, 16, (uint32_t)I->i("iy")); putf(d, RF + 160, 16, (uint32_t)I->i("sp")); putf(d, RF + 176, 16, (uint32_t)I->i("pc"));
  putf(d, RF + 192, 8, (uint32_t)I->i("i")); putf(d, RF + 200, 8, (uint32_t)I->i("r")); putf(d, RF + 208, 1, 0); putf(d, RF + 209, 1, 0);
  put_seq(d, 63, 63, 0); put_seq(d, 62, 60, 1); put_seq(d, 59, 59, 0); put_seq(d, 58, 56, 1); put_seq(d, 55, 53, 1);   // T1A of M1
  put_seq(d, 52, 45, mem[(int)I->i("pc")]); put_seq(d, 44, 42, 0); put_seq(d, 41, 34, 0);
  put_seq(d, 33, 18, (uint32_t)I->i("wz"));
  put_seq(d, 17, 10, (uint32_t)((I->i("q") != 0) ? 2 : 0));                    // Q: bit1 (pending) -> at the first tick it becomes bit0 "the previous one wrote F"
  put_seq(d, 9, 9, (uint32_t)I->i("iff1")); put_seq(d, 8, 8, (uint32_t)I->i("iff2")); put_seq(d, 7, 6, (uint32_t)I->i("im"));
  put_seq(d, 5, 0, 0);   // NMI sampled, NMI pending, INT, HALT, NMI level, BUSACK: at rest ("ei" of the vectors has no bit: the EI window follows from the instruction)
  ss_load(d);
  cpu->cen = 0; tick(); tick();
  // number of M1 cycles of the instruction from the name ("DD 28 0000" -> 2; "DD CB __ 46 0000" -> 2: the 4th byte is an RD; "00 0000" -> 1)
  int n_m1 = 1;
  if (const J* nm = c.get("name")) { int tok = 0; const char* s = nm->str; while (*s) { while (*s == ' ') s++; if (!*s) break; tok++; while (*s && *s != ' ') s++; }
    int k = tok - 1; n_m1 = (k == 4) ? 2 : k; if (n_m1 < 1) n_m1 = 1; }
  long half = 0; bool prev_iow = false, prev_m1 = false; int pc_exp = (int)F->i("pc"), m1_seen = 0; bool pc_bad = false;
  for (;;) {
    set_din(); cpu->eval();
    if (!cpu->wr_n && !cpu->mreq_n) mem[cpu->addr] = cpu->data_out;
    bool iow = (!cpu->wr_n && !cpu->iorq_n);
    if (iow && !prev_iow) port_wr_seen.push_back({(int)cpu->addr, (int)cpu->data_out});
    prev_iow = iow;
    // M1 counted on the fetch (M1 + MREQ + RD active, T1B): the instruction has n_m1 M1 fetches (prefixes included);
    // the next one is the following instruction: the run stops there (boundary T1A already executed: bit 0 of Q says
    // "wrote F") and the address must be the expected final PC
    bool m1f = (!cpu->m1_n && !cpu->mreq_n && !cpu->rd_n);
    if (m1f && !prev_m1) { m1_seen++; if (m1_seen > n_m1) { if ((int)cpu->addr != pc_exp) pc_bad = true; break; } }
    prev_m1 = m1f;
    cen_tick(); half++;
    if (half > 600) break;
  }
  uint8_t o[SS]; ss_dump(o);                                               // parallel read of the final state
  int sa = (int)fld(o, RF + 208, 1), sg = (int)fld(o, RF + 209, 1);
  // PHYSICAL banks: index [0] = active, [1] = shadow; sel_af/sel_gp = logical identity (needed by the renaming)
  auto R8 = [&](int base, int bank) { return fld(o, RF + base + (bank ? 0 : 8), 8); };
  uint32_t a = R8(0, 0), f = R8(16, 0), b = R8(32, 0), cc = R8(48, 0), dd = R8(64, 0), e = R8(80, 0), h = R8(96, 0), l = R8(112, 0);
  uint32_t a_ = R8(0, 1), f_ = R8(16, 1), b_ = R8(32, 1), c_ = R8(48, 1), d_ = R8(64, 1), e_ = R8(80, 1), h_ = R8(96, 1), l_ = R8(112, 1);
  (void)sa;
  // DE<->HL renaming (EX DE,HL): bit 2+bank of the Q field -> the LOGICAL values are swapped with respect to the physical ones
  if (get_seq(o, 12 + sg, 12 + sg))   { std::swap(dd, h); std::swap(e, l); }
  if (get_seq(o, 12 + !sg, 12 + !sg)) { std::swap(d_, h_); std::swap(e_, l_); }
  uint32_t ix = fld(o, RF + 128, 16), iy = fld(o, RF + 144, 16), sp = fld(o, RF + 160, 16), pc = fld(o, RF + 176, 16), ii = fld(o, RF + 192, 8), rr = fld(o, RF + 200, 8);
  uint32_t wz = get_seq(o, 33, 18), q = get_seq(o, 17, 10), iff1 = get_seq(o, 9, 9), iff2 = get_seq(o, 8, 8), im = get_seq(o, 7, 6);
  err[0] = 0;
  auto ck = [&](const char* n, long got, long exp) { if (got != exp) { char fmt[48]; std::snprintf(fmt, sizeof fmt, " %s=%%lX/%%lX", n); app(fmt, got, exp); } };
  if (pc_bad) app(" next-fetch=%lX/%lX", (long)cpu->addr, (long)pc_exp);
  if (half > 600) app(" timeout(%ld/%ld)", half, 600L);
  ck("A", a, F->i("a")); ck("F", f, F->i("f")); ck("B", b, F->i("b")); ck("C", cc, F->i("c")); ck("D", dd, F->i("d")); ck("E", e, F->i("e")); ck("H", h, F->i("h")); ck("L", l, F->i("l"));
  ck("AF'", (a_ << 8) | f_, F->i("af_")); ck("BC'", (b_ << 8) | c_, F->i("bc_")); ck("DE'", (d_ << 8) | e_, F->i("de_")); ck("HL'", (h_ << 8) | l_, F->i("hl_"));
  ck("IX", ix, F->i("ix")); ck("IY", iy, F->i("iy")); ck("SP", sp, F->i("sp")); ck("PC", pc, F->i("pc")); ck("I", ii, F->i("i")); ck("R", rr, F->i("r"));
  ck("WZ", wz, F->i("wz")); ck("IFF1", iff1, F->i("iff1")); ck("IFF2", iff2, F->i("iff2")); ck("IM", im, F->i("im"));
  long qexp = F->i("q");                                                       // q = F if the instruction wrote F, otherwise 0
  if (qexp != 0) { ck("Qw", q & 1, 1); ck("Qf", f, qexp); } else if (q & 1) ck("Qf0", f, 0);
  if (const J* r = F->get("ram")) for (J* e2 : r->arr) { int ad = (int)e2->arr[0]->num; if (mem[ad] != (uint8_t)e2->arr[1]->num) { char fmt[48]; std::snprintf(fmt, sizeof fmt, " mem[%04X]=%%lX/%%lX", ad); app(fmt, mem[ad], (long)e2->arr[1]->num); } }
  if (ports) for (J* e2 : ports->arr) if (e2->arr[2]->str[0] == 'w') {
    bool ok = false; for (auto& w : port_wr_seen) if (w.first == (int)e2->arr[0]->num && w.second == (int)e2->arr[1]->num) ok = true;
    if (!ok) app(" port[%lX]<-%lX missing", (long)e2->arr[0]->num, (long)e2->arr[1]->num);
  }
  long tstates = half / 2, texp = cyc ? (long)cyc->arr.size() : 0;
  if (tstates != texp) { st.tdiff++; app(" T=%ld/%ld", tstates, texp); }   // a different number of T-states fails the case
  st.cases++;
  if (!err[0]) { st.pass++; return 0; }
  st.fail++;
  if (nerr_shown < 3) {
    const J* nm = c.get("name");
    std::printf("  FAIL %s #%d (%s):%s  [T %ld/%ld]\n", fname, idx, nm ? nm->str : "", err, tstates, texp);
    if (logf) std::fprintf(logf, "  FAIL %s #%d (%s):%s  [T %ld/%ld]\n", fname, idx, nm ? nm->str : "", err, tstates, texp);
  }
  return 1;
}

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  const char* dir = nullptr; const char* one = nullptr; const char* logp = nullptr; int maxc = 1000;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
    else if (!std::strcmp(argv[i], "--file") && i + 1 < argc) one = argv[++i];
    else if (!std::strcmp(argv[i], "--max") && i + 1 < argc) maxc = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--log") && i + 1 < argc) logp = argv[++i];
  }
  if (!dir) { std::printf("usage: tb_sst --dir D [--file f.json] [--max N] [--log F]\n"); return 2; }
  FILE* logf = logp ? std::fopen(logp, "w") : nullptr;
  std::vector<char*> files;
  if (one) files.push_back(strdup(one));
  else { DIR* dp = opendir(dir); if (!dp) { std::printf("FATAL: %s\n", dir); return 2; } struct dirent* de;
    while ((de = readdir(dp))) { size_t L = std::strlen(de->d_name); if (L > 5 && !std::strcmp(de->d_name + L - 5, ".json")) files.push_back(strdup(de->d_name)); } closedir(dp);
    std::sort(files.begin(), files.end(), [](const char* x, const char* y) { return std::strcmp(x, y) < 0; }); }

  cpu = new Vcpu;
  cpu->clk = 0; cpu->cen = 0; cpu->rst = 1; cpu->wait_n = 1; cpu->busreq_n = 1; cpu->int_n = 1; cpu->nmi_n = 1; cpu->ss_load = 0; for (int w = 0; w < 9; w++) cpu->ss_din[w] = 0; cpu->data_in = 0;
  for (int i = 0; i < 4; i++) cen_tick(); cpu->rst = 0; tick(); tick();

  long tot = 0, totpass = 0, totfail = 0, files_bad = 0, tot_tdiff = 0;
  std::vector<char*> bad;
  for (char* fn : files) {
    char path[512]; std::snprintf(path, sizeof path, "%s/%s", dir, fn);
    FILE* f = std::fopen(path, "rb"); if (!f) { std::printf("missing %s\n", path); continue; }
    std::fseek(f, 0, SEEK_END); long len = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    std::vector<char> buf((size_t)len + 1); size_t rd = std::fread(buf.data(), 1, (size_t)len, f); std::fclose(f); buf[rd] = 0;
    P pr{buf.data(), rd}; J* root = pr.val();
    Stat st; int shown = 0;
    for (size_t k = 0; k < root->arr.size() && (int)k < maxc; k++) shown += run_case(*root->arr[k], logf, fn, (int)k, st, shown);
    if (st.tdiff) std::printf("%-20s %4ld/%4ld%s  T different in %ld\n", fn, st.pass, st.cases, st.fail ? "  FAIL" : "", st.tdiff);
    else          std::printf("%-20s %4ld/%4ld%s\n", fn, st.pass, st.cases, st.fail ? "  FAIL" : "");
    if (logf) std::fprintf(logf, "%-20s %4ld/%4ld fail %ld tdiff %ld\n", fn, st.pass, st.cases, st.fail, st.tdiff);
    tot += st.cases; totpass += st.pass; totfail += st.fail; tot_tdiff += st.tdiff; if (st.fail) { files_bad++; bad.push_back(fn); }
    std::fflush(stdout); if (logf) std::fflush(logf);
    free_pool();
  }
  std::printf("TOTAL: %ld cases, %ld passed, %ld failed; files with failures %ld/%zu; cases with different T %ld\n", tot, totpass, totfail, files_bad, files.size(), tot_tdiff);
  std::printf("opcodes with failures:"); for (char* b : bad) std::printf(" [%s]", b); std::printf("\n");
  if (logf) { std::fprintf(logf, "TOTAL: %ld cases, %ld passed, %ld failed; files with failures %ld/%zu; different T %ld\n", tot, totpass, totfail, files_bad, files.size(), tot_tdiff);
    std::fprintf(logf, "opcodes with failures:"); for (char* b : bad) std::fprintf(logf, " [%s]", b); std::fprintf(logf, "\n"); std::fclose(logf); }
  cpu->final(); delete cpu;
  return totfail ? 1 : 0;
}
