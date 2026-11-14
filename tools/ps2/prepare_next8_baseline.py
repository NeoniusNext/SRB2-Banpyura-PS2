"""Recreate the frozen preceding pass in an isolated build directory.

Only exports Git source and applies the frozen source patch there. The live
checkout, index, dependencies and user files are not changed.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/ps2'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, default=ROOT / 'build/nov15-next8/baseline-src')
    a = ap.parse_args()
    out = a.out.resolve()
    if not out.is_relative_to(ROOT / 'build') or out == ROOT / 'build':
        ap.error('--out must be a child of the repository build directory')
    manifest = json.loads((TOOLS / 'next8_baseline.json').read_text())
    patch = TOOLS / 'next8_baseline.patch'
    if hashlib.sha256(patch.read_bytes()).hexdigest() != manifest['patch_sha256']:
        raise RuntimeError('Frozen baseline patch checksum differs')

    def correct():
        return all((out / p).is_file() and hashlib.sha256((out / p).read_bytes()).hexdigest() == h
                   for p, h in manifest['source_sha256'].items())

    if out.exists() and any(out.iterdir()):
        if not correct():
            raise RuntimeError('Existing baseline differs; choose a new empty --out directory')
        print('Verified existing baseline:', out)
        return
    out.mkdir(parents=True, exist_ok=True)
    archive = subprocess.check_output(['git', 'archive', manifest['revision'], 'src', 'tools/ps2'], cwd=ROOT)
    with tarfile.open(fileobj=io.BytesIO(archive)) as stream:
        stream.extractall(out, filter='data')
    if patch.stat().st_size:
        subprocess.run(['git', 'apply', '--unidiff-zero', '--directory=' + out.relative_to(ROOT).as_posix(), str(patch)], cwd=ROOT, check=True)
    if not correct():
        raise RuntimeError('Recreated source does not match the preceding pass')
    print('Created and verified baseline:', out)


if __name__ == '__main__':
    main()
