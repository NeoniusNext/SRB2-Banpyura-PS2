"""Compile only owned units; audit profile guards and non-profile equivalence.

No linking or emulator use. --out must be an isolated directory. Saves the actual
commands, preprocessed units, objects, diagnostics, symbol lists and JSON summary.
Audits external entry points read-only; exit 1 if any forbidden path remains.
"""
import argparse
import difflib
import hashlib
import json
import os
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OWNED = ['src/filesrch.c', 'src/m_menu.c', 'src/m_misc.c', 'src/netcode/d_netcmd.c',
         'src/p_setup.c', 'src/r_picformats.c', 'src/w_wad.c', 'src/d_main.c',
         'src/netcode/d_clisrv.c', 'src/netcode/commands.c', 'src/netcode/gamestate.c']
HEADERS = ['src/filesrch.h', 'src/p_setup.h', 'src/r_picformats.h', 'src/w_wad.h']
EXTERNAL = ['src/netcode/client_connection.c', 'src/g_demo.c', 'src/netcode/d_net.c']
FORBIDDEN_COMMANDS = {'addfile', 'addfilelocal', 'addfolder', 'addfolderlocal', 'listwad',
                      'saveaddons', 'runsoc', 'password', 'clearpassword', 'login',
                      'promote', 'demote', 'motd', 'connect', 'kick', 'ban', 'banip',
                      'clearbans', 'showbanlist', 'reloadbans', 'downloads', 'ping',
                      'serverchangeteam', 'muteplayer', 'unmuteplayer', 'clearscores',
                      'set_http_login', 'list_http_logins', 'resendgamestate'}
STUBS = {'P_AddWadFile': '(void)wadfilename;returnfalse;', 'D_SetPassword': '(void)pw;',
         'D_ClearPassword': '', 'M_SortServerList': '', 'M_LoadJoinedIPs': '',
         'M_SaveJoinedIPs': '', 'M_AddToJoinedIPs': '(void)address;(void)date;(void)servname;',
         'M_RemoveJoinedIP': '(void)index;returnfalse;'}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def bodies(text):
    # Declarations have a semicolon; skip string literals so diagnostic text is not code.
    text = re.sub(r'"(?:\\.|[^"\\])*"', '""', text)
    text = re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)
    for m in re.finditer(r'\b(\w+)\s*\([^;{}]*\)\s*\{', text):
        depth, end = 1, m.end()
        while depth and end < len(text):
            depth += (text[end] == '{') - (text[end] == '}')
            end += 1
        yield m.group(1), text[m.end():end - 1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--legacy-library-defines', action='store_true', help='also inject HAVE_PNG/HAVE_ZLIB to test guards')
    a = ap.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    os.environ['SRB2_PS2_OUT'] = str(out)
    import build as b
    import strip_check as sc
    if a.legacy_library_defines:
        b.CFLAGS += ['-DHAVE_PNG', '-DHAVE_ZLIB']
    b.OBJ.mkdir(parents=True, exist_ok=True)
    b.gen_config()
    messages, commands, results = [], [], {}

    def log(message):
        print(message, flush=True)
        messages.append(message)
        (out / 'test.log').write_text('\n'.join(messages) + '\n', encoding='utf-8')

    def run(cmd, dest):
        if cmd[0] == str(b.CC) and '-MMD' in cmd:
            cmd = cmd + ['-MF', str(dest.with_suffix('.d'))]
        commands.append(cmd)
        p = subprocess.run(cmd, cwd=out, env=b.ENV, capture_output=True, text=True)
        dest.write_text(p.stdout, encoding='utf-8')
        dest.with_suffix(dest.suffix + '.stderr').write_text(p.stderr, encoding='utf-8')
        if p.returncode:
            raise RuntimeError(f'command failed ({p.returncode}): {cmd}\n{p.stderr}')
        return p.stdout, p.stderr

    before = {rel: sha(ROOT / rel) for rel in OWNED + HEADERS + EXTERNAL}
    owned_bad, external_bad, diagnostics, pc_bad = 0, 0, 0, 0
    for rel in OWNED + EXTERNAL:
        name = rel.replace('/', '__')
        text, stderr = run([str(b.CC)] + b.CFLAGS + b.INCS + ['-E', '-P', str(ROOT / rel)], out / (name + '.i'))
        diagnostics += int(bool(stderr.strip()))
        defined = dict(bodies(text))
        bad = [n for n in defined if any(n.startswith(prefix) for prefix in sc.FORBIDDEN_PREFIX)]
        registrations = re.findall(r'COM_AddCommand\s*\(\s*"([^"\n]+)"', text)
        bad += ['command:' + n for n in registrations if n in FORBIDDEN_COMMANDS]
        if rel == 'src/d_main.c':
            for option in ('-password', '-server', '-connect', '-dedicated', '-room', '-noupload', '-splitscreen'):
                if re.search(r'M_CheckParm\s*\(\s*"' + re.escape(option) + r'"', text):
                    bad.append('startup-option:' + option)
            if re.search(r'M_GetUrlProtocolArg\s*\(\s*\)\s*\|\|', text):
                bad.append('startup:URL-connect')
        stubs = {}
        for n, want in STUBS.items():
            if n in defined:
                good = re.sub(r'\s+', '', defined[n]) == want
                stubs[n] = good
                if not good:
                    bad.append('nonempty-compatibility-stub:' + n)
        if rel in OWNED:
            _, stderr = run([str(b.CC)] + b.CFLAGS + b.INCS + ['-c', str(ROOT / rel), '-o', str(b.obj_for(rel))],
                            out / (name + '.compile.stdout'))
            diagnostics += int(bool(stderr.strip()))
            syms = sc.nm(b.obj_for(rel))
            (out / (name + '.nm')).write_text('\n'.join(' '.join(s) for s in syms) + '\n', encoding='utf-8')
            bad += ['symbol:' + n for _, _, n in sc.forbidden(syms)]
            undefined, _ = run([str(sc.NM), '--undefined-only', str(b.obj_for(rel))], out / (name + '.undefined.nm'))
            for n in re.findall(r'\bU\s+(\w+)', undefined):
                if any(n.startswith(prefix) for prefix in sc.FORBIDDEN_PREFIX):
                    bad.append('undefined-forbidden-reference:' + n)
            owned_bad += len(bad)
        else:
            external_bad += len(bad)
        results[rel] = dict(forbidden=bad, compatibility_stubs=stubs, registered_commands=registrations)
        log(f'{rel}: {len(bad)} forbidden definitions/registrations/symbols; stubs {stubs}')
        for item in bad:
            log('  BLOCKER ' + item)

    # HEAD versions of owned files/headers; all unowned includes remain the same working tree.
    # Quote includes resolve through this overlay first, then ROOT/src. No git state is changed.
    head = out / 'pc-head'
    for rel in OWNED + HEADERS:
        p = head / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(subprocess.run(['git', 'show', 'HEAD:' + rel], cwd=ROOT, capture_output=True, check=True).stdout)
    pcflags = [f for f in b.CFLAGS if f not in ('-DPS2', '-DPS2_PROFILE')]
    pcflags += ['-DHAVE_PNG', '-DHAVE_ZLIB']
    for rel in OWNED:
        name = rel.replace('/', '__')
        current, _ = run([str(b.CC)] + pcflags + b.INCS + ['-E', '-P', str(ROOT / rel)], out / (name + '.pc-current.i'))
        original, _ = run([str(b.CC)] + pcflags + ['-I' + str(head / 'src'), '-I' + str(ROOT / 'src/netcode')] + b.INCS +
                          ['-E', '-P', str(head / rel)], out / (name + '.pc-head.i'))
        # Only diagnostic source paths/line numbers differ after insertion of guards
        # (asserts and Z_*2 allocation tracking macros).
        def normalize(t):
            t = re.sub(r'"[^"\n]*[\\/]src[\\/][^"\n]+\.(?:c|h)"', '"SOURCE"', t)
            t = re.sub(r'("SOURCE"\s*,)\s*\d+', r'\1 LINE', t)
            # Tag_* iteration macros use __LINE__ to create local counter names.
            ids = {}
            def counter(m):
                return ids.setdefault(m[0], 'ICNT_CANON_' + str(len(ids)))
            return re.sub(r'\bICNT_\d+\b', counter, t)
        same = normalize(current) == normalize(original)
        delta = ''.join(difflib.unified_diff(normalize(original).splitlines(True), normalize(current).splitlines(True),
                                           fromfile='HEAD', tofile='current'))
        (out / (name + '.pc.diff')).write_text(delta, encoding='utf-8')
        pc_bad += int(not same)
        results[rel]['nonprofile_equal'] = same
        log(f'non-profile HAVE_PNG/HAVE_ZLIB {rel}: {"equal" if same else "DIFF (see .pc.diff)"}')

    transport = (ROOT / 'src/netcode/d_net.c').read_bytes()
    original = subprocess.run(['git', 'show', 'HEAD:src/netcode/d_net.c'], cwd=ROOT, capture_output=True, check=True).stdout
    intact = transport.replace(b'\r\n', b'\n') == original.replace(b'\r\n', b'\n')
    preserved = {}
    for rel, names in [('src/netcode/d_clisrv.c', ('SV_StartSinglePlayerServer', 'SV_ResetServer', 'SV_SpawnServer', 'Got_AddPlayer')),
                       ('src/netcode/d_netcmd.c', ('D_MapChange', 'SendNameAndColor2'))]:
        current = dict(bodies((ROOT / rel).read_text(encoding='utf-8')))
        original = dict(bodies(subprocess.run(['git', 'show', 'HEAD:' + rel], cwd=ROOT, capture_output=True,
                                             check=True).stdout.decode('utf-8')))
        for name in names:
            same = name in current and name in original and current[name] == original[name]
            (out / (name + '.local.diff')).write_text(''.join(difflib.unified_diff(
                original.get(name, '').splitlines(True), current.get(name, '').splitlines(True),
                fromfile='HEAD', tofile='current')), encoding='utf-8')
            preserved[name] = same
            log(f'local rebound/Tails body {name}: {"equal HEAD" if same else "DIFF"}')
    changed = [rel for rel, digest in before.items() if sha(ROOT / rel) != digest]
    summary = dict(owned_forbidden=owned_bad, external_forbidden=external_bad, diagnostic_units=diagnostics,
                   nonprofile_different_units=pc_bad, rebound_source_equal_HEAD=intact,
                   changed_inputs=changed, input_sha256=before, files=results, preserved_local_bodies=preserved)
    (out / 'commands.json').write_text(json.dumps(commands, indent=2), encoding='utf-8')
    (out / 'report.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    log(f'SUMMARY owned={owned_bad}, external={external_bad}, diagnostics={diagnostics}, nonprofile_diff={pc_bad}, '
        f'rebound_equal_HEAD={intact}, changed_inputs={len(changed)}')
    return int(bool(owned_bad or external_bad or diagnostics or pc_bad or not intact or changed or not all(preserved.values())))


if __name__ == '__main__':
    raise SystemExit(main())
