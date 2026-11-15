#!/usr/bin/env python3
"""Differential replay of the real flat-plane culls and their local height cache.

The reference is pinned to the published source, rather than a second copy of
the candidate algorithm. This is a host logic/vertex-work test, not EE timing.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
BASE = "e10593b0dbd2ce970a0e56fc66452aedcd746891"


def function(source, name):
    match = re.search(r"^[\w \t*]+\b" + name + r"\([^;]*?\)\s*\{", source, re.M)
    if not match:
        raise RuntimeError("missing function " + name)
    start = match.start()
    brace = source.index("{", start)
    depth, i, state = 1, brace + 1, "code"
    while depth:
        ch, pair = source[i], source[i:i + 2]
        if state == "line":
            if ch == "\n":
                state = "code"
        elif state == "comment":
            if pair == "*/":
                state, i = "code", i + 1
        elif state in ('"', "'"):
            if ch == "\\":
                i += 1
            elif ch == state:
                state = "code"
        elif pair == "//":
            state, i = "line", i + 1
        elif pair == "/*":
            state, i = "comment", i + 1
        elif ch in ('"', "'"):
            state = ch
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
        i += 1
    return source[start:i] + "\n"


def published(path):
    return subprocess.check_output(["git", "show", BASE + ":" + path], cwd=ROOT, text=True)


def rejected_cache_sources():
    """Apply the rejected experiment only to disposable published copies."""
    patch = ROOT / "docs/research/opt15/patches/rejected-front-height-cache.patch"
    paths = ("src/hardware/hw_front.inc", "src/hardware/hw_main.c")
    with tempfile.TemporaryDirectory(prefix="ps2-front-cache-") as temp:
        directory = Path(temp)
        for path in paths:
            target = directory / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(published(path))
        subprocess.run(["git", "apply", str(patch)], check=True, cwd=directory)
        return tuple((directory / path).read_text() for path in paths)


def instrument(source, counter):
    expression = "pl->a * pts[i].x + pl->c * pts[i].y + d0 <= FR_MARGIN"
    if source.count(expression) != 1:
        raise RuntimeError("the production vertex expression changed")
    return source.replace(expression, "(" + counter + "++, " + expression + ")")


def build_header(front, main, negative, native=False):
    old_front = published("src/hardware/hw_front.inc")
    old_main = published("src/hardware/hw_main.c")
    plain = function(front, "HWR_FrPlaneHidden")
    if plain != function(old_front, "HWR_FrPlaneHidden"):
        raise RuntimeError("the uncached production predicate changed")
    cached = function(front, "HWR_FrPlaneHiddenCached")
    if negative == "outside-direction":
        cached = cached.replace("d0 >= cache->out[k]", "d0 <= cache->out[k]")
    elif negative == "inside-direction":
        cached = cached.replace("d0 <= cache->in[k]", "d0 >= cache->in[k]")
    cache_type = re.search(r"typedef struct\s*\{\s*float in\[5\], out\[5\];\s*unsigned int inside, outside;\s*\} fr_heightcache_t;", front)
    if not cache_type:
        raise RuntimeError("the production cache layout changed")
    old_wrapper = function(old_main, "HWR_FrPlaneCulled")
    wrapper = function(main, "HWR_FrPlaneCulled")
    init = re.search(r"\tif \(!fr_planes_out && gl_frontsector->ffloors && !\(hwr_fr_off & HWFR_NOHEIGHTCACHE\)\)\n\t\{[^}]*\}", main)
    if not init or "fr_heightcache.inside = fr_heightcache.outside = 0;" not in init.group():
        raise RuntimeError("the local production cache reset is missing")
    # All four calls must use the current local cache. No cache may escape the
    # single subsector function, including the skybox/split-screen view changes.
    subsector = function(main, "HWR_Subsector")
    calls = re.findall(r"HWR_FrPlaneCulled\([^;]*?\) // OPT12", subsector)
    if len(calls) != 4 or any(", fr_cache)" not in call for call in calls):
        raise RuntimeError("unexpected production plane-cull callers")
    if main.count("fr_heightcache_t fr_heightcache;") != 1 or main.count("fr_heightcache_t *fr_cache = NULL;") != 1:
        raise RuntimeError("the cache must be local to HWR_Subsector")
    init_text = init.group()
    if negative == "stale-scope":
        init_text = init_text.replace("fr_heightcache.inside = fr_heightcache.outside = 0;", "/* injected missing reset */")
    baseline = (plain if native else instrument(plain, "reference_vertices")).replace("HWR_FrPlaneHidden", "ReferencePlaneHidden")
    ordinary = plain if native else instrument(plain, "candidate_vertices")
    cached = cached if native else instrument(cached, "candidate_vertices")
    old_wrapper = old_wrapper.replace("HWR_FrPlaneCulled", "ReferencePlaneCulled").replace("HWR_FrPlaneHidden", "ReferencePlaneHidden")
    return "\n".join([
        cache_type.group(), baseline, ordinary, cached, old_wrapper, wrapper,
        "static fr_heightcache_t *BeginSubsector(fr_heightcache_t *storage, int fr_planes_out)\n{",
        "fr_heightcache_t fr_heightcache = *storage;",  # poison detects reliance on stale storage
        "fr_heightcache_t *fr_cache = NULL;", init_text,
        "*storage = fr_heightcache; return fr_cache ? storage : NULL;\n}",
    ])


def run(command, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True, **kwargs)
    if result.returncode:
        print(result.stdout + result.stderr)
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default=str(ROOT / "build/ht-front-cull"))
    ap.add_argument("--skip-sanitizers", action="store_true")
    ap.add_argument("--ee", action="store_true", help="also build and run isolated EE parity/Count benchmark")
    args = ap.parse_args()
    work = Path(args.out).resolve()
    work.mkdir(parents=True, exist_ok=True)
    cc = shutil.which("gcc") or shutil.which("cc")
    if not cc:
        raise RuntimeError("no host C compiler")
    front, main_source = rejected_cache_sources()
    report = {"baseline": BASE, "rejected_experiment": True, "sources": {p: hashlib.sha256(text.encode()).hexdigest() for p, text in zip(["src/hardware/hw_front.inc", "src/hardware/hw_main.c"], [front, main_source])}, "runs": []}
    variants = [("normal", []), ("outside-direction", []), ("inside-direction", []), ("stale-scope", [])]
    if not args.skip_sanitizers:
        variants.append(("sanitized", ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    for name, flags in variants:
        directory = work / name
        directory.mkdir(exist_ok=True)
        (directory / "hw_front_cull_under_test.h").write_text(build_header(front, main_source, name))
        exe = directory / "hw_front_cull_hosttest"
        command = [cc, "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-ffp-contract=off", *flags, "-I", str(directory), str(ROOT / "tools/ps2/hw_front_cull_hosttest.c"), "-o", str(exe), "-lm"]
        compiled = run(command)
        if compiled.returncode:
            return 1
        result = run([str(exe)])
        expected = name not in ("normal", "sanitized")
        ok = (result.returncode == 1) if expected else (result.returncode == 0)
        print(name + ": " + ("PASS" if ok else "FAIL") + " " + result.stdout.strip().splitlines()[-1])
        report["runs"].append({"name": name, "exit": result.returncode, "expected_failure": expected, "passed": ok, "stdout": result.stdout, "stderr": result.stderr})
        if not ok:
            (work / "report.json").write_text(json.dumps(report, indent=2) + "\n")
            return 1
    if args.ee:
        import build as B
        directory = work / "ee"
        directory.mkdir(exist_ok=True)
        (directory / "hw_front_cull_under_test.h").write_text(build_header(front, main_source, "ee", native=True))
        elf = directory / "FRONT_CULL.ELF"
        obj = directory / "hw_front_cull_hosttest.o"
        flags = [f for f in B.CFLAGS if f not in ("-MMD", "-MP")]
        command = [str(B.CC), *flags, "-Werror", "-DFRONT_CULL_NATIVE", "-I", str(directory), *B.INCS, "-c", str(ROOT / "tools/ps2/hw_front_cull_hosttest.c"), "-o", str(obj)]
        link = [str(B.CC), *B.LDFLAGS, str(obj), "-o", str(elf), "-ldebug", "-lpatches", "-lm"]
        (directory / "compile-command.json").write_text(json.dumps([command, link], indent=2) + "\n")
        compiled = run(command, env=B.ENV)
        if compiled.returncode:
            return 1
        linked = run(link, env=B.ENV)
        (directory / "compile.log").write_text(compiled.stdout + compiled.stderr + linked.stdout + linked.stderr)
        if linked.returncode:
            return 1
        result = run([shutil.which("python3"), str(ROOT / "tools/ps2/run_pcsx2.py"), "--elf", str(elf), "--log", str(directory / "run.log"), "--until", "EETEST DONE", "--timeout", "180"])
        log = (directory / "run.log").read_text(errors="replace") if (directory / "run.log").exists() else ""
        (directory / "results.txt").write_text("\n".join(line for line in log.splitlines() if "EETEST" in line or "FAIL" in line or "PASS queries" in line) + "\n")
        print((directory / "results.txt").read_text())
        if result.returncode or "FAIL" in log or "EETEST DONE" not in log:
            return 1
        report["ee"] = {"elf_sha256": hashlib.sha256(elf.read_bytes()).hexdigest(), "log": str(directory / "run.log"), "passed": True}
    report["passed"] = True
    (work / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
