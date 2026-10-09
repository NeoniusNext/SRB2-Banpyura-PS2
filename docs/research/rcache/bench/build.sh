#!/bin/bash
# usage: build.sh SRC.c OUT.ELF   (needs PS2DEV, e.g. /opt/ps2dev-x/ps2dev)
set -e
PS2DEV=${PS2DEV:-/opt/ps2dev-x/ps2dev}; SDK=$PS2DEV/ps2sdk
$PS2DEV/ee/bin/mips64r5900el-ps2-elf-gcc -D_EE -G0 -O2 -std=gnu11 -Wall -I$SDK/ee/include -I$SDK/common/include \
  -T$SDK/ee/startup/linkfile -L$SDK/ee/lib -Wl,-zmax-page-size=128 -Wl,--defsym,_stack_size=0x20000 "$1" -o "$2" -lkernel -ldebug -lc -lm
