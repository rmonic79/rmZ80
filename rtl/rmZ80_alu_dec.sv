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
// rmZ80_alu_dec — op -> fields of the shared adder (a pure function of op).
// Part of the rmZ80 ALU (see rtl/rmZ80_alu.sv for the specification and sources).
//   ad_sub  : operand B complemented (form a + ~b + 1)
//   ad_daa  : DAA: subtraction and carry-in follow N, rhs = diff
//   ad_rhs  : 0 lb (b, or the bit mask for RES), 1 constant 1 (INC/DEC), 2 a (NEG: 0 - a;
//             left shifts: a + a), 3 DAA diff
//   ad_lhs0 : lhs = 0 (NEG)
//   ad_lhsb : lhs = b (b - a - C: SBC HL,ss with HL in WZ)
//   ad_cin  : 0 zero, 1 C, 2 ~C (SBC), 3 one (subtractions), 4 a[7]
// =============================================================================

module rmZ80_alu_dec (
  input  logic [5:0] op,
  output logic       ad_sub,
  output logic       ad_daa,
  output logic [1:0] ad_rhs,
  output logic       ad_lhs0,
  output logic       ad_lhsb,      // lhs = b (reversed subtraction b-a-C: SBC HL,ss with HL in WZ)
  output logic [2:0] ad_cin        // 0 zero, 1 C, 2 ~C, 3 one, 4 a[7]
);
  import rmZ80_alu_pkg::*;
  always_comb begin
    ad_sub = 1'b0; ad_daa = 1'b0; ad_rhs = 2'd0; ad_lhs0 = 1'b0; ad_lhsb = 1'b0; ad_cin = 3'd0;
    case (op)
      OP_ADD, OP_LDIF, OP_INIF, OP_OUTIF: ;                         // a+b (block I/O: k = byte + (C+-1) or + L)
      OP_ADC:          ad_cin = 3'd1;                               // a+b+C
      OP_SUB, OP_CP, OP_CPIF: begin ad_sub = 1'b1; ad_cin = 3'd3; end   // a-b
      OP_SBC:          begin ad_sub = 1'b1; ad_cin = 3'd2; end      // a-b-C
      OP_INC:          ad_rhs = 2'd1;                               // a+1, C unchanged
      OP_DEC:          begin ad_sub = 1'b1; ad_rhs = 2'd1; ad_cin = 3'd3; end
      OP_NEG:          begin ad_sub = 1'b1; ad_lhs0 = 1'b1; ad_rhs = 2'd2; ad_cin = 3'd3; end
      OP_DAA:          begin ad_daa = 1'b1; ad_rhs = 2'd3; end      // docs/05 §8.2
      OP_ADDLO:        ;                                            // a+b (low step of ADD HL)
      OP_ADD16H, OP_ADC16H: ad_cin = 3'd1;                          // a+b+C (high step)
      OP_SBC16H:       begin ad_sub = 1'b1; ad_cin = 3'd2; end      // a-b-C (high step)
      OP_SBC_R, OP_SBC16H_R: begin ad_sub = 1'b1; ad_cin = 3'd2; ad_lhsb = 1'b1; ad_rhs = 2'd2; end   // b-a-C
      // LEFT rotates and shifts on the adder: a + a + cin
      OP_RLC, OP_RLCA: begin ad_rhs = 2'd2; ad_cin = 3'd4; end      // cin = a[7]
      OP_RL,  OP_RLA:  begin ad_rhs = 2'd2; ad_cin = 3'd1; end      // cin = C
      OP_SLA:          ad_rhs = 2'd2;                               // cin = 0
      OP_SLL:          begin ad_rhs = 2'd2; ad_cin = 3'd3; end      // cin = 1 ([SY])
      OP_RES:          ad_sub = 1'b1;                               // add_bx = ~mask
      default:         ;
    endcase
  end
endmodule
