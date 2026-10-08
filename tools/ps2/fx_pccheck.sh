#!/bin/bash
# OPT11-FX2: syntax and warning check of the shared engine files of THIS worktree with the flags of the PC build (build/pc-ref belongs to the main tree: its ninja compiles the
# main tree's sources, not these). usage: fx_pccheck.sh file.c...   (paths relative to src/, e.g. hardware/hw_main.c r_things.c)
cd "$(dirname "$0")/../.."
MAIN=/home/user/SRB2-Banpyura-PS2
for f in "$@"; do
	cmd=$(ninja -C build/pc-ref -t commands 2>/dev/null | grep -- "-c $MAIN/src/$f" | head -1)
	if [ -z "$cmd" ]; then echo "$f: no PC command found"; continue; fi
	cmd=${cmd//$MAIN\/src\//$PWD/src/}
	cmd=${cmd//-c $PWD\/src\/$f/-c $PWD/src/$f -fsyntax-only}
	# drop the output file argument
	cmd=$(echo "$cmd" | sed -E 's/ -o [^ ]+//; s/ -MF [^ ]+//; s/ -MT [^ ]+//; s/ -MD//')
	echo "$f: $(echo "$cmd" | grep -o -- ' -c [^ ]*')"
	if (cd build/pc-ref && eval "$cmd" 2>&1 | grep -E "error|warning" | head -20); then :; fi
	echo "$f: checked"
done
