#!/bin/bash
# OPT13 IR: PC-sampler profile of a HW demo on a build.py --sample ELF.  usage: ir_sample.sh ELF TAG N [extra engine args]  -> build/runs/TAG_dN/report.txt
cd "$(dirname "$0")/../.."
ELF=$1; TAG=$2; N=$3; shift 3
PAK=${PAK:-$PWD/build/pak2}
python3 -B tools/ps2/opt_run.py --name ${TAG}_d$N --elf $ELF --pak $PAK --out build/runs --demo DEMO_00$N --no-ref --timeout 2400 --until "gametics in" -- -renderer Hardware -zreserve 1536 -ps2prof -ps2sample "$@" > build/runs/${TAG}_d$N.log 2>&1
python3 tools/ps2/sample_report.py --elf $ELF --log build/runs/${TAG}_d$N/boot.txt --top 90 --lines 60 > build/runs/${TAG}_d$N/report.txt 2>&1
rm -f build/runs/${TAG}_d$N/pcsx2.log build/runs/${TAG}_d$N/SRB2.ELF build/runs/${TAG}_d$N/*.PAK
head -3 build/runs/${TAG}_d$N/report.txt
