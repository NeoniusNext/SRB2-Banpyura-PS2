#!/bin/bash
# OPT13 IR: one demo, several variants of command-line arguments, one after the other on ONE ELF (like hwf_ab.sh, with PAK selectable: default build/pak2).
# usage: tools/ps2/ir_ab.sh ELF DEMO_N PREFIX "tagA:-args" "tagB:-args" ...   ->  build/runs/<PREFIX>_<tag>/boot.txt  (summaries: tools/ps2/hwf_sum.py, tools/ps2/gm_polycmp.py)
# env: PAK (default build/pak2), UNTIL (default "gametics in"), EXTRA (engine args added to every run, default "-renderer Hardware -zreserve 1536 -ps2prof")
cd "$(dirname "$0")/../.."
ELF=$1; D=$2; PFX=$3; shift 3
export PS2DEV=/opt/ps2dev-x/ps2dev PATH=/opt/ps2dev-x/ps2dev/ee/bin:$PATH
PAK=${PAK:-$PWD/build/pak2}
for spec in "$@"; do
	tag=${spec%%:*}
	args=${spec#*:}
	name=${PFX}_${tag}
	python3 -B tools/ps2/opt_run.py --name "$name" --elf "$ELF" --pak "$PAK" --out build/runs --demo "DEMO_00${D}" --no-ref --timeout 2400 --until "${UNTIL:-gametics in}" -- ${EXTRA:--renderer Hardware -zreserve 1536 -ps2prof} $args > "build/runs/${name}.log" 2>&1
	rm -rf "build/runs/${name}/SRB2.ELF" build/runs/${name}/*.PAK build/runs/${name}/FINEACON.DAT build/runs/${name}/refout # (the disk is shared and small)
	echo "$name: $(grep -h 'timed .* gametics' build/runs/${name}/boot.txt | tail -1)"
done
