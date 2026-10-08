#!/bin/bash
# OPT11-FX3: pixel comparison of pairs of shot runs (tools/ps2/fx_ppmcmp.py without the top 40 rows): fx_cmp.sh RUN_A RUN_B [RUN_A RUN_B ...]  (names without _dN: both D1 and D4 pairs if the run exists)
cd "$(dirname "$0")/../.."
while [ $# -ge 2 ]; do
	for d in 1 2 3 4; do
		if [ -d "build/runs/$1_d$d" ] && [ -d "build/runs/$2_d$d" ]; then
			echo "D$d $1 vs $2: $(python3 tools/ps2/fx_ppmcmp.py $1_d$d $2_d$d 40 | tail -1)"
		fi
	done
	shift 2
done
