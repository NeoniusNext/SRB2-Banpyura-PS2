"""OPT14: a pk3 whose Lua folder is the awkward kind a community mod ships (tools/ps2/lua_equiv.py loads it with -file on the PC build and on the PS2 ELF; every script prints an LQ line when it is run).

usage: make_luapk3.py [out.pk3]            default build/opt14-addons/LP.pk3
Contents of Lua/: upper and lower case names, nested folders, a script with CRLF line endings, one with UTF-8 text, a 300 KB one, a script with a syntax error, one with a runtime error,
a script with a syntax error and one with a runtime error (a UTF-8 BOM at the start is not tested: the PC build says "unexpected symbol", the PS2 lexer, which takes bytes >= 0x80 for letters (PS2-103), "attempt to call global 'print'": a script with a BOM fails on both), one that `return`s a value, an empty one, a script that is only comments; stored and deflated entries. The order in which the engine runs them (the LQ lines) must be the PC's.
"""
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def build(out):
    out.parent.mkdir(parents=True, exist_ok=True)
    z = zipfile.ZipFile(out, 'w')

    def add(name, text, deflate=True):
        data = text if isinstance(text, bytes) else text.encode('utf-8')
        z.writestr(zipfile.ZipInfo(name, (2020, 1, 1, 0, 0, 0)), data, zipfile.ZIP_DEFLATED if deflate else zipfile.ZIP_STORED)

    # order of the zip entries on purpose not alphabetical
    add('Lua/main_b.lua', 'print("LQ pk3 run main_b")\nlocal t = {}\nfor i = 1, 10 do t[i] = i end\nprint("LQ pk3 main_b sum " .. #t)\n')
    add('Lua/Main_A.lua', 'print("LQ pk3 run Main_A")\nrawset(_G, "LP_SHARED", 41)\n', deflate=False)
    add('Lua/sub/c.lua', 'print("LQ pk3 run sub/c " .. tostring(LP_SHARED))\nrawset(_G, "LP_SHARED", LP_SHARED + 1)\n')
    add('Lua/Sub2/D.lua', 'print("LQ pk3 run Sub2/D " .. tostring(LP_SHARED))\n')
    add('Lua/sub/deeper/e.lua', 'print("LQ pk3 run sub/deeper/e")\n')
    add('Lua/crlf.lua', 'print("LQ pk3 run crlf")\r\nlocal s = [[a\r\nb]]\r\nprint("LQ pk3 crlf len " .. #s)\r\n-- comment\r\nprint("LQ pk3 crlf done")\r\n')
    add('Lua/utf8.lua', '-- русский comment ☃\nlocal s = "привет ☃ é"\nprint("LQ pk3 run utf8 " .. #s)\n')
    add('Lua/syntaxerr.lua', 'print("LQ pk3 run syntaxerr")\nlocal x = = 3\n')
    add('Lua/runtimeerr.lua', 'print("LQ pk3 run runtimeerr")\nlocal t = nil\nprint(t.x)\n')
    add('Lua/retval.lua', 'print("LQ pk3 run retval")\nreturn 5, "x"\n')
    add('Lua/empty.lua', '')
    add('Lua/comments.lua', '-- nothing\n--[[ block ]]\n')
    big = ['print("LQ pk3 run big")', 'local t = {']
    for i in range(1, 12001):
        big.append('  {id = %d, name = "entry%d", v = %d},' % (i, i, i * 7 % 1000))
    big.append('}')
    big.append('local s = 0 for i = 1, #t do s = s + t[i].v end')
    big.append('print("LQ pk3 big " .. #t .. " " .. s)')
    add('Lua/zbig.lua', '\n'.join(big) + '\n')
    add('Lua/lastone.lua', 'print("LQ pk3 run lastone " .. tostring(LP_SHARED))\nprint("LQ DONE_PK3")\n')
    # not scripts: must not be run
    add('Lua/readme.txt', 'print("LQ pk3 WRONG readme")\n')
    add('Misc/notlua.lua', 'print("LQ pk3 WRONG misc")\n')
    add('SOC/main.soc', 'Freeslot\nS_LPSOC1\n')
    z.close()


if __name__ == '__main__':
    build(Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'build/opt14-addons/LP.pk3')
    print('ok')
