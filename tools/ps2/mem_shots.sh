#!/bin/bash
# OPT11-MEM: the pictures of showmem (docs/GATES/g1/opt11-MODEL.md, section "showmem"): the lower right corner of the same scenes in both renderers, the three
# modes, the memory pressure colours (-showmembias), models, the heavy map MAP11 (CEZ2: Hardware gives it up for Software), the ping display, the menu item.
# usage: mem_shots.sh PROF_ELF [scene ...]      pictures: docs/GATES/g1/opt11-MODEL-showmem/<scene>.png (x3); the run directories are deleted after the pictures are taken
cd "$(dirname "$0")/../.."
ELF=$1
shift
export SRB2_PCWADDIR=${SRB2_PCWADDIR:-/opt/srb2-assets-model-wad}
OUT=docs/GATES/g1/opt11-MODEL-showmem
mkdir -p $OUT
HW="-renderer Hardware -zreserve 1536 -hwfbh 200"
SW="-renderer Software -zreserve 1536"
COMMON="-skipintro"
# name|renderer args|config lines|extra engine args|map|extra files|crop box (default corner)
SCENES=(
"hw_gfz1_on|$HW|showmem \"On\";showfps \"Full\"||1||"
"hw_gfz1_ram|$HW|showmem \"RAM\";showfps \"Full\"||1||"
"hw_gfz1_vram|$HW|showmem \"VRAM\";showfps \"Full\"||1||"
"hw_gfz1_nofps|$HW|showmem \"On\"||1||"
"sw_gfz1_on|$SW|showmem \"On\";showfps \"Full\"||1||"
"sw_gfz1_vram|$SW|showmem \"VRAM\";showfps \"Full\"||1||"
"hw_gfz1_models|$HW|showmem \"On\";showfps \"Full\";gr_models \"On\";chasecam \"On\"||1|tools/ps2/mdlscene.lua|"
"hw_gfz1_yellow|$HW|showmem \"On\";showfps \"Full\"|-showmembias 3200|1||"
"hw_gfz1_red|$HW|showmem \"On\";showfps \"Full\"|-showmembias 5000|1||"
"hw_gfz1_ping|$HW|showmem \"On\";showfps \"Full\";showping \"On\"|VC=con_hudlines~0;mindelay~3|1||"
"sw_cez2|$SW|showmem \"On\";showfps \"Full\"||11||"
"hw_cez2|-renderer Hardware|showmem \"On\";showfps \"Full\";gr_models \"On\"||11||"
)
for sc in "${SCENES[@]}"; do
	IFS='|' read -r name rargs cfg extra map files box <<< "$sc"
	if [ $# -gt 0 ]; then
		case " $* " in *" $name "*) ;; *) continue ;; esac
	fi
	rm -rf build/runs/ms_$name
	fl=()
	[ -n "$files" ] && fl=(--files "$files")
	vc=con_hudlines~0
	case "$extra" in VC=*) vc=${extra#VC=}; extra="" ;; esac
	python3 tools/ps2/hf_run.py ms_$name --elf "$ELF" --pak build/pak-m --cfg "$cfg" "${fl[@]}" --timeout 900 -- $COMMON -vidcmd "$vc" $rargs $extra -warp $map -vidshot k300 2>&1 | tail -1
	f=$(ls build/runs/ms_$name/vidshot-*.ppm 2>/dev/null | head -1)
	if [ -n "$f" ]; then
		python3 tools/ps2/mem_crop.py "$f" $OUT/$name.png ${box}
		grep -h "ps2_hwfb: HARDWARE -> SOFTWARE" build/runs/ms_$name/boot.txt | head -1
	fi
	rm -rf build/runs/ms_$name
done
