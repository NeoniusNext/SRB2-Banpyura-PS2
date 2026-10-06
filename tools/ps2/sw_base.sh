#!/bin/bash
# usage: runbase.sh ELF TAG [demos...]  -- baseline timing of demos with -ps2prof
ELF=$1; TAG=$2; shift 2
for d in "$@"; do
  python3 tools/ps2/opt_run.py --name $TAG-$d --elf $ELF --pak /home/user/SRB2-Banpyura-PS2/build/pak --out build/runs --demo $d --no-ref --timeout 1200 --until "gametics in" -- -ps2prof 2>&1 | tail -3
  python3 tools/ps2/prof_summary.py build/runs/$TAG-$d/boot.txt
  grep "gametics in" build/runs/$TAG-$d/boot.txt
done
