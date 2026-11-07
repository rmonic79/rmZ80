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
// rmZ80_alu_core — the ALU datapath. Part of the rmZ80 ALU (see rtl/rmZ80_alu.sv
// for the specification and sources).
//
// The adder fields, the DAA tables, the operation decode (sources of the result and
// of the flags, rmz80_fsel) and the effective subtraction all arrive ready (from the
// wrapper, or from P1 in the top module): after P1 there is no decode.
//   - the flags S, Z, YF, XF, P are computed ONCE from the result `res` (one zero
//     detector, one parity); each operation only selects the SOURCE of each flag;
//   - LEFT shifts and rotates go through the shared adder: a + a + cin with
//     cin = a[7] (RLC), C (RL), 0 (SLA), 1 (SLL); the carry is a[7] = add_c; only the
//     four right forms {x, a[7:1]} have their own logic;
//   - BIT/RES/SET use the logic network (AND/OR) with the operand `lb` = the bit
//     mask; RES = a & ~mask via add_bx (the complemented operand of the adder).
// The per-instruction rules are those of docs/05 and of the flags column of docs/03;
// the ALU bench checks them by exhaustive enumeration.
// =============================================================================
module rmZ80_alu_core #(
  // SCF/CCF, undocumented bits (docs/05 §9): NMOS = Q|A, CMOS = A
  parameter bit VARIANT_SCF_CCF_NMOS = 1'b1
)(
  input  logic [rmZ80_alu_pkg::FS_N-1:0] fsel,   // selectors already decoded from op (rmz80_fsel)
  input  logic        alu_sub,      // effective subtraction = ad_sub | (ad_daa & N), ready
  input  logic        ad_daa,
  input  logic [1:0]  ad_rhs,
  input  logic        ad_lhs0,
  input  logic        ad_lhsb,      // lhs = b (b-a-C)
  input  logic [2:0]  ad_cin,       // 0 zero, 1 C, 2 ~C, 3 one, 4 a[7] (left rotates)
  input  logic [1:0]  daa_diff_sel,
  input  logic        daa_c,
  input  logic        daa_h,
  input  logic [7:0]  a,
  input  logic [7:0]  b,
  input  logic [7:0]  f_in,
  input  logic [7:0]  q_in,
  input  logic        pv_in,        // external P/V: IFF2 for LD A,I / LD A,R
  input  logic [7:0]  lb_in,        // logic operand, ready: b, or the bit mask for BITX/RES/SET
  input  logic [15:0] a16,          // only for the flags of the block instructions (BC, {B, C})
  output logic [7:0]  res,
  output logic [7:0]  f_out
);
  import rmZ80_alu_pkg::*;

  // ---------------------------------------------------------------------------
  // Logic operand: b, or the bit mask for BIT/RES/SET
  // ---------------------------------------------------------------------------
  // the choice b / mask (op in {BITX, RES, SET}, bit = bit_sel) is made by the module that instantiates
  // the core: in the top module BEFORE P1 (off the path P1 -> ALU -> register file), in the wrapper
  // rmZ80_alu combinationally (ALU bench).
  wire [7:0] lb       = lb_in;

  // ---------------------------------------------------------------------------
  // Shared 8-bit adder, driven by the decoded fields.
  // Subtraction in complemented form: a + ~b + 1 (or + ~C for SBC), so that
  //   V = carry_in(bit7) XOR carry_out(bit7)   — docs/05 §4
  //   H = carry/borrow between bits 3 and 4    — docs/05 §5
  // DAA (docs/05 §8.2): diff ADDED if N=0, SUBTRACTED if N=1 (alu_sub = N).
  // Left rotates: rhs = a (code 2), cin according to the form.
  // ---------------------------------------------------------------------------
  logic [7:0] daa_diff;
  always_comb begin
    case (daa_diff_sel)
      2'd0: daa_diff = 8'h00;
      2'd1: daa_diff = 8'h06;
      2'd2: daa_diff = 8'h60;
      default: daa_diff = 8'h66;
    endcase
  end
  // fields of fsel (order of rmz80_fsel)
  wire [3:0] rsel = fsel[24:21];
  wire [1:0] shx  = fsel[20:19];
  logic [1:0] s_s, s_z, s_n;  logic [2:0] s_p, s_yx, s_h, s_c;  logic cpif;
  assign {s_s, s_z, s_yx, s_h, s_p, s_n, s_c, cpif} = fsel[18:0];
  wire [7:0]  add_lhs = ad_lhs0 ? 8'h00 : (ad_lhsb ? b : a);
  logic [7:0] add_rhs;
  always_comb begin
    case (ad_rhs)
      2'd0: add_rhs = lb;
      2'd1: add_rhs = 8'h01;
      2'd2: add_rhs = a;
      default: add_rhs = daa_diff;
    endcase
  end
  logic add_cin;
  always_comb begin
    if (ad_daa) add_cin = f_in[FB_N];
    else case (ad_cin)
      3'd0: add_cin = 1'b0;
      3'd1: add_cin = f_in[FB_C];
      3'd2: add_cin = ~f_in[FB_C];
      3'd3: add_cin = 1'b1;
      default: add_cin = a[7];
    endcase
  end

  wire [7:0] add_bx  = alu_sub ? ~add_rhs : add_rhs;
  wire [8:0] sum9    = {1'b0, add_lhs}       + {1'b0, add_bx}       + {8'b0, add_cin};
  // carry INTO bit k = sum[k] ^ lhs[k] ^ bx[k]: V and H from the single 9-bit sum
  wire       cin7    = sum9[7] ^ add_lhs[7] ^ add_bx[7];
  wire       cin4    = sum9[4] ^ add_lhs[4] ^ add_bx[4];
  wire [7:0] add_res = sum9[7:0];
  wire       add_v   = cin7 ^ sum9[8];               // cin(b7) XOR cout(b7) — docs/05 §4
  wire       add_h   = alu_sub ? ~cin4 : cin4;       // docs/05 §5
  wire       add_c   = alu_sub ? ~sum9[8] : sum9[8];

  // ---------------------------------------------------------------------------
  // RIGHT shifts (the left forms are a + a + cin on the adder).
  // Semantics: [UM] pp.205-236; SLL: docs/03 §5.1 (like SLA, bit 0 set) [SY]
  // ---------------------------------------------------------------------------
  logic sh_x;
  always_comb begin
    case (shx)
      2'd1:    sh_x = a[0];                  // RRC RRCA
      2'd2:    sh_x = f_in[FB_C];            // RR RRA
      2'd3:    sh_x = a[7];                  // SRA
      default: sh_x = 1'b0;                  // SRL
    endcase
  end
  wire [7:0] sh_r = {sh_x, a[7:1]};

  // ---------------------------------------------------------------------------
  // Logic network, complement, RLD/RRD (docs/05 §10), flags of the block instructions (docs/05 §9)
  // ---------------------------------------------------------------------------
  wire [7:0] and_r  = a & lb;          // AND, BIT (isolated bit)
  wire [7:0] or_r   = a | lb;          // OR, SET
  wire [7:0] xor_r  = a ^ lb;          // XOR
  wire [7:0] andn_r = a & add_bx;      // RES (add_bx = ~mask: ad_sub = 1)
  wire [7:0] cpl_r  = ~a;
  wire [7:0] rld_a  = {a[7:4], b[7:4]};
  wire [7:0] rrd_a  = {a[7:4], b[3:0]};
  wire [3:0] cpi_n  = add_res[3:0] - {3'b000, add_h};   // CPI: n = A-(HL)-H', bits 1 and 3
  wire       bc_nz  = (a16 != 16'h0000);                 // BC != 0
  // block I/O ([SY] §4.3): k = byte + operand (adder); B = a16[15:8] already decremented (the write of B
  // falls in the same tick of the control word, the flags are computed in the tick after); H = C = carry of k;
  // P = parity of ((k & 7) ^ B); N = byte[7]. INIF and OUTIF differ only in the operand: (C+-1)&FF or L
  wire [7:0] b_io   = a16[15:8];
  // block I/O instructions that REPEAT (SingleStepTests): with a = B and F of the step just computed:
  //   C=1: if N=1 (byte[7]) bq = B-1 and H = (B[3:0] == 0), else bq = B+1 and H = (B[3:0] == F); C=0: bq = B, H unchanged;
  //   P ^= even parity of (bq & 7) ^ 1, that is P ^= xor of the 3 low bits of bq
  wire [2:0] bq_lo  = f_in[FB_C] ? (f_in[FB_N] ? a[2:0] - 3'd1 : a[2:0] + 3'd1) : a[2:0];   // the 3 low bits are enough
  wire       io_rp  = f_in[FB_P] ^ (^bq_lo);
  wire       io_rh  = f_in[FB_C] ? (f_in[FB_N] ? (a[3:0] == 4'h0) : (a[3:0] == 4'hF)) : f_in[FB_H];
  wire       io_p   = ~^{b_io[7:3], add_res[2:0] ^ b_io[2:0]};
  wire       scf_y  = VARIANT_SCF_CCF_NMOS ? (q_in[FB_Y] | a[FB_Y]) : a[FB_Y];
  wire       scf_x  = VARIANT_SCF_CCF_NMOS ? (q_in[FB_X] | a[FB_X]) : a[FB_X];

  // ---------------------------------------------------------------------------
  // Result: one source per operation
  // ---------------------------------------------------------------------------
  always_comb begin
    case (rsel)                                         // source per operation: table in rmz80_fsel
      4'd0:    res = add_res;
      4'd1:    res = sh_r;
      4'd2:    res = and_r;
      4'd3:    res = or_r;
      4'd4:    res = xor_r;
      4'd5:    res = andn_r;
      4'd6:    res = cpl_r;
      4'd7:    res = {b[3:0], a[3:0]};                  // RLDM
      4'd8:    res = {a[3:0], b[7:4]};                  // RRDM
      4'd9:    res = rld_a;
      4'd10:   res = rrd_a;
      default: res = a;                                 // PASS TST SCF CCF LDIF, unknown op
    endcase
  end

  // ---------------------------------------------------------------------------
  // Flags: ONE parity and ONE zero detector on the result; each operation only
  // selects the source of each bit. F = {S, Z, YF, H, XF, P/V, N, C}.
  // Selectors: S  0 res[7]   1 f_in
  //            Z  0 res==0   1 f_in      2 res==0 & Z_in (16-bit high step)
  //            YX 0 res      1 f_in      2 operand b (CP, BIT)  3 n (block LD/CP)
  //               4 B (block I/O)   5 SCF/CCF variant
  //            H  0 add_h    1 zero      2 one   3 f_in   4 DAA H'   5 C_in (CCF)   6 carry of k (block I/O)
  //            P  0 add_v    1 parity    2 f_in  3 BC != 0   4 pv_in (IFF2, LD A,I/R)   5 parity of ((k&7)^B) (block I/O)
  //            N  0 zero     1 one       2 f_in  3 byte[7] (block I/O)
  //            S  2 and Z 3: from B (block I/O)
  //            C  0 add_c    1 a[0] (right shift)  2 f_in  3 one  4 ~C_in  5 DAA C'  6 zero
  // ---------------------------------------------------------------------------
  wire res_z = (res == 8'h00);
  wire res_p = ~(^res);                     // P=1 with an EVEN number of 1 bits — docs/05 §4

  // flag selectors: table per operation in rmz80_fsel (decoded before P1)

  logic f_s, f_z, f_y, f_x, f_h, f_p, f_n, f_c;
  always_comb begin
    case (s_s)  2'd0: f_s = res[7];  2'd1: f_s = f_in[FB_S];  default: f_s = b_io[7]; endcase
    case (s_z)
      2'd0: f_z = res_z;  2'd1: f_z = f_in[FB_Z];  2'd2: f_z = res_z & f_in[FB_Z];
      default: f_z = (b_io == 8'h00);
    endcase
    case (s_yx)
      3'd0: begin f_y = res[5];      f_x = res[3];      end
      3'd1: begin f_y = f_in[FB_Y];  f_x = f_in[FB_X];  end
      3'd2: begin f_y = b[5];        f_x = b[3];        end
      3'd3: begin f_y = cpif ? cpi_n[1] : add_res[1];   // LDI: n = A+(HL); CPI: n = A-(HL)-H'
                  f_x = cpif ? cpi_n[3] : add_res[3]; end
      3'd4: begin f_y = b_io[5];     f_x = b_io[3];     end   // block I/O: from B
      default: begin f_y = scf_y;    f_x = scf_x;       end
    endcase
    case (s_h)
      3'd0: f_h = add_h;  3'd1: f_h = 1'b0;  3'd2: f_h = 1'b1;  3'd3: f_h = f_in[FB_H];
      3'd4: f_h = daa_h;  3'd5: f_h = f_in[FB_C];  3'd6: f_h = add_c;  default: f_h = io_rh;   // 6: carry of k; 7: I/O repeat
    endcase
    case (s_p)  3'd0: f_p = add_v;  3'd1: f_p = res_p;  3'd2: f_p = f_in[FB_P];  3'd3: f_p = bc_nz;  3'd4: f_p = pv_in;  3'd5: f_p = io_p;  default: f_p = io_rp; endcase
    case (s_n)  2'd0: f_n = 1'b0;   2'd1: f_n = 1'b1;   2'd2: f_n = f_in[FB_N];  default: f_n = a[7]; endcase   // 3: bit 7 of the byte (block I/O)
    case (s_c)
      3'd0: f_c = add_c;  3'd1: f_c = a[0];  3'd2: f_c = f_in[FB_C];  3'd3: f_c = 1'b1;
      3'd4: f_c = ~f_in[FB_C];  3'd5: f_c = daa_c;  default: f_c = 1'b0;
    endcase
  end
  assign f_out = {f_s, f_z, f_y, f_h, f_x, f_p, f_n, f_c};

  // bits deliberately unused (the name "unused" excludes them from lint)
  wire _unused_ok = &{1'b0,
                      q_in[7:6], q_in[FB_H], q_in[2:0], cpi_n[2], cpi_n[0]};

endmodule
