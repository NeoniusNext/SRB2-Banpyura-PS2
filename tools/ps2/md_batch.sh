#!/bin/bash
# OPT11-MODEL: the checks of the model path one after the other (one emulator at a time); usage: md_batch.sh PROF_ELF TAG [steps]   steps: perf nopack split anim oom maps any
ELF=${1:-build/outp/SRB2.ELF}
TAG=${2:-b}
shift 2
[ $# -eq 0 ] && set -- perf nopack split anim
export SRB2_PCWADDIR=${SRB2_PCWADDIR:-/opt/srb2-assets-model-wad}
cd "$(dirname "$0")/../.."
for step in "$@"; do
	echo "=== $step $(date +%H:%M:%S)"
	case $step in
	perf)
		FX_NOCON=1 FX_PAK=$PWD/build/pak-m FX_CFG='gr_models "On"' tools/ps2/fx_demo.sh ${TAG}perf "$ELF" "D1"
		python3 tools/ps2/md_wall.py build/runs/${TAG}perf_d1/boot.txt ;;
	nopack|damage)
		for v in $([ $step = nopack ] && echo none || echo 'bad trunc'); do
			python3 tools/ps2/hf_run.py ${TAG}_pak_$v --elf "$ELF" --pak build/pak-$v --cfg 'gr_models "On";chasecam "On"' --files tools/ps2/mdlscene.lua --timeout 600 \
				-- -skipintro -warp 1 -renderer Hardware -zreserve 1536 -zck -ps2prof -vidshot k200 -hwfbh 200 2>&1 | tail -2
			f=build/runs/${TAG}_pak_$v/boot.txt
			echo "pak-$v: I_Error/OOM lines: $(grep -c 'I_Error\|HEAP CHECK FAILED\|Out of memory' $f); models.dat/model messages:"
			grep -i "models.dat\|MODELS.PAK\|model" $f | grep -v "^HWPROF\|MDLSCENE" | head -6
			grep "^models:\|^HWPROF41 model" $f | tail -2
		done ;;
	split)
		python3 tools/ps2/fx_pair.py ${TAG}_split --map 1 --shot 'k20=hf_split~1,k300' --cfg 'chasecam "Off"' --cmd 'gr_models~On;con_hudlines~0' --addon tools/ps2/mdlscene.lua --pak build/pak-m --elf "$ELF" 2>&1 | tail -1 ;;
	anim)
		python3 tools/ps2/fx_pair.py ${TAG}_anim --map 1 --shot 'k300,k301,k302,k303,k304,k305' --all --cfg 'chasecam "On"' --cmd 'gr_models~On;con_hudlines~0' --addon tools/ps2/mdlset_anim.lua,tools/ps2/mdlscene.lua --pak build/pak-m --elf "$ELF" 2>&1 | tail -8 ;;
	scenes) tools/ps2/md_scenes.sh "$ELF" ${TAG}s ;;
	matrixx) tools/ps2/md_matrix.sh "$ELF" fakecontrast_on_wall smoothcontrast_wall palettedepth_24 solvetjoin_off shadow_off shadow_sprite shear_third 2>&1 ;;
	anim3) for t in 301 303 307; do python3 tools/ps2/fx_pair.py ${TAG}_anim$t --map 1 --tick $t --cfg 'chasecam "On"' --cmd 'gr_models~On;con_hudlines~0' --addon tools/ps2/mdlset_anim.lua,tools/ps2/mdlscene.lua --pak build/pak-m --elf "$ELF" 2>&1 | tail -1; done ;;
	oom) tools/ps2/md_oom.sh "$ELF" ${TAG}oom ;;
	maps) tools/ps2/md_maps.sh "$ELF" ${TAG}map ;;
	any) python3 tools/ps2/md_all.py "$ELF" --batch 20 --any ;;
	esac
done
echo "md_batch $TAG done $(date +%H:%M:%S)"
