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
// rmZ80 — top module: datapath, pipeline and registered bus outputs. It ties together
// rmZ80_cw (control word and schedule row of all 1792 opcode positions), rmZ80_seq,
// rmZ80_regfile and the ALU (rmZ80_alu_dec and rmZ80_alu_daa before P1, rmZ80_alu_core after it;
// the wrapper rmZ80_alu is used only by the ALU bench).
//
// The top module decides nothing about the instruction set: it routes. Fixed rules (docs/06 §2.1):
//   - wr8 data: res or f_out (DIN and rd8 go through the ALU with op PASS);
//   - wr16: the address adder (also WZ and pairs, with addend 0) or AF8 = {res, f_out};
//   - ALU operands selected BEFORE P1 (A of the active bank, rd8, DIN, WZL, WZH) and read by
//     the ALU straight from P1; f_in = F of the active bank (through P1);
//   - a16 = rd16 (flags of the block instructions only); NO 16-bit adder in the ALU:
//     ADD/ADC/SBC HL = two 8-bit steps through WZ;
//   - address adder (the only 16-bit one): rd16 / WZ / restart vector + {+1, -1, sext(DIN), 0}
//     — PC/SP/WZ, JR/DJNZ, LD SP,HL;
//   - conditions for gate/ext of the schedule from F, or B for DJNZ, or P/V and Z for the
//     repeating block instructions; for JP cc the choice of the PC source is made from F here;
//   - refresh {I, R} from the register file; MREQ/RFSH from the sequencer (docs/02 §3.1).
//
// TIMING (docs/06 §9): the control word of the NEXT state is computed ahead (NX) and registered
// in P0 on the cen at which the state changes; P1, between the register read and the ALU, is
// updated on EVERY clock, never gated by cen: it is a function of the state, NOT state, outside
// the savestate; with rst only its write ENABLES are cleared. Contract: cen at least 2 clocks
// apart; after rst the first cen may come on the first clock; after ss_load at least 2 quiet
// clocks; no input pin inside the paths (data_in, wait_n, int_n, nmi_n, busreq_n enter only the
// sequencer).
//
// No architectural state in the top module. PARALLEL SAVESTATE: ss_dout[273:0] =
// {sequencer state [273:210], register file [209:0]} is the state itself, wires on the
// registers (no logic), readable at any time and consistent (it changes only on cen edges).
// ss_load = 1 for ONE clock with cen = 0 loads ss_din into the state on the edge; then at least
// 2 clocks with cen = 0 before the first cen (P0 and NX are recomputed from the loaded state,
// P1 after them). 0 M10K.
// =============================================================================

module rmZ80 #(
  parameter bit VARIANT_OUT_C0_FF = 1'b0    // OUT (C),0 (ED 71): 0 = writes 00h (NMOS), 1 = FFh (CMOS) — docs/03 §5.3
) (
  input  logic        clk,
  input  logic        cen,
  input  logic        rst,        // RESET (asynchronous, active high)
  // external bus (docs/02)
  output logic [15:0] addr,
  input  logic [7:0]  data_in,
  output logic [7:0]  data_out,
  output logic        data_oe,    // 1 = the core drives the data bus
  output logic        mreq_n,
  output logic        iorq_n,
  output logic        rd_n,
  output logic        wr_n,
  output logic        rfsh_n,
  output logic        m1_n,
  output logic        halt_n,
  input  logic        wait_n,
  input  logic        busreq_n,
  output logic        busack_n,
  input  logic        int_n,
  input  logic        nmi_n,
  // parallel savestate (docs/01 §6): complete state readable, loaded in one clock
  input  logic         ss_load,   // 1 for one clock (cen = 0): state <- ss_din on the edge
  input  logic [273:0] ss_din,
  output logic [273:0] ss_dout    // = current state: [273:210] sequencer (docs/06 §5), [209:0] register file (docs/01 §5)
);

  // ---- sequencer state (view dbg_st, order of docs/06 §5) -------------------
  logic [63:0] st;
  logic [rmZ80_alu_pkg::PD_N-1:0] seq_pd;   // pre-decode of IR from the sequencer
  logic seq_six;                                // DD/FD space, registered
  logic seq_int_acc;                            // interrupt accepted at the edge
  wire        phase   = st[63];
  wire [2:0]  t_idx  = st[62:60];
  wire [2:0]  m_idx  = st[58:56];
  wire [2:0]  mtype  = st[55:53];
  wire [7:0]  ir     = st[52:45];
  wire [7:0]  din_q  = st[41:34];
  wire [15:0] wz     = st[33:18];
  wire [2:0]  space = st[44:42];

  // ---- register file -----------------------------------------------------------
  logic [7:0]  rd8_data, wr8_data;
  logic [15:0] rd16_data, wr16_data;
  logic        r_inc;
  logic [7:0]  rf_a, rf_f, rf_i, rf_r, rf_b;
  logic        rf_sel_gp;

  // ---- control word computed AHEAD (docs/06 §9.3) -----------------------------------------------
  // The control word does not look at the current state: it computes the word of the NEXT state, and P0
  // registers it on the same edge (cen) at which the state changes. Two registers, each stage closed in
  // one clock:
  //   NX (every clock): coordinates of the next state = f(state, P0, F, B); no pins.
  //   P0 (on cen)     : cw(NX) = the word of the state that starts at that edge; the register file, P1,
  //                     the sequencer and the bus read from P0: state -> cw -> register file is not a path.
  // A T-state repeated by WAIT is foreseen by NX: the sequencer decides the repetition on the falling edge
  // and keeps it in tw (st[59]) for the whole phase B, so NX already holds "same T, phase A".
  // Events decided at the edge itself that NX does not foresee, checked exhaustively on the control word
  // (enumeration of all its inputs):
  //   - acceptance of an interrupt at the end of an instruction: M1 T1A has the same word in every case
  //     except the cycle type (M1/IACK), which is decided here from the state (row_type, below);
  //   - IR loaded from the bus (edge that opens T3 of M1, IACK of IM0, fourth DDCB byte): in the half T-state
  //     that starts, the word does not depend on IR except for the HL->IX/IY substitutions, which matter only
  //     when reading HL/H/L, and none is read there (checked below).
  // CONTRACT: after ss_load at least 2 clocks with cen = 0 (P0 and NX are reloaded from the loaded state).
  typedef struct packed {
    logic [4:0] rd8_sel;  logic [2:0] rd16_raw;  logic rd16_ix, rd16_xhl, jpcc_t, addr_wz;
    logic [1:0] dout_src; logic [5:0] alu_op;    logic [1:0] a_src, b_src;
    logic       wr8_en;   logic [4:0] wr8_sel;   logic wr8_src;    // RAW wr8_sel/wr16_sel: the IX/IY and DE<->HL
    logic       wr16_en;  logic [2:0] wr16_sel;  logic wr16_src;   //   substitutions are made in the register file
    logic [1:0] inc_op, inc_src, wz_we, wz_src, cond_mode;
    logic       swap_af, swap_gp, swap_dehl, rd8_ix, rd8_fd, rd8_xhl;
    logic       im_we;    logic [1:0] im_val, iff_op;  logic halt_set;
    logic [2:0] row_type, row_dur;  logic row_ext, row_last, nxt_gate, sp_set, ir_ld;
  } cw_t;
  // word of the reset state (M1 T1A, IR 00h, BASE): all zero except the row "M1 of 4 T, last"
  // (value of the control word, checked against it in the benches at every reset)
  // (row_type = 1 bit 8, row_dur = 1 bit 5, row_last bit 3 in field order: 69'h128)
  localparam logic [68:0] CW_RST = 69'h128;
  typedef struct packed {
    logic phase; logic [2:0] t, m, sp; logic six, halt, il, xhl, sh2, sh1;   // sh2/sh1: (IX+d) coordinates of the control word
    logic [7:1] moh, toh;                                                  // m and t one-hot
  } nx_t;
  localparam logic [29:0] NX_RST = 30'h24800081;   // after reset: M1 T1B (phase b29, t=1 b26, m=1 b23, moh[1] b7, toh[1] b0)

  cw_t cwn, p0;
  nx_t nx;
  logic [4:0] cw_wr8_subst;  logic [2:0] cw_wr16_subst;   // substituted selectors of the control word: benches only
  logic [2:0] cw_n_m, cw_n_byte, cw_rd16_sel; logic cw_pre_m1, cw_is_pfx, row_gate, row_rpt;   // benches only

  rmZ80_cw #(.EXT_PD(1'b1)) u_cw (
    .ir(ir), .pd(seq_pd), .six_i(nx.six), .sh2_i(nx.sh2), .sh1_i(nx.sh1), .moh_i(nx.moh), .toh_i(nx.toh), .m_idx(nx.m), .t_idx(nx.t), .phase(nx.phase), .space(nx.sp), .f_reg(8'h00),
    .rd8_sel(cwn.rd8_sel), .rd16_sel(cw_rd16_sel), .rd16_raw(cwn.rd16_raw), .rd16_ix(cwn.rd16_ix), .rd16_xhl(cwn.rd16_xhl), .jpcc_t(cwn.jpcc_t), .addr_wz(cwn.addr_wz), .dout_src(cwn.dout_src),
    .alu_op(cwn.alu_op), .a_src(cwn.a_src), .b_src(cwn.b_src),
    .wr8_en(cwn.wr8_en), .wr8_sel(cw_wr8_subst), .wr8_raw(cwn.wr8_sel), .wr8_src(cwn.wr8_src),
    .wr16_en(cwn.wr16_en), .wr16_sel(cw_wr16_subst), .wr16_raw(cwn.wr16_sel), .wr16_src(cwn.wr16_src),
    .inc_op(cwn.inc_op), .inc_src(cwn.inc_src), .wz_we(cwn.wz_we), .wz_src(cwn.wz_src), .cond_mode(cwn.cond_mode),
    .swap_af(cwn.swap_af), .swap_gp(cwn.swap_gp), .swap_dehl(cwn.swap_dehl), .xhl(nx.xhl),
    .rd8_ix(cwn.rd8_ix), .rd8_fd(cwn.rd8_fd), .rd8_xhl(cwn.rd8_xhl),
    .im_we(cwn.im_we), .im_val(cwn.im_val), .iff_op(cwn.iff_op), .halt_set(cwn.halt_set), .halt(nx.halt), .int_lat(nx.il),
    .row_type(cwn.row_type), .row_dur(cwn.row_dur), .row_gate(row_gate), .row_ext(cwn.row_ext),
    .row_rpt(row_rpt), .row_last(cwn.row_last), .nxt_gate(cwn.nxt_gate), .sp_set(cwn.sp_set), .ir_ld(cwn.ir_ld),
    .n_m(cw_n_m), .n_byte(cw_n_byte), .pre_m1(cw_pre_m1), .is_pfx(cw_is_pfx)
  );

  // ---- NX: coordinates of the next state (same rules as rmZ80_seq, without pins) ---------------
  // Condition for gate/ext of the schedule from the SOURCES of P1 (F and B are state: the same value P1
  // would register; mode 2 = B != 0, the register the control word reads in rd8 in those rows).
  // The conditions cc (F), cc of JR (F) and B != 0 depend only on the state and are written long before
  // the end of the M-cycle that uses them: registered on every clock, after the register only the choice
  // of the mode remains. Mode 3 (repeating block instructions: P/V and Z) is NOT registered: the flags of
  // the block instruction are written on the A->B edge of the last T-state, so it is evaluated in place
  // (one LUT: F[2], F[6], IR[0]). Check of NX below.
  logic [2:0] cq_d, cq;
  always_comb begin
    case (ir[5:3])
      3'd0: cq_d[0] = ~rf_f[6];  3'd1: cq_d[0] = rf_f[6];
      3'd2: cq_d[0] = ~rf_f[0];  3'd3: cq_d[0] = rf_f[0];
      3'd4: cq_d[0] = ~rf_f[2];  3'd5: cq_d[0] = rf_f[2];
      3'd6: cq_d[0] = ~rf_f[7];  default: cq_d[0] = rf_f[7];
    endcase
    case (ir[4:3])
      2'd0: cq_d[1] = ~rf_f[6];  2'd1: cq_d[1] = rf_f[6];
      2'd2: cq_d[1] = ~rf_f[0];  default: cq_d[1] = rf_f[0];
    endcase
    cq_d[2] = (rf_b != 8'h00);
  end
  always_ff @(posedge clk) cq <= cq_d;
  wire cond_nx = (p0.cond_mode == 2'd3) ? (rf_f[2] & ~(rf_f[6] & ir[0])) : cq[p0.cond_mode];
  wire [2:0] sp_val;                                            // space of the prefix (defined below)
  wire       last_nx = p0.row_last | (p0.nxt_gate & ~cond_nx);
  wire [2:0] durt_nx = 3'd3 + p0.row_dur - {2'b00, (p0.row_ext & ~cond_nx)};
  wire       q2_nx   = st[12] ^ (p0.swap_dehl & ~rf_sel_gp);    // DE<->HL renaming after the edge (bank 0/1)
  wire       q3_nx   = st[13] ^ (p0.swap_dehl &  rf_sel_gp);
  wire       gp_nx   = rf_sel_gp ^ p0.swap_gp;                  // EXX at the edge (P1 = P0)
  nx_t nx_nxt;
  function automatic logic [7:1] oh7(input logic [2:0] v);   // 1..7 -> one-hot (0 -> no bit)
    oh7 = 7'd0;  if (v != 3'd0) oh7[v] = 1'b1;
  endfunction
  wire [7:1] moh_cur = oh7(m_idx), toh_cur = oh7(t_idx);
  /* verilator lint_off UNUSEDSIGNAL */   // of pd_ir/pd_ss only bits 53 (has_d) and 11 (LD (HL),n) are used
  wire [rmZ80_alu_pkg::PD_N-1:0] pd_ir = rmZ80_alu_pkg::rmz80_pdec(ir);
  /* verilator lint_on UNUSEDSIGNAL */
  always_comb begin
    nx_nxt.phase = ~phase;
    nx_nxt.t = t_idx;  nx_nxt.m = m_idx;  nx_nxt.sp = space;  nx_nxt.six = seq_six;  nx_nxt.il = st[3];
    nx_nxt.halt = st[2] | p0.halt_set;
    nx_nxt.xhl  = gp_nx ? q3_nx : q2_nx;
    // one-hot: the same choices as the binary coordinates, from the decode of the CURRENT coordinates (state)
    nx_nxt.moh = moh_cur;  nx_nxt.toh = toh_cur;
    if (phase && !st[59]) begin                     // tw = 1 in phase B: T-state repeated by WAIT, coordinates held (phase -> A)
      if (t_idx == durt_nx) begin
        nx_nxt.t = 3'd1;  nx_nxt.toh = 7'b0000001;
        nx_nxt.moh = (last_nx || p0.sp_set) ? 7'b0000001 : {moh_cur[6:1], 1'b0};
        nx_nxt.m = (last_nx || p0.sp_set) ? 3'd1 : (m_idx + 3'd1);
        if (last_nx) begin nx_nxt.sp = 3'd0; nx_nxt.six = 1'b0; nx_nxt.il = 1'b0; end
        else if (p0.sp_set) begin nx_nxt.sp = sp_val; nx_nxt.six = (sp_val == 3'd3) || (sp_val == 3'd4); end
      end else begin nx_nxt.t = t_idx + 3'd1;  nx_nxt.toh = {toh_cur[6:1], 1'b0}; end
    end
    // (IX+d) coordinates of the control word ((IX+d) forms and LD (IX+d),n): from the current IR, which is
    // that of the next state except on the edges that load IR, where the word does not depend on IR (docs/06 §9.3)
    nx_nxt.sh2 = nx_nxt.six && pd_ir[53];
    nx_nxt.sh1 = nx_nxt.six && pd_ir[11];
  end
  // savestate load: NX <- CURRENT coordinates of the loaded state (P0 = cw(NX) on the next clock);
  // on the second clock NX <- the same T in phase A (placeholder: P0 is not yet valid for nx_nxt), then nx_nxt
  wire [2:0] ss_sp = ss_din[254:252];
  nx_t nx_ss, nx_cur;
  /* verilator lint_off UNUSEDSIGNAL */
  wire [rmZ80_alu_pkg::PD_N-1:0] pd_ss = rmZ80_alu_pkg::rmz80_pdec(ss_din[262:255]);   // loaded IR
  /* verilator lint_on UNUSEDSIGNAL */
  always_comb begin
    nx_ss.phase = ss_din[273];  nx_ss.t = ss_din[272:270];  nx_ss.m = ss_din[268:266];  nx_ss.sp = ss_sp;
    nx_ss.six  = (ss_sp == 3'd3) || (ss_sp == 3'd4);  nx_ss.halt = ss_din[212];  nx_ss.il = ss_din[213];
    nx_ss.xhl  = ss_din[0] ? ss_din[223] : ss_din[222];
    nx_ss.sh2  = nx_ss.six && pd_ss[53];  nx_ss.sh1 = nx_ss.six && pd_ss[11];
    nx_ss.moh  = oh7(ss_din[268:266]);  nx_ss.toh = oh7(ss_din[272:270]);
    nx_cur.phase = 1'b0;  nx_cur.t = t_idx;  nx_cur.m = m_idx;  nx_cur.sp = space;  nx_cur.six = seq_six;
    nx_cur.halt = st[2];  nx_cur.il = st[3];  nx_cur.xhl = rf_sel_gp ? st[13] : st[12];
    nx_cur.sh2 = seq_six && pd_ir[53];  nx_cur.sh1 = seq_six && pd_ir[11];
    nx_cur.moh = moh_cur;  nx_cur.toh = toh_cur;
  end
  logic ld2;                                                     // 1 clock after ss_load
  initial ld2 = 1'b0;

  always_ff @(posedge clk or posedge rst) begin                 // NX and the load delay: no enable
    if (rst) begin
      ld2 <= 1'b0;  nx <= NX_RST;
    end else begin
      ld2 <= ss_load;
      if (ss_load)  nx <= nx_ss;
      else if (ld2 || st[0]) nx <= nx_cur;        // st[0] = bus released (Tx): coordinates held
      else          nx <= nx_nxt;
    end
  end
  // P0 has cen as enable: ASYNCHRONOUS rst, so that the enable stays cen | ld2 and the clock enable from
  // the system does not go through an OR before reaching the flip-flops
  always_ff @(posedge clk or posedge rst) begin
    if (rst)              p0 <= CW_RST;
    else if (ld2 || cen)  p0 <= cwn;
  end
  initial begin nx = NX_RST; p0 = CW_RST; end

  // named view of the registered control word
  wire [4:0] cw_rd8_sel = p0.rd8_sel, cw_wr8_sel = p0.wr8_sel;
  wire [2:0] cw_wr16_sel = p0.wr16_sel, cw_rd16_raw = p0.rd16_raw;
  wire       cw_rd16_ix = p0.rd16_ix, cw_rd16_xhl = p0.rd16_xhl, cw_jpcc_t = p0.jpcc_t, cw_addr_wz = p0.addr_wz;
  wire       cw_wr8_en = p0.wr8_en, cw_wr16_en = p0.wr16_en, cw_swap_af = p0.swap_af, cw_swap_gp = p0.swap_gp;
  wire       cw_wr8_src = p0.wr8_src, cw_wr16_src = p0.wr16_src, cw_swap_dehl = p0.swap_dehl;
  wire       cw_rd8_ix = p0.rd8_ix, cw_rd8_fd = p0.rd8_fd, cw_rd8_xhl = p0.rd8_xhl;
  wire       cw_im_we = p0.im_we, cw_halt_set = p0.halt_set;
  wire [1:0] cw_im_val = p0.im_val, cw_iff_op = p0.iff_op;
  wire [1:0] cw_b_src = p0.b_src, cw_dout_src = p0.dout_src, cw_a_src = p0.a_src, cw_inc_op = p0.inc_op;
  wire [1:0] cw_inc_src = p0.inc_src, cw_wz_we = p0.wz_we, cw_wz_src = p0.wz_src, cw_cond_mode = p0.cond_mode;
  wire [5:0] cw_alu_op = p0.alu_op;
  wire [2:0] row_dur = p0.row_dur;
  wire       row_ext = p0.row_ext, row_last = p0.row_last, nxt_gate = p0.nxt_gate, sp_set = p0.sp_set, ir_ld = p0.ir_ld;
  // cycle type: P0, except the M1 that follows an interrupt accepted at the edge (IACK: INT in IM1/IM2
  // = space 7 not NMI, IM0 = int_lat in BASE), the same rule as the control word (k0, not DDCB); it is
  // sampled in T1A (at least 2 clocks after the edge: pd is already that of IR)
  wire [2:0] row_type = ((m_idx == 3'd1) && (((space == 3'd7) && !seq_pd[72]) || (st[3] && (space == 3'd0))))
                        ? 3'd6 : p0.row_type;

  // ALU operands selected BEFORE P1 (with the control word already registered, state -> P1 is short):
  // the ALU reads them from P1 without a multiplexer. A, din_q and WZ are state (they change only on cen).
  wire         use_mask_pre = (cw_alu_op == rmZ80_alu_pkg::OP_BITX) || (cw_alu_op == rmZ80_alu_pkg::OP_RES) ||
                              (cw_alu_op == rmZ80_alu_pkg::OP_SET);
  // the sources other than rd8 are selected first (state and P0): rd8 enters at the last level
  wire [7:0] opa_alt = (cw_a_src == 2'd2) ? din_q : rf_a;
  wire [7:0] opb_alt = (cw_b_src == 2'd1) ? din_q : (cw_b_src == 2'd2) ? wz[7:0] : wz[15:8];
  wire [7:0] opa_pre = (cw_a_src == 2'd1) ? rd8_data : opa_alt;
  wire [7:0] opb_pre = (cw_b_src == 2'd0) ? rd8_data : opb_alt;
  wire [7:0] lb_alt  = use_mask_pre ? (8'h01 << ir[5:3]) : opb_alt;          // logic operand without rd8
  wire       lb_rd8  = !use_mask_pre && (cw_b_src == 2'd0);

  // decode of the ALU operation and DAA tables BEFORE P1 (docs/06 §9.1):
  // the adder fields reach the core ready; DAA depends only on A and F
  logic ad_sub_pre, ad_daa_pre, ad_lhs0_pre, ad_lhsb_pre, daa_c_pre, daa_h_pre;
  logic [1:0] ad_rhs_pre, daa_sel_pre;
  logic [2:0] ad_cin_pre;
  rmZ80_alu_dec u_adec (.op(cw_alu_op), .ad_sub(ad_sub_pre), .ad_daa(ad_daa_pre),
                        .ad_rhs(ad_rhs_pre), .ad_lhs0(ad_lhs0_pre), .ad_lhsb(ad_lhsb_pre), .ad_cin(ad_cin_pre));
  rmZ80_alu_daa u_adaa (.a(rf_a), .f_in(rf_f), .diff_sel(daa_sel_pre),
                        .daa_c(daa_c_pre), .daa_h(daa_h_pre));

  // condition cc on F for JP cc (docs/03 §1.3), outside the control word
  logic cond_jp;
  always_comb begin
    case (ir[5:3])
      3'd0: cond_jp = ~rf_f[6];  3'd1: cond_jp = rf_f[6];
      3'd2: cond_jp = ~rf_f[0];  3'd3: cond_jp = rf_f[0];
      3'd4: cond_jp = ~rf_f[2];  3'd5: cond_jp = rf_f[2];
      3'd6: cond_jp = ~rf_f[7];  default: cond_jp = rf_f[7];
    endcase
  end
  wire jpcc_go = cw_jpcc_t & cond_jp;

  // ---- P1: pipeline register (docs/06 §9.1) — NOT state -----------------------
  logic [7:0]  p1_opa, p1_opb, p1_f;
  logic [15:0] p1_rd16;
  logic [rmZ80_alu_pkg::FS_N-1:0] p1_fsel;   // ALU selectors decoded from op before P1
  logic        p1_alu_sub, p1_ad_daa, p1_ad_lhs0, p1_ad_lhsb, p1_daa_c, p1_daa_h;
  logic [1:0]  p1_ad_rhs, p1_daa_sel;  logic [2:0] p1_ad_cin;
  logic        p1_wr8_en;   logic [4:0] p1_wr8_sel;   logic p1_wr8_src;    // 0 res, 1 f_out
  logic        p1_wr16_en;  logic [2:0] p1_wr16_sel;  logic p1_wr16_src;   // 0 address adder, 1 AF8
  logic        p1_wix8, p1_wix16, p1_wfd, p1_wxhl;   // write-side substitutions
  logic [1:0]  p1_inc_op, p1_inc_src, p1_wz_we, p1_wz_src, p1_cond_mode;
  logic        p1_swap_af, p1_swap_gp;
  logic [7:0]  p1_lb;   // logic operand of the ALU (b or the bit mask for BIT/RES/SET), selected before P1

  always_ff @(posedge clk or posedge rst) begin
    // with rst the write ENABLES of P1 are cleared. On release P1 holds "no action" = the word of M1 T1A
    // for NOP (IR = 00 after reset): the first cen may come on the FIRST clock after rst. The data
    // (opa/opb/rd16/f/alu) do not matter without an enable.
    if (rst) begin
      p1_wr8_en <= 1'b0; p1_wr16_en <= 1'b0; p1_wz_we <= 2'b00; p1_swap_af <= 1'b0; p1_swap_gp <= 1'b0;
    end else begin
      p1_opa <= opa_pre;  p1_opb <= opb_pre;  p1_rd16 <= rd16_data;  p1_f <= rf_f;
      p1_fsel <= rmZ80_alu_pkg::rmz80_fsel(cw_alu_op);
      p1_lb <= lb_rd8 ? rd8_data : lb_alt;
      p1_alu_sub <= ad_sub_pre | (ad_daa_pre & rf_f[rmZ80_alu_pkg::FB_N]);  p1_ad_daa <= ad_daa_pre;  p1_ad_rhs <= ad_rhs_pre;
      p1_ad_lhs0 <= ad_lhs0_pre;  p1_ad_lhsb <= ad_lhsb_pre;  p1_ad_cin <= ad_cin_pre;
      p1_daa_sel <= daa_sel_pre;  p1_daa_c <= daa_c_pre;  p1_daa_h <= daa_h_pre;
      // in the Tx states of a released bus the state is held at T1 of the next cycle: no write
      p1_wr8_en <= cw_wr8_en & ~st[0];  p1_wr8_sel <= cw_wr8_sel;  p1_wr8_src <= cw_wr8_src;
      p1_wr16_en <= cw_wr16_en & ~st[0]; p1_wr16_sel <= cw_wr16_sel; p1_wr16_src <= cw_wr16_src;
      p1_wix8 <= cw_rd8_ix;  p1_wix16 <= cw_rd16_ix;  p1_wfd <= cw_rd8_fd;  p1_wxhl <= cw_rd16_xhl;
      // JP cc: condition on F evaluated here, on the state; true -> PC <- WZ (inc_src 2, inc_op 3)
      p1_inc_op <= jpcc_go ? 2'd3 : cw_inc_op;  p1_inc_src <= jpcc_go ? 2'd2 : cw_inc_src;  p1_wz_we <= cw_wz_we & {2{~st[0]}};  p1_wz_src <= cw_wz_src;
      p1_cond_mode <= cw_cond_mode;  p1_swap_af <= cw_swap_af & ~st[0];  p1_swap_gp <= cw_swap_gp & ~st[0];
    end
  end

  rmZ80_regfile u_rf (
    .clk(clk), .cen(cen), .rst(rst),
    .rd8_sel(cw_rd8_sel), .rd8_data(rd8_data), .rd8_ix(cw_rd8_ix), .rd8_fd(cw_rd8_fd), .rd8_xhl(cw_rd8_xhl),
    .wr8_en(p1_wr8_en), .wr8_sel(p1_wr8_sel), .wr8_data(res), .wr8_f(wr8_data),
    .rd16_sel(cw_rd16_raw), .rd16_ix(cw_rd16_ix), .rd16_fd(cw_rd8_fd), .rd16_xhl(cw_rd16_xhl), .rd16_data(rd16_data),
    .wr16_en(p1_wr16_en), .wr16_sel(p1_wr16_sel), .wr16_data(inc_out), .wr16_af(wr16_data),
    .wr_ix8(p1_wix8), .wr_ix16(p1_wix16), .wr_fd(p1_wfd), .wr_xhl(p1_wxhl),
    .r_inc(r_inc), .swap_af(p1_swap_af), .swap_gp(p1_swap_gp), .sel_gp_o(rf_sel_gp),
    // LD A,I / LD A,R (ED 57 / ED 5F): if an interrupt is accepted at their end, P/V = 0 ([UM] pp.94-95)
    .pv_clr(seq_int_acc && (space == 3'd2) && ((ir == 8'h57) || (ir == 8'h5F))),
    .a_out(rf_a), .f_out(rf_f), .b_out(rf_b), .i_out(rf_i), .r_out(rf_r),
    .ss_load(ss_load), .ss_din(ss_din[209:0]), .ss_dout(ss_dout[209:0])
  );

  // ---- 8-bit ALU (operands and operation straight from P1) -----------------------
  logic [7:0]  res, f_out;

  // b_src 0 = rd8 (also the rd8 data for wr8/dout/cond), 1 = DIN, 2 = WZL, 3 = WZH (WZL/WZH: ADD HL)

  rmZ80_alu_core u_alu (
    .fsel(p1_fsel),
    .alu_sub(p1_alu_sub), .ad_daa(p1_ad_daa), .ad_rhs(p1_ad_rhs), .ad_lhs0(p1_ad_lhs0), .ad_lhsb(p1_ad_lhsb), .ad_cin(p1_ad_cin),
    .daa_diff_sel(p1_daa_sel), .daa_c(p1_daa_c), .daa_h(p1_daa_h),
    .a(p1_opa), .b(p1_opb), .f_in(p1_f), .q_in(st[10] ? 8'h00 : p1_f), .pv_in(st[8]),   // Q: F masked if the previous instruction wrote it; external P/V = IFF2
    .lb_in(p1_lb), .a16(p1_rd16),   // logic operand already selected
    .res(res), .f_out(f_out)
  );

  // ---- the ONLY 16-bit adder: rd16 (through P1) / WZ / restart vector
  //      + addend {+1, -1, sext(DIN), 0}. It serves PC/SP/WZ +-1, JR/DJNZ (PC+e), LD SP,HL and
  //      similar (addend 0), and the (IX+d) address. ADD/ADC/SBC HL go through the 8-bit ALU in
  //      two ticks (rmZ80_cw).
  logic [15:0] inc_in, inc_out, seq_addr, inc_add;
  logic [7:0]  seq_dout;
  logic        seq_mreq_n, seq_iorq_n, seq_rd_n, seq_wr_n, seq_rfsh_n;
  // source 3 = vector: RST (00 ttt 000) or, in the response to an interrupt (space 7), the synthetic IR
  // itself (66h NMI, 38h IM1)
  assign inc_in = (p1_inc_src == 2'd2) ? wz :
                  (p1_inc_src == 2'd3) ? ((space == 3'd7) ? {8'h00, ir} : {8'h00, 2'b00, ir[5:3], 3'b000})
                                       : p1_rd16;
  always_comb begin
    case (p1_inc_op)
      2'd0: inc_add = 16'h0001;
      2'd1: inc_add = 16'hFFFF;
      2'd2: inc_add = {{8{din_q[7]}}, din_q};
      default: inc_add = 16'h0000;
    endcase
  end
  assign inc_out = inc_in + inc_add;

  // ---- condition for gate/ext of the schedule (docs/06 §3.4), on P1 (used by the check below) ----
  logic cond_ok;
  always_comb begin
    case (p1_cond_mode)
      2'd0: case (ir[5:3])                       // cc on F
              3'd0: cond_ok = ~p1_f[6];  3'd1: cond_ok = p1_f[6];
              3'd2: cond_ok = ~p1_f[0];  3'd3: cond_ok = p1_f[0];
              3'd4: cond_ok = ~p1_f[2];  3'd5: cond_ok = p1_f[2];
              3'd6: cond_ok = ~p1_f[7];  default: cond_ok = p1_f[7];
            endcase
      2'd1: case (ir[4:3])                       // cc of JR on F
              2'd0: cond_ok = ~p1_f[6];  2'd1: cond_ok = p1_f[6];
              2'd2: cond_ok = ~p1_f[0];  default: cond_ok = p1_f[0];
            endcase
      2'd2: cond_ok = (p1_opb != 8'h00);         // DJNZ and INIR/INDR/OTIR/OTDR: B != 0 (rd8 = B)
      // repeating block instructions: LDIR = P/V (BC != 0); CPIR = P/V and not Z (A != (HL)).
      // ir is the ED opcode (ir[0] tells CPIR from LDIR); the block I/O instructions use mode 2.
      default: cond_ok = p1_f[2] & ~(p1_f[6] & ir[0]);
    endcase
  end

  // ---- data towards the register file and WZ: two sources per port ----------------
  //   wr8  : res (DIN and rd8 come through res with op PASS) or f_out
  //   wr16 : address adder (PC/SP +-1, WZ and pairs with addend 0) or AF8
  // the flags and AF8 reach ONLY F and AF: in the control word wr8_src = 1 only with wr8_sel = F and
  // wr16_src = 1 only with wr16_sel = AF (checked below in simulation); the other registers receive res
  // (8 bits) and the output of the address adder (16 bits)
  assign wr8_data  = p1_wr8_src  ? f_out : res;        // -> F only
  assign wr16_data = p1_wr16_src ? {p1_wr8_en ? rf_a : res, f_out}   // AF8: A kept if wr8 writes something else
                                 : inc_out;
  // WZ: byte from DIN (nn), whole from the adder, or {A, adder[7:0]} (MEMPTR of LD (rp/nn),A and OUT (n),A)
  wire [15:0] wz_din = (p1_wz_src == 2'd2) ? {rf_a, inc_out[7:0]} :
                       (p1_wz_we == 2'b11 || p1_wz_src == 2'd1) ? inc_out : {din_q, din_q};

  // ---- sequencer ------------------------------------------------------------------
  wire [15:0] addr_val = cw_addr_wz ? wz : rd16_data;
  // Q (docs/01 §3): the sequencer only needs to know WHEN F receives the ALU FLAGS (wr8 to F from f_out, or
  // wr16 to AF from AF8); POP AF (F from the data, wr8_src = 0) and EX AF,AF' do NOT count (z80test z80ccf, test 125)
  wire        q_we  = (p1_wr8_en && p1_wr8_sel == 5'd6 && p1_wr8_src) || (p1_wr16_en && p1_wr16_sel == 3'd7 && p1_wr16_src);
  wire [7:0]  dout_val = (cw_dout_src == 2'd1) ? res : (cw_dout_src == 2'd2) ? din_q :
                         (cw_dout_src == 2'd3) ? {8{VARIANT_OUT_C0_FF}} : p1_opb;   // 3 = OUT (C),0: 00h NMOS, FFh CMOS
  // end-of-M condition: the same as NX (registered cq + mode 3 in place), not from P1: at the edge it is
  // the state of the current half T-state, as in P1 (check cond_nx == cond_ok below)
  wire        sch_last = row_last | (nxt_gate & ~cond_nx);
  // prefix in its M1 (sp_set from the control word): the sequencer changes space at the end of the M-cycle
  // (docs/03 §3): CB -> 1, ED -> 2, DD -> 3, FD -> 4; CB after DD/FD -> DDCB (5) / FDCB (6) (docs/03 §3.1)
  assign      sp_val   = (ir == 8'hCB) ? ((space == 3'd3) ? 3'd5 : (space == 3'd4) ? 3'd6 : 3'd1)
                       : (ir == 8'hED) ? 3'd2 : (ir == 8'hDD) ? 3'd3 : 3'd4;
  wire [2:0]  sch_dur  = row_dur - {2'b00, (row_ext & ~cond_nx)};  // CALL cc: M3 4 -> 3 T

  rmZ80_seq u_seq (
    .clk(clk), .cen(cen), .rst(rst),
    .sch_type(row_type), .sch_dur(sch_dur), .sch_last(sch_last),
    .addr_val(addr_val), .dout_val(dout_val),
    .i_val(rf_i), .r_val(rf_r),
    .din(data_in), .wait_n(wait_n),
    .addr(seq_addr), .dout(seq_dout),
    .mreq_n(seq_mreq_n), .iorq_n(seq_iorq_n), .rd_n(seq_rd_n), .wr_n(seq_wr_n), .rfsh_n(seq_rfsh_n),
    .r_inc(r_inc), .dbg_st(st),
    .wz_we(p1_wz_we), .wz_din(wz_din),
    .im_we(cw_im_we), .im_val(cw_im_val), .iff_op(cw_iff_op), .halt_set(cw_halt_set),
    .nmi_n(nmi_n), .int_n(int_n), .busreq_n(busreq_n),
    .sp_set(sp_set), .sp_val(sp_val), .ir_ld(ir_ld), .q_we(q_we), .swap_dehl(cw_swap_dehl), .sel_gp(rf_sel_gp),
    .ss_load(ss_load), .ss_din(ss_din[273:210]), .ss_dout(ss_dout[273:210]),
    .pd(seq_pd), .six_o(seq_six), .int_acc(seq_int_acc)
  );

  // ---- REGISTERED bus outputs ---------------------------------------------------
  // Every output leaves a flip-flop updated on EVERY clock (not gated by cen, outside the savestate like
  // P1). The state changes only on cen edges and two cen are at least 2 clocks apart: the outside sees the
  // same waveform delayed by ONE clock on all signals together. The reason: in a real system addr/MREQ/RD
  // feed logic that comes back to cen (memory wait), and from combinational outputs the path ir -> cw ->
  // register file -> addr -> cen -> enable was too long. Initial values = bus inactive (no spurious strobe
  // at power-up).
  // ADDRESS: from ONE flip-flop, with no logic after it: refresh ? {I,R} : (addr_wz ? WZ : rd16).
  wire        refresh_now = ((mtype == 3'd1) && (t_idx >= 3'd3) && (t_idx <= 3'd4)) ||
                            ((mtype == 3'd6) && (t_idx >= 3'd5) && (t_idx <= 3'd6));   // = refresh_win of the sequencer
  // type of the CURRENT cycle for the M1 pin: in T1A the sequencer has not yet sampled the type of the new
  // M-cycle (mtype still holds that of the previous one; it is updated on the A->B edge of T1), but it is
  // already in the schedule row (row_type, from P0): M1 low from the START of T1 ([UM] p.8, figure 5; docs/02 §3)
  wire [2:0]  mt_now = ((t_idx == 3'd1) && !phase) ? row_type : mtype;
  logic [15:0] addr_q;
  logic [7:0]  data_out_q;
  logic        mreq_q, iorq_q, rd_q, wr_q, rfsh_q, m1_q, halt_q, busack_q;
  initial begin                                               // power-up: bus inactive, address 0000h
    addr_q = 16'h0000; data_out_q = 8'h00;
    mreq_q = 1'b1; iorq_q = 1'b1; rd_q = 1'b1; wr_q = 1'b1; rfsh_q = 1'b1; m1_q = 1'b1; halt_q = 1'b1; busack_q = 1'b1;
  end
  always_ff @(posedge clk or posedge rst) begin
    if (rst) begin
      // with rst the bus is that of the reset state (M1, T1A, PC = 0000h): the flip-flops load it at once,
      // so that the first cen may come on the first clock (no tail of the pre-reset state on the bus).
      // Address 0000h (= PC after reset).
      addr_q <= 16'h0000;
      data_out_q <= 8'h00;
      mreq_q <= 1'b1; iorq_q <= 1'b1; rd_q <= 1'b1; wr_q <= 1'b1; rfsh_q <= 1'b1;
      m1_q <= 1'b0; halt_q <= 1'b1; busack_q <= 1'b1;
    end else begin
      addr_q <= refresh_now ? {rf_i, rf_r} : cw_addr_wz ? wz : rd16_data;
      data_out_q <= seq_dout;
      mreq_q <= seq_mreq_n; iorq_q <= seq_iorq_n; rd_q <= seq_rd_n; wr_q <= seq_wr_n; rfsh_q <= seq_rfsh_n;
      // M1 low in T1-T2 of the fetch and, in the IACK ("special M1", docs/02 §7), until the end of the
      // two automatic waits (T1-T4): the device sees M1 with IORQ
      m1_q   <= ~(((mt_now == 3'd1) && (t_idx <= 3'd2)) || ((mt_now == 3'd6) && (t_idx <= 3'd4))) | st[0];   // Tx: M1 high ([PS] fig. 10)
      // BUSACK ([PS] fig. 10): low in the Tx states; rises on the falling edge of the Tx in which the release was sampled
      busack_q <= ~(st[0] & ~(st[15] & st[16]));
      halt_q <= ~st[2];                                       // HALT ([02] §7: active for the whole HALT)
    end
  end
  assign addr     = addr_q;
  assign data_out = data_out_q;
  assign mreq_n   = mreq_q;
  assign iorq_n   = iorq_q;
  assign rd_n     = rd_q;
  assign wr_n     = wr_q;
  assign rfsh_n   = rfsh_q;
  assign data_oe  = ~wr_q;
  assign m1_n     = m1_q;
  assign halt_n   = halt_q;
  assign busack_n = busack_q;

  // signals not used here (for lint)
  wire _unused_ok = &{1'b0, busreq_n, cond_ok, cw_rd16_sel, cw_wr8_subst, cw_wr16_subst, cw_pre_m1, cw_n_m, cw_n_byte, cw_is_pfx, seq_addr,
                      row_gate, row_rpt, st[59], st[17:14], st[11], st[9], st[7:4], st[1:0]};

`ifdef VERILATOR
/* verilator lint_off SYNCASYNCNET */   // benches only: rst read as data in the checks
`ifndef RMZ80_NOCHK   // heavy checks (second instance of the control word, compares on every clock), can be turned off
  // ---- checks of the pipeline (in every bench, on every clock) -------------------------------------
  // (1) P0 = the control word of the CURRENT state (second instance, simulation only, pd = pdec(IR)
  //     without the delay). Compared on the EFFECTS: reads per effective register (the IX/IY and DE<->HL
  //     substitutions matter only on the register they touch), the schedule row and the cycle type where
  //     the sequencer consumes them: at the cen edge, in phase B (B->A) and in T1A (A->B).
  /* verilator lint_off UNUSEDSIGNAL */   // (simulation only: outputs of the control word and fields not compared)
  cw_t cwc;
  logic [4:0] c_wr8_subst;  logic [2:0] c_wr16_subst;
  logic [2:0] c_rd16_sel, c_n_m, c_n_byte; logic c_pre_m1, c_is_pfx, c_row_gate, c_row_rpt;
  rmZ80_cw #(.EXT_PD(1'b1)) u_cwchk (
    .ir(ir), .pd(rmZ80_alu_pkg::rmz80_pdec(ir)), .six_i((space == 3'd3) || (space == 3'd4)),
    .moh_i(oh7(m_idx)), .toh_i(oh7(t_idx)),
    .sh2_i(((space == 3'd3) || (space == 3'd4)) && pd_ir[53]), .sh1_i(((space == 3'd3) || (space == 3'd4)) && pd_ir[11]), .m_idx(m_idx), .t_idx(t_idx),
    .phase(phase), .space(space), .f_reg(8'h00),
    .rd8_sel(cwc.rd8_sel), .rd16_sel(c_rd16_sel), .rd16_raw(cwc.rd16_raw), .rd16_ix(cwc.rd16_ix), .rd16_xhl(cwc.rd16_xhl), .jpcc_t(cwc.jpcc_t), .addr_wz(cwc.addr_wz), .dout_src(cwc.dout_src),
    .alu_op(cwc.alu_op), .a_src(cwc.a_src), .b_src(cwc.b_src),
    .wr8_en(cwc.wr8_en), .wr8_sel(c_wr8_subst), .wr8_raw(cwc.wr8_sel), .wr8_src(cwc.wr8_src),
    .wr16_en(cwc.wr16_en), .wr16_sel(c_wr16_subst), .wr16_raw(cwc.wr16_sel), .wr16_src(cwc.wr16_src),
    .inc_op(cwc.inc_op), .inc_src(cwc.inc_src), .wz_we(cwc.wz_we), .wz_src(cwc.wz_src), .cond_mode(cwc.cond_mode),
    .swap_af(cwc.swap_af), .swap_gp(cwc.swap_gp), .swap_dehl(cwc.swap_dehl), .xhl(rf_sel_gp ? st[13] : st[12]),
    .rd8_ix(cwc.rd8_ix), .rd8_fd(cwc.rd8_fd), .rd8_xhl(cwc.rd8_xhl),
    .im_we(cwc.im_we), .im_val(cwc.im_val), .iff_op(cwc.iff_op), .halt_set(cwc.halt_set), .halt(st[2]), .int_lat(st[3]),
    .row_type(cwc.row_type), .row_dur(cwc.row_dur), .row_gate(c_row_gate), .row_ext(cwc.row_ext),
    .row_rpt(c_row_rpt), .row_last(cwc.row_last), .nxt_gate(cwc.nxt_gate), .sp_set(cwc.sp_set), .ir_ld(cwc.ir_ld),
    .n_m(c_n_m), .n_byte(c_n_byte), .pre_m1(c_pre_m1), .is_pfx(c_is_pfx)
  );
  function automatic logic [4:0] eff8(input cw_t w);        // register actually read by the 8-bit port
    case (w.rd8_sel)                                         // (8-11 = IXH IXL IYH IYL: never issued for reads)
      5'd2: eff8 = w.rd8_xhl ? 5'd4 : 5'd2;
      5'd3: eff8 = w.rd8_xhl ? 5'd5 : 5'd3;
      5'd4: eff8 = w.rd8_ix ? (w.rd8_fd ? 5'd10 : 5'd8) : (w.rd8_xhl ? 5'd2 : 5'd4);
      5'd5: eff8 = w.rd8_ix ? (w.rd8_fd ? 5'd11 : 5'd9) : (w.rd8_xhl ? 5'd3 : 5'd5);
      default: eff8 = w.rd8_sel;
    endcase
  endfunction
  function automatic logic [2:0] eff16(input cw_t w);       // pair actually read by the 16-bit port
    case (w.rd16_raw ^ 3'd6)
      3'd1: eff16 = w.rd16_xhl ? 3'd2 : 3'd1;
      3'd2: eff16 = w.rd16_ix ? (w.rd8_fd ? 3'd5 : 3'd4) : (w.rd16_xhl ? 3'd1 : 3'd2);
      default: eff16 = w.rd16_raw ^ 3'd6;
    endcase
  endfunction
  function automatic logic [4:0] effw8(input cw_t w);       // effective target of the 8-bit write
    effw8 = w.wr8_sel;
    if (w.rd8_ix && effw8[4:1] == 4'b0010) effw8 = {3'b010, w.rd8_fd, effw8[0]};
    if (w.rd16_xhl && effw8[4:3] == 2'b00 && (effw8[2] ^ effw8[1])) effw8 = effw8 ^ 5'b00110;
    if (!w.wr8_en) effw8 = 5'd0;
  endfunction
  function automatic logic [2:0] effw16(input cw_t w);      // effective pair of the 16-bit write
    effw16 = w.wr16_sel;
    if (w.rd16_ix && effw16 == 3'd2) effw16 = {2'b10, w.rd8_fd};
    if (w.rd16_xhl && (effw16 == 3'd1 || effw16 == 3'd2)) effw16 = effw16 ^ 3'd3;
    if (!w.wr16_en) effw16 = 3'd0;
  endfunction
  function automatic cw_t canon(input cw_t w, input logic fb, input logic t1a);
    canon = w;
    canon.wr8_sel = 5'd0;  canon.wr16_sel = 3'd0;
    canon.rd8_sel = 5'd0; canon.rd8_ix = 1'b0; canon.rd8_fd = 1'b0; canon.rd8_xhl = 1'b0;
    canon.rd16_raw = 3'd0; canon.rd16_ix = 1'b0; canon.rd16_xhl = 1'b0;
    if (!fb) begin canon.row_dur = 3'd0; canon.row_ext = 1'b0; canon.row_last = 1'b0; canon.nxt_gate = 1'b0;
                   canon.sp_set = 1'b0; canon.ir_ld = 1'b0; end
    if (!t1a) canon.row_type = 3'd0;
  endfunction
  logic p0ok;  initial p0ok = 1'b0;
  always @(posedge clk) p0ok <= rst ? 1'b1 : ss_load ? 1'b0 : ld2 ? 1'b1 : p0ok;
  wire t1a_now = (t_idx == 3'd1) && !phase;
  wire fb_use = cen && phase, t1a_use = cen && t1a_now;
  cw_t p0_eff;
  always_comb begin p0_eff = p0; p0_eff.row_type = row_type; end
  always @(negedge clk) if (p0ok && !rst && !ss_load) begin
    if (canon(p0_eff, fb_use, t1a_use) != canon(cwc, fb_use, t1a_use) || eff8(p0) != eff8(cwc) || eff16(p0) != eff16(cwc) ||
        effw8(p0) != effw8(cwc) || effw16(p0) != effw16(cwc))
      $fatal(1, "P0 != cw(state): m=%0d t=%0d f=%0d sp=%0d ir=%02h p0=%h cw=%h", m_idx, t_idx, phase, space, ir, p0_eff, cwc);
  end
  /* verilator lint_on UNUSEDSIGNAL */
  // (2) NX = the coordinates the state takes at the cen edge, T-states repeated by WAIT included; when an
  //     interrupt is accepted (M1 T1A with space 7 or int_lat) the fields the acceptance changes are not compared
  nx_t nx_exp;  logic nx_chk;  initial nx_chk = 1'b0;
  logic [7:0] ir_exp;
  always @(posedge clk) begin nx_exp <= nx;  ir_exp <= ir;  nx_chk <= cen && !rst && !ss_load && p0ok; end
  wire nx_int = (m_idx == 3'd1) && t1a_now && ((space == 3'd7) || st[3]);
  always @(negedge clk) if (nx_chk) begin
    if (nx_exp.phase != phase || nx_exp.t != t_idx || nx_exp.m != m_idx || nx_exp.moh != oh7(m_idx) || nx_exp.toh != oh7(t_idx) ||
        (!nx_int && (nx_exp.sp != space || nx_exp.six != seq_six || nx_exp.il != st[3] || nx_exp.halt != st[2])) ||
        nx_exp.xhl != (rf_sel_gp ? st[13] : st[12]) ||
        (!nx_int && ir == ir_exp && (nx_exp.sh2 != (seq_six && pd_ir[53]) || nx_exp.sh1 != (seq_six && pd_ir[11]))))
      $fatal(1, "NX != state after the edge: nx=%h m=%0d t=%0d f=%0d sp=%0d", nx_exp, m_idx, t_idx, phase, space);
  end
  // (3) the end-of-M condition of the sequencer (cond_nx) equals the one from P1 where it is consumed
  always @(negedge clk) if (cen && !rst && phase && (nxt_gate || row_ext) && cond_nx != cond_ok)
    $fatal(1, "cond_nx != cond_ok: mode %0d ir=%02h", p1_cond_mode, ir);
`endif
/* verilator lint_on SYNCASYNCNET */
  // checks: flags only towards F, AF8 only towards AF (otherwise the register file would write them elsewhere)
  always @(negedge clk) begin
    if (cen && p1_wr8_en  && p1_wr8_src  && p1_wr8_sel  != 5'd6) $fatal(1, "wr8_src=1 with wr8_sel=%0d", p1_wr8_sel);
    if (cen && p1_wr16_en && p1_wr16_src && p1_wr16_sel != 3'd7) $fatal(1, "wr16_src=1 with wr16_sel=%0d", p1_wr16_sel);
  end
`endif

endmodule
