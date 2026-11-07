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
// tb_rmZ80_zex — HARNESS for external SELF-CHECKING test suites:
//   - zexdoc/zexall (CP/M .com at 0100h): minimal BDOS in Z80 code at FF00h (functions 2 and 9,
//     console output on port F1h); (0006h) = FF00h; the final "jp 0" = OUT (F0h) + JP 0100h
//     (the first OUT to F0 is the boot from reset, the second one is the exit);
//   - z80test (Spectrum, binary at 8000h): 0000h LD SP,FF00h / CALL org / OUT (F0h) / HALT;
//     RST 10h = OUT (F1h),A ; RET; 1601h (CHAN-OPEN) = RET; IN A,(FEh) = BFh.
// No oracle: the programs compute the CRC by themselves on the CPU and print the verdict.
// The console is captured from IOWR on port F1h (13 = end of line; 23 n m = TAB, skipped).
// Options: --bin F --org HHHH --mode cpm|spec  or  --tap F.tap (official z80test release, spec mode)
//          [--tests i,j,a-b] [--log F] [--maxT N] [--cen N]
// Exit code: in spec mode 0 only if the program prints "Result: all tests passed." (VERDICT line); in CP/M always 0.
// =============================================================================
#include "Vcpu.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>

double sc_time_stamp() { return 0; }
static Vcpu*   cpu;
static uint8_t mem[65536];
static int     N_CEN = 2;                       // clocks per half T (contract: >= 2)

static void tick() { cpu->clk = 0; cpu->eval(); cpu->clk = 1; cpu->eval(); }
static void cen_tick() { cpu->cen = 1; tick(); cpu->cen = 0; for (int i = 1; i < N_CEN; i++) tick(); }

enum Mode { CPM, SPEC };
static Mode        mode = CPM;
static std::string line;
static long long   half_t = 0;                 // half T-states executed
static FILE*       g_log = nullptr;
static std::chrono::steady_clock::time_point t0;
static int         skip_tab = 0;
static long        n_lines = 0;

static bool        spec_pass = false;          // z80test: line "Result: all tests passed." seen

static void flush_line() {
  double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (line.find("Result: all tests passed") != std::string::npos) spec_pass = true;
  std::printf("%s\n", line.c_str()); std::fflush(stdout);
  if (g_log) { std::fprintf(g_log, "%s   [T=%lld  %.0f s]\n", line.c_str(), half_t / 2, el); std::fflush(g_log); }
  line.clear(); n_lines++;
}
static void con_out(uint8_t c) {
  if (skip_tab) { skip_tab--; return; }
  if (c == 23) { skip_tab = 2; return; }         // Spectrum: TAB n,m
  if (c == 13) { flush_line(); return; }
  if (c == 10) return;
  if (c >= 32 && c < 127) line.push_back((char)c);
  else { char b[8]; std::snprintf(b, sizeof b, "<%02X>", c); line += b; }
}
static void set_din() {
  if (!cpu->iorq_n && cpu->m1_n) cpu->data_in = ((cpu->addr & 0xFF) == 0xFE) ? 0xBF : 0xFF;   // port: FEh = keyboard (MIC at 0)
  else                           cpu->data_in = mem[cpu->addr];
}

static std::vector<int> parse_list(const char* s) {
  std::vector<int> v; const char* p = s;
  while (*p) {
    int a = (int)std::strtol(p, (char**)&p, 10), b = a;
    if (*p == '-') { p++; b = (int)std::strtol(p, (char**)&p, 10); }
    for (int i = a; i <= b; i++) v.push_back(i);
    if (*p == ',') p++;
  }
  return v;
}

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  const char* bin = nullptr; const char* tap = nullptr; const char* logp = nullptr; const char* tests = nullptr;
  int org = 0x100; long long maxT = 0;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--bin") && i + 1 < argc) bin = argv[++i];
    else if (!std::strcmp(argv[i], "--tap") && i + 1 < argc) { tap = argv[++i]; mode = SPEC; }
    else if (!std::strcmp(argv[i], "--org") && i + 1 < argc) org = (int)std::strtol(argv[++i], nullptr, 16);
    else if (!std::strcmp(argv[i], "--mode") && i + 1 < argc) mode = std::strcmp(argv[++i], "spec") ? CPM : SPEC;
    else if (!std::strcmp(argv[i], "--tests") && i + 1 < argc) tests = argv[++i];
    else if (!std::strcmp(argv[i], "--log") && i + 1 < argc) logp = argv[++i];
    else if (!std::strcmp(argv[i], "--maxT") && i + 1 < argc) maxT = std::atoll(argv[++i]);
    else if (!std::strcmp(argv[i], "--cen") && i + 1 < argc) N_CEN = std::atoi(argv[++i]);
  }
  if (!bin && !tap) { std::printf("usage: tb_zex --bin F --org HHHH --mode cpm|spec | --tap F.tap  [--tests list] [--log F] [--maxT N] [--cen N]\n"); return 2; }
  if (N_CEN < 2) N_CEN = 2;
  if (logp) g_log = std::fopen(logp, "w");

  std::memset(mem, 0, sizeof mem);
  size_t len = 0;
  if (tap) {
    // ZX Spectrum .tap (the format of the z80test releases): blocks {16-bit length, flag, data, xor}.
    // The data block that follows a header of type 3 (code) is loaded at the address of the header.
    std::FILE* f = std::fopen(tap, "rb");
    if (!f) { std::printf("FATAL: %s not found\n", tap); return 2; }
    std::vector<uint8_t> t(70000); size_t n = std::fread(t.data(), 1, t.size(), f); std::fclose(f); t.resize(n);
    int code_org = -1; bool loaded = false;
    for (size_t i = 0; i + 2 <= t.size() && !loaded; ) {
      size_t bl = (size_t)t[i] | ((size_t)t[i + 1] << 8); i += 2;
      if (bl < 2 || i + bl > t.size()) { std::printf("FATAL: %s: truncated block\n", tap); return 2; }
      uint8_t x = 0; for (size_t k = 0; k < bl; k++) x ^= t[i + k];
      if (x) { std::printf("FATAL: %s: wrong block xor\n", tap); return 2; }
      if (t[i] == 0x00 && bl == 19 && t[i + 1] == 3) code_org = t[i + 14] | (t[i + 15] << 8);          // CODE header
      else if (t[i] == 0xFF && code_org >= 0) {
        len = bl - 2; if ((size_t)code_org + len > sizeof mem) { std::printf("FATAL: code beyond 64 KB\n"); return 2; }
        std::memcpy(mem + code_org, &t[i + 1], len); org = code_org; loaded = true;
      }
      i += bl;
    }
    if (!loaded) { std::printf("FATAL: %s: no code block\n", tap); return 2; }
    bin = tap;
  } else {
    std::FILE* f = std::fopen(bin, "rb");
    if (!f) { std::printf("FATAL: %s not found\n", bin); return 2; }
    len = std::fread(mem + org, 1, sizeof(mem) - (size_t)org, f); std::fclose(f);
  }
  std::printf("harness: %s, %zu bytes at %04X, mode %s, cen every %d clocks\n", bin, len, org, mode == CPM ? "CP/M" : "Spectrum", N_CEN);

  if (mode == CPM) {
    static const uint8_t boot[5] = { 0xD3, 0xF0, 0xC3, 0x00, 0x01 };        // OUT (F0),A ; JP 0100h
    static const uint8_t bdosjp[3] = { 0xC3, 0x00, 0xFF };                    // 0005: JP FF00h  -> (0006) = FF00h
    static const uint8_t bdos[] = {                                           // FF00: minimal BDOS (functions 2 and 9)
      0x79,             // LD A,C
      0xFE, 0x02,       // CP 2
      0x20, 0x04,       // JR NZ,+4
      0x7B,             // LD A,E
      0xD3, 0xF1,       // OUT (F1),A
      0xC9,             // RET
      0xFE, 0x09,       // CP 9
      0xC0,             // RET NZ
      0x1A,             // LD A,(DE)
      0xFE, 0x24,       // CP '$'
      0xC8,             // RET Z
      0xD3, 0xF1,       // OUT (F1),A
      0x13,             // INC DE
      0x18, 0xF7 };     // JR -9
    std::memcpy(mem + 0x0000, boot, sizeof boot);
    std::memcpy(mem + 0x0005, bdosjp, sizeof bdosjp);
    std::memcpy(mem + 0xFF00, bdos, sizeof bdos);
    // table of the tests: after "LD C,9 ; CALL bdos ; LD HL,tests" in the program
    int t = -1;
    for (int i = org; i < org + (int)len - 8; i++)
      if (mem[i] == 0x0E && mem[i + 1] == 0x09 && mem[i + 2] == 0xCD && mem[i + 5] == 0x21) { t = mem[i + 6] | (mem[i + 7] << 8); break; }
    if (t < 0) { std::printf("FATAL: table of the tests not found\n"); return 2; }
    std::vector<int> all;
    for (int i = 0; i < 128; i++) { int p = mem[t + 2 * i] | (mem[t + 2 * i + 1] << 8); if (!p) break; all.push_back(p); }
    std::vector<int> sel;
    if (tests) { for (int i : parse_list(tests)) if (i >= 0 && i < (int)all.size()) sel.push_back(i); }
    else for (int i = 0; i < (int)all.size(); i++) sel.push_back(i);
    std::printf("table at %04X: %zu tests, %zu selected\n", t, all.size(), sel.size());
    for (size_t k = 0; k < sel.size(); k++) {
      int p = all[(size_t)sel[k]];
      mem[t + 2 * k] = (uint8_t)(p & 0xFF); mem[t + 2 * k + 1] = (uint8_t)(p >> 8);
      char msg[40] = {0}; for (int j = 0; j < 31 && mem[p + 65 + j] != '$'; j++) msg[j] = (char)mem[p + 65 + j];
      std::printf("  [#%02d] %s  expected CRC %02x%02x%02x%02x\n", sel[k], msg, mem[p + 61], mem[p + 62], mem[p + 63], mem[p + 64]);
      if (g_log) std::fprintf(g_log, "[#%02d] %s  expected CRC %02x%02x%02x%02x\n", sel[k], msg, mem[p + 61], mem[p + 62], mem[p + 63], mem[p + 64]);
    }
    mem[t + 2 * sel.size()] = 0; mem[t + 2 * sel.size() + 1] = 0;
  } else {
    uint8_t boot[9] = { 0x31, 0x00, 0xFF, 0xCD, (uint8_t)(org & 0xFF), (uint8_t)(org >> 8), 0xD3, 0xF0, 0x76 };   // LD SP,FF00 ; CALL org ; OUT (F0) ; HALT
    static const uint8_t rst10[3] = { 0xD3, 0xF1, 0xC9 };                     // RST 10h: OUT (F1),A ; RET
    std::memcpy(mem + 0x0000, boot, sizeof boot);
    std::memcpy(mem + 0x0010, rst10, sizeof rst10);
    mem[0x1601] = 0xC9;                                                       // CHAN-OPEN: RET
    if (tests) {                                                              // subset: table after "LD BC,0 ; LD HL,testtable"
      int t = -1;
      for (int i = org; i < org + (int)len - 6; i++)
        if (mem[i] == 0x01 && mem[i + 1] == 0x00 && mem[i + 2] == 0x00 && mem[i + 3] == 0x21) { t = mem[i + 4] | (mem[i + 5] << 8); break; }
      if (t < 0) { std::printf("FATAL: testtable not found\n"); return 2; }
      std::vector<int> all;
      for (int i = 0; i < 512; i++) { int p = mem[t + 2 * i] | (mem[t + 2 * i + 1] << 8); if (!p) break; all.push_back(p); }
      std::vector<int> sel;
      for (int i : parse_list(tests)) if (i >= 0 && i < (int)all.size()) sel.push_back(i);
      std::printf("testtable at %04X: %zu tests, %zu selected (numbered by the program from 000 up, in the chosen order)\n", t, all.size(), sel.size());
      for (size_t k = 0; k < sel.size(); k++) { int p = all[(size_t)sel[k]]; mem[t + 2 * k] = (uint8_t)(p & 0xFF); mem[t + 2 * k + 1] = (uint8_t)(p >> 8); }
      mem[t + 2 * sel.size()] = 0; mem[t + 2 * sel.size() + 1] = 0;
    }
  }

  cpu = new Vcpu;
  cpu->clk = 0; cpu->cen = 0; cpu->rst = 1; cpu->wait_n = 1; cpu->busreq_n = 1; cpu->int_n = 1; cpu->nmi_n = 1;
  cpu->ss_load = 0; for (int w = 0; w < 9; w++) cpu->ss_din[w] = 0; cpu->data_in = 0;
  for (int i = 0; i < 4; i++) cen_tick();
  cpu->rst = 0; for (int i = 0; i < 2; i++) tick();                          // >= 2 quiet clocks after the reset

  t0 = std::chrono::steady_clock::now();
  int outf0 = 0; bool prev_iow = false; bool done = false; long long next_beat = 1000000000LL;
  while (!done) {
    set_din(); cpu->eval();
    if (!cpu->wr_n && !cpu->mreq_n) mem[cpu->addr] = cpu->data_out;
    bool iow = (!cpu->wr_n && !cpu->iorq_n);
    if (iow && !prev_iow) {
      uint8_t port = (uint8_t)(cpu->addr & 0xFF);
      if (port == 0xF1) con_out(cpu->data_out);
      else if (port == 0xF0) { outf0++; if (mode == SPEC || outf0 >= 2) done = true; }
    }
    prev_iow = iow;
    if (!cpu->halt_n) { if (!line.empty()) flush_line(); std::printf("HALT\n"); done = true; }
    cen_tick(); half_t++;
    if (maxT && half_t / 2 >= maxT) { if (!line.empty()) flush_line(); std::printf("STOP: maxT reached\n"); done = true; }
    if (half_t / 2 >= next_beat) {
      double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      if (g_log) { std::fprintf(g_log, "... T=%lld  %.0f s  (%.2f MT/s)\n", half_t / 2, el, half_t / 2 / el / 1e6); std::fflush(g_log); }
      next_beat += 1000000000LL;
    }
  }
  if (!line.empty()) flush_line();
  double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("END: T=%lld, %.0f s, %.2f MT/s, lines %ld\n", half_t / 2, el, el > 0 ? half_t / 2 / el / 1e6 : 0.0, n_lines);
  if (g_log) { std::fprintf(g_log, "END: T=%lld, %.0f s, %.2f MT/s\n", half_t / 2, el, el > 0 ? half_t / 2 / el / 1e6 : 0.0); std::fclose(g_log); }
  cpu->final(); delete cpu;
  // z80test: result from the verdict printed by the program; CP/M (zexdoc): the verdict is in the printed lines
  if (mode == SPEC) { std::printf("VERDICT: %s\n", spec_pass ? "PASS" : "FAIL"); return spec_pass ? 0 : 1; }
  return 0;
}
