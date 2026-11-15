"""Archive immutable inputs and complete PCSX2 settings around one official run.

This module never launches the emulator directly or changes its configuration.
Callers pass opt_run.py/run_hw_test.py commands, which retain their own locks.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

from next10_gs_run import critical_settings

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def snapshot(out, label):
    profile = Path(os.environ.get('SRB2_PCSX2_ROOT', '/workspace/.onboarding/pcsx2')).resolve()
    configs = sorted(profile.glob('slot*/**/*.ini'))
    runtimes = sorted(p for p in profile.glob('slot*/usr/bin/pcsx2-qt') if p.is_file())
    if not configs or not runtimes or not any(p.name == 'PCSX2.ini' for p in configs):
        raise RuntimeError('Complete standard PCSX2 profile/binary not found')
    for path in configs:
        dest = out / ('config-' + label) / path.relative_to(profile)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, dest)
    return {'config_sha256': {str(p): sha(p) for p in configs},
            'critical_settings': {str(p): critical_settings(p) for p in configs},
            'emulator_binary_sha256': {str(p): sha(p) for p in runtimes}}


def run_verified(command, out, inputs=()):
    """Run a wrapper command once; pin all non-UI settings and input bytes."""
    out = Path(out).resolve()
    if not out.is_relative_to(ROOT / 'build') or out == ROOT / 'build':
        raise RuntimeError('Runtime evidence must be under repository build/')
    if (out / 'run-pre.json').exists():
        raise RuntimeError('Runtime evidence exists; choose a fresh destination')
    out.mkdir(parents=True, exist_ok=True)
    files = {Path(p).resolve() for p in inputs}
    files.update([Path(__file__).resolve(), ROOT / 'tools/ps2/next10_gs_run.py',
                  ROOT / 'tools/ps2/opt_run.py', ROOT / 'tools/ps2/run_pcsx2.py',
                  Path('/workspace/.onboarding/emulator-env.sh')])
    before = {str(p): sha(p) for p in sorted(files)}
    cfg_before = snapshot(out, 'before')
    pre = {'command': list(map(str, command)), 'input_sha256': before, **cfg_before,
           'environment': {k: v for k, v in os.environ.items()
                           if k.startswith(('PCSX2', 'SRB2_PCSX2'))}}
    (out / 'run-pre.json').write_text(json.dumps(pre, indent=2) + '\n')
    process = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
    (out / 'orchestration.log').write_text(process.stdout + process.stderr)
    after = {str(p): sha(p) for p in sorted(files)}
    cfg_after = snapshot(out, 'after')
    post = {'command': pre['command'], 'rc': process.returncode, 'input_sha256': after,
            'all_frozen_inputs_unchanged': before == after,
            'all_non_UI_settings_unchanged': cfg_before['critical_settings'] == cfg_after['critical_settings'],
            'emulator_binary_unchanged': cfg_before['emulator_binary_sha256'] == cfg_after['emulator_binary_sha256'],
            'orchestration_sha256': sha(out / 'orchestration.log'), **cfg_after}
    (out / 'run-post.json').write_text(json.dumps(post, indent=2) + '\n')
    if not all(post[k] for k in ('all_frozen_inputs_unchanged', 'all_non_UI_settings_unchanged', 'emulator_binary_unchanged')):
        raise RuntimeError('Runtime inputs, critical configuration or emulator changed')
    return process
