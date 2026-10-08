#!/bin/bash
# usage: tools/ps2/geom/mapcamp.sh ELFNEW TAG MAP...  -> reference runs (outM) once, then ELFNEW default and ELFNEW with -hwgc 2 (every hit checked), compared with the reference stream
# results: build/logs/mapcmp_<TAG>d.txt (default), build/logs/mapcmp_<TAG>c.txt (check); build/logs/mapcamp_<TAG>.done at the end
W=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$W" || exit 1
NEW=$1; TAG=$2; shift 2
rm -f build/logs/mapcamp_$TAG.done
if [ ! -f build/logs/mapcmp_R3.txt ] || ! grep -q '^done' build/logs/mapcmp_R3.txt; then
  tools/ps2/geom/mapcmp3.sh ref $NEW R3 "-hwgc 0 -hwgo 7229" "$@"
fi
tools/ps2/geom/mapcmp3.sh new $NEW ${TAG}d "" "$@"
tools/ps2/geom/mapcmp3.sh new $NEW ${TAG}c "-hwgc 2" "$@"
echo done > build/logs/mapcamp_$TAG.done
