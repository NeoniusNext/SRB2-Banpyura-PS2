"""Reject incomplete/failed profiling runs without starting PCSX2."""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import opt_run
import perf_phases


def profile_line():
    return ('PROF win=1 frames=105 tics=105 total=1100 ' +
            ' '.join(f'{p}=100/105' for p in perf_phases.PHASES) + '\n')


class ProfileGates(unittest.TestCase):
    def test_uart_prefix(self):
        with tempfile.TemporaryDirectory() as directory:
            boot = Path(directory) / 'boot.txt'
            boot.write_text(profile_line() + '[EE] ' + profile_line())
            rows = perf_phases.parse(boot)
            self.assertEqual(len(rows), 2)
            self.assertEqual(rows[0], rows[1])
            self.assertEqual(perf_phases.summarise(rows, 1)['frames'], 210)

    def test_perf_exit_status(self):
        for rc, has_profile, scenarios, expected in (
            (0, True, 'DEMO_001', 0),
            (3, True, 'DEMO_001', 1),  # Golden mismatch with plausible timings.
            (0, False, 'DEMO_001', 1),
            (0, True, '', 1),
        ):
            with self.subTest(rc=rc, profile=has_profile, scenarios=scenarios):
                with tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    def runner(*args, **kwargs):
                        (root / 'candidate' / 'boot.txt').write_text(profile_line() if has_profile else '')
                        return subprocess.CompletedProcess([], rc, '', '')
                    argv = ['perf_phases.py', '--variant', 'candidate', '--out', directory,
                            '--scenarios', scenarios, '--no-build']
                    with patch.object(sys, 'argv', argv), patch.object(perf_phases.subprocess, 'run', runner), contextlib.redirect_stdout(io.StringIO()):
                        self.assertEqual(perf_phases.main(), expected)
                    report = json.loads((root / 'candidate' / 'perf.json').read_text())
                    self.assertEqual(report['passed'], expected == 0)

    def test_engine_completion(self):
        for complete, errors, until, expected in (
            (True, '', '', 0),
            (False, '', '', 4),
            (True, 'I_Error(): Out of memory allocating 71680 bytes\n', '', 4),
            (True, '', 'ZQUIT DONE', 4),
            (False, 'ZQUIT DONE\n', 'ZQUIT DONE', 0),  # Explicit bounded run.
        ):
            with self.subTest(complete=complete, errors=errors, until=until):
                with tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    ref = root / 'refout'
                    ref.mkdir()
                    if complete:
                        (ref / 'complete.txt').write_text('complete tics=1050\n')
                    def runner(*args, **kwargs):
                        (root / 'candidate' / 'boot.txt').write_text(errors)
                        return subprocess.CompletedProcess([], 0, '', '')
                    argv = ['opt_run.py', '--name', 'candidate', '--elf', str(root / 'SRB2.ELF'),
                            '--out', directory, '--demo', 'DEMO_001']
                    if until:
                        argv += ['--until', until]
                    with patch.object(sys, 'argv', argv), patch.object(opt_run, 'stage', return_value=ref), patch.object(opt_run.subprocess, 'run', runner), contextlib.redirect_stdout(io.StringIO()):
                        (root / 'candidate').mkdir()
                        self.assertEqual(opt_run.main(), expected)
                    report = json.loads((root / 'candidate' / 'summary.json').read_text())
                    self.assertEqual(report['rc'], expected)
                    self.assertEqual(report['ref_complete'], complete)


if __name__ == '__main__':
    unittest.main()
