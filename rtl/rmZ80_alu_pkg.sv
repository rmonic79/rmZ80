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
// rmZ80_alu_pkg — ALU operation codes (OP_*), bit positions of F, the opcode
// pre-decode function and the ALU operation decode function.
// Shared by rmZ80_alu, rmZ80_alu_dec, rmZ80_alu_daa, rmZ80_alu_core, rmZ80_cw, rmZ80_seq and the
// top module.
// =============================================================================
// ---- operation codes ------------------------------------------------------------
// benches that instantiate a single module (seq, decode) do not use every code
/* verilator lint_off UNUSEDPARAM */
package rmZ80_alu_pkg;
  // 0x00..0x07: the "ooo" field of quadrant 10 of the opcode (docs/03 §1.5)
  localparam logic [5:0] OP_ADD  = 6'h00, OP_ADC  = 6'h01, OP_SUB  = 6'h02,
                         OP_SBC  = 6'h03, OP_AND  = 6'h04, OP_XOR  = 6'h05,
                         OP_OR   = 6'h06, OP_CP   = 6'h07;
  localparam logic [5:0] OP_INC  = 6'h08, OP_DEC  = 6'h09, OP_NEG  = 6'h0A;
  // 0x0B: free
  localparam logic [5:0] OP_PASS = 6'h0C,   // res = a, F unchanged (loads through the ALU)
                         OP_TST  = 6'h0D;   // flags of IN r,(C) from the byte in a
  // RLD/RRD — docs/05 §10 ([UM] pp.238-241). a = A, b = m = the byte at (HL).
  //   RLD: (HL)' = {m[3:0], A[3:0]}   A' = {A[7:4], m[7:4]}
  //   RRD: (HL)' = {A[3:0], m[7:4]}   A' = {A[7:4], m[3:0]}
  localparam logic [5:0] OP_RLDM = 6'h0E,   // res = {b[3:0], a[3:0]}
                         OP_RRDM = 6'h0F;   // res = {a[3:0], b[7:4]}
  // 0x10..0x17: the "ooo" field of quadrant 00 of the CB space (docs/03 §1.5);
  // 0x16 = SLL, the 110 slot the manual does not assign ([SY], docs/03 §5.1)
  localparam logic [5:0] OP_RLC  = 6'h10, OP_RRC  = 6'h11, OP_RL   = 6'h12,
                         OP_RR   = 6'h13, OP_SLA  = 6'h14, OP_SRA  = 6'h15,
                         OP_SLL  = 6'h16, OP_SRL  = 6'h17;
  localparam logic [5:0] OP_RLCA = 6'h18, OP_RRCA = 6'h19, OP_RLA  = 6'h1A,
                         OP_RRA  = 6'h1B;
  // 0x1C..0x1F = 0x1C | ir[4:3] of 27h/2Fh/37h/3Fh (docs/06 §2.1, rule 4)
  localparam logic [5:0] OP_DAA  = 6'h1C, OP_CPL  = 6'h1D, OP_SCF  = 6'h1E,
                         OP_CCF  = 6'h1F;
  localparam logic [5:0] OP_RES  = 6'h21, OP_SET  = 6'h22;   // 20h: free; every BIT uses OP_BITX (26h)
  // flags of the block instructions (docs/06 §2.2): the sequencer presents in a16 the value of
  // BC ALREADY DECREMENTED, in a the register A, in b the byte transferred or compared
  localparam logic [5:0] OP_LDIF  = 6'h23,  // LDI/LDD/LDIR/LDDR
                         OP_CPIF  = 6'h24,  // CPI/CPD/CPIR/CPDR
                         OP_LDAI  = 6'h25,  // LD A,I / LD A,R: res = a, S/Z from the byte, H=N=0, P/V = pv_in (IFF2), C unchanged ([UM] p.94)
                         OP_BITX  = 6'h26,  // BIT: S/Z/P/V from the isolated bit, YF/XF from operand b (the register, the high byte of IX+d, or WZH; docs/05 §9)
                         OP_INIF  = 6'h2D,  // flags of INI/IND/INIR/INDR ([SY] §4.3): a = byte, b = (C+-1)&FF, a16[15:8] = B already decremented
                         OP_OUTIF = 6'h2E,  // flags of OUTI/OUTD/OTIR/OTDR ([SY] §4.3): a = byte, b = L after HL+-1, a16[15:8] = B already decremented
                         OP_YXB   = 6'h2F,  // F unchanged except YF/XF from bits 5/3 of b (repeating block instructions that repeat: b = PCH)
                         OP_IORF  = 6'h30,  // INIR/INDR/OTIR/OTDR that repeat: H and P/V adjusted from B (= a), C and N of F (SingleStepTests)
                         OP_SBC_R = 6'h31,  // b - a - C (SBC HL,ss with HL in WZ and ss in a: MEMPTR = HL+1)
                         OP_SBC16H_R = 6'h32;   // high step of SBC_R (16-bit Z, N=1)
  localparam logic [5:0] OP_RLDA  = 6'h27;  // res = {a[7:4], b[7:4]} + flags (docs/05 §10.3)
  // 16 bits in TWO 8-bit steps (the core has a single 16-bit adder, the address adder):
  // low byte first, then high byte, the carry passing through F.
  //   ADD HL,ss : step 1 OP_ADDLO  (a+b, only C changes in F)
  //               step 2 OP_ADD16H (a+b+C; S/Z/P unchanged, H/YF/XF/C from the high step, N=0)
  //   ADC HL,ss : step 1 OP_ADC, step 2 OP_ADC16H (Z = Z of the low byte AND Z of the high byte)
  //   SBC HL,ss : step 1 OP_SBC, step 2 OP_SBC16H (likewise, N=1)
  // Flags of the high step = "affected as by the second (high) 8 bit addition"
  // ([SY] §4.6 p.17, §8.6 n.2 p.31); H = carry 11->12 = bit 3->4 of the high byte.
  localparam logic [5:0] OP_ADDLO = 6'h28, OP_ADD16H = 6'h29, OP_ADC16H = 6'h2A;
  localparam logic [5:0] OP_RRDA  = 6'h2B;  // res = {a[7:4], b[3:0]} + flags (docs/05 §10.3)
  localparam logic [5:0] OP_SBC16H = 6'h2C;
  // bit positions of F (docs/01 §2)
  localparam int FB_C = 0, FB_N = 1, FB_P = 2, FB_X = 3;
  localparam int FB_H = 4, FB_Y = 5, FB_Z = 6, FB_S = 7;

  // ---- PRE-DECODE OF IR (timing) ----------------------------------------------------
  // The opcode patterns of the control word depend ONLY on IR. The sequencer registers them one clock
  // after it writes IR, from the IR register (cen pulses >= 2 clocks apart: no row uses IR in the half
  // T-state in which it is loaded): the control word reads them from flip-flops, and the 8-bit compares are off the path ir -> control word -> rd8_sel -> mux -> P1,
  // without changing the contract (cen >= 2 clocks). Definitions from docs/03 §1.
  localparam int PD_N = 76;
  function automatic logic [PD_N-1:0] rmz80_pdec(input logic [7:0] ir);
    logic x00, x01, x10, x11, src6, dst6, blk;
    logic [PD_N-1:0] p;
    x00 = (ir[7:6] == 2'b00); x01 = (ir[7:6] == 2'b01); x10 = (ir[7:6] == 2'b10); x11 = (ir[7:6] == 2'b11);
    src6 = (ir[2:0] == 3'd6); dst6 = (ir[5:3] == 3'd6);
    blk = (ir[7:5] == 3'b101) && !ir[2];
    p = '0;
    p[0]  = x00;  p[1] = x01;  p[2] = x10;  p[3] = x11;  p[4] = src6;  p[5] = dst6;
    p[6]  = (ir == 8'hCB) || (ir == 8'hDD) || (ir == 8'hED) || (ir == 8'hFD);   // p_pfx
    p[7]  = x01 && !src6 && !dst6;                        // p_ldrr
    p[8]  = x01 && src6 && !dst6;                         // p_ldrhl
    p[9]  = x01 && dst6 && !src6;                         // p_ldhlr
    p[10] = x00 && src6 && !dst6;                         // p_ldrn
    p[11] = (ir == 8'h36);                                // p_ldhln
    p[12] = x10 && !src6;                                 // p_alur
    p[13] = x10 && src6;                                  // p_aluhl
    p[14] = x11 && src6;                                  // p_alun
    p[15] = (ir == 8'h0A) || (ir == 8'h1A);               // p_ldabc
    p[16] = (ir == 8'h02) || (ir == 8'h12);               // p_ldbca
    p[17] = x00 && (ir[3:0] == 4'b0011);                  // p_incss
    p[18] = x00 && (ir[3:0] == 4'b1011);                  // p_decss
    p[19] = (ir == 8'hF9);                                // p_ldsphl
    p[20] = (ir == 8'h08);                                // p_exaf
    p[21] = (ir == 8'hD9);                                // p_exx
    p[22] = (ir == 8'hE9);                                // p_jphl
    p[23] = (ir == 8'h18);                                // p_jr
    p[24] = (ir[7:5] == 3'b001) && (ir[2:0] == 3'b000);   // p_jrcc
    p[25] = (ir == 8'h10);                                // p_djnz
    p[26] = (ir == 8'hC3);                                // p_jpnn
    p[27] = x11 && (ir[2:0] == 3'b010);                   // p_jpcc
    p[28] = (ir == 8'hCD);                                // p_call
    p[29] = x11 && (ir[2:0] == 3'b100);                   // p_callcc
    p[30] = (ir == 8'hC9);                                // p_ret
    p[31] = x11 && (ir[2:0] == 3'b000);                   // p_retcc
    p[32] = x11 && (ir[2:0] == 3'b111);                   // p_rst
    p[33] = x11 && (ir[3:0] == 4'b0101);                  // p_push
    p[34] = x11 && (ir[3:0] == 4'b0001);                  // p_pop
    p[35] = x00 && (ir[3:0] == 4'b0001);                  // p_lddnn
    p[36] = (ir == 8'h3A);                                // p_ldann
    p[37] = (ir == 8'h32);                                // p_ldnna
    p[38] = (ir == 8'h2A);                                // p_ldhlnn
    p[39] = (ir == 8'h22);                                // p_ldnnhl
    p[40] = (ir == 8'hE3);                                // p_exsphl
    p[41] = (ir == 8'hEB);                                // p_exdehl
    p[42] = x00 && (ir[3:0] == 4'b1001);                  // p_addhl
    p[43] = x00 && (ir[2:0] == 3'b100) && !dst6;          // p_incr
    p[44] = x00 && (ir[2:0] == 3'b101) && !dst6;          // p_decr
    p[45] = (ir == 8'h34);                                // p_inchl
    p[46] = (ir == 8'h35);                                // p_dechl
    p[47] = (ir[7:5] == 3'b000) && (ir[2:0] == 3'b111);   // p_rota
    p[48] = (ir == 8'h27);                                // p_daa
    p[49] = (ir == 8'h2F);                                // p_cpl
    p[50] = (ir[7:4] == 4'b0011) && (ir[2:0] == 3'b111);  // p_scc
    p[51] = (ir == 8'hDB);                                // p_in
    p[52] = (ir == 8'hD3);                                // p_out
    p[53] = p[8] | p[13] | p[9] | p[45] | p[46];          // has_d
    p[54] = p[7] | p[53] | p[12] | p[11] | p[43] | p[44] | p[10] | p[42] |
            (ir == 8'h21) || (ir == 8'h22) || (ir == 8'h2A) || (ir == 8'h23) ||
            (ir == 8'h2B) || (ir == 8'hF9) || (ir == 8'hE1) || (ir == 8'hE3) ||
            (ir == 8'hE5) || (ir == 8'hE9);                // ix_hit
    p[55] = (ir[7:5] == 3'b010) && (ir[2:0] == 3'b111);   // ed_ldai
    p[56] = (ir[7:4] == 4'b0110) && (ir[2:0] == 3'b111);  // ed_rld
    p[57] = x01 && (ir[3:0] == 4'b0011);                  // ed_stnn
    p[58] = x01 && (ir[3:0] == 4'b1011);                  // ed_ldnn
    p[59] = x01 && (ir[2:0] == 3'b000);                   // ed_in
    p[60] = x01 && (ir[2:0] == 3'b001);                   // ed_out
    p[61] = x01 && (ir[2:0] == 3'b010);                   // ed_a16
    p[62] = x01 && (ir[2:0] == 3'b101);                   // ed_ret
    p[63] = blk;                                          // ed_blk
    p[64] = blk && ir[4];                                 // ed_rep
    p[65] = blk && ir[1];                                 // ed_bio
    p[66] = blk && ir[1] && ir[0];                        // ed_bout
    p[67] = x01 && (ir[2:0] == 3'b100);                   // NEG (ED)
    p[68] = x01 && (ir[2:0] == 3'b110);                   // IM  (ED)
    p[69] = (ir == 8'hF3);                                // DI
    p[70] = (ir == 8'hFB);                                // EI
    p[71] = (ir == 8'h76);                                // HALT
    p[72] = (ir == 8'h66);                                // NMI (space 7)
    p[73] = (ir == 8'h7E);                                // IM2 (space 7)
    p[74] = blk && !ir[1] && !ir[0];                      // LDI/LDD/LDIR/LDDR
    p[75] = blk && !ir[1] &&  ir[0];                      // CPI/CPD/CPIR/CPDR
    return p;
  endfunction
  // ---- DECODE OF THE ALU OPERATION (timing) -------------------------------------------
  // Everything in the ALU core that depends ONLY on op: source of the result, bit shifted in on a
  // right shift, source of each flag, CPI case. The top module computes it from the registered
  // control word (P0) before P1; the core receives the selectors ready (docs/05, docs/03 flags column).
  // Vector FS_N = {rsel[3:0], shx[1:0], s_s[1:0], s_z[1:0], s_yx[2:0], s_h[2:0], s_p[2:0], s_n[1:0], s_c[2:0], cpif}
  //   rsel: 0 adder 1 right shift 2 AND 3 OR 4 XOR 5 RES 6 CPL 7 RLDM 8 RRDM 9 RLDA 10 RRDA 15 a
  //   shx : 0 zero (SRL) 1 a[0] 2 C 3 a[7]
  localparam int FS_N = 25;
  function automatic logic [FS_N-1:0] rmz80_fsel(input logic [5:0] op);
    logic [3:0] rsel; logic [1:0] shx, s_s, s_z, s_n; logic [2:0] s_yx, s_h, s_p, s_c; logic cpif;
    case (op)
      OP_ADD, OP_ADC, OP_SUB, OP_SBC, OP_CP, OP_INC, OP_DEC, OP_NEG, OP_DAA,
      OP_ADDLO, OP_ADD16H, OP_ADC16H, OP_SBC16H, OP_SBC_R, OP_SBC16H_R, OP_CPIF,
      OP_RLC, OP_RL, OP_SLA, OP_SLL, OP_RLCA, OP_RLA:   rsel = 4'd0;
      OP_RRC, OP_RR, OP_SRA, OP_SRL, OP_RRCA, OP_RRA:   rsel = 4'd1;
      OP_AND, OP_BITX:                                  rsel = 4'd2;
      OP_OR,  OP_SET:                                   rsel = 4'd3;
      OP_XOR:                                           rsel = 4'd4;
      OP_RES:                                           rsel = 4'd5;
      OP_CPL:                                           rsel = 4'd6;
      OP_RLDM:                                          rsel = 4'd7;
      OP_RRDM:                                          rsel = 4'd8;
      OP_RLDA:                                          rsel = 4'd9;
      OP_RRDA:                                          rsel = 4'd10;
      default:                                          rsel = 4'd15;   // PASS TST SCF CCF LDIF, unknown op
    endcase
    case (op)
      OP_RRC, OP_RRCA: shx = 2'd1;
      OP_RR,  OP_RRA:  shx = 2'd2;
      OP_SRA:          shx = 2'd3;
      default:         shx = 2'd0;          // SRL
    endcase
    s_s = 2'd0; s_z = 2'd0; s_yx = 3'd0; s_h = 3'd0; s_p = 3'd0; s_n = 2'd0; s_c = 3'd0;
    case (op)
      OP_ADD, OP_ADC: ;
      OP_SUB, OP_SBC, OP_NEG: s_n = 2'd1;
      OP_CP:  begin s_yx = 3'd2; s_n = 2'd1; end
      OP_AND: begin s_h = 3'd2; s_p = 3'd1; s_c = 3'd6; end
      OP_XOR, OP_OR: begin s_h = 3'd1; s_p = 3'd1; s_c = 3'd6; end
      OP_INC: s_c = 3'd2;
      OP_DEC: begin s_n = 2'd1; s_c = 3'd2; end
      OP_CPL: begin s_s = 2'd1; s_z = 2'd1; s_h = 3'd2; s_p = 3'd2; s_n = 2'd1; s_c = 3'd2; end
      OP_RLC, OP_RL, OP_SLA, OP_SLL:  begin s_h = 3'd1; s_p = 3'd1; end
      OP_RRC, OP_RR, OP_SRA, OP_SRL:  begin s_h = 3'd1; s_p = 3'd1; s_c = 3'd1; end
      OP_RLCA, OP_RLA: begin s_s = 2'd1; s_z = 2'd1; s_h = 3'd1; s_p = 3'd2; end
      OP_RRCA, OP_RRA: begin s_s = 2'd1; s_z = 2'd1; s_h = 3'd1; s_p = 3'd2; s_c = 3'd1; end
      OP_DAA: begin s_h = 3'd4; s_p = 3'd1; s_n = 2'd2; s_c = 3'd5; end
      OP_SCF: begin s_s = 2'd1; s_z = 2'd1; s_yx = 3'd5; s_h = 3'd1; s_p = 3'd2; s_c = 3'd3; end
      OP_CCF: begin s_s = 2'd1; s_z = 2'd1; s_yx = 3'd5; s_h = 3'd5; s_p = 3'd2; s_c = 3'd4; end
      OP_BITX: begin s_yx = 3'd2; s_h = 3'd2; s_p = 3'd1; s_c = 3'd2; end
      OP_RES, OP_SET, OP_PASS, OP_RLDM, OP_RRDM:
              begin s_s = 2'd1; s_z = 2'd1; s_yx = 3'd1; s_h = 3'd3; s_p = 3'd2; s_n = 2'd2; s_c = 3'd2; end
      OP_TST: begin s_h = 3'd1; s_p = 3'd1; s_c = 3'd2; end
      OP_LDIF: begin s_s = 2'd1; s_z = 2'd1; s_yx = 3'd3; s_h = 3'd1; s_p = 3'd3; s_c = 3'd2; end
      OP_CPIF: begin s_yx = 3'd3; s_p = 3'd3; s_n = 2'd1; s_c = 3'd2; end
      OP_RLDA, OP_RRDA: begin s_h = 3'd1; s_p = 3'd1; s_c = 3'd2; end
      OP_LDAI: begin s_h = 3'd1; s_p = 3'd4; s_c = 3'd2; end
      OP_INIF, OP_OUTIF: begin s_s = 2'd2; s_z = 2'd3; s_yx = 3'd4; s_h = 3'd6; s_p = 3'd5; s_n = 2'd3; s_c = 3'd0; end
      OP_YXB: begin s_s = 2'd1; s_z = 2'd1; s_yx = 3'd2; s_h = 3'd3; s_p = 3'd2; s_n = 2'd2; s_c = 3'd2; end
      OP_IORF: begin s_s = 2'd1; s_z = 2'd1; s_yx = 3'd1; s_h = 3'd7; s_p = 3'd6; s_n = 2'd2; s_c = 3'd2; end
      OP_ADDLO:  begin s_s = 2'd1; s_z = 2'd1; s_yx = 3'd1; s_h = 3'd3; s_p = 3'd2; s_n = 2'd2; end
      OP_ADD16H: begin s_s = 2'd1; s_z = 2'd1; s_p = 3'd2; end
      OP_ADC16H: s_z = 2'd2;
      OP_SBC16H, OP_SBC16H_R: begin s_z = 2'd2; s_n = 2'd1; end
      OP_SBC_R: s_n = 2'd1;
      default: begin s_s = 2'd1; s_z = 2'd1; s_yx = 3'd1; s_h = 3'd3; s_p = 3'd2; s_n = 2'd2; s_c = 3'd2; end
    endcase
    cpif = (op == OP_CPIF);
    return {rsel, shx, s_s, s_z, s_yx, s_h, s_p, s_n, s_c, cpif};
  endfunction
endpackage
