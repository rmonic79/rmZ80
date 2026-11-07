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
// rmZ80_regfile — the Z80 register file, synchronous on clk + cen
//
// Specification: docs/01-registers-and-state.md (§1 registers, §5 savestate inventory,
//                §6 implementation notes) and docs/04 §7 (reset).
//
// ZERO M10K by construction: ALL the state is one packed struct of flip-flops
// (no unpacked array, no pattern that could be inferred as RAM).
//
// Alternate set: PHYSICAL banks. Index [0] of a/f/b..l_bk is ALWAYS the active bank, [1] the
// shadow; swap_af / swap_gp SWAP the contents and toggle their selection bit (sel_af / sel_gp =
// LOGICAL identity of the active bank, docs/01 §6). Reads never go through a bank multiplexer.
// Savestate: the PHYSICAL state: in the fields a/f/b..l_bk index [0] is the active bank;
// sel_af/sel_gp say which logical bank it is (docs/01 §5).
//
// R: 7-bit increment with bit 7 PRESERVED (docs/01 §1). The 8-bit write (LD R,A)
// writes all 8 bits and, if simultaneous with r_inc, WINS.
// Simultaneous 8- and 16-bit writes to the same register: the 8-bit port wins
// (the control word never does it).
//
// RESET and POWER-UP (docs/04 §7): AF = SP = FFFFh ([SY] §2.4 p.9, measured), PC = 0000h,
// I = R = 0, selectors 0, every other register (undefined for the source) 0.
//
// PARALLEL SAVESTATE, GENERATED from the declaration (docs/01 §6):
// the state is ONE packed struct `state_t`; ss_dout is the vector view of the
// struct (wires, no logic), ss_din is loaded whole on the edge with ss_load = 1
// (independent of cen; overrides cen, rst overrides everything). The width is
// SS_LEN = $bits(state_t) = 210 (208 architectural + 2 selectors, docs/01 §5): if
// the struct changes, the assignment ss_dout = sv no longer matches in width and
// lint reports it. Bit order = field order, MSB first.
// =============================================================================

module rmZ80_regfile (
  input  logic        clk,
  input  logic        cen,       // clock enable of the core
  input  logic        rst,       // asynchronous, active high: reset values below

  // one 8-bit read port (combinational)
  input  logic [4:0]  rd8_sel,
  // substitutions on the DATA side of the 8-bit port: in parallel, outside the selector cone
  input  logic        rd8_ix,    // codes H/L read IXh/IXl (rd8_fd = 0) or IYh/IYl (rd8_fd = 1): DD/FD forms without (IX+d)
  input  logic        rd8_fd,
  input  logic        rd8_xhl,   // DE<->HL renamed (EX DE,HL): D/E read H/L and vice versa (unless already IX/IY)
  output logic [7:0]  rd8_data,
  // one 8-bit write port
  input  logic        wr8_en,
  input  logic [4:0]  wr8_sel,
  input  logic [7:0]  wr8_data,
  input  logic [7:0]  wr8_f,      // data for F: the ALU flags arrive ONLY here
  // one 16-bit read port (pair, towards the address bus; combinational)
  input  logic [2:0]  rd16_sel,   // RAW code of the control word: logical code = rd16_sel ^ 6 (0 at rest = PC)
  input  logic        rd16_ix,    // logical HL -> IX (rd16_fd = 0) or IY (rd16_fd = 1): indexed forms
  input  logic        rd16_fd,
  input  logic        rd16_xhl,   // DE<->HL renamed (only if HL is not already IX/IY)
  output logic [15:0] rd16_data,
  // DEDICATED outputs (docs/06 §2.2): A, F and B of the ACTIVE bank, I and R. They are
  // wires on the existing registers: no new state, the savestate stays 210 bits. They
  // feed the ALU (a, f_in), the conditions (B) and the refresh address ({I, R})
  // without using the rd8/rd16 ports.
  output logic [7:0]  a_out,
  output logic [7:0]  f_out,
  output logic [7:0]  i_out,
  output logic [7:0]  r_out,
  output logic [7:0]  b_out,     // B of the active bank (condition B != 0 of DJNZ/blocks for NX)
  // one 16-bit write port (from the address adder)
  input  logic        wr16_en,
  input  logic [2:0]  wr16_sel,
  // substitutions on the WRITE side: the selectors arrive raw from the control word, as for reads
  input  logic        wr_ix8,    // H/L -> IXh/IXl (wr_fd = 0) or IYh/IYl (wr_fd = 1): DD/FD forms without (IX+d)
  input  logic        wr_ix16,   // HL -> IX / IY
  input  logic        wr_fd,
  input  logic        wr_xhl,    // DE<->HL renamed (EX DE,HL), after the IX/IY substitution
  input  logic [15:0] wr16_data,
  input  logic [15:0] wr16_af,    // data for AF: {A, flags} arrives ONLY here
  // increment of R (M1 fetch)
  input  logic        r_inc,
  // bank swaps
  input  logic        swap_af,   // EX AF,AF'
  input  logic        swap_gp,   // EXX
  input  logic        pv_clr,    // P/V <- 0: interrupt accepted at the end of LD A,I / LD A,R ([UM] pp.94-95)
  output logic        sel_gp_o,  // active BC/DE/HL bank (for the DE<->HL renaming bit in the sequencer)
  // parallel savestate (see the header)
  input  logic         ss_load,
  input  logic [209:0] ss_din,
  output logic [209:0] ss_dout
);

  // ---- selectors of the 8-bit port -----------------------------------------
  // 0-7 follow the rrr field of the opcode (docs/03 §1.1): B C D E H L - A;
  // the value 110, which is not a register in the instruction set, is used for F.
  localparam logic [4:0] R8_B  = 5'd0,  R8_C   = 5'd1,  R8_D   = 5'd2,
                         R8_E  = 5'd3,  R8_H   = 5'd4,  R8_L   = 5'd5,
                         R8_F  = 5'd6,  R8_A   = 5'd7,
                         R8_IXH = 5'd8,  R8_IXL = 5'd9,
                         R8_IYH = 5'd10, R8_IYL = 5'd11,
                         R8_SPH = 5'd12, R8_SPL = 5'd13,
                         R8_PCH = 5'd14, R8_PCL = 5'd15,
                         R8_I   = 5'd16, R8_R   = 5'd17;
  // ---- selectors of the 16-bit port ----------------------------------------
  // 0-3 follow the dd field of the opcode (docs/03 §1.2): BC DE HL SP; then IX IY PC AF.
  localparam logic [2:0] R16_BC = 3'd0, R16_DE = 3'd1, R16_HL = 3'd2,
                         R16_SP = 3'd3, R16_IX = 3'd4, R16_IY = 3'd5,
                         R16_PC = 3'd6, R16_AF = 3'd7;

  // ---- the state: ONE place (docs/01 §6) -----------------------------------
  // Every field in here is automatically in the savestate.
  typedef struct packed {
    logic [1:0][7:0] a_bk, f_bk;                          // AF in the 2 banks
    logic [1:0][7:0] b_bk, c_bk, d_bk, e_bk, h_bk, l_bk;  // BC DE HL in the 2 banks
    logic [15:0]     ix, iy, sp, pc;
    logic [7:0]      i_reg, r_reg;
    logic            sel_af;                              // active AF bank
    logic            sel_gp;                              // active BC/DE/HL bank
  } state_t;

  localparam int SS_LEN = $bits(state_t);   // computed, not written (210 expected)

  state_t st;                               // PHYSICAL: [0] = active bank, [1] = shadow
  wire [SS_LEN-1:0] sv = st;                // savestate = the PHYSICAL state (bank [0] = active)
  assign ss_dout = sv;                      // savestate read: wires (equal widths by construction)

  // ---- next state: one-hot selection and ONE multiplexer per byte ---------------------------
  // The 8-bit port wins over the 16-bit one on the same target; R: LD R,A wins over the 7-bit
  // increment (bit 7 preserved, docs/01 §1); flags and AF8 only towards F/AF. One enable for all
  // the flip-flops (cen): no per-register enable (the flip-flops stay packable together).
  // effective target: IX/IY substitution first, then DE<->HL
  logic [4:0] w8e;  logic [2:0] w16e;
  always_comb begin
    w8e = wr8_sel;
    if (wr_ix8 && w8e[4:1] == 4'b0010) w8e = {3'b010, wr_fd, w8e[0]};
    if (wr_xhl && w8e[4:3] == 2'b00 && (w8e[2] ^ w8e[1])) w8e = w8e ^ 5'b00110;
    w16e = wr16_sel;
    if (wr_ix16 && w16e == R16_HL) w16e = {2'b10, wr_fd};
    if (wr_xhl && (w16e == R16_DE || w16e == R16_HL)) w16e = w16e ^ 3'd3;
  end
  wire [17:0] h8  = wr8_en  ? (18'd1 << w8e)  : 18'd0;   // one-hot of the 8-bit target
  wire [7:0]  h16 = wr16_en ? (8'd1  << w16e) : 8'd0;    // one-hot of the 16-bit pair
  function automatic logic [7:0] nb(input logic h8x, input logic [7:0] d8,
                                    input logic h16x, input logic [7:0] d16, input logic [7:0] q);
    return h8x ? d8 : (h16x ? d16 : q);
  endfunction
  state_t nxt;
  logic [7:0] wa, wf, wb, wc, wd, we, wh, wl;              // active bank after the writes
  always_comb begin
    nxt = st;
    wb = nb(h8[R8_B], wr8_data, h16[R16_BC], wr16_data[15:8], st.b_bk[0]);
    wc = nb(h8[R8_C], wr8_data, h16[R16_BC], wr16_data[7:0],  st.c_bk[0]);
    wd = nb(h8[R8_D], wr8_data, h16[R16_DE], wr16_data[15:8], st.d_bk[0]);
    we = nb(h8[R8_E], wr8_data, h16[R16_DE], wr16_data[7:0],  st.e_bk[0]);
    wh = nb(h8[R8_H], wr8_data, h16[R16_HL], wr16_data[15:8], st.h_bk[0]);
    wl = nb(h8[R8_L], wr8_data, h16[R16_HL], wr16_data[7:0],  st.l_bk[0]);
    wa = nb(h8[R8_A], wr8_data, h16[R16_AF], wr16_af[15:8],   st.a_bk[0]);
    wf = nb(h8[R8_F], wr8_f,    h16[R16_AF], wr16_af[7:0],    st.f_bk[0]);
    // swap = the active bank becomes the shadow and the shadow becomes active. EX AF,AF' and EXX write
    // no register of their own bank in the same tick (checked below): the shadow receives the old active only
    nxt.b_bk[0] = swap_gp ? st.b_bk[1] : wb;  nxt.b_bk[1] = swap_gp ? st.b_bk[0] : st.b_bk[1];
    nxt.c_bk[0] = swap_gp ? st.c_bk[1] : wc;  nxt.c_bk[1] = swap_gp ? st.c_bk[0] : st.c_bk[1];
    nxt.d_bk[0] = swap_gp ? st.d_bk[1] : wd;  nxt.d_bk[1] = swap_gp ? st.d_bk[0] : st.d_bk[1];
    nxt.e_bk[0] = swap_gp ? st.e_bk[1] : we;  nxt.e_bk[1] = swap_gp ? st.e_bk[0] : st.e_bk[1];
    nxt.h_bk[0] = swap_gp ? st.h_bk[1] : wh;  nxt.h_bk[1] = swap_gp ? st.h_bk[0] : st.h_bk[1];
    nxt.l_bk[0] = swap_gp ? st.l_bk[1] : wl;  nxt.l_bk[1] = swap_gp ? st.l_bk[0] : st.l_bk[1];
    nxt.a_bk[0] = swap_af ? st.a_bk[1] : wa;  nxt.a_bk[1] = swap_af ? st.a_bk[0] : st.a_bk[1];
    nxt.f_bk[0] = swap_af ? st.f_bk[1] : wf;  nxt.f_bk[1] = swap_af ? st.f_bk[0] : st.f_bk[1];
    nxt.ix[15:8] = nb(h8[R8_IXH], wr8_data, h16[R16_IX], wr16_data[15:8], st.ix[15:8]);
    nxt.ix[7:0]  = nb(h8[R8_IXL], wr8_data, h16[R16_IX], wr16_data[7:0],  st.ix[7:0]);
    nxt.iy[15:8] = nb(h8[R8_IYH], wr8_data, h16[R16_IY], wr16_data[15:8], st.iy[15:8]);
    nxt.iy[7:0]  = nb(h8[R8_IYL], wr8_data, h16[R16_IY], wr16_data[7:0],  st.iy[7:0]);
    nxt.sp[15:8] = nb(h8[R8_SPH], wr8_data, h16[R16_SP], wr16_data[15:8], st.sp[15:8]);
    nxt.sp[7:0]  = nb(h8[R8_SPL], wr8_data, h16[R16_SP], wr16_data[7:0],  st.sp[7:0]);
    nxt.pc[15:8] = nb(h8[R8_PCH], wr8_data, h16[R16_PC], wr16_data[15:8], st.pc[15:8]);
    nxt.pc[7:0]  = nb(h8[R8_PCL], wr8_data, h16[R16_PC], wr16_data[7:0],  st.pc[7:0]);
    if (pv_clr) nxt.f_bk[0][2] = 1'b0;
    if (h8[R8_I]) nxt.i_reg = wr8_data;
    if (h8[R8_R])   nxt.r_reg      = wr8_data;                         // LD R,A: all 8 bits, wins
    else if (r_inc) nxt.r_reg[6:0] = st.r_reg[6:0] + 7'd1;             // 7-bit increment
    // bank swaps: each toggles ONLY its own bit (docs/01 §6)
    if (swap_af) nxt.sel_af = ~st.sel_af;
    if (swap_gp) nxt.sel_gp = ~st.sel_gp;
  end

`ifdef VERILATOR
/* verilator lint_off SYNCASYNCNET */   // benches only
  // check: no write to a bank in the tick that swaps it
  always @(negedge clk) if (cen && !rst && !ss_load) begin
    if (swap_af && (h8[R8_A] || h8[R8_F] || h16[R16_AF])) $fatal(1, "write to AF together with EX AF,AF'");
    if (swap_gp && (h8[R8_B] || h8[R8_C] || h8[R8_D] || h8[R8_E] || h8[R8_H] || h8[R8_L] ||
                    h16[R16_BC] || h16[R16_DE] || h16[R16_HL])) $fatal(1, "write to BC/DE/HL together with EXX");
  end
/* verilator lint_on SYNCASYNCNET */
`endif

  // ---- reset AND power-up value ----------------------------------------------------------
  // [SY] §2.4 p.9 (and the RESET pin, p.10): AF and SP = FFFFh after reset (measured); PC = 0; I and R = 0
  // (§6.1 p.23 for R); every other register UNDEFINED: here 0 (defined; zero uses the clear of the flip-flops,
  // at no cost). The same value is the POWER-UP value (initial): no register is left to the value chosen by
  // the synthesis tool (ALLOW_POWER_UP_DONT_CARE).
  function automatic state_t rst_val();
    rst_val = '0;
    rst_val.a_bk[0] = 8'hFF;  rst_val.f_bk[0] = 8'hFF;  rst_val.sp = 16'hFFFF;   // AF = SP = FFFFh (active bank)
  endfunction
  initial st = rst_val();

  // ---- state register --------------------------------------------------------
  // ASYNCHRONOUS rst: the reset of the flip-flops does not go through their enable, so the enable is
  // cen | ss_load and the clock enable from the system reaches the registers with one level of logic less.
  always_ff @(posedge clk or posedge rst) begin
    if (rst) begin
      st <= rst_val();                     // docs/04 §7 + [SY] §2.4 (above)
    end else if (ss_load) begin
      st <= state_t'(ss_din);               // parallel load in one clock
    end else if (cen) begin
      st <= nxt;
    end
  end

  // ---- dedicated outputs: wires, no selection logic beyond the bank --------
  assign sel_gp_o = st.sel_gp;
  assign a_out = st.a_bk[0];
  assign b_out = st.b_bk[0];
  assign f_out = st.f_bk[0];
  assign i_out = st.i_reg;
  assign r_out = st.r_reg;

  // ---- combinational reads ---------------------------------------------------
  always_comb begin
    case (rd8_sel)
      R8_B:   rd8_data = st.b_bk[0];
      R8_C:   rd8_data = st.c_bk[0];
      R8_D:   rd8_data = rd8_xhl ? st.h_bk[0] : st.d_bk[0];
      R8_E:   rd8_data = rd8_xhl ? st.l_bk[0] : st.e_bk[0];
      R8_H:   rd8_data = rd8_ix ? (rd8_fd ? st.iy[15:8] : st.ix[15:8]) : (rd8_xhl ? st.d_bk[0] : st.h_bk[0]);
      R8_L:   rd8_data = rd8_ix ? (rd8_fd ? st.iy[7:0]  : st.ix[7:0])  : (rd8_xhl ? st.e_bk[0] : st.l_bk[0]);
      R8_F:   rd8_data = st.f_bk[0];
      R8_A:   rd8_data = st.a_bk[0];
      // R8_IXH..R8_IYL (8-11): NEVER issued by the control word on the READ port (the IX/IY
      // substitution is done on the data side with rd8_ix/rd8_fd), so they are not in this
      // multiplexer; they are valid for WRITES (wr8_sel).
      R8_SPH: rd8_data = st.sp[15:8];
      R8_SPL: rd8_data = st.sp[7:0];
      R8_PCH: rd8_data = st.pc[15:8];
      R8_PCL: rd8_data = st.pc[7:0];
      R8_I:   rd8_data = st.i_reg;
      R8_R:   rd8_data = st.r_reg;
      default: rd8_data = 8'h00;
    endcase
  end

  always_comb begin
    // labels ^ 6: the code arrives raw from the control word (0 at rest = PC); IX/IY and DE<->HL on the data side
    case (rd16_sel)
      R16_BC ^ 3'd6: rd16_data = {st.b_bk[0], st.c_bk[0]};
      R16_DE ^ 3'd6: rd16_data = rd16_xhl ? {st.h_bk[0], st.l_bk[0]}
                                          : {st.d_bk[0], st.e_bk[0]};
      R16_HL ^ 3'd6: rd16_data = rd16_ix ? (rd16_fd ? st.iy : st.ix)
                               : rd16_xhl ? {st.d_bk[0], st.e_bk[0]}
                                          : {st.h_bk[0], st.l_bk[0]};
      R16_SP ^ 3'd6: rd16_data = st.sp;
      R16_IX ^ 3'd6: rd16_data = st.ix;
      R16_IY ^ 3'd6: rd16_data = st.iy;
      R16_PC ^ 3'd6: rd16_data = st.pc;
      R16_AF ^ 3'd6: rd16_data = {st.a_bk[0], st.f_bk[0]};
      default: rd16_data = 16'h0000;
    endcase
  end

endmodule
