#!/bin/bash
# usage: gold.sh ELF TAG [demos...]  -- PS2REF run with -ps2ref-hashall, compare 30 frames vs golden/ps2-head and all frames vs tools/ps2/golden_allhash
ELF=$1; TAG=$2; shift 2
for d in "$@"; do
  python3 tools/ps2/opt_run.py --name $TAG-$d --elf $ELF --pak /home/user/SRB2-Banpyura-PS2/build/pak --out build/runs --demo $d --timeout 1500 -- -ps2ref-hashall 2>&1 | tail -2
  python3 tools/ps2/golden_check.py --run build/runs/$TAG-$d/refout --ref golden/ps2-head/$d --pixels | tail -3
  if [ -f tools/ps2/golden_allhash/$d.csv ]; then
    if cmp -s tools/ps2/golden_allhash/$d.csv build/runs/$TAG-$d/refout/allhash.csv; then echo "ALLHASH $d identical ($(wc -l < build/runs/$TAG-$d/refout/allhash.csv) frames)"; else echo "ALLHASH $d DIFFER"; diff tools/ps2/golden_allhash/$d.csv build/runs/$TAG-$d/refout/allhash.csv | head -5; fi
  fi
  rm -f build/runs/$TAG-$d/pcsx2.log build/runs/$TAG-$d/SRB2.ELF build/runs/$TAG-$d/*.PAK build/runs/$TAG-$d/refout/frame-*.idx
done
