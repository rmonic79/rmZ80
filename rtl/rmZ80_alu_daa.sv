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
// rmZ80_alu_daa — the TOTAL rule of DAA, docs/05 §8.2 (three tables, [SY] §4.7,
// checked against the [UM] table). Part of the rmZ80 ALU (see rtl/rmZ80_alu.sv).
// Depends ONLY on a (= A) and on C/H/N of f_in: the top module computes it before
// P1. diff_sel: 0 = 00h, 1 = 06h, 2 = 60h, 3 = 66h.
// =============================================================================

module rmZ80_alu_daa (
  input  logic [7:0] a,
  input  logic [7:0] f_in,
  output logic [1:0] diff_sel,
  output logic       daa_c,
  output logic       daa_h
);
  import rmZ80_alu_pkg::*;
  wire  [3:0] dhi = a[7:4];
  wire  [3:0] dlo = a[3:0];
  wire        dc  = f_in[FB_C];
  wire        dh  = f_in[FB_H];
  wire        dn  = f_in[FB_N];
  always_comb begin
    // "diff" table — 9 rows (an absent condition = any value); total by construction,
    // the default is unreachable (checked by the ALU bench)
    diff_sel = 2'd0;
    if      (!dc && dhi <= 4'h9 && !dh && dlo <= 4'h9) diff_sel = 2'd0;   // 00
    else if (!dc && dhi <= 4'h9 &&  dh && dlo <= 4'h9) diff_sel = 2'd1;   // 06
    else if (!dc && dhi <= 4'h8 &&        dlo >= 4'hA) diff_sel = 2'd1;   // 06
    else if (!dc && dhi >= 4'hA && !dh && dlo <= 4'h9) diff_sel = 2'd2;   // 60
    else if ( dc &&                !dh && dlo <= 4'h9) diff_sel = 2'd2;   // 60
    else if ( dc &&                 dh && dlo <= 4'h9) diff_sel = 2'd3;   // 66
    else if ( dc &&                       dlo >= 4'hA) diff_sel = 2'd3;   // 66
    else if (!dc && dhi >= 4'h9 &&        dlo >= 4'hA) diff_sel = 2'd3;   // 66
    else if (!dc && dhi >= 4'hA &&  dh && dlo <= 4'h9) diff_sel = 2'd3;   // 66

    // C' table — 5 rows
    daa_c = 1'b0;
    if      (!dc && dhi <= 4'h9 && dlo <= 4'h9) daa_c = 1'b0;
    else if (!dc && dhi <= 4'h8 && dlo >= 4'hA) daa_c = 1'b0;
    else if (!dc && dhi >= 4'h9 && dlo >= 4'hA) daa_c = 1'b1;
    else if (!dc && dhi >= 4'hA && dlo <= 4'h9) daa_c = 1'b1;
    else if ( dc)                               daa_c = 1'b1;

    // H' table — 5 rows (the column the manual does not have)
    daa_h = 1'b0;
    if      (!dn &&        dlo <= 4'h9) daa_h = 1'b0;
    else if (!dn &&        dlo >= 4'hA) daa_h = 1'b1;
    else if ( dn && !dh)                daa_h = 1'b0;
    else if ( dn &&  dh && dlo >= 4'h6) daa_h = 1'b0;
    else if ( dn &&  dh && dlo <= 4'h5) daa_h = 1'b1;
  end
endmodule
