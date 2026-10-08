#!/bin/bash
# usage: tools/ps2/geom/ab.sh ELFDIR DEMO_00n PREFIX "tagA:-hwgc 0" "tagB:-hwgc 1" ...   one run after the other; summary by tools/ps2/geom/hp2.py
W=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$W" || exit 1
export PS2DEV=/opt/ps2dev-x/ps2dev
export PATH=$PS2DEV/ee/bin:$PS2DEV/bin:$PS2DEV/dvp/bin:$PATH
ELF=$1; DEMO=$2; PFX=$3; shift 3
NAMES=""
for spec in "$@"; do
  tag=${spec%%:*}
  args=${spec#*:}
  name=${PFX}_${tag}
  python3 tools/ps2/hwrun.py --elf $W/build/$ELF/SRB2.ELF "$name=$DEMO" -- -hwdbg 0 $args > build/logs/r_$name.txt 2>&1
  rm -f build/runs/$name/SRB2.ELF
  NAMES="$NAMES $name"
done
python3 tools/ps2/geom/hp2.py $NAMES
python3 tools/ps2/hwsum.py --skip 1 $NAMES 2>/dev/null | grep -E "^==|^mean|^max|^FPS"
