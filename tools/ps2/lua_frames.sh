#!/bin/bash
# OPT14: a demo played with a content mod (Lua + sprites + SOC + HUD graphic) on the PC reference engine and on the PS2 ELF (--ps2ref build), frame dumps compared (golden_check.py):
# the physics (tics.csv: the Lua mod moves objects, changes sector lights) and the 30 sampled software frames must agree to the few pixels that the base game itself differs by.
# usage: tools/ps2/lua_frames.sh PS2REF_ELF NAME DEMO_001 file.pk3|file.lua [more engine args...]      results in build/lua-frames/NAME/{pc,ps2}
set -u
cd "$(dirname "$0")/../.."
ELF=${1:?ELF}; NAME=${2:?name}; DEMO=${3:?demo}; MOD=${4:?mod file}; shift 4
OUT=build/lua-frames/$NAME
ASSETS=${SRB2_ASSETS:-/opt/srb2-assets}
EXE=$(ls build/pc-golden/bin/*/* | head -1)
PAK=${SRB2_PAK:-build/pak2}
rm -rf $OUT; mkdir -p $OUT/pc/home/.srb2 $OUT/ps2
MODNAME=$(basename $MOD)
# PC
cp golden/phase0-v2/$DEMO.lmp $OUT/pc/home/.srb2/$DEMO.lmp
cp $MOD $OUT/pc/$MODNAME
printf 'fpscap "35"\nfullscreen "Off"\nshowfps "Off"\nshowping "Off"\n' > $OUT/pc/home/.srb2/reference.cfg
(cd $OUT/pc && SRB2WADDIR=$ASSETS SDL_AUDIODRIVER=dummy xvfb-run -a -s "-screen 0 800x600x24" timeout 600 $OLDPWD/$EXE -ps2ref $PWD -home $PWD/home \
   -config reference.cfg -nolog -noendtxt -win -width 320 -height 200 -renderer Software -file $PWD/$MODNAME "$@" -timedemo $DEMO.lmp > stdout.log 2>&1)
test -f $OUT/pc/complete.txt && echo "PC: $(ls $OUT/pc/frame-*.idx | wc -l) frames, $(wc -l < $OUT/pc/tics.csv) tics" || echo "PC run did not complete"
grep -a "WARNING\|ERROR\|LQ " $OUT/pc/stdout.log | cut -c1-200 | head -10
# PS2
mkdir -p build/runs/lf_$NAME
cp $MOD build/runs/lf_$NAME/$MODNAME
python3 tools/ps2/opt_run.py --name lf_$NAME --elf "$ELF" --pak "$PAK" --out build/runs --demo $DEMO --timeout 1200 -- -file $MODNAME "$@" > $OUT/ps2.log 2>&1
tail -3 $OUT/ps2.log
grep -a "WARNING\|ERROR\|LQ " build/runs/lf_$NAME/boot.txt | cut -c1-200 | head -10
echo "--- $NAME: PS2 against PC (tics must be identical, frames to a few pixels)"
python3 tools/ps2/golden_check.py --run build/runs/lf_$NAME/refout --ref $OUT/pc
