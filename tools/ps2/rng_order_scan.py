"""List every C statement with two or more calls of the network-synced RNG (P_Random*, P_SignedRandom, P_RandomChance) that may be evaluated in an
unspecified order (OPT10-S): function arguments and operands of arithmetic/comparison/bitwise operators are not sequenced in C. The RNG state advances
identically either way, but the values land in different places: MIPS GCC (PS2) evaluates arguments left to right, x86 GCC mostly right to left, so the
two builds can disagree about WHICH random value goes where (DEMO_003 state_hash, A_LightBeamReset).

usage: python tools/ps2/rng_order_scan.py [--all] [src ...]   (default: src/*.c, src/**/*.c except hardware/ and lua)
Statements whose calls are separated by a sequence point (&&, ||, ?:, the comma operator at paren depth 0, a statement boundary) are not reported
unless --all. Each hit prints file:line and the statement collapsed to one line; "UNSEQ" marks the ones with no separating sequence point.
"""
import re
import sys
from pathlib import Path

RNG = re.compile(r'\b(P_Random[A-Za-z]*|P_SignedRandom|P_RandomChance|M_Random[A-Za-z]*|M_SignedRandom)\s*\(')
SYNCED = re.compile(r'\b(P_Random(?!Peek)[A-Za-z]*|P_SignedRandom)\s*\(')


def strip(src):
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith('//', i):
            while i < n and src[i] != '\n':
                i += 1
        elif src.startswith('/*', i):
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append('\n' * src.count('\n', i, j))
            i = j
        elif c == '"' or c == "'":
            q = c
            i += 1
            while i < n and src[i] != q:
                i += 2 if src[i] == '\\' else 1
            i += 1
            out.append('""' if q == '"' else "'x'")
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def statements(text):
    """(line, statement) split at ';' '{' '}' outside parentheses."""
    depth = 0
    start = 0
    line = 1
    startline = 1
    for i, c in enumerate(text):
        if c == '\n':
            line += 1
        if c == '(':
            depth += 1
        elif c == ')':
            depth = max(0, depth - 1)
        elif depth == 0 and c in ';{}':
            s = text[start:i + 1]
            lead = len(s) - len(s.lstrip('\n \t'))
            yield startline + s[:lead].count('\n'), s.strip()
            start = i + 1
            startline = line
    return


def has_seq_between(stmt, spans):
    """True when every pair of consecutive RNG calls is separated by &&, ||, ?:, or a depth-0 comma (relative to the call)."""
    for (a_end, b_start) in zip([s[1] for s in spans[:-1]], [s[0] for s in spans[1:]]):
        between = stmt[a_end:b_start]
        d = 0
        ok = False
        for k, ch in enumerate(between):
            if ch == '(':
                d += 1
            elif ch == ')':
                d -= 1
            elif d == 0 and (between.startswith('&&', k) or between.startswith('||', k) or ch in '?:'):
                ok = True
            elif d == 0 and ch == ',':
                ok = False  # argument separator, not a sequence point (the comma operator is vanishingly rare here)
        if not ok:
            return False
    return True


def call_spans(stmt):
    spans = []
    for m in SYNCED.finditer(stmt):
        d = 0
        j = m.end() - 1
        while j < len(stmt):
            if stmt[j] == '(':
                d += 1
            elif stmt[j] == ')':
                d -= 1
                if d == 0:
                    break
            j += 1
        spans.append((m.start(), j + 1))
    return spans


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    show_all = '--all' in sys.argv
    root = Path(__file__).resolve().parents[2]
    files = [Path(a) for a in args] if args else sorted(p for p in (root / 'src').rglob('*.c') if 'hardware' not in p.parts and 'ps2' not in p.parts)
    hits = 0
    for f in files:
        try:
            text = strip(f.read_text(errors='replace'))
        except OSError:
            continue
        for line, st in statements(text):
            spans = call_spans(st)
            if len(spans) < 2:
                continue
            seq = has_seq_between(st, spans)
            if seq and not show_all:
                continue
            hits += 1
            print(f"{f.relative_to(root) if f.is_absolute() else f}:{line}: {'seq ' if seq else 'UNSEQ'} {re.sub(chr(10) + '|\\s+', ' ', st)[:400]}")
    print(f"{hits} statement(s)")


if __name__ == '__main__':
    main()
