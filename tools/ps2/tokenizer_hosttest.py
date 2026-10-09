"""PS2-LOAD-15: builds src/m_tokenizer.c twice (original and PS2_PROFILE) with renamed entry points and runs tokenizer_hosttest.c over the given TEXTMAP files.
usage: python3 tools/ps2/tokenizer_hosttest.py [TEXTMAP files ...]"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/hosttest'
OUT.mkdir(parents=True, exist_ok=True)
names = ['Tokenizer_Open', 'Tokenizer_Close', 'Tokenizer_Read', 'Tokenizer_SRB2Read', 'Tokenizer_GetEndPos', 'Tokenizer_SetEndPos', 'Tokenizer_SRB2SkipBlock', 'Tokenizer_SRB2ReadPair', 'Tokenizer_SRB2Next']
import shutil
GEN = OUT / 'tokgen'
GEN.mkdir(exist_ok=True)
for f in ('m_tokenizer.c', 'm_tokenizer.h'):  # copied so that the quoted includes find the stubs, not the engine headers
    shutil.copy2(ROOT / 'src' / f, GEN / f)
objs = []
for tag, defs in (('Orig', []), ('Fast', ['-DPS2_PROFILE'])):
    o = OUT / f'tok_{tag}.o'
    ren = [f'-D{n}={tag}_{n}' for n in names]
    subprocess.run(['cc', '-O2', '-Wall', '-Wno-unused-function', '-c', '-I', str(ROOT / 'tools/ps2/hoststub'), *defs, *ren, str(GEN / 'm_tokenizer.c'), '-o', str(o)], check=True)
    objs.append(str(o))
exe = OUT / 'tokenizer_hosttest'
subprocess.run(['cc', '-O2', '-Wall', str(ROOT / 'tools/ps2/tokenizer_hosttest.c'), *objs, '-o', str(exe)], check=True)
sys.exit(subprocess.run([str(exe), *sys.argv[1:]]).returncode)
