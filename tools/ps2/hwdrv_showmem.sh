#!/bin/bash
# usage: tools/ps2/hwdrv_showmem.sh NAME ELF DEMO FRAMES : timedemo-less run to -zquit with showmem on; prints the SHOWMEM line
cd "$(dirname "$0")/../.."
NAME=$1; ELF=$2; DEMO=$3; FR=$4
python3 tools/ps2/opt_run.py --name $NAME --elf $ELF --pak build/pak --out build/runs --demo $DEMO --no-ref --playdemo --timeout 900 --until "ZQUIT DONE" --cfg 'showmem "On"' -- -ps2prof -renderer Hardware -zreserve 1536 -zquit $FR 2>&1 | tail -1
grep -a "SHOWMEM now" build/runs/$NAME/boot.txt | tail -1
