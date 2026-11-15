#!/usr/bin/env python3
"""Freeze a PB vertex-zero reduction candidate; never edit production source."""
import argparse
import difflib
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[2]
REVISION = 'd4fbc7c319f19edc70eda220c3323d0350504c98'
SOURCE = 'src/hardware/hw_pbatch.inc'
BASE_SHA = '08e3ce0c810ba69f957ab8aaa0082e28bcdb1e3f7a3939f8d0da1e8133e2e350'
WORK = ROOT / 'build/nov15-next13'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def baseline_files():
    raw = subprocess.check_output(['git', 'archive', '--format=tar', REVISION, 'src'], cwd=ROOT)
    result = {}
    with tarfile.open(fileobj=io.BytesIO(raw), mode='r:') as archive:
        for member in archive:
            if member.isdir():
                continue
            if not member.isfile() or not member.name.startswith('src/') or '..' in Path(member.name).parts:
                raise RuntimeError('Unexpected source archive entry')
            result[member.name] = archive.extractfile(member).read()
    if len(result) != 602 or sha(result[SOURCE]) != BASE_SHA:
        raise RuntimeError('Canonical source or PB baseline differs')
    return result


def rewrite(text, version='v1'):
    start = text.index('static inline __attribute__((always_inline)) void HWR_PBPut(')
    end = text.index('// HWR_ProcessPolygon of a batched polygon', start)
    body = text[start:end]
    if version == 'v2':
        anchor = '\t\ttmin = tmax = hwr_i2f(w[4]);\n'
        if body.count(anchor) != 1:
            raise RuntimeError('Extrema initialization anchor differs')
        body = body.replace(anchor, anchor +
            '\t\t// Every caller rejects n < 3 before collection. Vertex zero already\n'
            '\t\t// initialized the extrema; retain its exact five-word copy only.\n'
            '\t\tHWR_PBC(0);\n', 1)
        if body.count('\t\t\tHWR_PBV(0);\n') != 2:
            raise RuntimeError('Specialized vertex-zero anchors differ')
        body = body.replace('\t\t\tHWR_PBV(0);\n', '')
        anchor = '\t\t\tfor (k = 0; k < n; k++)\n\t\t\t\tHWR_PBV(k);'
        if body.count(anchor) != 1:
            raise RuntimeError('General reduction anchor differs')
        body = body.replace(anchor, '\t\t\tfor (k = 1; k < n; k++)\n\t\t\t\tHWR_PBV(k);', 1)
        return text[:start] + body + text[end:]
    if version != 'v1':
        raise RuntimeError('Unknown isolated variant')
    anchor = '\t\tif (n == 4)\n\t\t{\n\t\t\tHWR_PBV(0);'
    if body.count(anchor) != 1:
        raise RuntimeError('Four-vertex reduction anchor differs')
    body = body.replace(anchor, '\t\t// Vertex zero already initialized all four extrema; copy its five words.\n'
                        '\t\t// MIN.S/MAX.S select an operand unchanged, including identical bit patterns.\n'
                        '\t\tif (n == 4)\n\t\t{\n\t\t\tHWR_PBC(0);', 1)
    anchor = '\t\telse if (n == 3)\n\t\t{\n\t\t\tHWR_PBV(0);'
    if body.count(anchor) != 1:
        raise RuntimeError('Three-vertex reduction anchor differs')
    body = body.replace(anchor, '\t\telse if (n == 3)\n\t\t{\n\t\t\tHWR_PBC(0);', 1)
    anchor = '\t\t\tfor (k = 0; k < n; k++)\n\t\t\t\tHWR_PBV(k);'
    if body.count(anchor) != 1:
        raise RuntimeError('General reduction anchor differs')
    body = body.replace(anchor, '\t\t\tif (n)\n\t\t\t\tHWR_PBC(0);\n'
                        '\t\t\tfor (k = 1; k < n; k++)\n\t\t\t\tHWR_PBV(k);', 1)
    return text[:start] + body + text[end:]


def expected(candidate=False):
    result = baseline_files()
    if candidate:
        result[SOURCE] = rewrite(result[SOURCE].decode(), 'v1' if candidate is True else candidate).encode()
    return result


def validate(path, candidate=False):
    path = Path(path).resolve()
    pins = {name: sha(data) for name, data in expected(candidate).items()}
    actual = {p.relative_to(path).as_posix(): sha(p.read_bytes())
              for p in sorted((path / 'src').rglob('*')) if p.is_file()}
    if actual != pins:
        raise RuntimeError('Frozen complete source differs: ' + str(path))
    metadata = json.loads((path / 'manifest.json').read_text())
    if metadata['revision'] != REVISION or metadata['source_sha256'] != pins:
        raise RuntimeError('Frozen complete manifest differs: ' + str(path))
    return pins


def freeze(path, candidate=False):
    path = Path(path).resolve()
    if not path.is_relative_to(WORK) or path == WORK:
        raise RuntimeError('Only isolated next13 build children are allowed')
    data = expected(candidate)
    if path.exists():
        return validate(path, candidate)
    for name, raw in data.items():
        target = path / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(raw)
    pins = {name: sha(raw) for name, raw in data.items()}
    (path / 'manifest.json').write_text(json.dumps({
        'revision': REVISION, 'variant': 'pb-vertex-zero-copy' if candidate else 'baseline',
        'source_sha256': pins, 'production_src_edited': False}, indent=2) + '\n')
    return validate(path, candidate)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline', type=Path, default=WORK / 'geometry-baseline-v1')
    p.add_argument('--candidate', type=Path, default=WORK / 'geometry-source-v1')
    p.add_argument('--out', type=Path, default=WORK / 'geometry-proposal-v1')
    p.add_argument('--version', choices=('v1','v2'), default='v1')
    a = p.parse_args()
    base, cur = freeze(a.baseline), freeze(a.candidate, a.version)
    a.out.mkdir(parents=True, exist_ok=True)
    if (a.out / 'report.json').exists():
        raise RuntimeError('Preserve prior completed evidence; choose a fresh --out')
    old = (a.baseline / SOURCE).read_text()
    new = (a.candidate / SOURCE).read_text()
    patch = ''.join(difflib.unified_diff(old.splitlines(True), new.splitlines(True),
                                         fromfile='a/' + SOURCE, tofile='b/' + SOURCE))
    (a.out / 'candidate.patch').write_text(patch)
    changed = [name for name in base if base[name] != cur[name]]
    if changed != [SOURCE]:
        raise RuntimeError('Candidate changes source beyond PBPut')
    report = {'status': 'ISOLATED HOST CANDIDATE; no EE compiler/emulator/prod edit',
              'revision': REVISION, 'source_files': len(base), 'changed_files': changed,
              'baseline': str(a.baseline.resolve()), 'candidate': str(a.candidate.resolve()),
              'baseline_sha256': base[SOURCE], 'candidate_sha256': cur[SOURCE],
              'version': a.version,
              'patch_sha256': sha(patch.encode()), 'tool_sha256': sha(Path(__file__).read_bytes())}
    (a.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
