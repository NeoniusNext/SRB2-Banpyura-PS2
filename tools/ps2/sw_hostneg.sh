#!/bin/bash
# The negative controls of sw_hostall.sh only (each must show different frames / stop the game): usage sw_hostneg.sh [N ...] (default 11 13 14)
cd "$(dirname "$0")/../.."
export JOBS=${JOBS:-2}
for n in ${@:-11 13 14}; do
  case $n in
    9)  tools/ps2/host_variant.sh neg9 "-DPS2_NODECHECK -DPS2_NEGCTL=9" differ DEMO_003 ;;
    10) tools/ps2/host_variant.sh neg10 "-DPS2_NEGCTL=10" differ DEMO_003 ;;
    11) tools/ps2/host_variant.sh neg11 "-DPS2_NEGCTL=11" differ DEMO_001 DEMO_002 DEMO_003 DEMO_004 ;;
    13) tools/ps2/host_variant.sh neg13 "-DPS2_NEGCTL=13" differ DEMO_003 ;;
    14) tools/ps2/host_variant.sh neg14 "-DPS2_NEGCTL=14" differ DEMO_001 DEMO_002 DEMO_003 ;;
  esac
done
