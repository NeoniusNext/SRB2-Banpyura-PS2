#!/bin/bash
# usage: tools/ps2/hwdrv_runz.sh NAME ELF DEMO ZRESERVE [engine args...]
cd "$(dirname "$0")/../.."
NAME=$1; ELF=$2; DEMO=$3; ZR=$4; shift 4
python3 tools/ps2/opt_run.py --name $NAME --elf $ELF --pak build/pak --out build/runs --demo $DEMO --no-ref --until "gametics in" --timeout 900 -- -ps2prof -renderer Hardware -zreserve $ZR "$@" 2>&1 | tail -1
