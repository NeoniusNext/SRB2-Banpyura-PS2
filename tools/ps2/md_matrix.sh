#!/bin/bash
# OPT11-MODEL: the OpenGL options of Options -> Video -> OpenGL and the console only ones, PC OpenGL against PS2 HW on the same scene.
# usage: md_matrix.sh ELF [name ...]      (all scenes when no name is given); output: one line per scene (MAD of the whole picture) + build/fx/mx_<name>.png
# every scene: map 1, k300, chasecam Off (the first person view), the option set on BOTH sides by the console on the third frame; the PC picture is cached (build/ref/fx_mx_<name>)
ELF=${1:-build/outq/SRB2.ELF}
shift
export SRB2_PCWADDIR=${SRB2_PCWADDIR:-/opt/srb2-assets-model-wad}
cd "$(dirname "$0")/../.."
declare -A CMD PRE MAP
# name | console commands | camera (--pre) | map
add() { CMD[$1]="$2"; PRE[$1]="$3"; MAP[$1]="${4:-1}"; ORDER+=("$1"); }
add base            "con_hudlines~0" ""
add shaders_off     "gr_shaders~Off;con_hudlines~0" ""
add palette_off     "gr_paletterendering~Off;con_hudlines~0" ""
add shear_on        "gr_shearing~On;con_hudlines~0" "teleport~-nop~-aim~-35"
add shear_off_aim   "gr_shearing~Off;con_hudlines~0" "teleport~-nop~-aim~-35"
add billboard_on    "gr_spritebillboarding~On;con_hudlines~0" "teleport~-nop~-aim~-35"
add skydome_off     "gr_skydome~Off;con_hudlines~0" "teleport~-nop~-aim~25"
add fov_110         "fov~110;con_hudlines~0" ""
add filter_bilinear "gr_filtermode~Bilinear;con_hudlines~0" ""
add filter_trilinear "gr_filtermode~Trilinear;con_hudlines~0" ""
add lightdither_on  "gr_lightdithering~On;con_hudlines~0" ""
add wireframe_on    "devmode~1;gr_wireframe~On;con_hudlines~0" ""
add hitbox_all      "devmode~1;renderhitbox~All;con_hudlines~0" ""
add skydome_on_up   "con_hudlines~0" "teleport~-nop~-aim~25"
add fakecontrast_wall "gr_fakecontrast~Off;con_hudlines~0" "teleport~-nop~-ang~90"
add smoothcontrast_wall "con_hudlines~0" "teleport~-nop~-ang~90"
add fakecontrast_off "gr_fakecontrast~Off;con_hudlines~0" ""
add slopecontrast_on "gr_slopecontrast~On;con_hudlines~0" ""
add batching_off    "gr_batching~Off;con_hudlines~0" ""
for n in "${@:-${ORDER[@]}}"; do
	[ -z "${CMD[$n]}" ] && { echo "unknown scene $n"; continue; }
	cp "$SRB2_PCWADDIR/reference.default" "$SRB2_PCWADDIR/reference.cfg"  # the PC engine saves every CV_SAVE variable there at exit: the option of the previous scene must not stay (-ps2ref-cmd takes 160 chars only)
	pre=()
	[ -n "${PRE[$n]}" ] && pre=(--pre "${PRE[$n]}")
	python3 tools/ps2/fx_pair.py "mx_$n" --elf "$ELF" --map "${MAP[$n]}" --tick 300 --cfg 'chasecam "Off"' --cmd "${CMD[$n]}" "${pre[@]}" 2>&1 | tail -1 | sed "s/^/$n: /"
done
