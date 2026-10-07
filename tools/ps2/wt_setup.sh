#!/bin/bash
# Prepare a git worktree of this repo for building/running the PS2 port (Linux cloud container, see docs/OPT10_BRIEF.md).
# Links the untracked shared inputs (SRB2 2.2.15 assets, cooked packs, PC reference engines, golden data) into the worktree.
# usage: cd <worktree> && tools/ps2/wt_setup.sh
set -e
MAIN=${SRB2_MAIN_TREE:-/home/user/SRB2-Banpyura-PS2}
cd "$(dirname "$0")/../.."
mkdir -p build
ln -sfn /opt/srb2-assets srb2-assets
[ -e golden ] || ln -sfn "$MAIN/golden" golden
for d in pak pc-golden pc-ref host-gen; do [ -e build/$d ] || ln -sfn "$MAIN/build/$d" build/$d; done
for f in srb2-assets golden build; do grep -qxF "$f" "$(git rev-parse --git-dir)/info/exclude" 2>/dev/null || echo "$f" >> "$(git rev-parse --git-dir)/info/exclude" 2>/dev/null || true; done
export PATH=/opt/ps2dev-x/ps2dev/ee/bin:$PATH
echo "worktree ready: $(pwd)"
echo "  toolchain: $(mips64r5900el-ps2-elf-gcc --version | head -1)"
echo "  packs:     $(ls build/pak/*.PAK | wc -l) (build/pak -> $MAIN/build/pak)"
echo "  golden:    $(ls golden/phase0-v2/run1 2>/dev/null | wc -l) demo dirs"
