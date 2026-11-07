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
// rmZ80_alu — the Z80 ALU, PURELY COMBINATIONAL
//
// Specification: docs/05-alu-and-flags.md and the flags column of docs/03-decoding.md
//                (layout of F: docs/01-registers-and-state.md §2).
//
// NO STATE: no clock, no reset, no inferred memory (0 M10K by construction).
// Contribution to the savestate: 0 bits — F, Q, WZ and A live in the sequencer and
// the register file.
//
// FOUR FILES (the split keeps the operation decode off the critical path):
//   rmZ80_alu_dec  : op -> fields of the shared adder (pure function of 6 bits);
//   rmZ80_alu_daa  : (a, f_in) -> diff/C'/H' of the total DAA rule;
//   rmZ80_alu_core : datapath; receives the fields and the tables ready;
//   rmZ80_alu      : this wrapper with the plain interface (op, a, b, f_in, ...) =
//                    dec + daa + core. It is the module the ALU bench tests.
// In the top module, dec and daa sit BEFORE the pipeline register P1 and the core
// after it: two levels of decode are out of the critical path.
//
// One 8-bit adder shared by ADD/ADC/SUB/SBC/CP/INC/DEC/NEG/DAA, by the left shifts
// and by the two steps of the 16-bit HL operations (the only 16-bit adder of the
// core is the address adder in the top module); one right shifter; one parity.
//
// Layout of F (docs/01 §2):  bit 7 6 5 4 3 2 1 0  =  S Z YF H XF P/V N C
// (YF = bit 5, XF = bit 3: the two bits the manual calls "not used")
//
// An unknown op gives res = a, f_out = f_in (pass-through).
//
// YF/XF (docs/05 §9): from the operand for CP and BIT ([SY] §8.4 n.1 p.30; z80test),
// from the result for DAA ([SY] §4.7), the NMOS/CMOS variant for SCF/CCF, F
// unchanged for RES/SET ([UM]), from the high step for the 16-bit additions ([SY]
// §4.6 p.17, §8.6 n.2 p.31), from n for LDI/LDD/CPI/CPD and their repeating forms ([SY] §4.2),
// from the decremented B for the block I/O instructions ([SY] §4.3), from PCH when a repeating
// block instruction repeats (SingleStepTests), from the result elsewhere ([SY] §2.3). DAA: the total
// rule of docs/05 §8.2 (three tables diff/C'/H', [SY] §4.7, checked against [UM]).
// =============================================================================

module rmZ80_alu #(
  parameter bit VARIANT_SCF_CCF_NMOS = 1'b1
)(
  input  logic [5:0]  op,       // operation (rmZ80_alu_pkg::OP_*)
  input  logic [7:0]  a,        // first operand (accumulator or register)
  input  logic [7:0]  b,        // second operand
  input  logic [7:0]  f_in,     // incoming F
  input  logic [7:0]  q_in,     // register Q (docs/01 §3): bits 5 and 3 are used
  input  logic        pv_in,    // IFF2: P/V of LD A,I / LD A,R
  input  logic [2:0]  bit_sel,  // bit index for BIT/RES/SET
  input  logic [15:0] a16,      // BC / {B, C} for the flags of the block instructions
  output logic [7:0]  res,      // 8-bit result
  output logic [7:0]  f_out     // outgoing F, all 8 bits
);
  logic ad_sub, ad_daa, ad_lhs0, ad_lhsb, daa_c, daa_h;
  logic [1:0] ad_rhs, daa_diff_sel;
  logic [2:0] ad_cin;

  rmZ80_alu_dec u_dec (.op(op), .ad_sub(ad_sub), .ad_daa(ad_daa), .ad_rhs(ad_rhs),
                       .ad_lhs0(ad_lhs0), .ad_lhsb(ad_lhsb), .ad_cin(ad_cin));
  rmZ80_alu_daa u_daa (.a(a), .f_in(f_in), .diff_sel(daa_diff_sel), .daa_c(daa_c), .daa_h(daa_h));
  wire       use_mask = (op == rmZ80_alu_pkg::OP_BITX) || (op == rmZ80_alu_pkg::OP_RES) || (op == rmZ80_alu_pkg::OP_SET);
  wire [7:0] lb       = use_mask ? (8'h01 << bit_sel) : b;   // logic operand (see rmZ80_alu_core)
  rmZ80_alu_core #(.VARIANT_SCF_CCF_NMOS(VARIANT_SCF_CCF_NMOS)) u_core (
    .fsel(rmZ80_alu_pkg::rmz80_fsel(op)), .alu_sub(ad_sub | (ad_daa & f_in[rmZ80_alu_pkg::FB_N])), .ad_daa(ad_daa), .ad_rhs(ad_rhs), .ad_lhs0(ad_lhs0), .ad_lhsb(ad_lhsb), .ad_cin(ad_cin),
    .daa_diff_sel(daa_diff_sel), .daa_c(daa_c), .daa_h(daa_h),
    .a(a), .b(b), .f_in(f_in), .q_in(q_in), .pv_in(pv_in), .lb_in(lb), .a16(a16),
    .res(res), .f_out(f_out)
  );
endmodule

// The operation codes (OP_*) and the bit positions of F (FB_*) are in
// rtl/rmZ80_alu_pkg.sv (package rmZ80_alu_pkg), imported by the three modules.
