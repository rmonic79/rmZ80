# 07 — Integrating rmZ80

For anyone replacing another Z80 core with rmZ80 in an FPGA design: what the
surrounding logic must respect.

## 1. Files

Nine files in `rtl/`, no other dependency, no block memory:

```
rmZ80.sv            top module, the one to instantiate
rmZ80_cw.sv         control word and schedule
rmZ80_seq.sv        sequencer: timing, bus, interrupts, bus request
rmZ80_regfile.sv    registers
rmZ80_alu_core.sv  rmZ80_alu_dec.sv  rmZ80_alu_daa.sv   ALU
rmZ80_alu.sv        ALU wrapper used only by the ALU bench (not instantiated by rmZ80; harmless to include)
rmZ80_alu_pkg.sv    package: compile it first
```

## 2. Ports

| Port | Dir | Notes |
|---|---|---|
| `clk` | in | the only clock |
| `cen` | in | clock enable: **one pulse per half T-state**, at least 2 clocks apart (§3) |
| `rst` | in | **active high, asynchronous**; release it synchronously to `clk` |
| `addr[15:0]` | out | |
| `data_in[7:0]` | in | |
| `data_out[7:0]`, `data_oe` | out | `data_oe` = 1 while the CPU drives the data bus (`WR` active) |
| `mreq_n`, `iorq_n`, `rd_n`, `wr_n`, `rfsh_n`, `m1_n`, `halt_n` | out | as on the Z80 (`02-bus-cycles.md`) |
| `wait_n`, `int_n`, `nmi_n` | in | `nmi_n` is edge-triggered (falling edge stored) |
| `busreq_n` | in | bus request: **tie to 1 if unused** |
| `busack_n` | out | bus acknowledge (`02-bus-cycles.md` §6) |
| `ss_load`, `ss_din[273:0]` | in | savestate load (§4) |
| `ss_dout[273:0]` | out | savestate: the state itself (§4) |

Parameter `VARIANT_OUT_C0_FF`: the byte `OUT (C),0` writes — 0 = 00h (NMOS,
default), 1 = FFh (CMOS). The NMOS/CMOS behaviour of `SCF`/`CCF` is the parameter
`VARIANT_SCF_CCF_NMOS` of `rmZ80_alu_core` (default NMOS; `05-alu-and-flags.md` §9;
not a parameter of `rmZ80`: override it on the instance `u_alu`).

## 3. Contract

1. **`cen` = two pulses per T-state, at least 2 clocks apart.** Each pulse advances
   the CPU by half a T-state (`02-bus-cycles.md` §8): for a Z80 clocked at
   *f*, `cen` runs at 2*f*. This is **twice the rate** that cores advancing one
   T-state per enable expect. Inside the CPU the work of each half T-state is split
   over two clocks (`06-sequencer.md` §9), so with `cen` on every clock the core is
   outside its contract. At 96 MHz, a 4 MHz Z80 has `cen` once every 12 clocks.
2. **Reset**: asynchronous, active high. After the release the first `cen` may
   come on the very next clock. The state after reset is defined
   (`04-interrupts.md` §7): `AF` and `SP` = FFFFh, `PC` = 0, interrupts disabled,
   mode 0, everything else 0; the same values at power-up.
3. **Savestate**: after the `ss_load` clock, **at least 2 clocks with `cen` = 0**
   before the next `cen`.
4. **No input inside the core's internal paths.** `data_in`, `wait_n`, `int_n`,
   `nmi_n` and `busreq_n` only enter the sequencer, where they are registered;
   every output leaves a flip-flop.

## 4. Savestate

- `ss_dout[273:0]` **is** the state: wires on the registers, valid and consistent
  at any time, changing only on clocks with `cen`. Read it whenever you like.
- To load: `ss_load` = 1 for **one** clock with `cen` = 0 (`ss_load` overrides
  `cen`; `rst` overrides everything), then the 2 quiet clocks of the contract.
- Layout: `[273:210]` sequencer state (`06-sequencer.md` §5), `[209:0]` registers
  (`01-registers-and-state.md` §5). In the banked fields index 0 is the active
  bank; `sel_af`/`sel_gp` say which logical bank is active.
- Nothing else is needed: no adaptation logic, no shifting, no long pause. The
  system stops the CPU (`cen` = 0) around the load and that is all.
- The format is this core's own: a savestate taken with another Z80 core is not
  compatible.

## 5. What looks different from other Z80 cores

- **`cen` at twice the rate** (§3, point 1).
- **Registered outputs.** All bus outputs, `addr` included, leave flip-flops, so
  they appear **one clock later** than from a core with combinational outputs —
  all together. In T-states nothing changes (the outside sees the same waveform
  delayed by one clock, and `cen` pulses are at least 2 clocks apart), but a
  decoder that samples on the clock rather than on `cen` should be checked. It is
  also why a system path such as *address → decode → `cen`* gets shorter instead
  of longer.
- **`busreq_n` is functional.** A core that ignored it may have it left floating
  or tied to 0 in an existing design: tie it to 1.
- **`nmi_n` is edge-triggered**, as on the chip.

## 6. What is verified

`STATUS.md` has the results of this tree; `README.md`, "Verification", how each
was obtained.
