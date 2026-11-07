#!/bin/sh
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

# Downloads the SingleStepTests Z80 vectors (MIT licence, about 1.3 GB) to the place where
# `make test` looks for them: reference/tests/SingleStepTests-z80 (outside version control).
# The revision is pinned to the one the results in STATUS.md were obtained with.
#
#   sh get_sst.sh            (from sim/)
#   sh get_sst.sh DIR        (elsewhere; then run the suite with SST_DIR=DIR/v1 make test)

URL=https://github.com/SingleStepTests/z80
REV=ebe1875d48f374bcfd4b505d8eb8ee751568b5f7
DEST=${1:-../reference/tests/SingleStepTests-z80}

if [ -d "$DEST/v1" ]; then echo "already present: $DEST/v1"; exit 0; fi
mkdir -p "$DEST" || exit 1
git -C "$DEST" init -q || exit 1
git -C "$DEST" fetch -q --depth 1 "$URL" "$REV" || exit 1
git -C "$DEST" checkout -q FETCH_HEAD || exit 1
[ -d "$DEST/v1" ] || { echo "no v1 directory in $DEST"; exit 1; }
echo "vectors in $DEST/v1 (revision $REV)"
