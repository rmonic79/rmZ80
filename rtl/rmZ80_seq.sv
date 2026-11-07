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
// rmZ80_seq — the sequencer: T-state and M-cycle counters, bus waveforms, IR and the
// data latch, WZ, Q, interrupts, HALT, bus request, and the 64-bit sequencer state.
//
// Specification: docs/06-sequencer.md (§3 advancement, §5 state) and
//                docs/02-bus-cycles.md (§2-§7 waveforms).
//
// The schedule row of the current M-cycle (type, length, last M-cycle) comes from the
// control word (rmZ80_cw, through the top module). Resolution: HALF A T-STATE, one
// `cen` per half (A = first half of the T-state, B = second — docs/02 §8, docs/06 §3.1).
//
// Waveforms (source alongside):
//   M1:   address from the start of T1; MREQ+RD half a T later (T1B) [02 §3]; data
//         sampled on the edge that OPENS T3, the same edge releases RD and MREQ [02 §3];
//         refresh in T3-T4: RFSH active, {I, R} on the address, NEVER RD [02 §3].
//   RD/WR: 3 T; MREQ/RD as in the fetch [02 §4]; WR inactive half a T-state before
//         the bus changes (released in T3B) [02 §4].
//   IORD/IOWR: 3 + 1 automatic TW = 4 T [02 §5]; IORQ instead of MREQ.
//   IACK: special M1, IORQ instead of MREQ, 4 + 2 automatic TW = 6 T [02 §7].
//   WAIT: sampled on the falling edge (tick A->B) of T2 and of every TW, automatic or
//         not [02 §2]; the decision is held in tw and the T-state repeats on the rising edge.
//   R:    one r_inc pulse per M1 and per IACK (docs/03 §3.2), in the LAST half T-state of
//         the refresh window (T4B for M1, T6B for IACK). docs/02 §3.2: the refresh address
//         must stay STABLE over the whole window T3A-T4B, so the increment can only fall
//         before T3A or after T4B; here after: the bus shows R BEFORE the increment. The
//         source does not fix the instant (docs/02 §10).
//   MREQ during the M1 refresh: ACTIVE from T3B to T4A (docs/02 §3.1 point 4,
//         figure 5: low on the falling edge of T3, high on that of T4).
//
// CHOICES WHERE THE SPECIFICATION IS SILENT (docs/02 §10):
//   - assertion of WR (here T2A) and of IORQ/RD in I/O and IACK cycles (here T1B);
//   - MREQ during the IACK refresh (T5-T6): inactive here — [02] §7 puts IORQ in place
//     of MREQ and §3.1 speaks only of M1;
//   - bit 7 of the low byte of the refresh address ([02] §3 speaks of 7 low bits):
//     here r_val[7];
//   - IACK: no RD (the vector is read with IORQ alone) and refresh in T5-T6;
//   - extended M1 (5/6 T): the T-states after the 4th are internal, without bus activity;
//   - release of IORQ/RD/WR in I/O cycles: on the edge of the sampling T-state.
//
// STATE = docs/06 §5, 64 bits: one packed struct; the PARALLEL savestate is the vector
// itself (ss_dout = dbg_st), loaded whole with ss_load; SS_LEN = $bits (COMPUTED). WZ is
// written by the top module through wz_we/wz_din, the prefix space through sp_set/sp_val.
// At reset and at power-up ALL the state has a defined value (docs/04 §7).
// 0 M10K by construction: no unpacked array, no synchronous read.
// =============================================================================

module rmZ80_seq (
  input  logic        clk,
  input  logic        cen,
  input  logic        rst,        // asynchronous, active high

  // schedule row of the current M-cycle (from the control word; stable for the whole M-cycle)
  input  logic [2:0]  sch_type,   // MT_*
  input  logic [2:0]  sch_dur,    // length in T = 3 + sch_dur (3..7; 7 = IACK with 2 waits + 1 internal)
  input  logic        sch_last,   // last M-cycle of the instruction

  // values from the register file and the control word
  input  logic [15:0] addr_val,   // operating address
  input  logic [7:0]  dout_val,   // data to drive in a write
  input  logic [7:0]  i_val,      // register I (high byte of the refresh address, [02] §3)
  input  logic [7:0]  r_val,      // register R (low byte of the refresh address)
  // write of WZ from the top module (docs/06 §2.2): bit 0 = low byte, bit 1 = high byte,
  // both = all of WZ. The data is formed by the top module (DIN already sampled in din_q,
  // or the output of the address adder).
  input  logic [1:0]  wz_we,
  input  logic [15:0] wz_din,
  // prefix state (docs/06 §2.1 rule 11): at the end of a non-last M-cycle with sp_set=1,
  // space <- sp_val (the top module raises it in the M1 of a prefix); at the end of the
  // instruction (sch_last) space <- BASE.
  input  logic        sp_set,
  input  logic [2:0]  sp_val,
  // a prefix after a prefix (docs/03 §3.3): DD/FD/ED after DD/FD -> new space, and the next
  // M-cycle is again the M1 of the opcode; DD CB -> DDCB space, and the M-cycles keep counting
  input  logic        ir_ld,      // read that loads IR: fourth byte of DDCB/FDCB
  input  logic        q_we,       // the top module is writing the ALU flags to F (wr8 to F or wr16 to AF): register Q (docs/01 §3)
  input  logic        swap_dehl,  // EX DE,HL: toggles the DE<->HL renaming bit of the active bank (q_reg[2+sel_gp])
  input  logic        sel_gp,     // active BC/DE/HL bank (from the register file)
  // IM and IFF written by the control word (on cen):
  //   im_we: IM <- im_val (IM 0/1/2, docs/04 §2)
  //   iff_op: 1 = DI (IFF1 = IFF2 = 0, docs/04 §3); 2 = RETN/RETI (IFF1 <- IFF2, docs/04 §6)
  input  logic        im_we,
  input  logic [1:0]  im_val,
  input  logic [1:0]  iff_op,     // 3 = EI (IFF1 = IFF2 = 1; window closed at the end of the instruction, docs/04 §3)
  // HALT and NMI (docs/02 §7, docs/04 §4)
  //   halt_set: HALT executed -> halt_q = 1: the following M1 cycles read the bus but IR <- 00 (NOP), PC
  //             does not advance (control word), the refresh keeps running; left when an interrupt is accepted
  //   nmi_n:    falling edge stored in nmi_pend (nmi_lvl = level at the previous tick), sampled with INT
  //             (below); accepted at the end of the instruction: space <- 7, IR <- 66h (pseudo-instruction:
  //             M1 of 5 T ignored, push PC, PC <- 0066h), IFF1 <- 0 (IFF2 unchanged), leaves HALT.
  //             NMI has priority over INT.
  input  logic        halt_set,
  input  logic        nmi_n,
  // INT (docs/02 §7, docs/04 §1, §5): level, sampled together with the NMI edge on the edge that opens the
  // last T-state of every M-cycle; at the end of the instruction the sample decides (NMI has priority), if
  // IFF1 = 1 and the instruction is not EI/DI: IFF1 = IFF2 = 0, int_lat = 1, leaves HALT; IM1: space <- 7,
  // IR <- 38h; IM2: space <- 7, IR <- 7Eh (8-bit vector from the bus in the IACK, docs/04 §5); IM0: the next
  // M1 is the IACK and loads IR
  input  logic        int_n,
  // BUSREQ (docs/02 §6): sampled on the rising edge that opens the last T-state of EVERY M-cycle
  // ([UM] p.11, [PS] p.29 fig. 10); if active, the CPU releases the bus from the next edge (Tx).
  input  logic        busreq_n,

  // bus
  input  logic [7:0]  din,
  input  logic        wait_n,
  output logic [15:0] addr,
  output logic [7:0]  dout,
  output logic        mreq_n,
  output logic        iorq_n,
  output logic        rd_n,
  output logic        wr_n,
  output logic        rfsh_n,

  output logic        r_inc,      // one pulse per M1/IACK, to the register file
  output logic [63:0] dbg_st,     // view of the state (contract = order of docs/06 §5)

  // parallel savestate (load in one clock, overrides cen; rst overrides everything)
  input  logic        ss_load,
  input  logic [63:0] ss_din,
  output logic [63:0] ss_dout,

  // pre-decode of IR: pd = rmz80_pdec(IR), registered one clock after IR from the IR register
  // (rst and ss_load write it at once). Derived, not in the savestate (on a load it is computed
  // from the IR field of ss_din).
  output logic [rmZ80_alu_pkg::PD_N-1:0] pd,
  // copy of "space is DD or FD" (3 or 4), written on the same edges and from the same values
  // as the space: the control word computes its coordinates with one level instead of two
  output logic        six_o,
  // interrupt (NMI or INT) accepted at the edge with cen: for P/V of LD A,I / LD A,R ([UM] pp.94-95)
  output logic        int_acc
);
  import rmZ80_alu_pkg::*;   // only rmz80_pdec / PD_N are needed here
  initial begin pd = {{(PD_N-1){1'b0}}, 1'b1}; six_o = 1'b0; end   // power-up: IR = 00h -> only x00 (= rmz80_pdec(00h))
`ifdef VERILATOR
/* verilator lint_off SYNCASYNCNET */   // benches only: rst read as data in the check
  // check, in every bench: pd is ALWAYS the decode of IR
  logic [7:0] ir_prev; logic pd_now;
  always @(posedge clk) begin ir_prev <= st.ir; pd_now <= rst | ss_load; end
  always @(negedge clk) if (pd != rmz80_pdec(pd_now ? st.ir : ir_prev)) $fatal(1, "pd != pdec(IR of the previous clock): IR=%02h", st.ir);
  always @(negedge clk) if (six_o != (st.space == 3'd3 || st.space == 3'd4)) $fatal(1, "six_o out of step: space=%0d", st.space);
/* verilator lint_on SYNCASYNCNET */
`endif

  // ---- M-cycle types ---------------------------------------------------------
  localparam logic [2:0] MT_NONE = 3'd0, MT_M1  = 3'd1, MT_RD   = 3'd2,
                         MT_WR   = 3'd3, MT_IORD = 3'd4, MT_IOWR = 3'd5,
                         MT_IACK = 3'd6;

  // ---- state — docs/06 §5, IN THE ORDER of the table ---------------------------
  typedef struct packed {
    logic        phase;      // half of the T-state: 0 = A (first), 1 = B
    logic [2:0]  t_idx;     // T-state within the M-cycle (1..6)
    logic        tw;        // wait state: in phase B WAIT was sampled low (the T repeats); in phase A the T is a repetition
    logic [2:0]  m_idx;     // M-cycle within the instruction (1..6)
    logic [2:0]  mtype;     // type of the current M-cycle
    logic [7:0]  ir;        // current opcode
    logic [2:0]  space;    // prefix space (docs/06 §4.1)
    logic [7:0]  din_q;     // data latch in flight
    logic [15:0] wz;        // WZ/MEMPTR
    logic [7:0]  q_reg;     // bit 0 = the previous instruction wrote F, bit 1 = F written in this one; bits 2/3 = DE<->HL renamed (bank 0/1); bit 4 = BUSREQ sampled at the start of the last T, bit 5 = release sampled in the Tx, bit 6 = Tx in phase B; bit 7 = INT low at the edge that opens the last T (docs/04 §1)
    logic        iff1, iff2;
    logic [1:0]  im;
    logic        nmi_s;     // NMI edge stored at the edge that opens the last T: "sampled at the same time as the interrupt line" ([UM] p.13)
    logic        nmi_pend;  // NMI edge stored
    logic        int_lat;   // INT response in progress (mode 0: the instruction supplied by the device)
    logic        halt_q;
    logic        nmi_lvl;   // level of NMI at the previous tick (for the edge)
    logic        busack;
  } seq_state_t;

  localparam int SS_LEN = $bits(seq_state_t);   // computed: 64 expected

  seq_state_t st;
  // power-up = reset state: no bit left to the value chosen by the synthesis tool
  initial begin
    st = '0;  st.t_idx = 3'd1;  st.m_idx = 3'd1;  st.mtype = 3'd1;   // M1 T1A, IR 00h, space BASE, everything else 0
  end
  // ONE decoder: the source of IR is chosen first (ss_load: IR field of ss_din; M1 in HALT: 00h;
  // otherwise the bus data). Interrupts and rst write constants.
  // pd = rmz80_pdec(IR) one clock late, from the IR register: no logic between the data pin and a
  // flip-flop. The clock of delay does not change the behaviour: IR is loaded on the edge that opens
  // T3 (M1/IACK) or in the read of the fourth DDCB byte, and no row of the control word that ends in P1
  // uses IR in that first half T-state (checked: benches with cen every 2 clocks, SingleStepTests);
  // the direct reads of the control word at the cen edge already see pd updated (cen at least 2 clocks
  // apart). rst and ss_load write it at once.
  always_ff @(posedge clk or posedge rst) begin
    // ONE decoder: the source is chosen first (ss_load: IR field of ss_din, order of docs/06 §5 = st;
    // otherwise IR); rst writes the constant pdec(00h)
    if (rst) pd <= rmz80_pdec(8'h00);
    else     pd <= rmz80_pdec(ss_load ? ss_din[52:45] : st.ir);
  end
  wire [SS_LEN-1:0] sv = st;
  assign ss_dout = sv;                      // savestate read: wires
  assign dbg_st = sv;

  // ---- geometry of the cycle -------------------------------------------------
  // length 8 (IM0 on an instruction with an M1 of 6 T + 2 waits) = 3 + 5 -> 0 in 3 bits: the cycle
  // runs T1..T7 and closes on the eighth T, which is 0 in the counter (docs/04 §5: "+2 T")
  wire [2:0] durT = 3'd3 + sch_dur;

  logic [2:0] t3p;              // the "T3": data sampling / release
  always_comb begin
    case (st.mtype)
      MT_IORD, MT_IOWR: t3p = 3'd4;   // 3 + 1 automatic TW ([02] §5)
      MT_IACK:          t3p = 3'd5;   // 4 + 2 automatic TW ([02] §7)
      default:          t3p = 3'd3;
    endcase
  end

  wire has_bus  = (st.mtype != MT_NONE);
  // T-states in which WAIT is sampled: T2 and every TW ([02] §2); the sampling is at the A->B tick
  wire sample_w = has_bus && (st.t_idx >= 3'd2) && (st.t_idx < t3p);

  wire refresh_win = (st.mtype == MT_M1   && st.t_idx >= 3'd3 && st.t_idx <= 3'd4) ||
                     (st.mtype == MT_IACK && st.t_idx >= 3'd5 && st.t_idx <= 3'd6);

  // "operating" window: from (T1,B) to (t3p-1,B) included
  wire aft1B = (st.t_idx > 3'd1) || st.phase;
  wire env   = (st.t_idx < t3p) && aft1B;

  // ---- bus outputs (combinational on the state; registered in the top module) ----
  assign addr = refresh_win ? {i_val, r_val} : addr_val;
  assign dout = dout_val;

  // M1 refresh: MREQ active from T3B to T4A — docs/02 §3.1 point 4 (figure 5)
  wire rfsh_mreq = (st.mtype == MT_M1) &&
                   ((st.t_idx == 3'd3 && st.phase) || (st.t_idx == 3'd4 && !st.phase));
  assign mreq_n = ~( (((st.mtype == MT_M1) || (st.mtype == MT_RD)) ? env :
                      (st.mtype == MT_WR) ? (env || (st.t_idx == t3p && !st.phase))
                                          : 1'b0) | rfsh_mreq );
  assign rd_n   = ~( ((st.mtype == MT_M1) || (st.mtype == MT_RD) ||
                      (st.mtype == MT_IORD)) ? env : 1'b0 );
  assign iorq_n = ~( ((st.mtype == MT_IORD) || (st.mtype == MT_IACK)) ? env :
                     (st.mtype == MT_IOWR) ? (env || (st.t_idx == t3p && !st.phase))
                                           : 1'b0 );
  assign wr_n   = ~( ((st.mtype == MT_WR) || (st.mtype == MT_IOWR))
                     ? ((st.t_idx >= 3'd2 && st.t_idx < t3p) ||
                        (st.t_idx == t3p && !st.phase))
                     : 1'b0 );
  assign rfsh_n = ~refresh_win;

  // R: pulse in the LAST half T-state of the refresh window (T4B / T6B), so that the increment
  // falls AFTER the window and the address stays stable (docs/02 §3.2). The source does not fix
  // the instant (docs/02 §10).
  assign r_inc  = refresh_win && st.phase &&
                  (st.t_idx == ((st.mtype == MT_M1) ? 3'd4 : 3'd6));

  // ---- loads from the bus: registered conditions ------------------------------------------
  // The edge that OPENS the sampling T-state ([02] §3): M1 -> IR (in HALT 00h, NOP [02] §7; in the response
  // to an interrupt, space 7, the byte is ignored [04] §4); IACK -> IR (IM0) or vector into din_q (IM2);
  // RD/IORD -> IR (fourth DDCB byte, ir_ld) or din_q. The conditions depend only on the state and on P0
  // (constant within the half T-state): registered on every clock, so at the cen edge they are those of the
  // current state (cen at least 2 clocks apart; after rst 0: T1A loads nothing; after ss_load valid from the
  // second quiet clock).
  wire at_ld = st.phase && (st.t_idx + 3'd1 == t3p);
  wire ld_ir_din_d  = at_ld && (((st.mtype == MT_M1) && (st.space != 3'd7) && !st.halt_q) ||
                                ((st.mtype == MT_IACK) && (st.space != 3'd7)) ||
                                ((st.mtype == MT_RD || st.mtype == MT_IORD) && ir_ld));
  wire ld_ir_zero_d = at_ld && (st.mtype == MT_M1) && (st.space != 3'd7) && st.halt_q;
  wire ld_dq_din_d  = at_ld && (st.mtype == MT_RD || st.mtype == MT_IORD) && !ir_ld;
  wire ld_dq_vec_d  = at_ld && (st.mtype == MT_IACK) && (st.space == 3'd7);
  logic ld_ir_din_q, ld_ir_zero_q, ld_dq_din_q, ld_dq_vec_q;
  initial begin ld_ir_din_q = 1'b0; ld_ir_zero_q = 1'b0; ld_dq_din_q = 1'b0; ld_dq_vec_q = 1'b0; end
  always_ff @(posedge clk or posedge rst) begin
    if (rst) begin ld_ir_din_q <= 1'b0; ld_ir_zero_q <= 1'b0; ld_dq_din_q <= 1'b0; ld_dq_vec_q <= 1'b0; end
    else begin ld_ir_din_q <= ld_ir_din_d; ld_ir_zero_q <= ld_ir_zero_d; ld_dq_din_q <= ld_dq_din_d; ld_dq_vec_q <= ld_dq_vec_d; end
  end
`ifdef VERILATOR
  // the registered selections equal the conditions of the current state at every cen edge
  always @(negedge clk) if (cen && !rst && !ss_load &&
      {ld_ir_din_q, ld_ir_zero_q, ld_dq_din_q, ld_dq_vec_q} != {ld_ir_din_d, ld_ir_zero_d, ld_dq_din_d, ld_dq_vec_d})
    $fatal(1, "registered load selections differ from the state: t=%0d mtype=%0d", st.t_idx, st.mtype);
  // the load T-state is never the last of the M-cycle (the two actions do not overlap)
  always @(negedge clk) if (cen && at_ld && (st.t_idx == durT)) $fatal(1, "load in the last T-state of the M-cycle");
`endif

  // ---- advancement: next state as a SIGNAL -----------------------------------------
  // st_cen = state after an edge WITH cen (without rst/ss_load); the register copies it on cen.
  // The last assignment wins; reads are from the current state. NX (top module) mirrors these rules.
  seq_state_t st_cen;  logic six_cen;
  always_comb begin
    st_cen = st;  six_cen = six_o;
      // WZ: byte write from the top module (independent of the phase)
      if (wz_we[0]) st_cen.wz[7:0] = wz_din[7:0];
      if (wz_we[1]) st_cen.wz[15:8] = wz_din[15:8];
      if (im_we) st_cen.im = im_val;                                      // IM n
      // Q (docs/01 §3, rule from z80test): bit 1 = "F written in THIS instruction" (q_we from the top
      // module, already in P1); bit 0 = "the PREVIOUS instruction wrote F", set at the first tick of EVERY
      // M1 (a prefix too: SingleStepTests give A|F for DD 37, that is the prefix clears it).
      // SCF/CCF (NMOS): YF/XF = A | (F & ~bit0). Bits 2/3: DE<->HL renaming per bank.
      if (st.m_idx == 3'd1 && st.t_idx == 3'd1 && !st.phase && !st.tw) begin   // EVERY M1, prefixes included
        st_cen.q_reg[0] = st.q_reg[1] | q_we; st_cen.q_reg[1] = 1'b0;
      end else if (q_we) st_cen.q_reg[1] = 1'b1;
      if (swap_dehl) st_cen.q_reg[2 + sel_gp] = ~st.q_reg[2 + sel_gp];        // EX DE,HL = renaming (bit 2: bank 0, bit 3: bank 1)
      if (iff_op == 2'd1) begin st_cen.iff1 = 1'b0; st_cen.iff2 = 1'b0; end   // DI
      else if (iff_op == 2'd2) st_cen.iff1 = st.iff2;                     // RETN, RETI
      else if (iff_op == 2'd3) begin st_cen.iff1 = 1'b1; st_cen.iff2 = 1'b1; end   // EI
      if (halt_set) st_cen.halt_q = 1'b1;                                 // HALT
      st_cen.nmi_lvl = ~nmi_n;                                            // NMI edge: level at the previous tick
      if (~nmi_n && !st.nmi_lvl) st_cen.nmi_pend = 1'b1;
      if (!st.phase) begin
        st_cen.phase = 1'b1;                       // A -> B (falling edge)
        // WAIT sampled on the FALLING edge of T2 and of every TW ([UM] p.7, docs/02 §2): the decision stays in tw
        // for phase B, and on the rising edge the T-state repeats if tw = 1
        st_cen.tw = sample_w && !wait_n;
        // the type of the new M-cycle is sampled at its start (in T1A all bus outputs are at rest for any
        // type), so that the schedule row of the CURRENT cycle stays valid until its last tick
        if (st.t_idx == 3'd1) st_cen.mtype = sch_type;
      end else begin
        st_cen.phase = 1'b0;                       // B -> A of the next T-state
        if (st.tw) begin
          st_cen.tw = 1'b1;                       // the T-state repeats as a TW ([02] §2); tw stays 1 in phase A of the TW
        end else begin
          st_cen.tw = 1'b0;
          if (st.t_idx == durT) begin          // end of the M-cycle
            st_cen.t_idx = 3'd1;
            st_cen.m_idx = (sch_last || sp_set) ? 3'd1 : (st.m_idx + 3'd1);   // after the M1 of a prefix the count restarts from 1
            if (sch_last) begin                     // end of the instruction: BASE, then the interrupt WINDOW ([02] §7)
              st_cen.space = 3'd0; six_cen = 1'b0; st_cen.int_lat = 1'b0;
              if (!st.q_reg[4]) begin               // window at the end of EVERY instruction; never if BUSREQ was sampled ([UM] p.12)
                if (st.nmi_s) begin                                    // NMI accepted (docs/04 §4), priority: pending at the sampling edge
                  st_cen.space = 3'd7; six_cen = 1'b0; st_cen.ir = 8'h66;
                  st_cen.iff1 = 1'b0;      // IFF2 is NOT touched: it keeps the state from before ([UM] p.19 table 1; [SY] pp.20-21, p.24, measured with a nested NMI)
                  st_cen.nmi_pend = 1'b0; st_cen.halt_q = 1'b0;
                end else if (st.q_reg[7] && st.iff1 && iff_op != 2'd1 && iff_op != 2'd3) begin   // INT low at the sampling edge (docs/04 §3, §5)
                  st_cen.iff1 = 1'b0; st_cen.iff2 = 1'b0; st_cen.int_lat = 1'b1; st_cen.halt_q = 1'b0;
                  if (st.im == 2'd1)      begin st_cen.space = 3'd7; six_cen = 1'b0; st_cen.ir = 8'h38; end
                  else if (st.im == 2'd2) begin st_cen.space = 3'd7; six_cen = 1'b0; st_cen.ir = 8'h7E; end
                end
              end
            end
            else if (sp_set)  begin st_cen.space = sp_val; six_cen = (sp_val == 3'd3) || (sp_val == 3'd4); end  // M1 of a prefix: new space
          end else begin
            st_cen.t_idx = st.t_idx + 3'd1;
            // edge that OPENS the sampling T-state ([02] §3); in the response to an interrupt (space 7)
            // the byte read by the M1 is IGNORED ([04] §4); in HALT the M1 reads but executes a NOP ([02] §7).
            // The conditions are registered one clock earlier (ld_*_q, above): after the data pin only the
            // selection remains, one level
            if (ld_ir_din_q)  st_cen.ir    = din;
            if (ld_ir_zero_q) st_cen.ir    = 8'h00;
            if (ld_dq_din_q)  st_cen.din_q = din;
            if (ld_dq_vec_q)  st_cen.din_q = din;    // IM2: 8-bit vector, bit 0 included ([SY] p.20 and p.24, measured; docs/04 §5)
          end
        end
      end
      // ---- BUSREQ / BUSACK (docs/02 §6; [PS] p.29 and fig. 10, [UM] pp.11-12) ----------------------------------
      // Rising edge = end of phase B (B -> A). BUSREQ is sampled on the edge that OPENS the last T-state of any
      // M-cycle (TLM); if it was active, on the edge that closes TLM the CPU enters the Tx states: BUSACK low,
      // address, data, MREQ IORQ RD WR released, RFSH and M1 high; the state stays at T1 of the next cycle,
      // HELD. In every Tx BUSREQ is sampled again on the rising edge that opens it: if it is high, BUSACK rises
      // on the falling edge of that Tx and T1 starts on the next rising edge. No interrupt and no refresh while
      // the bus is released; the NMI edge is stored (it is taken at the end of an instruction, afterwards).
      if (st.busack) begin
        st_cen = st;  six_cen = six_o;                                   // everything held...
        st_cen.nmi_lvl = ~nmi_n;                                         // ...except the NMI edge
        if (~nmi_n && !st.nmi_lvl) st_cen.nmi_pend = 1'b1;
        if (!st.q_reg[6]) st_cen.q_reg[6] = 1'b1;                        // falling edge: Tx from A to B
        else if (st.q_reg[5]) begin                                      // rising edge, release sampled: T1 starts
          st_cen.busack = 1'b0;  st_cen.q_reg[5] = 1'b0;  st_cen.q_reg[6] = 1'b0;
        end else begin                                                   // rising edge: another Tx, sample again
          st_cen.q_reg[6] = 1'b0;  st_cen.q_reg[5] = busreq_n;
        end
      end else if (st.phase && !st.tw) begin
        // rising edge that opens the last T-state of every M-cycle (TLM): BUSREQ ([PS] fig. 10), INT ([UM] p.12 and
        // fig. 9: "the rising edge of the final clock"; in HALT "during each T4", p.14) and NMI ("at the same time",
        // p.13) are sampled. At the end of the instruction the sample of the last M-cycle executed decides.
        if (st.t_idx != durT && (st.t_idx + 3'd1) == durT) begin
          st_cen.q_reg[4] = ~busreq_n;  st_cen.q_reg[7] = ~int_n;  st_cen.nmi_s = st.nmi_pend | (~nmi_n & ~st.nmi_lvl);
        end
        if (st.t_idx == durT && st.q_reg[4]) begin                                         // closes TLM: release the bus
          st_cen.busack = 1'b1;  st_cen.q_reg[4] = 1'b0;  st_cen.q_reg[6] = 1'b0;  st_cen.q_reg[5] = busreq_n;
        end
      end
  end

  // same condition as the acceptance branches of st_cen (end of the instruction, no bus release in progress or
  // requested, NMI sampled, or INT sampled with IFF1 and outside the EI/DI window)
  assign int_acc = st.phase && !st.busack && !st.tw && (st.t_idx == durT) && sch_last && !st.q_reg[4] &&
                   (st.nmi_s || (st.q_reg[7] && st.iff1 && iff_op != 2'd1 && iff_op != 2'd3));

  // ---- state register ----------------------------------------------------------
  // ASYNCHRONOUS rst (as in the register file): outside the enable of the flip-flops
  always_ff @(posedge clk or posedge rst) begin
    if (rst) begin
      st.phase  <= 1'b0; st.t_idx <= 3'd1; st.tw <= 1'b0;
      st.m_idx <= 3'd1; st.mtype <= MT_M1;                // constant: M1 in T1A; the real type is sampled
      st.space <= 3'd0; six_o <= 1'b0;                   // at the first cen
      st.iff1 <= 1'b0; st.iff2 <= 1'b0; st.im <= 2'd0;   // [04] §7
      st.nmi_s <= 1'b0; st.nmi_pend <= 1'b0; st.int_lat <= 1'b0;
      st.halt_q <= 1'b0; st.nmi_lvl <= 1'b0; st.busack <= 1'b0;
      st.ir <= 8'h00; st.q_reg <= 8'h00;                  // IR = NOP (no M1 action of the interrupted opcode),
                                                          // Q and the DE<->HL renaming to zero
      st.din_q <= 8'h00; st.wz <= 16'h0000;              // no source fixes them; DEFINED values (not left to the
                                                          // synthesis tool: with power-up don't care they changed with every netlist)
    end else if (ss_load) begin
      st <= seq_state_t'(ss_din);           // parallel load in one clock
      six_o <= (ss_din[44:42] == 3'd3) || (ss_din[44:42] == 3'd4);
    end else if (cen) begin
      st <= st_cen;  six_o <= six_cen;
    end
  end

endmodule
