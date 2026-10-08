#!/bin/bash
# usage: tools/ps2/geom/mk.sh OUTNAME [lto0] [extra build.py args...]   -> build/OUTNAME/SRB2.ELF, log build/logs/OUTNAME.txt
W=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$W" || exit 1
export PS2DEV=/opt/ps2dev-x/ps2dev
export PATH=$PS2DEV/ee/bin:$PS2DEV/bin:$PS2DEV/dvp/bin:$PATH
OUT=$1; shift
if [ "$1" = "lto0" ]; then export SRB2_PS2_LTO=0; shift; fi
export SRB2_PS2_OUT=$W/build/$OUT SRB2_PS2_NO= SRB2_PS2_HW=1
mkdir -p build/logs
python3 tools/ps2/build.py --jobs 2 "$@" > build/logs/$OUT.txt 2>&1
echo "rc=$?" >> build/logs/$OUT.txt
tail -3 build/logs/$OUT.txt
