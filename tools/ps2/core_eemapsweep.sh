#!/bin/bash
# OPT11-CORE: the map sweep (tools/ps2/core_mapsweep.sh) on the EE under PCSX2, for the maps the host profile cannot load (UDMF): two PS2REF ELFs (old code: PS2_NOOPT_CORE, new code),
# each map is entered with -warp, nobody presses a key, the per-tic state hash of the first TICS tics (-ps2ref-maptics, fixed RNG seed) goes to <run>/refout/tics.csv; the two files must be equal.
# usage: core_eemapsweep.sh ELF_OLD ELF_NEW OUTTAG [TICS=150] [maps ...]   (default: every UDMF map of the inventory)
ELF_A=$1; ELF_B=$2; TAG=$3; TICS=${4:-150}; shift 4 || shift $#
cd "$(dirname "$0")/../.."
PAK=${SRB2_PAKDIR:-/home/user/SRB2-Banpyura-PS2/build/pak}
MAPS=${@:-$(python3 - <<'EOF'
import json
d=json.load(open("build/ps2-sw-continuation-map-inventory.json"))
print(" ".join(m["map"] for m in d if m["kind"] in ("Match","CTF","SP","MP-special") and m.get("udmf")))
EOF
)}
same=0; bad=0
for M in $MAPS; do
  for side in a b; do
    [ $side = a ] && ELF=$ELF_A || ELF=$ELF_B
    python3 tools/ps2/opt_run.py --name $TAG-$side-$M --elf "$ELF" --pak "$PAK" --out build/runs --map MAP$M --timeout 420 --until "end of logstream" -- -ps2ref host:/refout -ps2ref-maptics $TICS > /dev/null 2>&1
    rm -f build/runs/$TAG-$side-$M/pcsx2.log build/runs/$TAG-$side-$M/SRB2.ELF build/runs/$TAG-$side-$M/*.PAK build/runs/$TAG-$side-$M/refout/frame-*.idx
  done
  A=build/runs/$TAG-a-$M/refout; B=build/runs/$TAG-b-$M/refout
  if [ -f $A/complete.txt ] && [ -f $B/complete.txt ]; then
    if cmp -s $A/tics.csv $B/tics.csv; then same=$((same+1)); echo "MAP$M identical: $(($(wc -l < $A/tics.csv) - 1)) tics"; else bad=$((bad+1)); echo "MAP$M DIFFERENT"; fi
  else
    bad=$((bad+1)); echo "MAP$M incomplete ($( [ -f $A/complete.txt ] && echo A-ok || echo A-missing ) $( [ -f $B/complete.txt ] && echo B-ok || echo B-missing ))"
  fi
done
echo "maps identical $same, different or incomplete $bad"
