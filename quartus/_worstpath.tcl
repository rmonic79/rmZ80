# This file is part of rmZ80, a Zilog Z80 core in SystemVerilog.
# Copyright (c) 2026 Umberto Parisi (rmonic79)
#
# rmZ80 is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
# License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any
# later version. It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
# more details: LICENSE in the root of the repository, or <https://www.gnu.org/licenses/>.
#
# A commercial license, for use without the obligations of the GPL (for example in closed-source products), is
# available from the author, who can also be contacted for other licensing needs.

# worst setup paths of the last build: quartus_sta -t _worstpath.tcl (from quartus/)
project_open rmZ80 -revision rmZ80
create_timing_netlist
read_sdc
update_timing_netlist
puts "===== TOP 8 WORST SETUP PATHS ====="
report_timing -setup -npaths 8 -detail summary -stdout
puts "===== WORST PATH FULL DETAIL ====="
report_timing -setup -npaths 1 -detail full_path -stdout
project_close
