#!/bin/bash
# usage: sw_sample.sh ELF TAG [demos...]  -- PC-sampler profile of demos on a --prof ELF: build/runs/TAG-DEMO/report.txt (functions, categories, source lines)
ELF=$1; TAG=$2; shift 2
for d in "$@"; do
  python3 tools/ps2/opt_run.py --name $TAG-$d --elf $ELF --pak /home/user/SRB2-Banpyura-PS2/build/pak --out build/runs --demo $d --no-ref --timeout 1500 --until "gametics in" -- -ps2prof -ps2sample 2>&1 | tail -2
  python3 tools/ps2/sample_report.py --elf $ELF --log build/runs/$TAG-$d/pcsx2.log --top 70 --lines 60 > build/runs/$TAG-$d/report.txt 2>&1
  head -3 build/runs/$TAG-$d/report.txt
  rm -f build/runs/$TAG-$d/pcsx2.log build/runs/$TAG-$d/SRB2.ELF build/runs/$TAG-$d/*.PAK
done
