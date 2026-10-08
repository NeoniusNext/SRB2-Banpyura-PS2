#!/bin/bash
# usage: tools/ps2/geom/final3.sh ELFDIR TAG  -> for the four demos: def (default), base (-hwgc 0 -hwgo 7229: the cache and the micro optimisations of OPT11 GEOM off), off (-hwgc 0), pl (-hwgc 1: with planes); summary in build/logs/final3_<TAG>.txt
W=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$W" || exit 1
ELF=$1; TAG=$2
OUT=build/logs/final3_$TAG.txt
: > $OUT
for n in 1 2 3 4; do
  tools/ps2/geom/ab.sh $ELF DEMO_00$n ${TAG}$n "def:-zreserve 1536" "base:-zreserve 1536 -hwgc 0 -hwgo 7229" "off:-zreserve 1536 -hwgc 0" "pl:-zreserve 1536 -hwgc 1" > build/logs/final3_${TAG}_$n.txt 2>&1
  for c in def base off pl; do
    echo "D$n $c: $(python3 tools/ps2/hwsum.py --skip 1 ${TAG}${n}_$c 2>/dev/null | grep -E '^mean|^max' | tr -s ' ' | cut -d' ' -f1-3 | tr '\n' ' ') | $(python3 tools/ps2/geom/hp2.py ${TAG}${n}_$c 2>/dev/null | head -1 | cut -c1-200)" >> $OUT
  done
  rm -f build/runs/${TAG}${n}_*/SRB2.ELF
done
echo done >> $OUT
