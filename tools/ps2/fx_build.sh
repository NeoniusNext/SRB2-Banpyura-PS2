#!/bin/bash
# OPT11-FX: build of the HW ELF in build/<out> from this worktree.  usage: fx_build.sh OUT [build.py options]   (e.g. fx_build.sh out-prof --prof)
set -e
cd "$(dirname "$0")/../.."
OUT=$1
shift
export PATH=/opt/ps2dev-x/ps2dev/ee/bin:$PATH
export SRB2_PS2_OUT=$PWD/build/$OUT
export SRB2_PS2_NO=
export SRB2_PS2_HW=1
python3 tools/ps2/build.py "$@" --jobs 2
ls -l "$SRB2_PS2_OUT/SRB2.ELF"
