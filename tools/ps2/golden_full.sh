#!/bin/bash
# The four attract demos of the full configuration (SRB2_PS2_NO= --ps2ref ELF) against golden/ps2-head, bit for bit (OPT10-X: vanilla mode must not change).
# usage: tools/ps2/golden_full.sh ELF [OUTNAME-PREFIX]   (run in the worktree; results in build/runs/<prefix>N/ and build/logs/<prefix>.txt)
set -u
cd "$(dirname "$0")/../.."
ELF=${1:?ELF}
P=${2:-gf}
PAK=${SRB2_PAK:-build/pakx}
: > build/logs/$P.txt
for n in 1 2 3 4; do
  python3 tools/ps2/opt_run.py --name $P$n --elf "$ELF" --pak "$PAK" --out build/runs --demo DEMO_00$n --timeout 900 >> build/logs/$P.txt 2>&1
  echo "--- DEMO_00$n vs golden/ps2-head" >> build/logs/$P.txt
  python3 tools/ps2/golden_check.py --run build/runs/$P$n/refout --ref golden/ps2-head/DEMO_00$n --pixels >> build/logs/$P.txt 2>&1
  echo "--- DEMO_00$n vs PC golden (tics only)" >> build/logs/$P.txt
  python3 tools/ps2/golden_check.py --run build/runs/$P$n/refout --ref golden/phase0-v2/run1/DEMO_00$n >> build/logs/$P.txt 2>&1
done
echo DONE >> build/logs/$P.txt
