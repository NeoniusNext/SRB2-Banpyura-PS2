#!/bin/bash
# OPT11-FX3: one run per value of -hwfx on the same ELF, without the profiler console: fx_flagsrun.sh TAG ELF "D1 D4" FLAG...  -> build/runs/<TAG>f<FLAG>_dN
# compare two of them with: fx_abtab.py <TAG>f<A> <TAG>f<B>
cd "$(dirname "$0")/../.."
TAG=$1
ELF=$2
DEMOS=$3
shift 3
export FX_NOCON=1
for f in "$@"; do
	tools/ps2/fx_demo.sh ${TAG}f$f "$ELF" "$DEMOS" -hwfx $f
done
echo "fx_flagsrun $TAG done"
