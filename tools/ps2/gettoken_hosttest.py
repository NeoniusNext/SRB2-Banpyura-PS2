"""PS2-LOAD-24: M_GetToken / M_GetTokenPooled / M_UnGetToken of src/m_misc.c, the fast PS2_PROFILE version against the original, on the text lumps of the game and random texts.

usage: python3 tools/ps2/gettoken_hosttest.py [FILE ...]      (the files are the TEXTURES / ANIMDEFS / SPRTINFO / S_SKIN / SOC lumps; see below)
The token parser is cut out of m_misc.c (from "Token parser variables" to the end of M_UnGetToken), compiled twice with renamed entry points (original, and with
PS2_PROFILE) and driven by tools/ps2/gettoken_hosttest.c: after every call the strings returned (or NULL) have to be equal, and so has what the next calls return
after M_UnGetToken (it moves back to the position the call started at).
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/hosttest/gt'
OUT.mkdir(parents=True, exist_ok=True)
src = (ROOT / 'src/m_misc.c').read_text()
a = src.index('// Token parser variables')
b = src.index('static tokenizer_t *globalTokenizer')
body = src[a:b]
prelude = '''#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t UINT8; typedef uint32_t UINT32; typedef int boolean;
#define true 1
#define false 0
#define PU_STATIC 1
#define Z_Malloc(n, t, u) malloc(n)
#define Z_Free(p) free(p)
#define M_Memcpy memcpy
'''
for tag, define in (('orig', ''), ('fast', '#define PS2_PROFILE 1\n')):
    (OUT / f'tok_{tag}.c').write_text(define + prelude + body.replace('M_GetTokenPooled', f'{tag}_M_GetTokenPooled').replace('M_GetToken', f'{tag}_M_GetToken').replace('M_UnGetToken', f'{tag}_M_UnGetToken').replace('M_FreeToken', f'{tag}_M_FreeToken'))
    # (a second replace pass would double-rename M_GetTokenPooled: the order above renames Pooled first, then the plain name inside what is left)
for tag in ('orig', 'fast'):
    t = (OUT / f'tok_{tag}.c').read_text()
    t = t.replace(f'{tag}_{tag}_', f'{tag}_')
    (OUT / f'tok_{tag}.c').write_text(t)
for tag in ('orig', 'fast'):
    subprocess.run(['cc', '-O2', '-Wall', '-Wno-unused-function', '-Wno-unused-variable', '-c', str(OUT / f'tok_{tag}.c'), '-o', str(OUT / f'tok_{tag}.o')], check=True)
exe = OUT / 'gettoken_hosttest'
subprocess.run(['cc', '-O2', '-Wall', str(ROOT / 'tools/ps2/gettoken_hosttest.c'), str(OUT / 'tok_orig.o'), str(OUT / 'tok_fast.o'), '-o', str(exe)], check=True)
files = sys.argv[1:] or sorted(str(p) for p in OUT.glob('f*.txt'))
sys.exit(subprocess.run([str(exe), *files]).returncode)
