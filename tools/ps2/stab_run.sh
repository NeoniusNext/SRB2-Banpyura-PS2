#!/bin/bash
# OPT11-STAB: the whole stability pass of one final ELF with ONE command, on any ELF (the merged build included):
#
#   bash tools/ps2/stab_run.sh ELF [TAG] [STAGE ...]
#
# Stages (default: all, in this order; one emulator at a time except the network stage, every stage prints "== name" and "STAGE-DONE name"):
#   ui       menus / intro / title / renderer toggles / finales / NiGHTS / special stage, both renderers          (stab_ui.py)
#   video    NTSC / PAL / 480p x internal modes x both renderers                                                   (stab_video.py)
#   files    damaged / garbage config, game data, saves                                                           (stab_files.py)
#   packs    missing / truncated / corrupted data packs                                                           (stab_packs.py)
#   hwfb     HW start without memory (-hwnomem), fallback and return at every map of a chain (-hwfbtest)
#   flats    software chain of all maps with -flatcheck: the flat that is made straight from the patches (PS2-180) equals the composite route for every texture used as a flat
#   lint     the VIF1 chain lint (-hwdbg 536870912): no FLUSH inside a GIF packet (the MAPMD/MAPME hang), maps with odd texture sizes   (PS2-HW-144)
#   leak     50 map changes in software and in hardware (used bytes, C heap, stack must not grow)
#   inject   out-of-memory injection sweeps: level load / frame, software and hardware                            (oom_inject.py)
#   addons   Lua, limits, skins/sounds/music, UDMF map, Lua HUD (both renderers, + injection)                     (addon_compare.py, ftest_run.py)
#   interp   interpreter + EE data cache against the recompiler (needs build/pcsx2-int, opt10-S.md)
#   split    split screen, two scripted pads, both renderers
#   net      PS2 <-> PC, PS2 <-> PS2, local mock master only (needs build/pc-net, build/pcsx2-net1/2; see net_env.py)
#   demos    the 4 demos to the end: software (PC tics + PS2 frames), hardware (PC tics), hardware with injection (demosw demohw demohi; need the PS2REF ELF, built here)
#   sweep    84 maps, one session chain per renderer (chain_sweep.py) and every map on a cold boot (map_sweep.py) (chainsw chainhw coldsw coldhw)
#   soak     20 game minutes in one session per renderer (stab_soak.py) (soaksw soakhw)
# The log is build/logs/stab-TAG.txt (TAG defaults to the ELF's file name); the runs are build/runs/<TAG>-*.
# Environment: SRB2_STAB_PAK = the cooked packs (default: the shared build/pak of the main tree, read only), SRB2_STAB_FRAMES = frames per map (35),
# SRB2_STAB_MINUTES = soak game minutes (20), SRB2_STAB_ZDBG = a ZDEBUG ELF for the heap checks (build/out-zdbg2/SRB2.ELF).
cd "$(dirname "$0")/../.." || exit 2
ROOT=$(pwd)
ELF=${1:?usage: stab_run.sh ELF [TAG] [STAGE ...]}
case $ELF in /*) ;; *) ELF=$ROOT/$ELF ;; esac
shift
TAG=${1:-$(basename "$ELF" .ELF)}
[ $# -gt 0 ] && shift
STAGES=${*:-ui video files packs hwfb lint flats leak inject addons interp split net demos sweep soak}
PAK=${SRB2_STAB_PAK:-/home/user/SRB2-Banpyura-PS2/build/pak}
FRAMES=${SRB2_STAB_FRAMES:-35}
MINUTES=${SRB2_STAB_MINUTES:-20}
ZDBG=${SRB2_STAB_ZDBG:-$ROOT/build/out-zdbg2/SRB2.ELF}
ADD=$ROOT/build/opt10-x/addons
mkdir -p build/logs build/runs
L=$ROOT/build/logs/stab-$TAG.txt
OPT="python3 -B tools/ps2/opt_run.py"

say() { echo "$@" >> "$L"; }
run() { "$@" >> "$L" 2>&1; }
grep_run() { # grep_run RUN PATTERN [cut]
	grep -h "$2" "build/runs/$1/boot.txt" 2>/dev/null | cut -c1-${3:-230} >> "$L"
}

stage_ui() {
	run python3 tools/ps2/stab_ui.py --elf "$ELF" --tag "$TAG-ui" --scenario menus,intro,title-long
	run python3 tools/ps2/stab_ui.py --elf "$ELF" --tag "$TAG-ui" --scenario toggle-level,toggle-title,toggle-menu
	run python3 tools/ps2/stab_ui.py --elf "$ELF" --tag "$TAG-ui" --scenario ending,credits,evaluation,continue,gameend,intro-cmd
	run python3 tools/ps2/stab_ui.py --elf "$ELF" --tag "$TAG-ui" --scenario special,nights
}

stage_video() {
	run python3 tools/ps2/stab_video.py --elf "$ELF" --tag "$TAG-vid" --outputs ntsc,pal,480p --modes 0,1,3,7,10
}

stage_files() {
	run python3 tools/ps2/stab_files.py --elf "$ELF" --tag "$TAG-f"
	run python3 tools/ps2/stab_files.py --elf "$ELF" --tag "$TAG-fh" --only cfg-garbage,cfg-badvalues,gamedata-garbage,gamedata-trunc,gamedata-flip,save-garbage --extra "-renderer Hardware"
}

stage_packs() {
	[ -d build/pakx ] || python3 tools/ps2/net_env.py > /dev/null 2>&1
	run python3 tools/ps2/stab_packs.py --elf "$ELF" --tag "$TAG-p"
	run python3 tools/ps2/stab_packs.py --elf "$ELF" --tag "$TAG-ph" --only nopaks,nosrb2,nozones,magic-srb2,body-srb2,body-zones,trunc-chars,body-music --extra "-renderer Hardware"
}

stage_hwfb() {
	say "-- hwnomem: the GS driver finds no memory for its work arrays (start fails, software, every other start fails again)"
	$OPT --name "$TAG-hwnomem" --elf "$ELF" --pak "$PAK" --out build/runs --map MAP01 --timeout 900 -- -zck -zquit 150 -renderer Hardware -hwnomem -zchain 02,03,04 >> "$L" 2>&1
	grep_run "$TAG-hwnomem" "HWE\|hardware renderer\|ps2_hwfb\|I_Error" 200
	say "-- fallback and return at every map of a chain (each map falls back once, the next one returns)"
	local CH=02,03,04,05,06,07,08,09,10,12,13,14,15,16,22,23,25,26,27,30,31,32,33,40,41,42,50,51,60,70,71
	$OPT --name "$TAG-fbchain" --elf "$ELF" --pak "$PAK" --out build/runs --map MAP01 --timeout 4000 --until "ZQUIT DONE" -- -zck -zstack -zquit 14 -renderer Hardware -hwfbtest 5,12 -hwfbmax 999 -zchain $CH >> "$L" 2>&1
	grep_run "$TAG-fbchain" "ZCHAIN n=\|ps2_hwfb:\|I_Error\|OOM:" 230
	if [ -f "$ZDBG" ]; then
		say "-- ZDEBUG ELF: heap check after every allocation (-zheap 1) across a fallback and a return"
		$OPT --name "$TAG-zdbg" --elf "$ZDBG" --pak "$PAK" --out build/runs --map MAP01 --timeout 3000 -- -zck -zheap 1 -zquit 150 -renderer Hardware -zoomtest 40,160 -zchain 02,03,04 >> "$L" 2>&1
		grep_run "$TAG-zdbg" "ps2_hwfb\|OOM\|I_Error\|ZCHAIN" 200
	fi
}

stage_flats() {
	say "-- flat route self-test: -flatcheck over every map (software, one session chain); a DIFFER line is an error"
	python3 tools/ps2/chain_sweep.py --elf "$ELF" --tag "$TAG-flats" --pak "$PAK" --kinds SP,MP-special,Match,CTF --frames 12 -- -flatcheck >> "$L" 2>&1
	grep -h "FLATCHECK\|FLATSTREAM" build/runs/sweep/$TAG-flats/*/boot.txt 2>/dev/null | cut -c1-200 | sort | uniq -c | sort -rn | head -60 >> "$L"
}

stage_lint() {
	local M
	say "-- chain lint of the VIF1 transport (HWVAL LINT errors must be 0, no WATCHDOG)"
	for M in MAPMD MAPME MAP01 MAP23 MAP11; do
		$OPT --name "$TAG-lint-$M" --elf "$ELF" --pak "$PAK" --out build/runs --map $M --timeout 900 --until "ZQUIT DONE" -- -zck -zquit 100 -renderer Hardware -hwdbg 536870912 >> "$L" 2>&1
		say "$M:"
		grep_run "$TAG-lint-$M" "HWVAL LINT\|WATCHDOG: GIF\|I_Error" 200
	done
}

stage_leak() {
	local CH
	CH=$(python3 -c "print(','.join(['01']*49))")
	say "-- 50 map changes, software"
	$OPT --name "$TAG-leak-sw" --elf "$ELF" --pak "$PAK" --out build/runs --map MAP01 --timeout 3000 --until "ZQUIT DONE" -- -zck -zstack -zquit 10 -zchain "$CH" >> "$L" 2>&1
	grep_run "$TAG-leak-sw" "ZCHAIN n=\(1\|2\|3\|10\|25\|49\|50\) " 230
	say "-- 50 map changes, hardware"
	$OPT --name "$TAG-leak-hw" --elf "$ELF" --pak "$PAK" --out build/runs --map MAP01 --timeout 4000 --until "ZQUIT DONE" -- -zck -zstack -zquit 10 -renderer Hardware -zchain "$CH" >> "$L" 2>&1
	grep_run "$TAG-leak-hw" "ZCHAIN n=\(1\|2\|3\|10\|25\|49\|50\) " 230
}

stage_inject() {
	run python3 tools/ps2/oom_inject.py --elf "$ELF" --tag "$TAG-i" --mode level --points 12,22,29,35,41,67,101,160,255,410,650,1000,1300,1800,2300,3000,3700,4600,5300,6100,7000,8500
	run python3 tools/ps2/oom_inject.py --elf "$ELF" --tag "$TAG-i" --mode frame --points 1,4,9,30,80,200,500,1200,3000
	run python3 tools/ps2/oom_inject.py --elf "$ELF" --tag "$TAG-i" --mode hwlevel --points 40,300,1200,2500,4000,6000,9000
	run python3 tools/ps2/oom_inject.py --elf "$ELF" --tag "$TAG-i" --mode hwframe --points 1,5,20,60,150,400,1000,2500
}

stage_addons() {
	say "-- add-ons: base"
	run python3 tools/ps2/addon_compare.py --name "$TAG-sum-base" --files "" --elf "$ELF"
	say "-- add-ons: Lua (ZL)"
	run python3 tools/ps2/addon_compare.py --name "$TAG-zl" --files "$ADD/ZL.pk3" --until "FTLUA think36" --elf "$ELF"
	say "-- add-ons: limits (ZF)"
	run python3 tools/ps2/addon_compare.py --name "$TAG-zf" --files "$ADD/ZF.pk3" --until "FTLUA done" --elf "$ELF"
	say "-- add-ons: skin/sound/music/Lua (ZS)"
	run python3 tools/ps2/addon_compare.py --name "$TAG-zs" --files "$ADD/ZS.pk3" --until "FTLUA done" --elf "$ELF"
	say "-- UDMF map (UM)"
	run python3 tools/ps2/addon_compare.py --name "$TAG-um" --files "$ADD/UM.pk3" --warp 99 --elf "$ELF"
	say "-- Lua HUD software"
	run python3 tools/ps2/ftest_run.py --name "$TAG-hud-sw" --elf "$ELF" --pak build/pakx --out build/runs --files "$ADD/ZH.pk3" --until "ZQUIT DONE" --timeout 600 -- -file ZH.pk3 -skipintro -warp 1 -vidshot l90 -zquit 120
	grep_run "$TAG-hud-sw" "FTLUA hud\|I_Error\|OOM" 200
	say "-- Lua HUD hardware"
	run python3 tools/ps2/ftest_run.py --name "$TAG-hud-hw" --elf "$ELF" --pak build/pakx --out build/runs --files "$ADD/ZH.pk3" --until "ZQUIT DONE" --timeout 900 -- -file ZH.pk3 -skipintro -warp 1 -renderer Hardware -vidshot l90 -zquit 120
	grep_run "$TAG-hud-hw" "FTLUA hud\|I_Error\|OOM" 200
	say "-- Lua HUD hardware + out-of-memory injection (the guard must not jump inside a Lua hook)"
	run python3 tools/ps2/ftest_run.py --name "$TAG-hud-hwi" --elf "$ELF" --pak build/pakx --out build/runs --files "$ADD/ZH.pk3" --until "ZQUIT DONE" --timeout 900 -- -file ZH.pk3 -skipintro -warp 1 -renderer Hardware -zoomevery 8 -zoomany -zquit 200
	grep_run "$TAG-hud-hwi" "FTLUA hud\|I_Error\|OOM\|ps2_hwfb: fall" 200
}

stage_interp() {
	[ -x build/pcsx2-int/AppRun ] || { say "interp: build/pcsx2-int is missing, skipped"; return; }
	say "-- interpreter + EE data cache against the recompiler: software MAP01 (same frames)"
	$OPT --name "$TAG-int-sw" --elf "$ELF" --pak "$PAK" --out build/runs --emu build/pcsx2-int/AppRun --map MAP01 --timeout 2400 -- -zsingle -zck -zquitall 300 -vidshot l150,l290 >> "$L" 2>&1
	$OPT --name "$TAG-rec-sw" --elf "$ELF" --pak "$PAK" --out build/runs --map MAP01 --timeout 1200 -- -zsingle -zck -zquitall 300 -vidshot l150,l290 >> "$L" 2>&1
	run python3 tools/ps2/ppm_diff.py "build/runs/$TAG-int-sw/vidshot-320x200-l150.ppm" "build/runs/$TAG-rec-sw/vidshot-320x200-l150.ppm"
	run python3 tools/ps2/ppm_diff.py "build/runs/$TAG-int-sw/vidshot-320x200-l290.ppm" "build/runs/$TAG-rec-sw/vidshot-320x200-l290.ppm"
	say "-- the same in hardware"
	$OPT --name "$TAG-int-hw" --elf "$ELF" --pak "$PAK" --out build/runs --emu build/pcsx2-int/AppRun --map MAP01 --timeout 3600 -- -renderer Hardware -zsingle -zck -zquitall 200 -vidshot l100,l190 >> "$L" 2>&1
	$OPT --name "$TAG-rec-hw" --elf "$ELF" --pak "$PAK" --out build/runs --map MAP01 --timeout 1800 -- -renderer Hardware -zsingle -zck -zquitall 200 -vidshot l100,l190 >> "$L" 2>&1
	run python3 tools/ps2/ppm_diff.py "build/runs/$TAG-int-hw/vidshot-320x200-l100.ppm" "build/runs/$TAG-rec-hw/vidshot-320x200-l100.ppm"
	run python3 tools/ps2/ppm_diff.py "build/runs/$TAG-int-hw/vidshot-320x200-l190.ppm" "build/runs/$TAG-rec-hw/vidshot-320x200-l190.ppm"
	say "-- interpreter: hardware fallback and return"
	$OPT --name "$TAG-int-fb" --elf "$ELF" --pak "$PAK" --out build/runs --emu build/pcsx2-int/AppRun --map MAP01 --timeout 3600 -- -renderer Hardware -zsingle -zck -zquit 60 -hwfbtest 30 -zchain 02 >> "$L" 2>&1
	grep_run "$TAG-int-fb" "ps2_hwfb\|HWE acq\|I_Error\|ZCHAIN" 200
}

stage_split() {
	local R EXTRA
	python3 - > build/split-pad.txt <<'PYEOF'
import subprocess, sys
a = subprocess.run([sys.executable, 'tools/ps2/gen_padscript.py', '1', '10', '600'], capture_output=True, text=True).stdout.strip().split(',')
b = subprocess.run([sys.executable, 'tools/ps2/gen_padscript.py', '2', '10', '600', '--seed', '2'], capture_output=True, text=True).stdout.strip().split(',')
print(','.join(sorted(a + b, key=lambda s: (int(s.split(':')[0]), s))))
PYEOF
	for R in Software Hardware; do
		mkdir -p "build/runs/$TAG-split-$R"
		cp build/split-pad.txt "build/runs/$TAG-split-$R/pad.txt"
		EXTRA=""
		[ $R = Hardware ] && EXTRA="-renderer Hardware"
		say "-- split screen $R"
		$OPT --name "$TAG-split-$R" --elf "$ELF" --pak "$PAK" --out build/runs --map MAP01 --timeout 1800 --until "VIDSHOT COMPLETE" -- $EXTRA -splitscreen -padscript file:pad.txt -zsingle -zck -vidshot l250,l500 >> "$L" 2>&1
		grep_run "$TAG-split-$R" "joystick\|padscript\|I_Error\|OOM\|ps2_hwfb" 200
	done
}

stage_net() {
	export SRB2_PCSX2_ROOT=$ROOT/build/pcsx2-roots
	mkdir -p build/stab-net/run
	python3 tools/ps2/net_env.py > /dev/null 2>&1
	python3 tools/ps2/net_specs9.py --elf "$ELF" --base build/stab-net > /dev/null 2>&1
	run python3 tools/ps2/net_batch.py --specs build/stab-net/specs --out build/stab-net/run --retries 2 \
		ps2srv-pccli pcsrv-ps2cli mode-match-pcsrv-ps2cli mode-ctf-pcsrv-ps2cli mode-race-pcsrv-ps2cli mode-tag-pcsrv-ps2cli mode-coop-pcsrv-ps2cli \
		ps2host-menu menu-browse server-kill client-kill soak-pcsrv-ps2cli soak-ps2srv-pccli soak-ps2srv-ps2cli \
		addons-udp addons-http sw-net-coop hw-net-coop hw-net-match
}

demo_suite() { # demo_suite PREFIX sw|hw [extra engine args]
	local P=$1 M=$2 n
	shift 2
	for n in 1 2 3 4; do
		if [ "$M" = hw ]; then
			$OPT --name "$TAG-$P$n" --elf "$ELF" --pak "$PAK" --out build/runs --demo DEMO_00$n --timeout 2400 -- -renderer Hardware "$@" >> "$L" 2>&1
		else
			$OPT --name "$TAG-$P$n" --elf "$ELF" --pak "$PAK" --out build/runs --demo DEMO_00$n --timeout 2400 -- "$@" >> "$L" 2>&1
		fi
		say "--- DEMO_00$n vs PC golden (tics)"
		run python3 tools/ps2/golden_check.py --run "build/runs/$TAG-$P$n/refout" --ref "golden/phase0-v2/run1/DEMO_00$n"
		if [ "$M" = sw ]; then
			say "--- DEMO_00$n vs golden/ps2-head (bitwise frames; only valid while the software output is not meant to change)"
			run python3 tools/ps2/golden_check.py --run "build/runs/$TAG-$P$n/refout" --ref "golden/ps2-head/DEMO_00$n" --pixels
		fi
		grep_run "$TAG-$P$n" "gametics in\|OOM\|I_Error\|ps2_hwfb: fall" 200
	done
}

# The demos need the PS2REF ELF (tic log and frame dump, -DPS2REF) of the SAME source tree as ELF: SRB2_STAB_REFELF names one, otherwise it is built here (build/out-ref, incremental,
# the environment of the caller: SRB2_PS2_NO / SRB2_PS2_HW as for the ELF itself; the default is the full configuration with the hardware renderer).
ref_elf() { # prints the PS2REF ELF (SRB2_STAB_REFELF, or built here)
	local REF=${SRB2_STAB_REFELF:-}
	if [ -z "$REF" ]; then
		say "-- building the PS2REF ELF from this tree (build/out-ref)"
		export PS2DEV=${PS2DEV:-/opt/ps2dev-x/ps2dev}
		export PATH=$PS2DEV/ee/bin:$PS2DEV/bin:$PS2DEV/dvp/bin:$PATH
		SRB2_PS2_OUT=$ROOT/build/out-ref SRB2_PS2_NO=${SRB2_PS2_NO-} SRB2_PS2_HW=${SRB2_PS2_HW-1} python3 tools/ps2/build.py --ps2ref --jobs 2 >> "$L" 2>&1
		REF=$ROOT/build/out-ref/SRB2.ELF
	fi
	echo "$REF"
}
stage_demosw() {
	local REF
	REF=$(ref_elf)
	[ -f "$REF" ] || { say "demos: no PS2REF ELF ($REF), skipped"; return; }
	say "-- demos, software, on $REF ($(stat -c %s "$REF") bytes)"
	ELF=$REF demo_suite dsw sw
}
stage_demohw() {
	local REF
	REF=$(ref_elf)
	[ -f "$REF" ] || { say "demos: no PS2REF ELF ($REF), skipped"; return; }
	say "-- demos, hardware, on $REF"
	ELF=$REF demo_suite dhw hw
}
stage_demohi() {
	local REF
	REF=$(ref_elf)
	[ -f "$REF" ] || { say "demos: no PS2REF ELF ($REF), skipped"; return; }
	say "-- demos, hardware with an out-of-memory injection every 200 frames, on $REF"
	ELF=$REF demo_suite dhi hw -zoomevery 200
}

stage_chainsw() {
	say "-- chain sweep, software (all maps in session chains)"
	run python3 tools/ps2/chain_sweep.py --elf "$ELF" --tag "$TAG-sw" --pak "$PAK" --kinds SP,MP-special,Match,CTF --frames "$FRAMES" -- -zstack
}
stage_chainhw() {
	say "-- chain sweep, hardware"
	run python3 tools/ps2/chain_sweep.py --elf "$ELF" --tag "$TAG-hw" --pak "$PAK" --kinds SP,MP-special,Match,CTF --frames "$FRAMES" -- -renderer Hardware -zstack
}
stage_coldsw() {
	say "-- cold per-map sweep (one boot per map), software"
	run python3 tools/ps2/map_sweep.py --elf "$ELF" --tag "$TAG-cold-sw" --out build/runs/msweep --pak "$PAK" --kinds SP,MP-special,Match,CTF --frames "$FRAMES" -- -zstack
}
stage_coldhw() {
	say "-- cold per-map sweep, hardware"
	run python3 tools/ps2/map_sweep.py --elf "$ELF" --tag "$TAG-cold-hw" --out build/runs/msweep --pak "$PAK" --kinds SP,MP-special,Match,CTF --frames "$FRAMES" -- -renderer Hardware -zstack
}

stage_soaksw() {
	run python3 tools/ps2/stab_soak.py --elf "$ELF" --tag "$TAG-soak" --renderer Software --minutes "$MINUTES"
}
stage_soakhw() {
	run python3 tools/ps2/stab_soak.py --elf "$ELF" --tag "$TAG-soak" --renderer Hardware --minutes "$MINUTES"
}

echo "stab_run: ELF $ELF TAG $TAG stages: $STAGES -> $L"
say "# stab_run $(date -u +%FT%TZ) ELF $ELF ($(stat -c %s "$ELF") bytes) stages: $STAGES"
expand() { # aliases of the long stages (each part is resumable)
	local x
	for x in $STAGES; do
		case $x in
			demos) echo -n "demosw demohw demohi " ;;
			sweep) echo -n "chainsw chainhw coldsw coldhw " ;;
			soak) echo -n "soaksw soakhw " ;;
			*) echo -n "$x " ;;
		esac
	done
}
STAGES=$(expand)
# Resumable: a stage that finished in an earlier run with this TAG (STAGE-DONE in the log) is not run again (SRB2_STAB_RERUN=1 runs everything), so a restart of the
# machine or of the session costs only the stage that was running.
for s in $STAGES; do
	if [ -z "$SRB2_STAB_RERUN" ] && grep -q "^STAGE-DONE $s\$" "$L" 2>/dev/null; then
		echo "stab_run: stage $s is done (log), skipped"
		continue
	fi
	say "== $s"
	"stage_$s"
	say "STAGE-DONE $s"
done
say "STAB-RUN-DONE"
