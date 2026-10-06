"""Audit a native host-profile CMake project, engine PE imports and host audio DLL graph.

No engine changes or DLL copies. Records direct dependencies separately from
shared SDL_mixer/libopenmpt transitive imports (zlib is allowed only there).
"""
import argparse
import hashlib
import json
import re
import subprocess
import xml.etree.ElementTree as ET
from collections import Counter
from pathlib import Path

VCVARS = Path(r'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat')


def sha(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', required=True, type=Path)
    ap.add_argument('--exe', required=True, type=Path)
    ap.add_argument('--dependencies', type=Path, default=Path('D:/AI-projects/SRB2B-plus/build/deps/vcpkg_installed'))
    ap.add_argument('--out', required=True, type=Path)
    a = ap.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    exe = a.exe.resolve()
    depbin = a.dependencies.resolve() / 'x64-windows/bin'
    project = a.build.resolve() / 'src/SRB2SDL2.vcxproj'
    text = project.read_text(encoding='utf-8-sig')
    xml = ET.fromstring(text)
    ns = {'m': 'http://schemas.microsoft.com/developer/msbuild/2003'}
    sources = [e.attrib['Include'] for e in xml.findall('.//m:ClCompile', ns) if 'Include' in e.attrib]
    definitions = sorted({d for e in xml.findall('.//m:PreprocessorDefinitions', ns) for d in (e.text or '').split(';')})
    libraries = sorted({d for e in xml.findall('.//m:AdditionalDependencies', ns) for d in (e.text or '').split(';')})
    bad = [d for d in definitions if d in ('HAVE_PNG', 'HAVE_ZLIB', 'HAVE_CURL')]
    bad += [s for s in sources if Path(s.replace('\\', '/')).name.lower() == 'apng.c']
    bad += [lib for lib in libraries if re.search(r'(?:^|[/\\])(?:lib)?(?:png\w*|zlib\w*|z|curl\w*)\.lib$', lib, re.I)]
    inputs = {str(exe): sha(exe), str(project): sha(project)}
    graph, commands = {}, []

    def imports(path):
        commands.append(['dumpbin', '/nologo', '/dependents', str(path)])
        bat = out / 'dumpbin.bat'
        bat.write_text(f'@echo off\ncall "{VCVARS}" >nul 2>&1\nif errorlevel 1 exit /b 1\n'
                       + subprocess.list2cmdline(commands[-1]) + '\n', encoding='utf-8')
        p = subprocess.run(['cmd', '/c', str(bat)], cwd=out, capture_output=True, text=True,
                           encoding='oem', errors='replace')
        (out / (path.name + '.imports.log')).write_text(p.stdout + p.stderr, encoding='utf-8')
        if p.returncode:
            raise RuntimeError(f'dumpbin failed ({p.returncode}): {p.stdout}{p.stderr}')
        return sorted(set(re.findall(r'^\s+(\S+\.dll)\s*$', p.stdout, re.M | re.I)))

    direct = imports(exe)
    if not direct:
        raise RuntimeError('empty engine import list')
    bad += [d for d in direct if re.search(r'^(?:lib)?(?:png|zlib|curl)', d, re.I)]
    todo = list(direct)
    while todo:
        name = todo.pop()
        if name in graph or not (depbin / name).is_file():
            continue
        dll = depbin / name
        inputs[str(dll)] = sha(dll)
        graph[name] = imports(dll)
        todo += graph[name]

    warnings = Counter()
    errors = 0
    for log in sorted(a.build.glob('build-*.log')):
        data = log.read_text(encoding='utf-8-sig', errors='replace')
        warnings.update(re.findall(r'\bwarning ([CD]\d+)', data))
        errors += len(re.findall(r'\berror (?:[A-Z]+\d+|:)', data))
    changed = [p for p, digest in inputs.items() if sha(Path(p)) != digest]
    report = dict(engine_bytes=exe.stat().st_size, engine_sha256=inputs[str(exe)], direct_imports=direct,
                  sources=len(sources), forbidden_project_or_direct_imports=bad,
                  libraries=libraries, definitions=definitions, host_dll_import_graph=graph,
                  build_warning_counts=dict(warnings), build_errors=errors, input_sha256=inputs, inputs_changed=changed)
    (out / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    (out / 'commands.json').write_text(json.dumps(commands, indent=2), encoding='utf-8')
    lines = [f'engine {exe} {exe.stat().st_size} bytes SHA256 {inputs[str(exe)]}',
             f'engine sources: {len(sources)}, forbidden project/direct imports: {len(bad)}',
             'direct engine libraries: ' + '; '.join(libraries), 'direct engine DLLs: ' + ', '.join(direct),
             f'host dependency DLLs followed: {len(graph)}',
             'transitive zlib importers: ' + ', '.join(n for n, ds in graph.items() if any('zlib' in d.lower() for d in ds)),
             'MSVC warning counts (all build logs): ' + str(dict(warnings)), f'build errors: {errors}',
             f'input preservation: {len(inputs)} files hashed, {len(changed)} changed']
    lines += ['FORBIDDEN ' + n for n in bad]
    (out / 'test.log').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print('\n'.join(lines), flush=True)
    return int(bool(bad or changed or errors))


if __name__ == '__main__':
    raise SystemExit(main())
