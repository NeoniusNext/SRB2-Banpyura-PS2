"""Build the unchanged HW_TEST fixture with current test-only engine hooks.

Use --reference-ref to compile a snapshot of the driver's tracked HW files,
without changing the working tree. Builds are serial and do not run PCSX2.
"""
import argparse
import hashlib
import io
import json
import subprocess
import tarfile
from pathlib import Path

import build as B


def source_root(out, reference):
    if not reference:
        return B.ROOT
    root = out / "source"
    src = root / "src"
    ps2 = src / "ps2"
    ps2.mkdir(parents=True, exist_ok=True)
    for child in (B.ROOT / "src").iterdir():
        dest = src / child.name
        if child.name != "ps2" and not dest.exists():
            dest.symlink_to(child, target_is_directory=child.is_dir())
    for child in (B.ROOT / "src/ps2").iterdir():
        dest = ps2 / child.name
        if child.name != "hw" and not dest.exists():
            dest.symlink_to(child, target_is_directory=child.is_dir())
    archive = subprocess.run(["git", "archive", reference, "src/ps2/hw"],
                             cwd=B.ROOT, capture_output=True, check=True).stdout
    with tarfile.open(fileobj=io.BytesIO(archive)) as stream:
        for member in stream.getmembers():
            if member.name.startswith("/") or ".." in Path(member.name).parts:
                raise RuntimeError("Invalid source snapshot path")
        stream.extractall(root, filter="data")
    return root


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--reference-ref")
    args = ap.parse_args()
    out = B.OUT.resolve()
    obj = out / "hwt"
    obj.mkdir(parents=True, exist_ok=True)
    B.gen_config()
    root = source_root(out, args.reference_ref)
    flags = list(dict.fromkeys([f for f in B.CFLAGS if f != "-DNOHW"] + ["-DHWRENDER"]))
    fixture = (B.ROOT / "tools/ps2/hw_test.c").read_text()
    fixture = fixture.replace('#include "hw_test_ext.inc"',
                              '#include "' + str(B.ROOT / "tools/ps2/hw_test_ext.inc") + '"\n'
                              '#include "' + str(B.ROOT / "tools/ps2/gs_list_ee_bench.inc") + '"')
    if fixture.count("\t\ttest_models();") != 1:
        raise RuntimeError("Missing model-test insertion point")
    fixture = fixture.replace("\t\ttest_models();", "\t\ttest_models();\n\t\ttest_gs_list_model_perf();")
    fixture_path = obj / "hw_test.c"
    fixture_path.write_text(fixture)
    sources = [fixture_path, root / "src/ps2/hw/ps2_hwd.c",
               B.ROOT / "tools/ps2/gs_list_ee_stubs.c"]
    commands, objects, logs = [], [], []
    for source in sources:
        target = obj / (source.stem + ".o")
        command = [str(B.CC), *flags, *B.INCS, "-c", str(source), "-o", str(target)]
        commands.append(command)
        result = subprocess.run(command, cwd=B.ROOT, env=B.ENV, text=True, capture_output=True)
        logs.append(f"{source}: exit={result.returncode}\n{result.stdout}{result.stderr}")
        (obj / "build.log").write_text("\n".join(logs))
        if result.returncode or result.stdout.strip() or result.stderr.strip():
            print(logs[-1])
            return 1
        objects.append(str(target))
    for name in ("ps2_hw_vu1", "ps2_hw_vu1_spr"):
        source = root / f"src/ps2/hw/vu1/{name}.vsm"
        target = obj / f"{name}_vsm.o"
        command = [str(B.DEV / ("dvp/bin/dvp-as" + B.EXE)), str(source), "-o", str(target)]
        commands.append(command)
        subprocess.run(command, env=B.ENV, cwd=B.ROOT, check=True)
        objects.append(str(target))
    elf = out / "HW_TEST.ELF"
    command = [str(B.CC), *B.LDFLAGS, *objects, "-o", str(elf),
               "-Wl,-Map=" + str(obj / "hw_test.map"), "-lgskit", "-ldmakit", "-ldebug", "-lpatches", "-lm"]
    commands.append(command)
    result = subprocess.run(command, env=B.ENV, cwd=B.ROOT, text=True, capture_output=True)
    output = result.stdout + result.stderr
    (obj / "link.log").write_text(output)
    diagnostics = [line for line in output.splitlines() if line.strip()
                   and "linking abicalls files with non-abicalls files" not in line]
    if result.returncode or diagnostics:
        print(output)
        return 1
    inputs = sources + list((root / "src/ps2/hw").glob("*.inc"))
    inputs += list((root / "src/ps2/hw").glob("*.h"))
    inputs += list((root / "src/ps2/hw/vu1").glob("*.vsm"))
    inputs += [Path(__file__), B.ROOT / "tools/ps2/gs_list_ee_bench.inc",
               B.ROOT / "tools/ps2/hw_test_ext.inc"]
    (obj / "build-report.json").write_text(json.dumps({
        "commands": commands, "reference_ref": args.reference_ref,
        "elf_sha256": hashlib.sha256(elf.read_bytes()).hexdigest(),
        "elf_bytes": elf.stat().st_size,
        "compile_diagnostics": 0, "link_diagnostics_excluding_sdk_abi_warning": 0,
        "input_sha256": {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs},
    }, indent=2))
    print(f"Built {elf} ({elf.stat().st_size} bytes); compile diagnostics=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
