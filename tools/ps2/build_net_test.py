"""Build the network probe tools/ps2/net_test.c (OPT6-S) into build/opt6-s/nettest/NETTEST.ELF."""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEV = Path('D:/ps2dev')
SDK = DEV / 'ps2sdk'
OUT = ROOT / 'build/opt6-s/nettest'
OUT.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, PS2DEV=str(DEV), PS2SDK=str(SDK))
env['PATH'] = ';'.join(str(p) for p in [DEV/'ee/bin', DEV/'iop/bin', DEV/'bin', Path('C:/Windows/System32'), Path('C:/Windows')])
cc = DEV/'ee/bin/mips64r5900el-ps2-elf-gcc.exe'
cmd = [str(cc), '-D_EE', '-G0', '-O2', '-std=gnu11', '-Wall', '-Wextra',
       '-I'+str(SDK/'ee/include'), '-I'+str(SDK/'common/include'), '-I'+str(SDK/'ports/include'),
       '-T'+str(SDK/'ee/startup/linkfile'), '-L'+str(SDK/'ee/lib'), '-L'+str(SDK/'ports/lib'),
       '-Wl,-zmax-page-size=128', '-Wl,--defsym,_stack_size=0x80000',
       str(ROOT/'tools/ps2/net_test.c'), '-o', str(OUT/'NETTEST.ELF'),
       '-lps2_drivers', '-lps2ip', '-lnetman', '-lpoweroff', '-lpatches', '-ldebug', '-lm']
p = subprocess.run(cmd, env=env, capture_output=True, text=True)
print(p.stdout + p.stderr)
sys.exit(p.returncode)
