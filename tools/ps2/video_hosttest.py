"""Host test of the PS2 video mode tables (src/ps2/ps2_vmodes.h) and of docs/VIDEO_MODES.md.

python tools/ps2/video_hosttest.py [--out build/agent-vid-host] [--negative-controls]

1. builds tools/ps2/video_hosttest.c with MSVC (the header under test is compiled as it is) and runs it: CRTC values against a
   copy of gsKit's arithmetic, VRAM budget of every (output format x internal mode) pair, picture placement properties;
2. compares the numbers the header produces with the two tables of docs/VIDEO_MODES.md (names, sizes, pages, magnification);
3. --negative-controls: every group has a deliberate wrong expectation (mutate=N) that has to turn exactly that group red.
Exit code 0 only when everything passed.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VCVARS = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat")
DOC = ROOT / "docs/VIDEO_MODES.md"


def build(work):
    bat = work / "build.bat"
    lines = ["@echo off", f'call "{VCVARS}" x64 >nul 2>&1', "if errorlevel 1 exit /b 1",
             subprocess.list2cmdline(["cl", "/nologo", "/std:c17", "/O2", "/W4", "/WX", "/D_CRT_SECURE_NO_WARNINGS", "/I" + str(ROOT / "src/ps2"),
                                      str(ROOT / "tools/ps2/video_hosttest.c"), "/Fe:video_hosttest.exe", "/Fo:video_hosttest.obj"])]
    bat.write_text("\n".join(lines) + "\n", encoding="utf-8")
    r = subprocess.run(["cmd", "/c", str(bat)], cwd=work, capture_output=True, text=True, encoding="oem", errors="replace")
    (work / "build.log").write_text(r.stdout + r.stderr, encoding="utf-8")
    if r.returncode:
        print(r.stdout + r.stderr)
        raise SystemExit("host build failed")
    return work / "video_hosttest.exe"


def run(exe, *args):
    r = subprocess.run([str(exe), *args], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def cells(line):
    return [c.strip() for c in line.strip().strip("|").split("|")]


def num(s):
    return int(re.sub(r"[^0-9]", "", s))


def doc_tables():
    modes, outs = {}, {}
    for line in DOC.read_text(encoding="utf-8").splitlines():
        if not line.startswith("|"):
            continue
        c = cells(line)
        if re.fullmatch(r"\d+", c[0]) and len(c) == 9 and re.fullmatch(r"\d+x\d+", c[1]):
            modes[int(c[0])] = c
        elif re.fullmatch(r"\d+", c[0]) and len(c) == 12:
            outs[int(c[0])] = c
    return modes, outs


def check_doc(out):
    bad = []
    modes, outs = doc_tables()
    seen_m, seen_o = set(), set()
    for line in out.splitlines():
        t = line.split()
        if line.startswith("MODE "):
            m, w, h, px, five, dup, pages = (int(v) for v in t[1:8])
            seen_m.add(m)
            c = modes.get(m)
            if not c:
                bad.append(f"mode {m} missing in the doc")
                continue
            exp = {1: f"{w}x{h}", 2: px, 4: five, 5: dup, 6: pages}
            for k, v in exp.items():
                got = c[k] if isinstance(v, str) else num(c[k])
                if got != v:
                    bad.append(f"mode {m}: doc column {k} = {c[k]!r}, code says {v}")
        elif line.startswith("OUT "):
            f = line[4:].split("|")
            oid = int(f[0])
            name, arg, gsm = f[1], f[2], f[3]
            il, ffmd, fbw, fbh, mh, mv, dw, dh, pg, pg2, withtex, worst, dar_w, dar_h, flags = (int(v) for v in f[4:19])
            seen_o.add(oid)
            c = outs.get(oid)
            if not c:
                bad.append(f"output {oid} missing in the doc")
                continue
            # [outid, name, mode, interlace/ffmd, FB, MAG, shown, pages, x2, +tex, aspect, check]
            if c[1] != name:
                bad.append(f"output {oid}: name {c[1]!r} vs {name!r}")
            if c[2].lower() != gsm.lower():
                bad.append(f"output {oid}: mode {c[2]} vs {gsm}")
            kind = ("interlaced" if il else "progressive") + " / " + ("FRAME" if ffmd else "FIELD")
            if oid < 11 or "то же" not in c[3]:
                if c[3] != kind:
                    bad.append(f"output {oid}: {c[3]!r} vs {kind!r}")
            if c[4] != f"{fbw}x{fbh}":
                bad.append(f"output {oid}: fb {c[4]} vs {fbw}x{fbh}")
            if c[5] != f"{mh}x{mv}":
                bad.append(f"output {oid}: mag {c[5]} vs {mh}x{mv}")
            if c[6] and not c[6].startswith(f"{dw}x{dh}"):
                bad.append(f"output {oid}: shown {c[6]} vs {dw}x{dh}")
            if num(c[7]) != pg or num(c[8]) != pg2 or num(c[9]) != withtex:
                bad.append(f"output {oid}: pages {c[7]}/{c[8]}/{c[9]} vs {pg}/{pg2}/{withtex}")
            if c[10] != f"{dar_w}:{dar_h}":
                bad.append(f"output {oid}: aspect {c[10]} vs {dar_w}:{dar_h}")
    for m in modes:
        if m not in seen_m:
            bad.append(f"doc mode {m} not in the code")
    for o in outs:
        if o not in seen_o:
            bad.append(f"doc output {o} not in the code")
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=ROOT / "build/agent-vid-host")
    ap.add_argument("--negative-controls", action="store_true")
    a = ap.parse_args()
    work = a.out.resolve()
    work.mkdir(parents=True, exist_ok=True)
    exe = build(work)
    rc, out = run(exe)
    (work / "test.log").write_text(out, encoding="utf-8")
    summary = [l for l in out.splitlines() if l.startswith("T ")]
    print("\n".join(summary[-10:]))
    ok = rc == 0
    bad = check_doc(out)
    print("doc check:", "OK" if not bad else f"{len(bad)} difference(s)")
    for b in bad[:30]:
        print("  ", b)
    ok = ok and not bad
    if a.negative_controls:
        expect = {1: 1, 2: 2, 3: 3, 5: 5, 6: 6}
        for n, grp in expect.items():
            rc2, out2 = run(exe, f"mutate={n}")
            fails = sorted({int(m.group(1)) for m in re.finditer(r"T FAIL g(\d+)", out2)})
            good = rc2 != 0 and fails == [grp]
            print(f"negative control mutate={n}: red groups {fails} -> {'OK' if good else 'NOT DETECTED'}")
            ok = ok and good
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
