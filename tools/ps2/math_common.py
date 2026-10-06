"""Shared helpers of the math_* host tests: MSVC builds in an isolated work dir, HEAD snapshot of the original sources."""
import os
import re
import shutil
import subprocess
import sys
import tarfile
import io
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VCVARS = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat")


def head_snapshot(dest):
    """Extract `git archive HEAD src` into dest (original sources, no working-tree edits). Returns dest/src."""
    dest = Path(dest)
    marker = dest / '.head'
    head = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=ROOT, capture_output=True, text=True).stdout.strip()
    if marker.exists() and marker.read_text() == head:
        return dest / 'src'
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    data = subprocess.run(['git', 'archive', 'HEAD', 'src'], cwd=ROOT, capture_output=True, check=True).stdout
    with tarfile.open(fileobj=io.BytesIO(data)) as t:
        t.extractall(dest)
    marker.write_text(head)
    return dest / 'src'


def msvc_build(work, name, units, arch='x64', link_extra=(), cl_extra=()):
    """units: list of (source, object name, [extra cl args]). Returns the exe path; raises on error."""
    work = Path(work)
    work.mkdir(parents=True, exist_ok=True)
    lines = ['@echo off', f'call "{VCVARS}" {arch} >nul 2>&1', 'if errorlevel 1 exit /b 1']
    objs = []
    for source, obj, extra in units:
        objs.append(obj)
        cmd = ['cl', '/nologo', '/std:c17', '/O2', '/W3', '/DNDEBUG', '/D_CRT_SECURE_NO_WARNINGS', '/wd4005', '/wd4996',
               *cl_extra, *extra, '/c', str(source), '/Fo:' + obj]
        lines += [subprocess.list2cmdline(cmd), 'if errorlevel 1 exit /b 1']
    lines.append(subprocess.list2cmdline(['link', '/nologo', '/OUT:' + name + '.exe', *objs, *link_extra]))
    lines.append('if errorlevel 1 exit /b 1')
    bat = work / 'build.bat'
    bat.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    r = subprocess.run(['cmd', '/c', str(bat)], cwd=work, capture_output=True, text=True, encoding='oem', errors='replace')
    (work / 'build.log').write_text(r.stdout + r.stderr, encoding='utf-8')
    if r.returncode:
        print(r.stdout + r.stderr)
        raise RuntimeError(f'build failed: {work}')
    return work / (name + '.exe')


def run(exe, args=(), log=None, cwd=None, timeout=None):
    r = subprocess.run([str(exe), *args], capture_output=True, text=True, cwd=cwd, timeout=timeout)
    text = r.stdout + r.stderr
    if log:
        Path(log).write_text(text, encoding='utf-8')
    return r.returncode, text
