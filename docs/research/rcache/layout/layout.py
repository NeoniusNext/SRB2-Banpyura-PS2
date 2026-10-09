#!/usr/bin/env python3
"""OPT13-RCACHE: struct layouts (EE ABI) from the DWARF of probe.o (readelf text). usage: layout.py probe.o NAME... ; prints offset/size/64-byte line of every member and the holes.
No gdb/pyelftools needed (the ps2dev gdb is musl-linked and does not start here)."""
import re, subprocess, sys
import os
RE = os.environ.get('READELF', '/opt/ps2dev-x/ps2dev/ee/bin/mips64r5900el-ps2-elf-readelf')
PTR = 4

def load(obj):
    global PTR
    txt = subprocess.run([RE, '--debug-dump=info', obj], capture_output=True, text=True).stdout
    m = re.search(r'Pointer Size:\s+(\d+)', txt)
    if m: PTR = int(m.group(1))
    dies = {}; cur = None
    for line in txt.splitlines():
        m = re.match(r'\s*<(\d+)><([0-9a-f]+)>: Abbrev Number: \d+ \((DW_TAG_\w+)\)', line)
        if m:
            cur = {'tag': m.group(3), 'depth': int(m.group(1)), 'attrs': {}, 'kids': [], 'off': int(m.group(2), 16)}
            dies[cur['off']] = cur
            continue
        m = re.match(r'\s*<[0-9a-f]+>\s+(DW_AT_\w+)\s*:\s*(.*)$', line)
        if m and cur is not None:
            cur['attrs'][m.group(1)] = m.group(2).strip()
    # parent links by depth order
    stack = []
    for off in sorted(dies):
        d = dies[off]
        while stack and stack[-1]['depth'] >= d['depth']:
            stack.pop()
        if stack:
            stack[-1]['kids'].append(d)
        stack.append(d)
    return dies

def nm(a):
    m = re.search(r'\(indirect string, offset: 0x[0-9a-f]+\): (.*)$', a) or re.search(r'^(.*)$', a)
    return m.group(1) if m else a

def ref(a):
    m = re.search(r'<0x([0-9a-f]+)>', a or '')
    return int(m.group(1), 16) if m else None

def num(a):
    if a is None: return None
    m = re.match(r'(0x[0-9a-f]+|\d+)', a)
    return int(m.group(1), 0) if m else None

def tsize(dies, off):
    d = dies.get(off)
    if d is None: return 0
    t = d['tag']
    if 'DW_AT_byte_size' in d['attrs']: return num(d['attrs']['DW_AT_byte_size'])
    if t == 'DW_TAG_pointer_type': return PTR
    if t in ('DW_TAG_typedef', 'DW_TAG_const_type', 'DW_TAG_volatile_type', 'DW_TAG_restrict_type', 'DW_TAG_atomic_type'):
        return tsize(dies, ref(d['attrs'].get('DW_AT_type')))
    if t == 'DW_TAG_array_type':
        n = 1
        for k in d['kids']:
            if k['tag'] == 'DW_TAG_subrange_type':
                ub = num(k['attrs'].get('DW_AT_upper_bound')); c = num(k['attrs'].get('DW_AT_count'))
                n *= (c if c is not None else ((ub + 1) if ub is not None else 0))
        return n * tsize(dies, ref(d['attrs'].get('DW_AT_type')))
    return 0

def tname(dies, off, depth=0):
    d = dies.get(off)
    if d is None: return 'void'
    t = d['tag']; a = d['attrs']
    if 'DW_AT_name' in a and t in ('DW_TAG_base_type', 'DW_TAG_typedef', 'DW_TAG_structure_type', 'DW_TAG_union_type', 'DW_TAG_enumeration_type'):
        return nm(a['DW_AT_name'])
    if t == 'DW_TAG_pointer_type': return tname(dies, ref(a.get('DW_AT_type')), depth + 1) + '*'
    if t in ('DW_TAG_const_type', 'DW_TAG_volatile_type'): return tname(dies, ref(a.get('DW_AT_type')), depth + 1)
    if t == 'DW_TAG_array_type':
        sub = [num(k['attrs'].get('DW_AT_upper_bound')) for k in d['kids'] if k['tag'] == 'DW_TAG_subrange_type']
        return tname(dies, ref(a.get('DW_AT_type')), depth + 1) + ''.join('[%s]' % (s + 1 if s is not None else '?') for s in sub)
    if t == 'DW_TAG_structure_type': return 'struct{}'
    if t == 'DW_TAG_union_type': return 'union{}'
    return t

def find_type(dies, name):
    for off, d in dies.items():
        if d['tag'] == 'DW_TAG_typedef' and 'DW_AT_name' in d['attrs'] and nm(d['attrs']['DW_AT_name']) == name:
            return ref(d['attrs'].get('DW_AT_type'))
    for off, d in dies.items():
        if d['tag'] in ('DW_TAG_structure_type', 'DW_TAG_union_type') and 'DW_AT_name' in d['attrs'] and nm(d['attrs']['DW_AT_name']) == name and 'DW_AT_declaration' not in d['attrs']:
            return off
    return None

def strip_cv(dies, off):
    while off in dies and dies[off]['tag'] in ('DW_TAG_typedef', 'DW_TAG_const_type', 'DW_TAG_volatile_type'):
        off = ref(dies[off]['attrs'].get('DW_AT_type'))
    return off

def dump(dies, name, line=64, indent=0, base=0, out=None):
    off = find_type(dies, name)
    if off is None:
        print('%s: not found' % name); return
    off = strip_cv(dies, off)
    d = dies[off]; size = num(d['attrs'].get('DW_AT_byte_size'))
    print('%s: %d bytes = %.2f lines of %d; align-to-line waste %d' % (name, size, size / line, line, (-size) % line))
    members(dies, d, base, 0, line)

def members(dies, d, base, depth, line):
    end = base
    for k in d['kids']:
        if k['tag'] != 'DW_TAG_member': continue
        a = k['attrs']; o = base + (num(a.get('DW_AT_data_member_location')) or 0)
        ty = strip_cv(dies, ref(a.get('DW_AT_type'))); sz = tsize(dies, ref(a.get('DW_AT_type')))
        bit = ''
        if 'DW_AT_bit_size' in a: bit = ':%d' % num(a['DW_AT_bit_size']); sz = max(sz, 0)
        hole = o - end if (o > end and depth == 0) else 0
        n = nm(a['DW_AT_name']) if 'DW_AT_name' in a else '(anon)'
        print('  %s+%3d..%3d L%d  %-24s %s%s%s' % ('  ' * depth, o, o + sz, o // line, n, tname(dies, ref(a.get('DW_AT_type'))), bit, ('   <hole %d>' % hole if hole else '')))
        if ty in dies and dies[ty]['tag'] in ('DW_TAG_structure_type', 'DW_TAG_union_type') and 'DW_AT_name' not in dies[ty]['attrs'] and depth < 2:
            members(dies, dies[ty], o, depth + 1, line)
        if not bit: end = max(end, o + sz)

if __name__ == '__main__':
    dies = load(sys.argv[1])
    for n in sys.argv[2:]:
        dump(dies, n); print()
