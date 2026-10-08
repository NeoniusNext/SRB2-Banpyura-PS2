#!/bin/bash
# OPT11-MODEL: the model scenes (PC OpenGL against PS2 HW) with the MAD of the whole picture and of the region of the models (tools/ps2/md_cmp.py).
# usage: md_scenes.sh ELF TAG [scene ...]    scenes: g1 (15 enemies / objects + Sonic, lighting Off), g1L (the same with gr_modellighting On), pl (five skins in five colours), fx (one model in 15 blend / render variants)
# the PC pictures are cached in build/ref/fx_mdl_<scene> (the PC engine of SRB2_PCWADDIR: private directory with models/ and models.dat, see the report), the PC pictures without models in fx_mdl_<scene>off
ELF=${1:-build/outm/SRB2.ELF}
TAG=${2:-new}
shift 2
export SRB2_PCWADDIR=${SRB2_PCWADDIR:-/opt/srb2-assets-model-wad}
cd "$(dirname "$0")/../.."
declare -A ADDON CMD REF OFF
ADDON[g1]="tools/ps2/mdlscene.lua"; CMD[g1]="gr_models~On;con_hudlines~0"; REF[g1]=mdl_g1; OFF[g1]=mdl_g1off
ADDON[g1L]="tools/ps2/mdlscene.lua"; CMD[g1L]="gr_models~On;gr_modellighting~On;con_hudlines~0"; REF[g1L]=mdl_g1L; OFF[g1L]=mdl_g1off
ADDON[pl]="tools/ps2/mdlset_players.lua,tools/ps2/mdlscene.lua"; CMD[pl]="gr_models~On;con_hudlines~0"; REF[pl]=mdl_pl; OFF[pl]=mdl_ploff
ADDON[fx]="tools/ps2/mdlset_fx.lua,tools/ps2/mdlscene.lua"; CMD[fx]="gr_models~On;con_hudlines~0"; REF[fx]=mdl_fx; OFF[fx]=mdl_fxoff
[ $# -eq 0 ] && set -- g1 g1L pl fx
for n in "$@"; do
	[ -z "${ADDON[$n]}" ] && { echo "unknown scene $n"; continue; }
	[ -f build/ref/fx_${OFF[$n]}/shot-0.png ] || python3 tools/ps2/fx_pair.py "${OFF[$n]}" --refonly --map 1 --tick 300 --cfg 'chasecam "On"' --cmd 'con_hudlines~0' --addon "${ADDON[$n]}" > /dev/null 2>&1
	echo -n "$n: "
	python3 tools/ps2/fx_pair.py "${TAG}_$n" --ref "${REF[$n]}" --elf "$ELF" --map 1 --tick 300 --cfg 'chasecam "On"' --cmd "${CMD[$n]}" --addon "${ADDON[$n]}" --pak build/pak-m 2>&1 | tail -1
	hw=$(ls -t build/runs/fx_${TAG}_$n/vidshot-*.ppm 2>/dev/null | head -1)
	[ -n "$hw" ] && python3 tools/ps2/md_cmp.py build/ref/fx_${REF[$n]}/shot-0.png build/ref/fx_${OFF[$n]}/shot-0.png "$hw" --out build/fx/mdlcmp_${TAG}_$n.png | head -2
done
