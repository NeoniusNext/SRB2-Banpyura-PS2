#!/bin/bash
# usage: tools/ps2/hwdrv_abnew.sh TAG ELF "demos"   -> TAG_dN_new (defaults) and TAG_dN_old (-hwkeep 1 -hwfid 1)
cd "$(dirname "$0")/../.."
TAG=$1; ELF=$2; DEMOS=$3
for d in $DEMOS; do
  tools/ps2/hwdrv_runz.sh ${TAG}_d${d}_new $ELF DEMO_00$d 1536 > /dev/null
  tools/ps2/hwdrv_runz.sh ${TAG}_d${d}_old $ELF DEMO_00$d 1536 -hwkeep 1 -hwfid 1 > /dev/null
done
