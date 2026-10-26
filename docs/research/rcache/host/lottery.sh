#!/bin/bash
# OPT13-RCACHE: the memory-layout "lottery" sweeps (section 6 of OPT13_RCACHE.md). One emulator at a time; ~1.5 min per run on a quiet machine.
#   lottery.sh arena ELF       arena capacity sweep   (-zarena 21936-delta, delta 0..1024 KiB) on D4, D2, D3 hardware demos  -> build/runs/lot_d<N>_<delta>
#   lottery.sh pad ELF         arena base sweep       (-zpadding N: N bytes in the command line move the arena, same capacity) -> build/runs/pad_d<N>_<pad>
#   lottery.sh diag ELF        the same sweep with -zreport 525 on D3 and D2 (an ELF built with the [zslow] counters: build/exp-zc, see section 6.2) -> build/runs/zc_d<N>_<delta>
# summaries: host/sweepsum.py lot_d4_ lot_d2_ lot_d3_ pad_d4_ pad_d2_ pad_d3_ ;  host/zsweep.py zc_d zc_d2
set -e
cd "$(dirname "$0")/../../../.."
export PS2DEV=${PS2DEV:-/opt/ps2dev-x/ps2dev} PATH=$PS2DEV/ee/bin:$PATH
MODE=$1; ELF=$2
PAK=${SRB2_PAKDIR:-/home/user/SRB2-Banpyura-PS2/build/pak}
run() { n=$1; d=$2; shift 2
  python3 -B tools/ps2/opt_run.py --name $n --elf $ELF --pak $PAK --out build/runs --demo DEMO_00$d --no-ref --timeout 1800 --until "gametics in" -- -renderer Hardware -zreserve 1536 -ps2prof "$@" > build/runs/$n.log 2>&1
  rm -f build/runs/$n/SRB2.ELF; }
case $MODE in
  arena) for d in 4 2 3; do for delta in 0 16 32 64 128 256 512 1024; do run lot_d${d}_$delta $d -zarena $((21936 - delta)); done; done ;;
  pad)   for d in 4 2 3; do for pad in 0 16 48 80 144 272 528 1040 2064; do
           if [ $pad -eq 0 ]; then x=""; else x="-zpadding $(head -c $pad /dev/zero | tr '\0' 'x')"; fi
           run pad_d${d}_$pad $d $x; done; done ;;
  diag)  for delta in 0 16 32 64 128 256 512 1024; do run zc_d3_$delta 3 -zreport 525 -zarena $((21936 - delta)); done
         for delta in 0 512 1024; do run zc_d2_$delta 2 -zreport 525 -zarena $((21936 - delta)); done ;;
  *) echo "usage: lottery.sh arena|pad|diag ELF"; exit 1 ;;
esac
