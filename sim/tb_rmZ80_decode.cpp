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
// tb_rmZ80_decode — self-checking bench of the SCHEDULE (1792 opcode positions).
// The schedule is produced by rtl/rmZ80_cw.sv (a default plus exceptions on the same
// opcode patterns that select the actions): the bench queries rmZ80_cw alone with
// m_idx = M from 1 to n_m (t_idx = 1, phase = 0, f_reg = 0). The prefix byte is the
// is_pfx bit; an unassigned DD/FD position (base instruction with the prefix M1 in
// front) is pre_m1.
// exit 0 = PASS.
//
// REFERENCE = docs/03-decoding.md, Appendix A, READ FROM THE FILE AT RUN TIME
// (no embedded copy: if it were transcribed again, bench and RTL would share the
// same misreading).
// For each of the 1792 rows the decoder is queried and compared on
// (BYTES, M, T, SPLIT) plus the alternative timing (M2, T2, SPLIT2), derived from
// the gate/ext bits of the schedule row ([06] §3.4).
//
// MASKED (declared):
//  - the `type` field of the schedule rows: the Appendix has no cycle-type field,
//    so it cannot be checked here;
//  - the SPLIT of the [SY] rows that have '-' in the appendix: the total M and T are
//    checked; the split is a declared choice of the decoder;
//  - the 180 UNASSIGNED rows: no data in the appendix; the declared CONTRACT is
//    checked ([06] §4.2): the BASE row of IR plus one M1 (4 T) in front, counted
//    separately.
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include "Vdec.h"
#include "verilated.h"

double sc_time_stamp() { return 0; }

static Vdec* dc;
static long n_err = 0, n_prt = 0;

#define CK(cond, ...) do { if (!(cond)) { n_err++; \
  if (n_prt < 30) { n_prt++; std::printf("FAIL "); std::printf(__VA_ARGS__); \
    std::printf("\n"); } } } while (0)

struct Row {                       // POD, no std::string
  char sp[6];
  int  op;
  int  byte_, m, t;
  char split[20], m2s[4], t2s[4], split2[20];
  int  na, pre, sy;
};
static Row rows[2048];
static int n_rows = 0;
static Row base_of[256];
static int base_ok[256];

static int split_list(const char* s, int* v) {
  if (!std::strcmp(s, "-")) return 0;
  int n = 0, cur = 0, any = 0;
  for (const char* p = s; *p; p++) {
    if (*p == ',') { v[n++] = cur; cur = 0; any = 0; }
    else { cur = cur * 10 + (*p - '0'); any = 1; }
  }
  if (any) v[n++] = cur;
  return n;
}

static int sp_code(const char* s) {
  if (!std::strcmp(s, "BASE")) return 0;
  if (!std::strcmp(s, "CB"))   return 1;
  if (!std::strcmp(s, "ED"))   return 2;
  if (!std::strcmp(s, "DD"))   return 3;
  if (!std::strcmp(s, "FD"))   return 4;
  if (!std::strcmp(s, "DDCB")) return 5;
  return 6;                       // FDCB
}

struct Sched {
  int is_pfx, pre_m1, n_m, n_byte;
  int dur[8], gate[8], ext[8], rpt[8];
};
static void query(int sp, int op, Sched* s) {
  dc->space = (uint8_t)sp; dc->ir = (uint8_t)op; dc->m_idx = 1;
  dc->t_idx = 1; dc->phase = 0; dc->f_reg = 0; dc->halt = 0; dc->xhl = 0; dc->eval();
  s->is_pfx = dc->is_pfx; s->pre_m1 = dc->pre_m1;
  s->n_m = dc->n_m; s->n_byte = dc->n_byte;
  for (int m = 1; m <= s->n_m && m <= 7; m++) {
    dc->m_idx = (uint8_t)m; dc->eval();
    s->dur[m - 1] = 3 + dc->row_dur;
    s->gate[m - 1] = dc->row_gate; s->ext[m - 1] = dc->row_ext;
    s->rpt[m - 1] = dc->row_rpt;
    CK(dc->row_last == (m == s->n_m), "%d;%02X last at M%d", sp, op, m);
  }
}

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  dc = new Vdec;

  // ---- reading Appendix A from the document ---------------------------------
  std::FILE* f = std::fopen("../docs/03-decoding.md", "rb");
  if (!f) { std::printf("FATAL: docs/03-decoding.md not found\n"); return 1; }
  char ln[512];
  int in_block = 0;
  std::memset(base_ok, 0, sizeof base_ok);
  while (std::fgets(ln, sizeof ln, f)) {
    size_t L = std::strlen(ln);
    while (L && (ln[L - 1] == '\n' || ln[L - 1] == '\r')) ln[--L] = 0;
    if (!in_block) { if (!std::strcmp(ln, "```text")) in_block = 1; continue; }
    if (!std::strcmp(ln, "```")) break;
    // split on ';' into 14 fields
    char* fl[14]; int nf = 0;
    char* p = ln; fl[nf++] = p;
    for (; *p && nf < 14; p++) if (*p == ';') { *p = 0; fl[nf++] = p + 1; }
    if (nf != 14) { CK(false, "malformed row (%d fields)", nf); continue; }
    Row* r = &rows[n_rows++];
    std::memset(r, 0, sizeof *r);
    std::snprintf(r->sp, sizeof r->sp, "%s", fl[0]);
    r->op = (int)std::strtol(fl[1], nullptr, 16);
    r->na = !std::strcmp(fl[2], "UNASSIGNED");
    r->pre = !std::strncmp(fl[2], "PREFIX", 6) || !std::strncmp(fl[2], "SUBPREFIX", 9);
    if (!r->na && !r->pre) {
      r->byte_ = std::atoi(fl[3]); r->m = std::atoi(fl[4]); r->t = std::atoi(fl[5]);
      std::snprintf(r->split, sizeof r->split, "%s", fl[6]);
      std::snprintf(r->m2s, sizeof r->m2s, "%s", fl[7]);
      std::snprintf(r->t2s, sizeof r->t2s, "%s", fl[8]);
      std::snprintf(r->split2, sizeof r->split2, "%s", fl[9]);
    }
    r->sy = (std::strstr(fl[12], "SY") != nullptr);
    if (!std::strcmp(fl[0], "BASE")) { base_of[r->op] = *r; base_ok[r->op] = 1; }
  }
  std::fclose(f);
  std::printf("rows read from the appendix: %d\n", n_rows);
  CK(n_rows == 1792, "1792 rows expected");

  long c_um = 0, c_um_bad = 0, c_sy = 0, c_sy_bad = 0;
  long c_na = 0, c_na_bad = 0, c_pre = 0;

  for (int i = 0; i < n_rows; i++) {
    const Row* r = &rows[i];
    int sp = sp_code(r->sp);
    Sched s;
    query(sp, r->op, &s);
    int npre = (sp == 0) ? 0 : (sp >= 5 ? 2 : 1);   // M1 of the prefixes: the cw counts the M-cycles from the M1 of the opcode

    if (r->pre) {
      CK(s.is_pfx == 1, "%s;%02X prefix: is_pfx %d", r->sp, r->op, s.is_pfx);
      // BASE: 2 M1, 1 byte. SUBPREFIX (DD CB): the instruction continues in the DDCB space
      if (sp == 0) CK(s.n_m == 2 && s.n_byte == 1, "%s;%02X prefix: M %d BYTES %d", r->sp, r->op, s.n_m, s.n_byte);
      c_pre++;
      continue;
    }

    int e_byte, e_m, e_t, e_split[8], n_esplit;
    int has_alt = 0, e_m2 = 0, e_t2 = 0, e_split2[8], n_esplit2 = 0;
    int contract = 0;
    if (r->na) {
      c_na++;
      if (!base_ok[r->op]) { CK(false, "%s;%02X without a BASE row", r->sp, r->op); continue; }
      const Row* b = &base_of[r->op];
      if (b->pre) {                 // DD;DD etc.: it stays a prefix ([SY] §3.7)
        CK(s.is_pfx == 1, "%s;%02X prefix chain: is_pfx %d", r->sp, r->op, s.is_pfx);
        continue;
      }
      contract = 1;
      e_byte = b->byte_ + 1; e_m = b->m + 1; e_t = b->t + 4;
      n_esplit = split_list(b->split, e_split + 1);
      if (n_esplit) { e_split[0] = 4; n_esplit++; }
      if (std::strcmp(b->m2s, "-")) {
        has_alt = 1;
        e_m2 = std::atoi(b->m2s) + 1; e_t2 = std::atoi(b->t2s) + 4;
        n_esplit2 = split_list(b->split2, e_split2 + 1);
        if (n_esplit2) { e_split2[0] = 4; n_esplit2++; }
      }
      CK(s.pre_m1 == 1, "%s;%02X unassigned position without pre_m1", r->sp, r->op);
    } else {
      e_byte = r->byte_; e_m = r->m; e_t = r->t;
      n_esplit = split_list(r->split, e_split);
      if (std::strcmp(r->m2s, "-")) {
        has_alt = 1;
        e_m2 = std::atoi(r->m2s); e_t2 = std::atoi(r->t2s);
        n_esplit2 = split_list(r->split2, e_split2);
      }
      CK(s.pre_m1 == 0, "%s;%02X pre_m1 on an assigned row", r->sp, r->op);
      CK(s.is_pfx == 0, "%s;%02X is_pfx on a data row", r->sp, r->op);
    }

    long bad0 = n_err;
    int tsum = 4 * npre; for (int m = 0; m < s.n_m; m++) tsum += s.dur[m];
    CK(s.n_byte == e_byte, "%s;%02X BYTES %d/%d", r->sp, r->op, s.n_byte, e_byte);
    CK(s.n_m + npre == e_m, "%s;%02X M %d/%d", r->sp, r->op, s.n_m + npre, e_m);
    CK(tsum == e_t, "%s;%02X T %d/%d", r->sp, r->op, tsum, e_t);
    if (n_esplit) {
      CK(n_esplit == s.n_m + npre, "%s;%02X split n %d/%d", r->sp, r->op, n_esplit, s.n_m + npre);
      for (int m = 0; m < s.n_m && m + npre < n_esplit; m++)
        CK(s.dur[m] == e_split[m + npre], "%s;%02X split M%d %d/%d",
           r->sp, r->op, m + npre + 1, s.dur[m], e_split[m + npre]);
    }
    // alternative timing derived from gate/ext ([06] §3.4)
    int a_m = npre, a_t = 4 * npre, a_split[8] = {4, 4}, any_cond = 0;
    for (int m = 0; m < s.n_m; m++) {
      if (s.gate[m] || s.ext[m] || s.rpt[m]) any_cond = 1;
      if (!s.gate[m]) { int d = s.dur[m] - s.ext[m]; a_split[a_m++] = d; a_t += d; }
    }
    if (has_alt) {
      CK(a_m == e_m2, "%s;%02X M2 %d/%d", r->sp, r->op, a_m, e_m2);
      CK(a_t == e_t2, "%s;%02X T2 %d/%d", r->sp, r->op, a_t, e_t2);
      if (n_esplit2) {
        CK(n_esplit2 == a_m, "%s;%02X split2 n", r->sp, r->op);
        for (int m = 0; m < a_m && m < n_esplit2; m++)
          CK(a_split[m] == e_split2[m], "%s;%02X split2 M%d %d/%d",
             r->sp, r->op, m + 1, a_split[m], e_split2[m]);
      }
    } else {
      CK(!any_cond, "%s;%02X conditional bits without an alternative in the appendix",
         r->sp, r->op);
    }

    int bad = (n_err != bad0);
    if (contract) { if (bad) c_na_bad++; }
    else if (r->sy) { c_sy++; if (bad) c_sy_bad++; }
    else { c_um++; if (bad) c_um_bad++; }
  }

  // ---- report --------------------------------------------------------------
  std::printf("[UM] rows: %ld checked, %ld different\n", c_um, c_um_bad);
  std::printf("[SY] rows: %ld checked, %ld different (split '-' masked)\n",
              c_sy, c_sy_bad);
  std::printf("UNASSIGNED (declared contract BASE row + M1): %ld, %ld different\n",
              c_na, c_na_bad);
  std::printf("prefixes (is_pfx): %ld\n", c_pre);
  std::printf("errors: %ld -> %s\n", n_err, n_err ? "FAIL" : "PASS");
  delete dc;
  return n_err ? 1 : 0;
}
