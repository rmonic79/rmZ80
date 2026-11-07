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
// rmZ80_cw — THE CONTROL WORD for (prefix space, IR, M, T, half), AND the SCHEDULE ROW
// of the current M-cycle (type, length, last M-cycle, conditions), generated from the
// SAME opcode patterns that select the actions, as a default plus exceptions.
// This file is the place of the control word (docs/06 §2) and of the schedule
// (docs/06 §3.4, §4). It covers all 1792 opcode positions; the schedule is checked
// against Appendix A of docs/03 by the decode bench.
//
// CONVENTIONS (docs/06 §2.1):
//   - PC+1 of the fetch in M1 T3A (refresh on the bus); PC+1 of operand reads in T3B;
//     JP cc: PC <- cond ? WZ : PC+1 in one tick, the condition decided in the top module
//     from F (jpcc_t);
//   - stack: address always rd16(SP), pre-decrement (PUSH/RST M1 T5B, CALL M3 T4B) and
//     post-increment (POP/RET/EX (SP) T3B); the pushed byte on rd8 (PCH/PCL or a half of
//     the pair); high byte first;
//   - WZ: nn of JP/CALL/LD (nn); temporary of EX (SP),HL and ADD HL (b = WZL/WZH);
//     restart vector through the address adder, source 3;
//   - INC r / rotates: F written in a tick of its own before the register;
//   - conditions: cond_mode 0 = cc on F, 1 = cc of JR on F, 2 = B != 0 (rd8),
//     3 = block repeat (P/V, and Z for CPIR/CPDR); used for gate/ext (docs/06 §3.4).
//
// Codes: sel8 B0 C1 D2 E3 H4 L5 F6 A7 SPH12 SPL13 PCH14 PCL15 I16 R17; sel16 BC0 DE1
// HL2 SP3 PC6 AF7; dout_src 0 rd8 / 1 res / 2 DIN / 3 OUT (C),0 constant; wr8_src 0 res /
// 1 f_out (DIN and rd8 reach res with op PASS 0Ch); wr16_src 0 address adder / 1 AF8
// {res,f_out} (WZ towards PC/HL goes through the adder with addend 0);
// inc_op 0 +1 / 1 -1 / 2 +sext(DIN) / 3 0; inc_src 1 rd16 / 2 WZ / 3 restart vector;
// wz_we 01 WZL<-DIN / 10 WZH<-DIN / 11 WZ<-adder; a_src 0 A / 1 rd8 / 2 DIN;
// b_src 0 rd8 / 1 DIN / 2 WZL / 3 WZH (ADD HL in two steps, JR on the address adder;
// no 16-bit adder in the ALU).
// =============================================================================

module rmZ80_cw
  import rmZ80_alu_pkg::*;
#(
  // 1 = the opcode patterns arrive already registered from the sequencer (pd = rmz80_pdec(IR));
  // 0 = computed here from ir (decode bench, which instantiates the control word alone). Same function.
  parameter bit EXT_PD = 1'b0
) (
  input  logic [7:0] ir,
  input  logic [PD_N-1:0] pd,
  input  logic       six_i,     // DD/FD space registered by the sequencer (used with EXT_PD = 1)
  input  logic [7:1] moh_i, toh_i,   // (EXT_PD = 1) m_idx and t_idx one-hot, registered in NX
  input  logic       sh2_i, sh1_i,   // (EXT_PD = 1) six && has_d, six && LD (IX+d),n: from the registered coordinates
  input  logic [2:0] m_idx,
  input  logic [2:0] t_idx,
  input  logic       phase,       // 0 = A, 1 = B
  input  logic [2:0] space,     // 0 BASE, 1 CB, 2 ED, 3 DD, 4 FD, 5 DDCB, 6 FDCB, 7 interrupt response
  input  logic [7:0] f_reg,      // unused (F of the active bank: JP cc is decided in the top module)
  input  logic       halt,       // halt_q of the sequencer: in HALT the PC does not advance (M1 of NOP, docs/02 §7)
  input  logic       int_lat,    // INT response in progress: in IM0 the M1 of the instruction supplied by the device is an IACK (+2 T)
  input  logic       xhl,        // DE<->HL renamed in the active bank (EX DE,HL)
  // ---- control word ----
  output logic [4:0] rd8_sel,
  output logic [2:0] rd16_sel,
  output logic [2:0] rd16_raw,   // RAW accumulated code (= rd16_x): the XOR, IX/IY and DE<->HL are applied by the register file
  output logic       rd16_ix,    //   on the data side; rd16_sel remains as the logical view for the decode bench
  output logic       rd16_xhl,
  output logic       jpcc_t,     // row of JP cc in M3 T3B: the jump (PC <- WZ) is decided OUTSIDE from F
  output logic       addr_wz,
  output logic [1:0] dout_src,
  output logic [5:0] alu_op,
  output logic [1:0] a_src,
  output logic [1:0] b_src,      // 0 rd8, 1 DIN, 2 WZL, 3 WZH
  output logic       wr8_en,
  output logic [4:0] wr8_sel,
  output logic       wr8_src,     // 0 res (DIN/rd8 through op PASS), 1 f_out
  output logic       wr16_en,
  output logic [2:0] wr16_sel,
  output logic [4:0] wr8_raw,    // write selectors BEFORE the IX/IY and DE<->HL substitutions:
  output logic [2:0] wr16_raw,   //   the register file applies them on the write side (rd8_ix/rd16_ix/rd8_fd/xhl)
  output logic       wr16_src,    // 0 address adder (also WZ with addend 0), 1 AF8 {res,f_out}
  output logic [1:0] inc_op,
  output logic [1:0] inc_src,
  output logic [1:0] wz_we,
  output logic [1:0] wz_src,     // source of WZ (MEMPTR): 0 DIN (low/high byte), 1 adder, 2 {A, adder[7:0]}
  output logic [1:0] cond_mode,
  output logic       swap_af,
  output logic       swap_gp,
  output logic       im_we,       // IM <- im_val
  output logic [1:0] im_val,
  output logic [1:0] iff_op,      // 1 DI, 2 RETN/RETI (IFF1 <- IFF2), 3 EI
  output logic       halt_set,    // HALT executed
  // ---- schedule row of the current M-cycle (docs/06 §3.4) ----
  output logic [2:0] row_type,   // MT_* (where Appendix A gives no cycle type: docs/06 §4.2, §7)
  output logic [2:0] row_dur,    // length in T = 3 + row_dur (3 bits: IACK 7 T)
  output logic       row_gate,   // (benches only) M-cycle present only if the condition is true
  output logic       row_ext,    // the last T-state of the M-cycle exists only if the condition is true
  output logic       row_rpt,    // (benches only) at the end of the M-cycle, if true, PC <- PC-2
  output logic       row_last,   // last M-cycle of the instruction
  output logic       nxt_gate,   // gate of row m_idx+1 (cut of the conditional M-cycles)
  output logic       sp_set,     // M1 of a prefix (BASE) or of a second prefix (DD/FD): the sequencer changes space
  output logic       ir_ld,      // read that loads IR: fourth byte of DDCB/FDCB
  output logic       swap_dehl,  // EX DE,HL: toggles the renaming bit of the active bank
  output logic       rd8_ix, rd8_fd, rd8_xhl,   // data-side substitutions of the rd8 port of the register file
  // ---- benches only (not connected in the top module) ----
  output logic [2:0] n_m,
  output logic [2:0] n_byte,
  output logic       pre_m1,     // unassigned DD/FD: base instruction with the prefix M1 in front
  output logic       is_pfx      // the opcode is a prefix byte
);

  // ---- coordinates ---------------------------------------------------------------
  wire fA = ~phase, fB = phase;
  // decoded coordinates: with EXT_PD they arrive one-hot from NX, otherwise from the compare
  wire [7:1] mdec = {m_idx == 3'd7, m_idx == 3'd6, m_idx == 3'd5, m_idx == 3'd4, m_idx == 3'd3, m_idx == 3'd2, m_idx == 3'd1};
  wire [7:1] tdec = {t_idx == 3'd7, t_idx == 3'd6, t_idx == 3'd5, t_idx == 3'd4, t_idx == 3'd3, t_idx == 3'd2, t_idx == 3'd1};
  wire [7:1] moh = EXT_PD ? moh_i : mdec;
  wire [7:1] toh = EXT_PD ? toh_i : tdec;
  wire m1 = moh[1], m2 = moh[2], m3 = moh[3], m4 = moh[4], m5 = moh[5], m6 = moh[6], m7 = moh[7];
  wire t1 = toh[1], t2 = toh[2], t3 = toh[3], t4 = toh[4], t5 = toh[5], t6 = toh[6], t7 = toh[7];
  wire [1:0] dd  = ir[5:4];
  wire [2:0] ooo = ir[5:3];
  wire [4:0] r_dst = {2'b0, ir[5:3]};
  wire [4:0] r_src = {2'b0, ir[2:0]};
  wire [4:0] hi8_dd = (dd == 2'd3) ? 5'd12 : {2'b0, dd, 1'b0};   // B D H SPH
  wire [4:0] lo8_dd = (dd == 2'd3) ? 5'd13 : {2'b0, dd, 1'b1};   // C E L SPL
  wire [4:0] hi8_qq = (dd == 2'd3) ? 5'd7  : {2'b0, dd, 1'b0};   // B D H A
  wire [4:0] lo8_qq = (dd == 2'd3) ? 5'd6  : {2'b0, dd, 1'b1};   // C E L F

  // ---- condition cc on F (docs/03 §1.3); unused, kept for the port --------------------
  logic cond_f;
  always_comb begin
    case (ir[5:3])
      3'd0: cond_f = ~f_reg[6];  3'd1: cond_f = f_reg[6];
      3'd2: cond_f = ~f_reg[0];  3'd3: cond_f = f_reg[0];
      3'd4: cond_f = ~f_reg[2];  3'd5: cond_f = f_reg[2];
      3'd6: cond_f = ~f_reg[7];  default: cond_f = f_reg[7];
    endcase
  end

  // ---- spaces -----------------------------------------------------------------------
  wire base = (space == 3'd0), scb = (space == 3'd1), sed = (space == 3'd2),
       six  = EXT_PD ? six_i : ((space == 3'd3) || (space == 3'd4)),
       sxcb = (space == 3'd5) || (space == 3'd6),
       sint = (space == 3'd7);   // response to an interrupt: synthetic IR 66h (NMI), 38h (IM1), 7Eh (IM2)
  wire [PD_N-1:0] pdi = EXT_PD ? pd : rmz80_pdec(ir);   // opcode patterns (package)
  wire _unused_pd = ^{pd, pdi[3:2], six_i, sh2_i, sh1_i, moh_i, toh_i, mdec, tdec};                               // with EXT_PD = 0 (decode bench) pd is not used
  wire i_nmi = sint && pdi[72], i_im2 = sint && pdi[73];

  // ---- opcode patterns INDEPENDENT of the space (docs/03 §1, quadrants) --------------
  // They serve the actions (qualified by `base`) and the schedule of BASE, DD and FD
  // (BASE rows reused: IX/IY transformation and the unassigned positions).
  // (definitions in rmZ80_alu_pkg::rmz80_pdec, bit by bit in the same order)
  wire x00 = pdi[0], x01 = pdi[1];   // pdi[2] x10, pdi[3] x11: used only inside rmz80_pdec
  wire src6 = pdi[4], dst6 = pdi[5];
  wire p_pfx    = pdi[6];
  wire p_ldrr   = pdi[7];
  wire p_ldrhl  = pdi[8];
  wire p_ldhlr  = pdi[9];
  wire p_ldrn   = pdi[10];
  wire p_ldhln  = pdi[11];
  wire p_alur   = pdi[12];
  wire p_aluhl  = pdi[13];
  wire p_alun   = pdi[14];
  wire p_ldabc  = pdi[15];
  wire p_ldbca  = pdi[16];
  wire p_incss  = pdi[17];
  wire p_decss  = pdi[18];
  wire p_ldsphl = pdi[19];
  wire p_exaf   = pdi[20];
  wire p_exx    = pdi[21];
  wire p_jphl   = pdi[22];
  wire p_jr     = pdi[23];
  wire p_jrcc   = pdi[24];
  wire p_djnz   = pdi[25];
  wire p_jpnn   = pdi[26];
  wire p_jpcc   = pdi[27];
  wire p_call   = pdi[28];
  wire p_callcc = pdi[29];
  wire p_ret    = pdi[30];
  wire p_retcc  = pdi[31];
  wire p_rst    = pdi[32];
  wire p_push   = pdi[33];
  wire p_pop    = pdi[34];
  wire p_lddnn  = pdi[35];
  wire p_ldann  = pdi[36];
  wire p_ldnna  = pdi[37];
  wire p_ldhlnn = pdi[38];
  wire p_ldnnhl = pdi[39];
  wire p_exsphl = pdi[40];
  wire p_exdehl = pdi[41];
  wire p_addhl  = pdi[42];
  wire p_incr   = pdi[43];
  wire p_decr   = pdi[44];
  wire p_inchl  = pdi[45];
  wire p_dechl  = pdi[46];
  wire p_rota   = pdi[47];   // 07 0F 17 1F
  wire p_daa    = pdi[48];
  wire p_cpl    = pdi[49];
  wire p_scc    = pdi[50];   // 37 SCF, 3F CCF
  wire p_in     = pdi[51];
  wire p_out    = pdi[52];

  // ---- executable families = patterns in the BASE space ---------------------------------
  // ---- indexed space DD/FD (docs/03 §3.3-§3.4, §5.2): the BASE actions apply here too, ONE copy:
  // (1) M coordinates relative to the BASE form (me1..me5: the displacement and the internal cycle of
  // the (IX+d) forms shift by 2, LD (IX+d),n by 1; the prefix is outside the count);
  // (2) substitution of the selectors on output: HL -> IX/IY (16 bits) for the indexed forms (ix_hit),
  // H/L -> IXh/IXl (8 bits) only if the instruction does not use (IX+d); the other positions (the prefix
  // "does not touch" them) run as they are, with the prefix M1 in front.
  wire has_d  = pdi[53];     // (IX+d) forms with the internal cycle
  wire ix_hit = pdi[54];
  wire bx    = base || six;                            // executable: BASE and DD/FD (with and without displacement)
  wire xd    = six && (has_d || p_ldhln);              // forms with displacement d (docs/03 §3.4)
  wire sfd   = (space == 3'd4) || (space == 3'd6);   // FD / FDCB: IY
  wire xs16  = six && ix_hit;                          // HL -> IX/IY
  wire xs8   = xs16 && !has_d && !p_ldhln;             // H/L -> IXh/IXl
  wire [2:0] ixsel = {2'b10, sfd};                     // 4 = IX, 5 = IY
  // M coordinates of the BASE form: in DD/FD the (IX+d) forms have RD d + internal cycle in front (+2),
  // LD (IX+d),n only RD d (+1). Multiplexer on the already decoded M: one LUT per signal, no subtractor
  wire sh2 = EXT_PD ? sh2_i : (six && has_d), sh1 = EXT_PD ? sh1_i : (six && p_ldhln);
  wire me1 = sh2 ? m3 : sh1 ? m2 : m1;
  wire me2 = sh2 ? m4 : sh1 ? m3 : m2;
  wire me3 = sh2 ? m5 : sh1 ? m4 : m3;
  wire me4 = sh2 ? m6 : sh1 ? m5 : m4;
  wire me5 = sh2 ? m7 : sh1 ? m6 : m5;

  wire e_ldrr = bx && p_ldrr,   e_ldrhl = bx && p_ldrhl,  e_ldhlr = bx && p_ldhlr;
  wire e_ldrn = bx && p_ldrn,   e_ldhln = bx && p_ldhln;
  wire e_alur = bx && p_alur,   e_aluhl = bx && p_aluhl,  e_alun  = bx && p_alun;
  wire e_ldabc = bx && p_ldabc, e_ldbca = bx && p_ldbca;
  wire e_incss = bx && p_incss, e_decss = bx && p_decss,  e_ldsphl = bx && p_ldsphl;
  wire e_exaf = bx && p_exaf,   e_exx = bx && p_exx,      e_jphl = bx && p_jphl;
  wire e_jr = bx && p_jr,       e_jrcc = bx && p_jrcc,    e_djnz = bx && p_djnz;
  wire e_jpnn = bx && p_jpnn,   e_jpcc = bx && p_jpcc;
  wire e_call = bx && p_call,   e_callcc = bx && p_callcc;
  wire e_ret = bx && p_ret,     e_retcc = bx && p_retcc,  e_rst = bx && p_rst;
  wire e_push = bx && p_push,   e_pop = bx && p_pop,      e_lddnn = bx && p_lddnn;
  wire e_ldann = bx && p_ldann, e_ldnna = bx && p_ldnna;
  wire e_ldhlnn = bx && p_ldhlnn, e_ldnnhl = bx && p_ldnnhl;
  wire e_exsphl = bx && p_exsphl, e_exdehl = bx && p_exdehl, e_addhl = bx && p_addhl;
  wire e_incr = bx && p_incr,   e_decr = bx && p_decr;
  wire e_inchl = bx && p_inchl, e_dechl = bx && p_dechl;
  wire e_rota = bx && p_rota,   e_daa = bx && p_daa,      e_cpl = bx && p_cpl;
  wire e_in     = bx && p_in,  e_out = bx && p_out;                  // IN A,(n) / OUT (n),A: port {A, n} in WZ
  wire e_rdpc   = e_ldrn | e_alun | e_jr | e_jrcc | e_djnz | e_ldhln | e_in | e_out;   // M2 = read at PC
  wire e_nn     = e_jpnn | e_jpcc | e_call | e_callcc;                  // nn -> WZ in M2/M3
  wire e_ldnn   = e_ldann | e_ldnna | e_ldhlnn | e_ldnnhl;
  // CB space (docs/03 §2.2): rotate/shift (00 ooo rrr), BIT (01 bbb rrr), RES (10), SET (11)
  wire cbx   = scb || sxcb;                    // CB and DDCB/FDCB: the same family of operations
  wire c_rot = cbx && x00;
  wire c_bit = cbx && x01;
  wire c_rs  = cbx && ir[7];
  wire c_hl  = src6;
  // BIT: YF/XF from the operand b (docs/05 §9): the register for BIT b,r (b = rd8), WZH for BIT b,(HL)
  // and BIT b,(IX+d) (b_src 3, below)
  wire [5:0] cb_op = c_rot ? {3'b010, ooo} : c_bit ? 6'h26 : (ir[6] ? 6'h22 : 6'h21);   // BIT always BITX: b = register / WZH

  // -- ED patterns (schedule for all; actions for each family)
  wire ed_ldai = pdi[55];    // 47 4F 57 5F (M1 5 T)
  wire ed_rld  = pdi[56];    // 67 6F
  wire ed_stnn = pdi[57];    // LD (nn),dd
  wire ed_ldnn = pdi[58];    // LD dd,(nn)
  wire ed_in   = pdi[59];
  wire ed_out  = pdi[60];
  wire ed_a16  = pdi[61];    // SBC/ADC HL,ss
  wire ed_ret  = pdi[62];    // RETN/RETI (+dup)
  wire ed_blk  = pdi[63];    // LDI CPI INI OUTI and variants
  wire ed_rep  = pdi[64];    // ...R
  wire ed_bio  = pdi[65];    // INI/OUTI (M1 5 T)
  wire ed_bout = pdi[66];    // OUTI/OTIR
  // ED space, actions
  wire s_neg  = sed && pdi[67];                                // NEG (+dup)
  wire s_im   = sed && pdi[68];                                // IM 0/1/2 (+dup)
  wire s_ldxa = sed && ed_ldai && !ir[4];                      // LD I,A (47), LD R,A (4F)
  wire s_ldai = sed && ed_ldai &&  ir[4];                      // LD A,I (57), LD A,R (5F)
  wire s_rld  = sed && ed_rld;                                 // RRD (67), RLD (6F)
  wire s_a16  = sed && ed_a16;                                 // ADC (ir[3]=1) / SBC HL,ss
  wire s_stnn = sed && ed_stnn, s_ldnn = sed && ed_ldnn, s_ret = sed && ed_ret;
  wire s_ld   = sed && pdi[74];                                // LDI LDD LDIR LDDR
  wire s_cp   = sed && pdi[75];                                // CPI CPD CPIR CPDR
  wire s_rep  = sed && ed_rep;                                 // LDIR LDDR CPIR CPDR INIR INDR OTIR OTDR
  wire s_bio  = sed && ed_bio;                                 // INI IND OUTI OUTD and ...R
  wire s_bout = sed && ed_bout;                                // OUTI OUTD OTIR OTDR
  wire s_in   = sed && ed_in,  s_out = sed && ed_out;          // IN r,(C) / IN F,(C), OUT (C),r / OUT (C),0
  wire e_di   = bx && pdi[69];                                 // DI
  wire e_ei   = bx && pdi[70];                                 // EI
  wire e_halt = bx && pdi[71];                                 // HALT

  // ---- recurring actions (text macros: one always_comb process) --------
  // PC <- PC + 1
// in the instruction supplied by the device in IM0 (int_lat outside space 7 of IM1/IM2) the PC does NOT advance: the
// first byte arrives in the IACK and the others with memory reads ([UM] p.184, [PS] p.7), and a CALL must push the
// interrupted PC ([UM] p.19: "a 3-byte call to any location in memory could be executed"; deduction, docs/04 §5)
`define PC_INC1 begin if (!(int_lat && !sint)) begin rd16_x |= (3'd6) ^ 3'd6; inc_op |= 2'd0; inc_src |= 2'd1; \
                      wr16_en |= 1'b1; wr16_sel |= 3'd6; wr16_src |= 1'b0; end end
  // SP <- SP +/- 1 (address = SP): dec = 1 decrements
`define SP_STEP(dec) begin rd16_x |= (3'd3) ^ 3'd6; inc_op |= (dec) ? 2'd1 : 2'd0; inc_src |= 2'd1; \
                           wr16_en |= 1'b1; wr16_sel |= 3'd3; wr16_src |= 1'b0; end
  // register <- DIN (through the ALU in PASS: res = a = DIN)
`define WR8_DIN(sel) begin wr8_en |= 1'b1; wr8_sel |= (sel); a_src |= 2'd2; alu_op |= 6'h0C; wr8_src |= 1'b0; end
  // AF <- ALU(A, b); with cp_only = 1 (CP) F only
`define ACC_OP(op, cp_only) begin a_src |= 2'd0; alu_op |= (op); \
        if (cp_only) begin wr8_en |= 1'b1; wr8_sel |= 5'd6; wr8_src |= 1'b1; end \
        else begin wr16_en |= 1'b1; wr16_sel |= 3'd7; wr16_src |= 1'b1; end end
  // pair <- WZ (through the adder, addend 0)
`define WR16_WZ(sel) begin inc_src |= 2'd2; inc_op |= 2'd3; wr16_en |= 1'b1; wr16_sel |= (sel); wr16_src |= 1'b0; end

  // OR-ACCUMULATED FORM: every field starts at ZERO and the sites write it with `|=`. The sites active
  // in the same tick are mutually exclusive per field (or write the same value), so the OR is exact and
  // synthesis builds flat AND-OR instead of priority chains. The address at rest is PC (code 6): rd16 is
  // accumulated as an XOR against PC (rd16_x), so that the default is zero and rd16_sel = rd16_x ^ 6. The
  // other defaults are already zero (inc_src 0 = rd16 like 1; wr16_sel/alu_op irrelevant without enable).
  logic [2:0] rd16_x;
  always_comb begin
    rd8_sel = 5'd0; rd16_x = 3'd0; addr_wz = 1'b0; dout_src = 2'd0;
    alu_op = 6'd0; a_src = 2'd0; b_src = 2'd0;
    wr8_en = 1'b0; wr8_sel = 5'd0; wr8_src = 1'b0;
    wr16_en = 1'b0; wr16_sel = 3'd0; wr16_src = 1'b0;
    inc_op = 2'd0; inc_src = 2'd0; wz_we = 2'b00; wz_src = 2'd0; cond_mode = 2'd0;
    swap_af = 1'b0; swap_gp = 1'b0; swap_dehl = 1'b0; im_we = 1'b0; im_val = 2'd0; iff_op = 2'd0; halt_set = 1'b0;

    // === M1 T3A: PC+1 of the fetch, for every opcode (the address is already the refresh address);
    //     with the CB prefix the real opcode is read in the second M1 (m_idx = 2).
    //     It does NOT advance: in HALT (M1 of NOP, docs/02 §7), in the M1 of the response to an interrupt
    //     (byte ignored), nor in the IACK of IM0 (int_lat: the instruction does not come from memory)
    if (m1 && !sxcb && !halt && !sint && !int_lat && t3 && fA) `PC_INC1   // M1 in every space (prefix or opcode; in DDCB the first M is RD d)

    // === operand reads at PC: PC+1 in T3B
    if (e_rdpc && me2 && t3 && fB) `PC_INC1

    // === displacement (IX+d): M2 RD d at PC (PC+1); M3 T1A WZ <- IX/IY + sext(d) on the address adder
    // (the same mechanism as JR: din_q holds d); the access M-cycles of the BASE form (me2/me3, for
    // LD (IX+d),n only the WR me3) address WZ instead of HL.
    // In DDCB/FDCB (after DD;CB the count restarts) d is in the first M: RD d (PC+1), RD opcode (PC+1), WZ in M2 T1A
    if (((xd && m2) || (sxcb && (m1 || m2))) && t3 && fB) `PC_INC1
    if (((xd && m3) || (sxcb && m2)) && t1 && fA) begin rd16_x |= ixsel ^ 3'd6; inc_op |= 2'd2; inc_src |= 2'd1; wz_we |= 2'b11; end
    if (xd && (has_d ? (me2 || me3) : me3)) addr_wz |= 1'b1;

    // === DDCB/FDCB: m1 RD d, m2 RD opcode -> IR (ir_ld), m3 RD 4 T at WZ, m4 WR at WZ <- res.
    // As CB on (HL): F of BIT in m3 T4A, F of the rotates in the last tick (m4 T3B). Double effect
    // (docs/03 §5.4): with rrr != 110 the result also goes into register rrr (m4 T3A; BIT writes nothing)
    if (sxcb && (m3 || m4)) begin
      addr_wz |= 1'b1; a_src |= 2'd2; alu_op |= cb_op;
      if (c_bit) b_src |= 2'd3;                                                 // BIT b,(IX+d): b = WZH for YF/XF ([SY] §4.1, docs/05 §9)
      if (c_bit && m3 && t4 && fA) begin wr8_en |= 1'b1; wr8_sel |= 5'd6; wr8_src |= 1'b1; end
      if (m4) begin
        dout_src |= 2'd1;
        if (!src6 && t3 && fA) begin wr8_en |= 1'b1; wr8_sel |= r_src; wr8_src |= 1'b0; end
        if (c_rot && t3 && fB) begin wr8_en |= 1'b1; wr8_sel |= 5'd6; wr8_src |= 1'b1; end
      end
    end

    // === 8-bit transfers
    if (e_ldrr && me1 && t4 && fA) begin              // LD r,r': dst <- rd8(src) through the ALU in PASS
      rd8_sel |= r_src; a_src |= 2'd1; alu_op |= 6'h0C; wr8_en |= 1'b1; wr8_sel |= r_dst; wr8_src |= 1'b0;
    end
    if (e_ldrhl && me2) begin                         // LD r,(HL)
      rd16_x |= (3'd2) ^ 3'd6;
      if (t3 && fA) `WR8_DIN(r_dst)
    end
    if (e_ldhlr && me2) begin                         // LD (HL),r
      rd16_x |= (3'd2) ^ 3'd6; dout_src |= 2'd0; rd8_sel |= r_src;
    end
    if (e_ldrn && me2 && t3 && fA) `WR8_DIN(r_dst)    // LD r,n
    if (e_ldhln && me3) begin                         // LD (HL),n: M3 WR at HL <- DIN
      rd16_x |= (3'd2) ^ 3'd6; dout_src |= 2'd2;
    end
    // MEMPTR/WZ (SingleStepTests; docs/01 §7): after LD A,(rp) WZ = rp+1; after LD (rp),A WZ = {A, (rp+1)&FF};
    // likewise for (nn); after LD rp,(nn)/LD (nn),rp WZ = nn+1; ADD/ADC/SBC HL: WZ = HL+1; RLD/RRD: HL+1;
    // JR/DJNZ taken: target; RET*: popped PC; IN A,(n): {A,n}+1; OUT (n),A: {A, n+1}; IN/OUT (C): BC+1;
    // OUTI/OUTD: BC(B after)+-1; CPI/CPD: WZ+-1; repeating forms that repeat: PC+1. EX DE,HL does not touch WZ.
    if (e_ldabc && me2) begin                         // LD A,(BC)/(DE)
      rd16_x |= ({1'b0, dd}) ^ 3'd6;
      if (t3 && fA) `WR8_DIN(5'd7)
      if (t3 && fB) begin inc_op |= 2'd0; inc_src |= 2'd1; wz_we |= 2'b11; end          // WZ <- rp+1
    end
    if (e_ldbca && me2) begin                         // LD (BC)/(DE),A
      rd16_x |= ({1'b0, dd}) ^ 3'd6; dout_src |= 2'd0; rd8_sel |= 5'd7;
      if (t3 && fB) begin inc_op |= 2'd0; inc_src |= 2'd1; wz_we |= 2'b11; wz_src |= 2'd2; end   // WZ <- {A, (rp+1)&FF}
    end

    // === ALU on the accumulator
    if (e_alur && me1 && t4 && fA) begin              // ALU A,r
      rd8_sel |= r_src; b_src |= 2'd0; `ACC_OP({3'b000, ooo}, ooo == 3'd7)
    end
    if (e_aluhl && me2) begin                         // ALU A,(HL)
      rd16_x |= (3'd2) ^ 3'd6;
      if (t3 && fA) begin b_src |= 2'd1; `ACC_OP({3'b000, ooo}, ooo == 3'd7) end
    end
    if (e_alun && me2 && t3 && fA) begin              // ALU A,n (all 8 operations)
      b_src |= 2'd1; `ACC_OP({3'b000, ooo}, ooo == 3'd7)
    end
    if ((e_rota || e_daa || e_cpl) && me1 && t4 && fA)   // RLCA..RRA, DAA, CPL
      `ACC_OP(e_rota ? {3'b011, 1'b0, ir[4:3]} : (e_daa ? 6'h1C : 6'h1D), 1'b0)
    if (bx && p_scc && me1 && t4 && fA)                  // SCF (1Eh) / CCF (1Fh): F only; YF/XF = Q|A (NMOS) or A (CMOS)
      `ACC_OP({5'b01111, ir[3]}, 1'b1)
    if ((e_incr || e_decr) && me1 && t4) begin        // INC/DEC r: T4A F, T4B r
      rd8_sel |= r_dst; a_src |= 2'd1; alu_op |= e_incr ? 6'h08 : 6'h09; wr8_en |= 1'b1;
      if (fA) begin wr8_sel |= 5'd6; wr8_src |= 1'b1; end
      else    begin wr8_sel |= r_dst; wr8_src |= 1'b0; end
    end
    if ((e_inchl || e_dechl) && (me2 || me3)) begin    // INC/DEC (HL): RD 4 T, WR
      rd16_x |= (3'd2) ^ 3'd6; a_src |= 2'd2; alu_op |= e_inchl ? 6'h08 : 6'h09;
      if (me2 && t4 && fA) begin wr8_en |= 1'b1; wr8_sel |= 5'd6; wr8_src |= 1'b1; end
      if (me3) dout_src |= 2'd1;
    end

    // === 16 bits
    if ((e_incss || e_decss) && me1 && t5 && fA) begin   // INC/DEC ss
      rd16_x |= ({1'b0, dd}) ^ 3'd6; inc_op |= e_decss ? 2'd1 : 2'd0; inc_src |= 2'd1;
      wr16_en |= 1'b1; wr16_sel |= {1'b0, dd}; wr16_src |= 1'b0;
    end
    if (e_ldsphl && me1 && t5 && fA) begin            // LD SP,HL
      rd16_x |= (3'd2) ^ 3'd6; inc_op |= 2'd3; inc_src |= 2'd1;
      wr16_en |= 1'b1; wr16_sel |= 3'd3; wr16_src |= 1'b0;
    end
    if (e_lddnn && (me2 || me3)) begin                 // LD dd,nn: low, high, PC+1
      if (t3 && fA) `WR8_DIN(me2 ? lo8_dd : hi8_dd)
      if (t3 && fB) `PC_INC1
    end
    if (e_jphl && me1 && t4 && fB) begin              // JP (HL): PC <- HL
      rd16_x |= (3'd2) ^ 3'd6; inc_op |= 2'd3; inc_src |= 2'd1;
      wr16_en |= 1'b1; wr16_sel |= 3'd6; wr16_src |= 1'b0;
    end
    if (e_exaf && me1 && t4 && fA) swap_af |= 1'b1;    // EX AF,AF'
    if (e_exx  && me1 && t4 && fA) swap_gp |= 1'b1;    // EXX
    // EX DE,HL = RENAMING: no data movement and MEMPTR untouched. The xhl bit of the active bank (bit 2/3 of the
    // Q field, in the savestate) toggles; while it is 1 the selectors DE<->HL (and D/E<->H/L) are swapped on output
    if (e_exdehl && me1 && t4 && fA) swap_dehl |= 1'b1;
    if (e_addhl) begin                               // ADD HL,ss in TWO 8-bit steps:
      if (me1 && t4 && fA) begin rd16_x |= (3'd2) ^ 3'd6; wz_we |= 2'b11; inc_op |= 2'd3; inc_src |= 2'd1; end  // WZ <- HL
      if (me2 && t3 && fA) begin                      // M2 (internal): L <- lo(ss) + WZL, F: C only
        rd8_sel |= lo8_dd; a_src |= 2'd1; b_src |= 2'd2; alu_op |= 6'h28;
        wr8_en |= 1'b1; wr8_sel |= 5'd5; wr8_src |= 1'b0;
        wr16_en |= 1'b1; wr16_sel |= 3'd7; wr16_src |= 1'b1;          // F through AF8 (A kept)
      end
      if (me3 && t1 && fA) begin                      // M3 (internal): H <- hi(ss) + WZH + C, flags of the high step
        rd8_sel |= hi8_dd; a_src |= 2'd1; b_src |= 2'd3; alu_op |= 6'h29;
        wr8_en |= 1'b1; wr8_sel |= 5'd4; wr8_src |= 1'b0;
        wr16_en |= 1'b1; wr16_sel |= 3'd7; wr16_src |= 1'b1;
      end
      if (me3 && t3 && fB) begin inc_src |= 2'd2; inc_op |= 2'd0; wz_we |= 2'b11; end   // MEMPTR: WZ <- HL+1
    end

    // === relative jumps and DJNZ
    if (e_djnz && me1 && t4 && fA) begin              // B <- B-1 without touching F
      rd8_sel |= 5'd0; a_src |= 2'd1; alu_op |= 6'h09; wr8_en |= 1'b1; wr8_sel |= 5'd0; wr8_src |= 1'b0;
    end
    if (e_djnz && me2 && t3 && fB) begin rd8_sel |= 5'd0; cond_mode |= 2'd2; end   // B != 0
    if (e_jrcc && me2 && t3 && fB) cond_mode |= 2'd1;                              // cc of JR on F
    if ((e_jr || e_jrcc || e_djnz) && me3 && t1 && fA) begin   // PC <- PC + sext(e) on the address adder; MEMPTR = target
      rd16_x |= (3'd6) ^ 3'd6; inc_op |= 2'd2; inc_src |= 2'd1;
      wr16_en |= 1'b1; wr16_sel |= 3'd6; wr16_src |= 1'b0; wz_we |= 2'b11;
    end

    // === absolute jumps, CALL, RET, RST (stack and WZ conventions in the header of this file)
    if ((e_nn || e_ldnn) && (me2 || me3)) begin        // nn -> WZ (low, high), PC+1
      rd16_x |= (3'd6) ^ 3'd6;
      if (t3 && fA) wz_we |= me2 ? 2'b01 : 2'b10;
      if (t3 && fB) begin                            // EXCLUSIVE sites (OR form): jump or PC+1
        // JP cc: here the "false" variant (PC+1); if the condition on F is true the top module replaces
        // inc_src/inc_op with those of PC <- WZ (jpcc_t): the control word does not depend on F
        if (me3 && e_jpnn) `WR16_WZ(3'd6)                          // JP nn: PC <- WZ
        else `PC_INC1                                              // JP cc (false here), or nn still to be read
      end
      if (me3 && t4 && fB && (e_call || e_callcc)) `SP_STEP(1'b1)  // CALL: SP-1 in the internal T4
    end
    if ((e_call || e_callcc) && (me4 || me5)) begin    // push PCH at SP, SP-1; push PCL at SP; PC <- WZ
      rd16_x |= (3'd3) ^ 3'd6; dout_src |= 2'd0; rd8_sel |= me4 ? 5'd14 : 5'd15;
      if (me4 && t3 && fB) `SP_STEP(1'b1)
      if (me5 && t3 && fB) `WR16_WZ(3'd6)
    end
    if ((e_ret || e_retcc) && (me2 || me3)) begin      // PCL <- (SP), SP+1; PCH <- (SP), SP+1; MEMPTR = popped PC
      rd16_x |= (3'd3) ^ 3'd6;
      if (t3 && fA) begin `WR8_DIN(me2 ? 5'd15 : 5'd14) wz_we |= me2 ? 2'b01 : 2'b10; end
      if (t3 && fB) `SP_STEP(1'b0)
    end
    if ((e_push || e_rst) && me1 && t5 && fB) `SP_STEP(1'b1)   // pre-decrement
    if ((e_push || e_rst) && (me2 || me3)) begin
      rd16_x |= (3'd3) ^ 3'd6; dout_src |= 2'd0;
      rd8_sel |= e_rst ? (me2 ? 5'd14 : 5'd15) : (me2 ? hi8_qq : lo8_qq);
      if (e_rst && me2 && t3 && fA) begin wz_we |= 2'b11; inc_src |= 2'd3; inc_op |= 2'd3; end   // WZ <- vector
      if (me2 && t3 && fB) `SP_STEP(1'b1)
      if (e_rst && me3 && t3 && fB) `WR16_WZ(3'd6)
    end
    if (e_pop && (me2 || me3)) begin                   // low <- (SP), SP+1; high <- (SP), SP+1
      rd16_x |= (3'd3) ^ 3'd6;
      if (t3 && fA) `WR8_DIN(me2 ? lo8_qq : hi8_qq)
      if (t3 && fB) `SP_STEP(1'b0)
    end

    // === LD with an extended address (after nn -> WZ in M2/M3)
    if ((e_ldann || e_ldnna) && me4) begin
      addr_wz |= 1'b1;
      if (e_ldann && t3 && fA) `WR8_DIN(5'd7)
      if (e_ldnna) begin dout_src |= 2'd0; rd8_sel |= 5'd7; end
      if (t3 && fB) begin inc_src |= 2'd2; inc_op |= 2'd0; wz_we |= 2'b11; if (e_ldnna) wz_src |= 2'd2; end   // MEMPTR: nn+1 / {A, (nn+1)&FF}
    end
    if ((e_ldhlnn || e_ldnnhl) && (me4 || me5)) begin
      addr_wz |= 1'b1;
      if (e_ldhlnn && t3 && fA) `WR8_DIN(me4 ? 5'd5 : 5'd4)
      if (e_ldnnhl) begin dout_src |= 2'd0; rd8_sel |= me4 ? 5'd5 : 5'd4; end
      if (me4 && t3 && fB) begin wz_we |= 2'b11; inc_op |= 2'd0; inc_src |= 2'd2; end   // WZ+1
    end

    // === CB space — rotate/shift, BIT, RES, SET
    // on a register (second M1, T4A, one tick): r <- res through wr8 and F through wr16 AF with A kept
    // (AF8 keeps A when wr8 is active): RL/RR use the old C both in the result and in the flags, so no
    // second tick is needed
    if (scb && !c_hl && m1 && t4 && fA) begin
      rd8_sel |= r_src; a_src |= 2'd1; alu_op |= cb_op;
      if (c_rot || c_rs) begin wr8_en |= 1'b1; wr8_sel |= r_src; wr8_src |= 1'b0; end   // r <- res
      if (c_rot) begin wr16_en |= 1'b1; wr16_sel |= 3'd7; wr16_src |= 1'b1; end          // F (A kept)
      if (c_bit) begin wr8_en |= 1'b1; wr8_sel |= 5'd6; wr8_src |= 1'b1; end             // F only
    end
    // on (HL): M2 = RD at HL 4 T; M3 = WR at HL <- res (rotates, RES/SET). F: BIT in M3 T4A;
    // rotates in the LAST tick of M3 (T3B), so that res on the bus uses the old C
    if (scb && c_hl && (m2 || m3)) begin
      rd16_x |= (3'd2) ^ 3'd6; a_src |= 2'd2; alu_op |= cb_op;
      if (c_bit) b_src |= 2'd3;                                                 // BIT b,(HL): YF/XF from WZH (MEMPTR)
      if (c_bit && m2 && t4 && fA) begin wr8_en |= 1'b1; wr8_sel |= 5'd6; wr8_src |= 1'b1; end
      if (m3) begin
        dout_src |= 2'd1;
        if (c_rot && t3 && fB) begin wr8_en |= 1'b1; wr8_sel |= 5'd6; wr8_src |= 1'b1; end
      end
    end

    // === EX (SP),HL
    if (e_exsphl && (me2 || me3 || me4 || me5)) begin
      rd16_x |= (3'd3) ^ 3'd6;
      if (me2 && t3 && fA) wz_we |= 2'b01;
      if (me2 && t3 && fB) `SP_STEP(1'b0)
      if (me3 && t3 && fA) wz_we |= 2'b10;
      if (me4) begin dout_src |= 2'd0; rd8_sel |= 5'd4; if (t3 && fB) `SP_STEP(1'b1) end
      if (me5) begin dout_src |= 2'd0; rd8_sel |= 5'd5;
        if (t5 && fB) `WR16_WZ(3'd2) end
    end

    // === ED space: m1 = the M1 of the opcode
    if (s_neg && m1 && t4 && fA) `ACC_OP(6'h0A, 1'b0)                     // NEG: A <- 0 - A
    if (s_im && m1 && t4 && fA) begin im_we |= 1'b1; im_val |= ir[4] ? (ir[3] ? 2'd2 : 2'd1) : 2'd0; end
    // LD I,A / LD R,A (through PASS) in T5A: the write of R falls AFTER the refresh increment of this M1 (T4B),
    // so R = A as on the silicon (z80test LD A,R: LD R,A then LD A,R must give A+2, not A+3)
    if (s_ldxa && m1 && t5 && fA) begin
      rd8_sel |= 5'd7; a_src |= 2'd1; alu_op |= 6'h0C;
      wr8_en |= 1'b1; wr8_sel |= ir[3] ? 5'd17 : 5'd16; wr8_src |= 1'b0;
    end
    if (s_a16) begin                                 // ADC/SBC HL,ss: WZ <- HL (as ADD HL: MEMPTR = HL+1); ss from rd8;
      if (m1 && t4 && fA) begin rd16_x |= (3'd2) ^ 3'd6; wz_we |= 2'b11; inc_op |= 2'd3; inc_src |= 2'd1; end   // SBC reversed (WZ - ss - C)
      if (m2 && t3 && fA) begin
        rd8_sel |= lo8_dd; a_src |= 2'd1; b_src |= 2'd2; alu_op |= ir[3] ? 6'h01 : 6'h31;
        wr8_en |= 1'b1; wr8_sel |= 5'd5; wr8_src |= 1'b0; wr16_en |= 1'b1; wr16_sel |= 3'd7; wr16_src |= 1'b1;
      end
      if (m3 && t1 && fA) begin
        rd8_sel |= hi8_dd; a_src |= 2'd1; b_src |= 2'd3; alu_op |= ir[3] ? 6'h2A : 6'h32;
        wr8_en |= 1'b1; wr8_sel |= 5'd4; wr8_src |= 1'b0; wr16_en |= 1'b1; wr16_sel |= 3'd7; wr16_src |= 1'b1;
      end
      if (m3 && t3 && fB) begin inc_src |= 2'd2; inc_op |= 2'd0; wz_we |= 2'b11; end   // MEMPTR: WZ <- HL+1
    end
    if ((s_stnn || s_ldnn) && (m2 || m3)) begin      // nn -> WZ (low, high), PC+1
      if (t3 && fA) wz_we |= m2 ? 2'b01 : 2'b10;
      if (t3 && fB) `PC_INC1
    end
    if ((s_stnn || s_ldnn) && (m4 || m5)) begin      // (WZ) low, (WZ+1) high
      addr_wz |= 1'b1;
      if (s_ldnn && t3 && fA) `WR8_DIN(m4 ? lo8_dd : hi8_dd)
      if (s_stnn) begin dout_src |= 2'd0; rd8_sel |= m4 ? lo8_dd : hi8_dd; end
      if (m4 && t3 && fB) begin wz_we |= 2'b11; inc_op |= 2'd0; inc_src |= 2'd2; end   // WZ+1
    end
    if (s_ret && (m2 || m3)) begin                   // RETN/RETI = RET plus IFF1 <- IFF2 (docs/04 §6)
      rd16_x |= (3'd3) ^ 3'd6;
      if (t3 && fA) begin `WR8_DIN(m2 ? 5'd15 : 5'd14) wz_we |= m2 ? 2'b01 : 2'b10; end   // MEMPTR = popped PC
      if (t3 && fB) `SP_STEP(1'b0)
      if (m3 && t3 && fB) iff_op |= 2'd2;                                     // RETN, RETI and the duplicates: IFF1 <- IFF2 ([SY] §5.3; SingleStepTests)
    end
    // DI/EI in the LAST tick: the sequencer closes the INT window at the end of the instruction (docs/04 §3)
    if (e_di && me1 && t4 && fB) iff_op |= 2'd1;      // DI: IFF1 = IFF2 = 0
    if (e_ei && me1 && t4 && fB) iff_op |= 2'd3;      // EI: IFF1 = IFF2 = 1
    if (e_halt && me1 && t4 && fA) halt_set |= 1'b1;  // HALT: from here M1 of NOP with PC held
    // === response to an interrupt (space 7, synthetic IR) — docs/04 §4-§5
    //     NMI 66h: M1 5 T (byte ignored, PC held), push PCH, push PCL, PC <- 0066 (11 T)
    //     IM1 38h: IACK 7 T (4 + 2 waits + 1), push, push, PC <- 0038 (13 T)
    //     IM2 7Eh: IACK 7 T (8-bit vector v in DIN), WZ <- PC (T6A), PCH <- I (T7A), PCL <- v (T7B),
    //              push WZH, push WZL (the saved PC), RD (PC) -> WZL, PC+1, RD (PC) -> WZH, PC <- WZ (19 T)
    if (sint) begin
      if (m1 && fB && (i_nmi ? t5 : t7)) `SP_STEP(1'b1)                  // pre-decrement in the last tick of M1
      if (i_im2 && m1) begin
        if (t6 && fA) begin wz_we |= 2'b11; inc_op |= 2'd3; inc_src |= 2'd1; end          // WZ <- PC (rd16 = PC at rest)
        if (t7 && fA) begin rd8_sel |= 5'd16; a_src |= 2'd1; alu_op |= 6'h0C;               // PCH <- I
                            wr8_en |= 1'b1; wr8_sel |= 5'd14; wr8_src |= 1'b0; end
        if (t7 && fB) `WR8_DIN(5'd15)                                                       // PCL <- vector
      end
      if (m2 || m3) begin
        rd16_x |= (3'd3) ^ 3'd6; dout_src |= 2'd0;
        if (i_im2) b_src |= m2 ? 2'd3 : 2'd2;                            // data = WZH, WZL (return PC)
        else rd8_sel |= m2 ? 5'd14 : 5'd15;                              // data = PCH, PCL
        if (m2 && t3 && fB) `SP_STEP(1'b1)
        if (m3 && t3 && fB && !i_im2) begin inc_src |= 2'd3; inc_op |= 2'd3; wr16_en |= 1'b1; wr16_sel |= 3'd6; wr16_src |= 1'b0; end   // PC <- vector
      end
      if (i_im2 && (m4 || m5)) begin                                     // table: (PC), (PC+1) -> WZ -> PC
        if (t3 && fA) wz_we |= m4 ? 2'b01 : 2'b10;
        if (m4 && t3 && fB) `PC_INC1
        if (m5 && t3 && fB) `WR16_WZ(3'd6)
      end
    end
    if ((s_ld || s_cp) && m2) rd16_x |= (3'd2) ^ 3'd6;                    // RD at HL
    if (s_ld && m3) begin                            // WR at DE 5 T: DE+-1 T3B, HL+-1 T4A, BC-1 T4B, F T5A
      if (t1 || t2 || t3) begin rd16_x |= (3'd1) ^ 3'd6; dout_src |= 2'd2; end
      if (t3 && fB) begin inc_op |= {1'b0, ir[3]}; inc_src |= 2'd1; wr16_en |= 1'b1; wr16_sel |= 3'd1; wr16_src |= 1'b0; end
      if (t4 && fA) begin rd16_x |= (3'd2) ^ 3'd6; inc_op |= {1'b0, ir[3]}; inc_src |= 2'd1; wr16_en |= 1'b1; wr16_sel |= 3'd2; wr16_src |= 1'b0; end
      if (t4 && fB) begin rd16_x |= (3'd0) ^ 3'd6; inc_op |= 2'd1; inc_src |= 2'd1; wr16_en |= 1'b1; wr16_sel |= 3'd0; wr16_src |= 1'b0; end
      if (t5 && fA) begin rd16_x |= (3'd0) ^ 3'd6; b_src |= 2'd1; alu_op |= 6'h23;   // F <- LDI-F(A, DIN, BC)
                          wr8_en |= 1'b1; wr8_sel |= 5'd6; wr8_src |= 1'b1; end
    end
    if (s_cp && m3) begin                            // internal 5 T: BC-1 T1A, HL+-1 T1B, F T2A
      if (t1 && fA) begin rd16_x |= (3'd0) ^ 3'd6; inc_op |= 2'd1; inc_src |= 2'd1; wr16_en |= 1'b1; wr16_sel |= 3'd0; wr16_src |= 1'b0; end
      if (t1 && fB) begin rd16_x |= (3'd2) ^ 3'd6; inc_op |= {1'b0, ir[3]}; inc_src |= 2'd1; wr16_en |= 1'b1; wr16_sel |= 3'd2; wr16_src |= 1'b0; end
      if (t2 && fA) begin rd16_x |= (3'd0) ^ 3'd6; b_src |= 2'd1; alu_op |= 6'h24;   // F <- CPI-F(A, DIN, BC)
                          wr8_en |= 1'b1; wr8_sel |= 5'd6; wr8_src |= 1'b1; end
      if (t2 && fB) begin inc_src |= 2'd2; inc_op |= {1'b0, ir[3]}; wz_we |= 2'b11; end   // MEMPTR: WZ <- WZ+-1
    end
    if (s_rep && m3 && fB && (s_bio ? (s_bout ? t4 : t3) : t5)) begin     // last tick of the 3rd M: repeats if ...
      if (s_bio) begin cond_mode |= 2'd2; rd8_sel |= 5'd0; end           // ... B != 0 (INIR/OTIR: B already decremented, read directly)
      else cond_mode |= 2'd3;                                              // ... BC != 0 (LDIR) / BC != 0 and A != (HL) (CPIR)
    end
    if (s_rep && m4 && t1) begin                     // PC <- PC - 2 in two ticks (T1A, T1B)
      inc_op |= 2'd1; inc_src |= 2'd1; wr16_en |= 1'b1; wr16_sel |= 3'd6; wr16_src |= 1'b0;
    end
    if (s_rep && m4) begin                           // repeating forms that REPEAT: MEMPTR = PC+1; YF/XF from bits 5/3 of PCH
      if (t3 && fA) begin inc_op |= 2'd0; inc_src |= 2'd1; wz_we |= 2'b11;
                          b_src |= 2'd0; rd8_sel |= 5'd14; alu_op |= 6'h2F; wr8_en |= 1'b1; wr8_sel |= 5'd6; wr8_src |= 1'b1; end
      if (s_bio && t3 && fB) begin rd8_sel |= 5'd0; a_src |= 2'd1; alu_op |= 6'h30; wr8_en |= 1'b1; wr8_sel |= 5'd6; wr8_src |= 1'b1; end   // I/O: H and P from B, C, N
    end

    // === I/O. Port address on the bus: IN A,(n)/OUT (n),A = {A, n} built in WZ (M1 T4A: WZ <- AF from the
    // adder, the bus in T4 belongs to the refresh; M2 T3B: WZL <- n); IN r,(C), OUT (C),r and the block
    // instructions = BC. Flags of IN r,(C): TST (S/Z/P from the byte, H=N=0, C unchanged). Flags of the block
    // I/O instructions: S/Z/YF/XF from the decremented B, N, H, C and P/V from the formulas of [SY] §4.3
    // (docs/05 §9). INI/IND: port with B BEFORE the decrement, B-1 in the last tick of the IORD; OUTI/OUTD:
    // B-1 in M1 T5A, port with B-1 ([UM] pp.298/309, docs/03 note n2).
    if ((e_in || e_out) && me1 && t4 && fA) begin rd16_x |= (3'd7) ^ 3'd6; inc_op |= 2'd3; inc_src |= 2'd1; wz_we |= 2'b11; end
    if ((e_in || e_out) && me2 && t3 && fB) wz_we |= 2'b01;                       // WZL <- n (with the PC+1 of e_rdpc)
    if ((e_in || e_out) && me3) begin
      addr_wz |= 1'b1;
      if (e_in && t4 && fA) `WR8_DIN(5'd7)                                          // A <- port, F unchanged
      if (e_out) begin dout_src |= 2'd0; rd8_sel |= 5'd7; end
      if (t4 && fB) begin inc_src |= 2'd2; inc_op |= 2'd0; wz_we |= 2'b11; if (e_out) wz_src |= 2'd2; end   // MEMPTR: {A,n}+1 / {A, n+1}
    end
    if ((s_in || s_out) && m2) begin                                              // IORD/IOWR at BC 4 T
      rd16_x |= (3'd0) ^ 3'd6;
      if (s_in && t4 && fA) begin                                                 // r <- byte (rrr != 6), F <- TST
        a_src |= 2'd2; alu_op |= 6'h0D;
        if (dst6) begin wr8_en |= 1'b1; wr8_sel |= 5'd6; wr8_src |= 1'b1; end    // IN F,(C): F only (AF8 would keep A only with wr8 active)
        else begin wr16_en |= 1'b1; wr16_sel |= 3'd7; wr16_src |= 1'b1; wr8_en |= 1'b1; wr8_sel |= r_dst; wr8_src |= 1'b0; end
      end
      if (s_out) begin dout_src |= dst6 ? 2'd3 : 2'd0; rd8_sel |= r_dst; end     // OUT (C),0: variant constant
      if (t1 && fA) begin inc_op |= 2'd0; inc_src |= 2'd1; wz_we |= 2'b11; end     // MEMPTR: BC+1 (before IN r,(C) writes B)
    end
    if (s_bio) begin
      // B <- B-1 (without F): OUTI in M1 T5A, INI in the last T of the IORD (T4A). F from the formulas of [SY] §4.3:
      // INI/IND: WZ <- BC+-1 in T1A of the IORD (WZL = (C+-1)&FF), then F <- INI-F(byte, WZL, B still in the register)
      // in T4B; OUTI/OUTD: F <- OUTI-F(byte, L already updated, B after) in T4A of the IOWR
      if ((s_bout ? (m1 && t5) : (m2 && t4)) && fA) begin
        rd8_sel |= 5'd0; a_src |= 2'd1; alu_op |= 6'h09; wr8_en |= 1'b1; wr8_sel |= 5'd0; wr8_src |= 1'b0;
      end
      if (!s_bout && m2 && t1 && fA) begin inc_op |= {1'b0, ir[3]}; inc_src |= 2'd1; wz_we |= 2'b11; end   // WZ <- BC+-1 (rd16 = BC already for the port)
      if (!s_bout && m2 && t4 && fB) begin a_src |= 2'd2; b_src |= 2'd2; alu_op |= 6'h2D; wr8_en |= 1'b1; wr8_sel |= 5'd6; wr8_src |= 1'b1; end
      if ( s_bout && m3 && t4 && fA) begin a_src |= 2'd2; b_src |= 2'd0; rd8_sel |= 5'd5; alu_op |= 6'h2E; wr8_en |= 1'b1; wr8_sel |= 5'd6; wr8_src |= 1'b1; end
      if (s_bout ? m3 : m2) rd16_x |= (3'd0) ^ 3'd6;                              // port at BC
      if (s_bout && m3 && t1 && fA) begin inc_op |= {1'b0, ir[3]}; inc_src |= 2'd1; wz_we |= 2'b11; end   // MEMPTR: BC(B after)+-1
      if (s_bout ? m2 : m3) begin                                                 // memory at HL, HL+-1 in T3A
        rd16_x |= (3'd2) ^ 3'd6;
        if (t3 && fA) begin inc_op |= {1'b0, ir[3]}; inc_src |= 2'd1; wr16_en |= 1'b1; wr16_sel |= 3'd2; wr16_src |= 1'b0; end
      end
      if (m3) dout_src |= 2'd2;                                                   // data = din_q (INI: WR; OUTI: IOWR)
    end

    // === LD A,I / LD A,R: M1 5 T; A <- I/R with S/Z from the byte, H=N=0, P/V = IFF2 (pv_in), C unchanged
    if (s_ldai && m1 && t5 && fA) begin rd8_sel |= ir[3] ? 5'd17 : 5'd16; a_src |= 2'd1; `ACC_OP(6'h25, 1'b0) end
    // === RLD/RRD: RD (HL); internal cycle 4 T; WR (HL) <- RLD-M/RRD-M(A, m) with A still unchanged;
    //     A and F (RLD-A/RRD-A) in the last tick of the WR: they go through P1 after the byte on the bus has been sampled
    if (s_rld && m2) rd16_x |= (3'd2) ^ 3'd6;
    if (s_rld && m4) begin
      rd16_x |= (3'd2) ^ 3'd6; b_src |= 2'd1; dout_src |= 2'd1;
      if (t3 && fB) begin `ACC_OP(ir[3] ? 6'h27 : 6'h2B, 1'b0) inc_op |= 2'd0; inc_src |= 2'd1; wz_we |= 2'b11; end   // + MEMPTR: HL+1
      else alu_op |= ir[3] ? 6'h0E : 6'h0F;
    end

    wr8_raw = wr8_sel;  wr16_raw = wr16_sel;         // raw, before the substitutions
    rd16_sel = rd16_x ^ 3'd6;                        // PC at rest
    // IX/IY substitution (docs/03 §3.4, §5.2): HL -> IX/IY (16 bits) in the indexed forms,
    // H/L -> IXh/IXl (8 bits) only without (IX+d); codes 4/5 -> 8/9 (IX) or 10/11 (IY)
    if (xs16 && rd16_sel == 3'd2) rd16_sel = ixsel;
    if (xs16 && wr16_sel == 3'd2) wr16_sel = ixsel;
    // 8-bit READ port: the substitutions (IX/IY, DE<->HL) are made by the register file on the data side
    // (rd8_ix/rd8_fd/rd8_xhl): the selector cone stays short (path ir -> me -> rd8_sel -> mux -> P1)
    if (xs8 && wr8_sel[4:1] == 4'b0010) wr8_sel = {3'b010, sfd, wr8_sel[0]};
    // DE<->HL renaming after EX DE,HL (after the IX/IY substitution: the logical code HL under DD/FD is already IX/IY)
    if (xhl) begin
      if (rd16_sel[2:0] == 3'd1 || rd16_sel[2:0] == 3'd2) rd16_sel = rd16_sel ^ 3'd3;
      if (wr16_sel[2:0] == 3'd1 || wr16_sel[2:0] == 3'd2) wr16_sel = wr16_sel ^ 3'd3;
      if (wr8_sel[4:3] == 2'b00 && (wr8_sel[2] ^ wr8_sel[1])) wr8_sel = wr8_sel ^ 5'b00110;
    end
  end
  assign rd8_ix  = xs8;
  assign jpcc_t  = (e_nn || e_ldnn) && me3 && t3 && fB && e_jpcc;
  assign rd16_raw = rd16_x;
  assign rd16_ix  = xs16;
  assign rd16_xhl = xhl;
  assign rd8_fd  = sfd;
  assign rd8_xhl = xhl;

  // =============================================================================
  // SCHEDULE (docs/06 §3.4) for ALL 1792 opcode positions: the row of the current M-cycle.
  // Coordinates: m=1 is always the M1 of the opcode (after a prefix the sequencer restarts from 1;
  // docs/03 §3.3); k = M relative to the M1 of the opcode (0 = that M1). DEFAULT: M1 of the opcode
  // 4 T; every following M = RD 3 T. Everything else (long M1, NONE, WR, I/O, gate/ext/rpt, number of
  // M-cycles) are EXCEPTIONS enumerated by pattern, from Appendix A (docs/03; decode bench).
  // DD/FD = BASE rows + (IX+d) transformation: RD d + NONE 5 in front of the (HL) forms (RD 5 for
  // LD (IX+d),n); the unassigned positions = BASE rows with the prefix M1 in front (docs/06 §4.2). Where
  // Appendix A gives no cycle type (ED block instructions), the type follows docs/03 note n2.
  // =============================================================================
  localparam logic [2:0] MT_NONE = 3'd0, MT_M1 = 3'd1, MT_RD = 3'd2,
                         MT_WR = 3'd3, MT_IORD = 3'd4, MT_IOWR = 3'd5, MT_IACK = 3'd6;

  // M relative to the M1 of the opcode, in EVERY space: after the M1 of a prefix the sequencer restarts
  // from 1, so the prefix never enters the count of the M-cycles of the instruction
  wire [2:0] k   = m_idx - 3'd1;
  wire k0 = (k == 3'd0), k1 = (k == 3'd1), k2 = (k == 3'd2), k3 = (k == 3'd3);

  // -- BASE pattern: M-cycles after the M1 (b_nk), length of the M1 of the opcode (b_m1x = T-3: 1=4 2=5 3=6), bytes
  logic [2:0] b_nk, b_m1x; logic [1:0] b_nb;
  always_comb begin
    b_nk = 3'd0; b_m1x = 3'd1; b_nb = 2'd1;
    if (p_ldrhl | p_aluhl | p_ldabc | p_ldhlr | p_ldbca | p_pfx) b_nk = 3'd1;
    if (p_ldrn | p_alun)                        begin b_nk = 3'd1; b_nb = 2'd2; end
    if (p_ldhln | p_in | p_out | p_jr | p_jrcc | p_djnz) begin b_nk = 3'd2; b_nb = 2'd2; end
    if (p_inchl | p_dechl | p_ret | p_pop | p_push | p_rst | p_retcc | p_addhl) b_nk = 3'd2;
    if (p_lddnn | p_jpnn | p_jpcc)              begin b_nk = 3'd2; b_nb = 2'd3; end
    if (p_ldann | p_ldnna)                      begin b_nk = 3'd3; b_nb = 2'd3; end
    if (p_ldhlnn | p_ldnnhl | p_call | p_callcc) begin b_nk = 3'd4; b_nb = 2'd3; end
    if (p_exsphl)                               b_nk = 3'd4;
    if (p_incss | p_decss | p_ldsphl)           b_m1x = 3'd3;
    if (p_push | p_rst | p_retcc | p_djnz)      b_m1x = 3'd2;
  end

  // -- BASE row at relative M kb >= 1: {type[2:0], dur[2:0], gate, ext}
  function automatic logic [7:0] brow(input logic [2:0] kb);
    logic [2:0] t, d; logic g, e, q1, q2, q3, q4;
    q1 = (kb == 3'd1); q2 = (kb == 3'd2); q3 = (kb == 3'd3); q4 = (kb == 3'd4);
    t = MT_RD; d = 3'd0; g = 1'b0; e = 1'b0;
    if (p_pfx)                                   t = MT_M1;            // second M1 (benches only)
    if (p_ldhlr | p_ldbca | p_push | p_rst)      t = MT_WR;
    if ((p_ldhln | p_inchl | p_dechl) & q2)      t = MT_WR;
    if ((p_inchl | p_dechl) & q1)                d = 3'd1;             // RD 4 T
    if (p_retcc)                                 g = 1'b1;
    if (p_addhl)                                 begin t = MT_NONE; d = q1 ? 3'd1 : 3'd0; end
    if ((p_jr | p_jrcc | p_djnz) & q2)           begin t = MT_NONE; d = 3'd2; g = ~p_jr; end
    if ((p_ldnna & q3) | (p_ldnnhl & (q3 | q4))) t = MT_WR;
    if ((p_call | p_callcc) & q2)                begin d = 3'd1; e = p_callcc; end
    if ((p_call | p_callcc) & (q3 | q4))         begin t = MT_WR; g = p_callcc; end
    if (p_exsphl & q2)                           d = 3'd1;
    if (p_exsphl & (q3 | q4))                    begin t = MT_WR; d = q4 ? 3'd2 : 3'd0; end
    if (p_out & q2)                              begin t = MT_IOWR; d = 3'd1; end
    if (p_in & q2)                               begin t = MT_IORD; d = 3'd1; end
    brow = {t, d, g, e};
  endfunction

  // (ED patterns: defined above, next to the CB ones, because the actions use them too)

  // (has_d, ix_hit: defined above, next to the executable families)

  // -- row of the current M-cycle, per space
  logic [2:0] nk, kb, m1x; logic [1:0] nb; logic [7:0] r;   // r = {type, dur, gate, ext}
  always_comb begin
    // ONE instance of brow: the k relative to the BASE rows is remapped first
    // (IX/IY transformation: RD d + NONE 5 in front, RD 5 for LD (IX+d),n)
    kb = k;
    if (sh2)      kb = k - 3'd2;
    else if (sh1) kb = k1 ? 3'd1 : 3'd2;
    nk = b_nk; m1x = b_m1x; nb = b_nb; r = brow(kb); row_rpt = 1'b0;
    if (scb) begin                                              // CB: r 8 T; (HL) RD 4 [+ WR 3]
      nk = src6 ? (x01 ? 3'd1 : 3'd2) : 3'd0; m1x = 3'd1; nb = 2'd2;
      r = k1 ? {MT_RD, 3'd1, 2'b00} : {MT_WR, 3'd0, 2'b00};
    end
    if (sed) begin                                              // ED: default 2 x M1 4 T
      nk = 3'd0; m1x = (ed_ldai | ed_bio) ? 3'd2 : 3'd1; nb = (ed_stnn | ed_ldnn) ? 2'd0 : 2'd2;  // nb 0 = 4 bytes (see n_byte)
      r = {MT_RD, 3'd0, 2'b00};
      if (ed_rld) begin nk = 3'd3; if (k2) r = {MT_NONE, 3'd1, 2'b00}; if (k3) r = {MT_WR, 3'd0, 2'b00}; end
      if (ed_stnn | ed_ldnn) begin nk = 3'd4; if (ed_stnn & (k3 | (k == 3'd4))) r = {MT_WR, 3'd0, 2'b00}; end
      if (ed_in)  begin nk = 3'd1; r = {MT_IORD, 3'd1, 2'b00}; end
      if (ed_out) begin nk = 3'd1; r = {MT_IOWR, 3'd1, 2'b00}; end
      if (ed_a16) begin nk = 3'd2; r = {MT_NONE, k1 ? 3'd1 : 3'd0, 2'b00}; end
      if (ed_ret) nk = 3'd2;
      if (ed_blk) begin
        nk = ed_rep ? 3'd3 : 3'd2;
        if (k1 && ed_bio && !ed_bout) r = {MT_IORD, 3'd1, 2'b00};  // INI/IND: port (3+1 T) BEFORE the write (docs/03 note n2)
        if (k2) r = ed_bio ? (ed_bout ? {MT_IOWR, 3'd1, 2'b00} : {MT_WR, 3'd0, 2'b00})   // OUTI: RD then IOWR; INI: IORD then WR
                           : (ir[0] ? {MT_NONE, 3'd2, 2'b00} : {MT_WR, 3'd2, 2'b00});   // CPI internal; LDI = WR at DE 5 T
        if (k3) begin r = {MT_NONE, 3'd2, 2'b10}; row_rpt = 1'b1; end
      end
    end
    if (six) begin                                              // DD/FD: BASE rows transformed
      nb = p_pfx ? 2'd1 : (b_nb + 2'd1 + {1'b0, has_d | p_ldhln});   // 2 bits: 0 = 4 bytes
      if (p_pfx) nk = 3'd1;                                     // DD;CB / DD;DD...: the M1 of the 2nd prefix is NOT the last (sp_set)
      if (has_d) begin
        nk = b_nk + 3'd2;
        if (k1) r = {MT_RD, 3'd0, 2'b00};
        else if (k2) r = {MT_NONE, 3'd2, 2'b00};
      end else if (p_ldhln) begin
        nk = 3'd3;
        if (k2) r = {MT_RD, 3'd2, 2'b00};
      end
    end
    if (sxcb) begin                                             // DDCB/FDCB: d, opcode (RD 5), RD 4 [, WR 3]
      nk = x01 ? 3'd2 : 3'd3; m1x = 3'd1; nb = 2'd0;              // the first M is RD d (no M1 here)
      r = k0 ? {MT_RD, 3'd0, 2'b00} : k1 ? {MT_RD, 3'd2, 2'b00} :
          k2 ? {MT_RD, 3'd1, 2'b00} : {MT_WR, 3'd0, 2'b00};
    end
    if (sint) begin                                             // NMI: M1 5 T, WR, WR (11 T); IM1: IACK 7, WR, WR (13);
      nk = i_im2 ? 3'd4 : 3'd2; m1x = i_nmi ? 3'd2 : 3'd4; nb = 2'd1;   // IM2: IACK 7, WR, WR, RD, RD (19) — docs/04 §4-§5
      r = (k3 || (k == 3'd4)) ? {MT_RD, 3'd0, 2'b00} : {MT_WR, 3'd0, 2'b00};
    end
    // M1 (of a prefix or of an opcode: always the first M of the row, except in DDCB/FDCB where the first
    // M is RD d). IACK in place of the M1: in the response to INT (IM1/IM2, 7 T already in m1x) and in IM0
    // (int_lat, BASE space: the instruction from the bus, M1 + 2 T — docs/04 §5)
    if (k0 && !sxcb) r = ((sint && !i_nmi) || (int_lat && base)) ? {MT_IACK, (sint ? m1x : m1x + 3'd2), 2'b00}
                                                                 : {MT_M1, m1x, 2'b00};
  end

  assign row_type = r[7:5];
  assign row_dur  = r[4:2];
  assign row_gate = r[1];
  assign row_ext  = r[0];
  assign row_last = (k == nk);
  // gate of the next row (RET cc: M2/M3; JR cc, DJNZ: M3; CALL cc: M4/M5; ...R block instructions: last M)
  assign nxt_gate = ((base | six) && ((p_retcc && (k0 | k1)) || ((p_jrcc | p_djnz) && k1) ||
                                      (p_callcc && (k2 | k3)))) ||
                    (sed && ed_rep && k2);
  // prefix in BASE and prefix after DD/FD (CB -> DDCB/FDCB, DD/FD/ED -> new space): always the M1
  assign sp_set   = (base || six) && p_pfx && m1;  // the sequencer changes space and restarts from M1 (docs/03 §3.3)
  assign ir_ld    = sxcb && m2;                    // the fourth byte of DDCB/FDCB goes into IR (docs/03 §2.6)

  // benches only
  assign n_m    = nk + 3'd1;                       // M-cycles from the M1 of the opcode (prefixes are outside the count)
  assign n_byte = (nb == 2'd0) ? 3'd4 : {1'b0, nb};
  assign pre_m1 = six && !ix_hit && !p_pfx;
  assign is_pfx = (base | six) && p_pfx;

  // F is no longer used by the control word (JP cc is decided in the top module); the port remains
  wire _unused_ok = &{1'b0, f_reg, cond_f};

endmodule
