#!/bin/bash
# OPT12 HWFRONT: one demo, several variants of command line arguments, one after the other on ONE ELF.
# usage: tools/ps2/hwf_ab.sh ELF DEMO_N PREFIX "tagA:-args" "tagB:-args" ...   ->  build/runs/<PREFIX>_<tag>/boot.txt (summaries: tools/ps2/fx_prof.py, tools/ps2/gm_polycmp.py)
cd "$(dirname "$0")/../.."
ELF=$1; D=$2; PFX=$3; shift 3
export PS2DEV=/opt/ps2dev-x/ps2dev PATH=/opt/ps2dev-x/ps2dev/ee/bin:$PATH
for spec in "$@"; do
	tag=${spec%%:*}
	args=${spec#*:}
	name=${PFX}_${tag}
	python3 -B tools/ps2/opt_run.py --name "$name" --elf "$ELF" --pak /home/user/SRB2-Banpyura-PS2/build/pak --out build/runs --demo "DEMO_00${D}" --no-ref --timeout 2400 --until "${UNTIL:-gametics in}" -- -renderer Hardware -zreserve 1536 -ps2prof $args > "build/runs/${name}.log" 2>&1
	rm -f "build/runs/${name}/SRB2.ELF"
	echo "$name: $(grep -h 'timed .* gametics' build/runs/${name}/boot.txt | tail -1)"
done
