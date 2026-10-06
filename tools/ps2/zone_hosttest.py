"""Run the actual PS2 zone arena (src/z_zone.c + src/ps2/ps2_mem.c) on x86 and the native host profile on x64; no PCSX2.

usage: python tools/ps2/zone_hosttest.py [--out build/agent-zone-d4/host] [--negative-controls] [--x64] [--identical]
  default      ps2-x86 (release: 16-byte header), ps2-x86-zdebug (ZDEBUG: owner file:line, red zones on), host-profile x64
  --x64        also compile/run the PS2 path with 64-bit pointers (portability only; the EE is 32-bit)
  --negative-controls   each mutation of the allocator must make the test fail
  --identical  the non-PS2 preprocessed output of z_zone.c must equal the pinned vanilla source (host unchanged)
Logs and executables stay in the output directory. Engine sources are not rewritten.
"""
import argparse
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VCVARS = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat")
PIN = "0e09462308610005f640ed84b21ec8a4ef116a4b"
FILELINE = re.compile(r'"(?:[^"\\]|\\.)*z_zone[a-z_]*\.c", \d+')  # ZDEBUG: __FILE__, __LINE__ of the Z_Free call sites
GUARDS = ["__DOOMDEF__", "__DOOMTYPE__", "__DOOMSTAT__", "__R_PATCH__", "__R_PICFORMATS__", "__I_SYSTEM__",
          "__I_VIDEO__", "__M_MISC__", "__COMMAND_H__", "__M_ARGV__", "LUA_SCRIPT_H"]


def vc(work, arch, cmd, log):
    bat = work / "step.bat"
    bat.write_text(f'@echo off\ncall "{VCVARS}" {arch} >nul 2>&1\nif errorlevel 1 exit /b 1\n'
                   + subprocess.list2cmdline(cmd) + "\n", encoding="utf-8")
    r = subprocess.run(["cmd", "/c", str(bat)], cwd=work, capture_output=True, text=True, encoding="oem", errors="replace")
    (work / log).write_text(r.stdout + r.stderr, encoding="utf-8")
    return r


def run_case(out, name, arch, defines, expect_failure=False, timeout=3600, test_args=()):
    work = out / name
    work.mkdir(exist_ok=True)
    cmd = ["cl", "/nologo", "/std:c17", "/O2", "/W4", "/WX", "/D_CRT_SECURE_NO_WARNINGS", "/DPARANOIA",
           "/I" + str(ROOT / "src"), "/I" + str(ROOT / "src/ps2"), *defines, "/Fe:zone_hosttest.exe", str(ROOT / "tools/ps2/zone_hosttest.c")]
    if expect_failure:
        cmd.insert(1, "/wd4702")  # a mutation can make the compiler prove an assertion always exits
    built = vc(work, arch, cmd, "build.log")
    if built.returncode:
        print(built.stdout + built.stderr)
        raise RuntimeError(f"{name}: build failed")
    tested = subprocess.run([str(work / "zone_hosttest.exe"), *test_args], cwd=work, capture_output=True, text=True, timeout=timeout)
    (work / "test.log").write_text(tested.stdout + tested.stderr, encoding="utf-8")
    print(f"{name}: exit={tested.returncode}")
    if expect_failure:
        if tested.returncode == 0:
            raise RuntimeError(f"{name}: mutation survived all test assertions")
        last = (tested.stderr.strip().splitlines() or tested.stdout.strip().splitlines() or ["(crash)"])[-1]
        print("  failed as expected:", last)
        if tested.returncode not in (1, 2) or not re.search(r"FAIL:|Unexpected I_Error:", tested.stderr):
            raise RuntimeError(f"{name}: negative control must fail a test assertion, not crash")
    else:
        print(tested.stdout + tested.stderr, end="")
    if (tested.returncode != 0) != expect_failure:
        raise RuntimeError(f"{name}: unexpected test result")


# (file, [(old, new, expected occurrences)]) per mutation: each is a regression of a bug class the tests must catch
MUTATIONS = {
    "no-coalesce-next": ("src/ps2/ps2_mem.c",
        [("if (next < za_end && !(((zablock_t *)next)->sf & ZAF_USED))", "if (0 && next < za_end && !(((zablock_t *)next)->sf & ZAF_USED))", 1)]),
    "no-coalesce-prev": ("src/ps2/ps2_mem.c",
        [("\tif (pf)\n\t{\n\t\tsize_t psize", "\tif (0 && pf)\n\t{\n\t\tsize_t psize", 1)]),
    "ignored-alignment": ("src/ps2/ps2_mem.c",
        [("low = ((uintptr_t)f + ZA_HDR + align - 1) & ~(uintptr_t)(align - 1);", "low = ((uintptr_t)f + ZA_HDR + 15) & ~(uintptr_t)15;", 1),
         ("high = ((uintptr_t)fend - need + ZA_HDR) & ~(uintptr_t)(align - 1);", "high = ((uintptr_t)fend - need + ZA_HDR) & ~(uintptr_t)15;", 1)]),
    "accounting-drift": ("src/ps2/ps2_mem.c", [("\tza_used -= size;\n", "\tza_used -= size - 16;\n", 1)]),
    "block-too-small": ("src/ps2/ps2_mem.c",
        [("size_t n = ZA_HDR + size + (za_redzone ? ZA_GUARD_MIN : 0);", "size_t n = ZA_HDR + (size > 15 ? size - 16 : size) + (za_redzone ? ZA_GUARD_MIN : 0);", 1)]),
    "red-zone-unchecked": ("src/ps2/ps2_mem.c", [("(b->sf & ZAF_REDZONE) && !ZA_GuardOk(b)", "0 && (b->sf & ZAF_REDZONE) && !ZA_GuardOk(b)", 2)]),
    "evict-current-frame": ("src/z_zone.c", [("(current || Z_Age(block) != 0)", "(current || 1)", 1)]),
    "long-age-lru-collapsed": ("src/z_zone.c", [("if (limit == Z_AGE_BUCKETS - 1)", "if (0 && limit == Z_AGE_BUCKETS - 1)", 1)]),
    "realloc-source-purged": ("src/z_zone.c", [("ZA_TAG(block) <= hightag && block != zpinned)", "ZA_TAG(block) <= hightag)", 1)]),
    "locked-eviction": ("src/z_zone.c", [("!(current || Z_Age(block) != 0)", "!(current || zpurgelock || Z_Age(block) != 0)", 1)]),
    "owner-not-cleared": ("src/z_zone.c", [("\tif (block->user != NULL)\n\t\t*block->user = NULL;\n\n\tif (Z_RegTag(ZA_TAG(block)))", "\tif (Z_RegTag(ZA_TAG(block)))", 1)]),
    # PS2-76: the cache registry and the purge flag
    "registry-removal-skipped": ("src/z_zone.c", [("\tif (Z_RegTag(ZA_TAG(block)))\n\t\tZ_RegRemove(block);\n\treturn ZA_Free(ptr);", "\treturn ZA_Free(ptr);", 1)]),
    "registry-unsorted": ("src/z_zone.c", [("\tUINT32 lo = 0, hi = zreg_n;\n", "\tUINT32 lo = zreg_n, hi = zreg_n;\n", 1)]),
    "registry-retag-ignored": ("src/z_zone.c", [("\tif (oldtag != newtag)\n\t{", "\tif (0)\n\t{", 1)]),
    "purge-flag-never-set": ("src/z_zone.c", [("\tif (newtag >= PU_PURGELEVEL)\n\t\tzpurge_maybe = true;", "\t(void)newtag;", 1)]),
    "makeroom-skips-free-before-first": ("src/z_zone.c", [("first = prev ? prev : zreg[0];", "(void)prev; first = zreg[0];", 1)]),
    "touch-ignored": ("src/z_zone.c", [("\tif (ptr)\n\t\tZA_SetStamp(ZA_BLOCK(ptr), zframe);", "\t(void)ptr;", 1)]),
    "cache-release-ignored": ("src/z_zone.c", [("ZA_SetStamp(block, (zframe - 1) & Z_FRAME_MASK);", "ZA_SetStamp(block, zframe);", 1)]),
    "cache-release-rejuvenates": ("src/z_zone.c", [("block != zpinned && Z_Age(block) == 0)", "block != zpinned)", 1)]),
    # (the earlier "continue from the old end of the freed block" mutant is equivalent: the stale header of an absorbed free
    # neighbour keeps its size until the next allocation, so it survived; this one changes behaviour)
    "iterate-ignores-callback": ("src/z_zone.c", [("\t\t\tif (free)\n\t\t\t\tblock = ZA_BlockAt(Z_FreeBlock(ZA_BLOCK(mem)));", "\t\t\tif (free && !free)\n\t\t\t\tblock = ZA_BlockAt(Z_FreeBlock(ZA_BLOCK(mem)));", 1)]),
    "alignbits-32-accepted": ("src/z_zone.c", [("if (alignbits < 0 || alignbits >= 32)", "if (alignbits < 0 || alignbits >= 64)", 2)]),
    "lock-total-bytes-only": ("src/z_zone.c", [("if (ZA_LargestFree() >= bytes + Z_EVICT_ALIGN_PAD)", "if (ZA_FreeBytes() >= bytes)", 1)]),
    "lock-spares-current-frame": ("src/z_zone.c", [("if (!ZA_ISFREE(b) && !Z_Evictable(b, current))", "if (!ZA_ISFREE(b) && !Z_Evictable(b, current && 0))", 1),
                                                   ("Z_EvictLRU(bytes - ZA_FreeBytes() + Z_EVICT_SLACK, true);", "(void)bytes;", 1)]),
    "room-cost-ignores-age": ("src/z_zone.c", [("return (ZA_SIZE(b) >> 8) * (4096u >> age) + 1;", "(void)age; return (ZA_SIZE(b) >> 8) + 1;", 1)]),
    "no-lock-headroom": ("src/z_zone.c", [("\t\t\tZ_EnsureFree(zheadroom);\n", "\t\t\t(void)zheadroom;\n", 1)]),
    "purgable-not-first": ("src/z_zone.c", [("\tif (!zpurgelock)\n\t\tZ_FreeTagRange(PU_PURGELEVEL, INT32_MAX);\n\tp = ZA_Alloc(size, align, side);", "\tp = ZA_Alloc(size, align, side);", 1)]),
    "no-inplace-realloc": ("src/z_zone.c", [("&& ZA_Resize(ptr, size))", "&& 0 && ZA_Resize(ptr, size))", 1)]),
    "reclaim-hook-ignored": ("src/z_zone.c", [("if (!Z_EvictLRU(want, false) && !Z_Reclaim(want))", "if (!Z_EvictLRU(want, false))", 1)]),
    "reclaim-before-cache": ("src/z_zone.c", [("if (!Z_EvictLRU(want, false) && !Z_Reclaim(want))", "if (!Z_Reclaim(want) && !Z_EvictLRU(want, false))", 1)]),
    "budget-ignores-live-heap": ("src/ps2/ps2_mem.c", [("growth = stackbase - brk;", "growth = stackbase;", 1)]),
}


def negative_controls(out):
    originals = {}
    for name, (rel, edits) in MUTATIONS.items():
        text = originals.setdefault(rel, (ROOT / rel).read_text(encoding="utf-8"))
        for old, new, count in edits:
            if text.count(old) != count:
                raise RuntimeError(f"Mutation anchor changed: {name}: {old!r} x{text.count(old)}")
        modified = text
        for old, new, count in edits:
            modified = modified.replace(old, new)
        source = out / (name + ".c")
        source.write_text(modified, encoding="utf-8")
        defines = ["/DZONE_EXPECT_HDR16"]
        if rel == "src/z_zone.c":
            defines.append('/DZONE_HOST_SOURCE="' + source.as_posix() + '"')
        else:
            defines.append('/DZONE_MEM_SOURCE="' + source.as_posix() + '"')
        run_case(out, "negative-" + name, "x86", defines, True)


def identical(out):
    """The host (non-PS2) code of z_zone.c must be the pinned vanilla text: compare preprocessed output."""
    pin = out / "z_zone_vanilla.c"
    pin.write_text(subprocess.run(["git", "show", f"{PIN}:src/z_zone.c"], cwd=ROOT, capture_output=True, text=True,
                                  encoding="utf-8", check=True).stdout, encoding="utf-8")
    work = out / "identical"
    work.mkdir(exist_ok=True)
    for label, defs in {"plain": [], "zdebug": ["/DZDEBUG"], "zdebug+paranoia": ["/DZDEBUG", "/DPARANOIA"],
                        "valgrind-free+hwrender": ["/DHWRENDER"]}.items():
        texts = []
        for src in (pin, ROOT / "src/z_zone.c"):
            pp = work / (src.stem + "." + label.replace("+", "_") + ".i")
            cmd = ["cl", "/nologo", "/EP", "/I" + str(ROOT / "src"), *[f"/D{g}" for g in GUARDS], *defs, str(src)]
            r = vc(work, "x64", cmd, "pp.log")
            if r.returncode:
                raise RuntimeError("preprocess failed:\n" + r.stderr)
            # ZDEBUG expands __FILE__/__LINE__ at each Z_Free call site: file name and line numbers are not code
            lines = [FILELINE.sub('"z_zone.c", 0', ln.rstrip()) for ln in r.stdout.splitlines() if ln.strip()]
            # the vanilla file is CRLF in the tree and LF in git; only whitespace/blank lines are normalised
            pp.write_text("\n".join(lines), encoding="utf-8")
            texts.append(lines)
        same = texts[0] == texts[1]
        print(f"identical[{label}]: vanilla {len(texts[0])} lines, current {len(texts[1])} lines -> {'IDENTICAL' if same else 'DIFFERENT'}")
        if not same:
            import difflib
            for ln in list(difflib.unified_diff(texts[0], texts[1], lineterm="", n=1))[:40]:
                print(ln)
            raise RuntimeError("host z_zone.c differs from the pinned vanilla source")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "build/agent-zone-d4/host"))
    ap.add_argument("--negative-controls", action="store_true")
    ap.add_argument("--x64", action="store_true")
    ap.add_argument("--identical", action="store_true")
    ap.add_argument("--only", default="", help="comma list of cases: x86,zdebug,x64,native")
    ap.add_argument("--trace-only", action="store_true", help="only the fragmentation trace (policy experiment)")
    ap.add_argument("--prefer", type=int, choices=range(1, 17), help="test free-list candidate count override (1..16)")
    args = ap.parse_args()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    only = set(filter(None, args.only.split(",")))
    if only - {"x86", "zdebug", "x64", "native"}:
        ap.error("unknown --only case")
    test_args = (["--trace-only"] if args.trace_only else []) + (["--prefer", str(args.prefer)] if args.prefer else [])
    if not only or "x86" in only:
        run_case(out, "ps2-x86", "x86", ["/DZONE_EXPECT_HDR16"], test_args=test_args)
    if not only or "zdebug" in only:
        run_case(out, "ps2-x86-zdebug", "x86", ["/DZDEBUG"], test_args=test_args)
    if args.x64 or "x64" in only:
        run_case(out, "ps2-x64", "x64", [], test_args=test_args)
    if not only or "native" in only:
        run_case(out, "host-profile", "x64", ["/DZONE_HOST_NATIVE"])
    if args.identical:
        identical(out)
    if args.negative_controls:
        negative_controls(out)
    print("All requested zone host tests passed (negative controls failed as expected).")


if __name__ == "__main__":
    main()
