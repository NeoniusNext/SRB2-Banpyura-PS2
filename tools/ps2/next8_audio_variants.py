#!/usr/bin/env python3
"""Recreate the rejected pass-eight MIDI sources from the frozen baseline.

Writes only under build. Production source is untouched. The exact hashes
include comments; scalar-build is the uncommented source used by its actual
complete-engine LTO measurement, while scalar is its commented host freeze.
"""
import argparse
import hashlib
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
BASELINE_SHA='15b44555b6290a1b0a9519a4b09e06f5c7e2d48b19a042e748b9913f47e3ec11'
EXPECTED={
 'scalar':'434984c9a96992c93a7ccf69ddb30c1321336452e6aeacfbfb7bfe84614860e2',
 'scalar-build':'f1cb1e48cf8f574750928a568999d60278bc985eed895c63a8ca3bfad9b2f50d',
 'channel':'8c15942d09fe8a31881500981186c0aef9603968ae34e10662c901772b61f57d',
}
AUDIO_FILES=('ps2_audio.c','ps2_audio.h','ps2_music.c','ps2_music.h','ps2_midi.c','ps2_midi.h','ps2_athread.c','ps2_athread.h','ps2_pcmconv.h')


def replace_once(text,old,new):
    assert text.count(old)==1,(old,text.count(old))
    return text.replace(old,new)


def midi_variant(source,variant):
    assert hashlib.sha256(source.encode()).hexdigest()==BASELINE_SHA
    text=replace_once(source,'\tsize_t start, end, pos, cacheoff, cached;\n\tuint64_t tick;\n\tunsigned running, ended;',
                      '\tuint64_t tick;\n\tsize_t start, end, pos, cacheoff, cached;\n\tuint8_t running, ended;')
    text=replace_once(text,'typedef struct { unsigned program, volume, expression, pan, sustain, bend; } midi_channel;',
                     'typedef struct { uint8_t program, volume, expression, pan, sustain; uint16_t bend; } midi_channel;')
    if variant in ('scalar','scalar-build'):
        text=replace_once(text,'\tunsigned active, channel, key, velocity, released, held, env, age;',
                         '\tuint8_t active, channel, key, velocity, released, held;\n\tuint16_t env;\n\tunsigned age;')
        assert text.count('n->velocity * c->volume * c->expression')==2
        text=text.replace('n->velocity * c->volume * c->expression','(unsigned)n->velocity * c->volume * c->expression')
        text=replace_once(text,'(32768 - n->env)/256','(32768u - n->env)/256')
    if variant!='scalar-build':
        text=replace_once(text,'\tsize_t start, end, pos, cacheoff, cached;',
                         '\t// Keep the full callback count, including oversized failed-read results.\n\tsize_t start, end, pos, cacheoff, cached;')
        text=replace_once(text,'\tuint8_t running, ended;','\t// Running status is 0 or 128..239; ended is boolean.\n\tuint8_t running, ended;')
        text=replace_once(text,'typedef struct { uint8_t program, volume, expression, pan, sustain; uint16_t bend; } midi_channel;',
                         '// Event validates all data bytes as 0..127; bend combines two such bytes.\ntypedef struct { uint8_t program, volume, expression, pan, sustain; uint16_t bend; } midi_channel;')
        if variant=='scalar':
            text=replace_once(text,'\tuint16_t env;','\t// A resumed attack can reach 32992 (32736+256), still below 33024.\n\tuint16_t env;')
            text=replace_once(text,'\tunsigned age;','\t// Age, oscillator state, source positions and event timing retain full width.\n\tunsigned age;')
        else:
            text=replace_once(text,'typedef struct\n{\n\tunsigned active, channel, key, velocity, released, held, env, age;',
                             '// A resumed attack can reach 32992. Keep the original full-width synth\n// state, age, source positions and event timing.\ntypedef struct\n{\n\tunsigned active, channel, key, velocity, released, held, env, age;')
    assert hashlib.sha256(text.encode()).hexdigest()==EXPECTED[variant],variant
    return text


def prepare_variant(baseline,out,variant):
    out=out.resolve();assert out.is_relative_to(ROOT/'build')
    source=baseline.resolve()/'src/ps2';dest=out/'src/ps2';dest.mkdir(parents=True,exist_ok=True)
    for name in AUDIO_FILES:
        data=(source/name).read_bytes()
        if name=='ps2_midi.c':data=midi_variant(data.decode(),variant).encode()
        (dest/name).write_bytes(data)
    return out


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline',type=Path,default=ROOT/'build/nov15-next8/baseline-src')
    ap.add_argument('--out',type=Path,default=ROOT/'build/nov15-next8/audio/reconstructed')
    ap.add_argument('--variant',choices=(*EXPECTED,'all'),default='all')
    a=ap.parse_args();result={}
    for variant in EXPECTED if a.variant=='all' else (a.variant,):
        dest=a.out/variant if a.variant=='all' else a.out
        path=prepare_variant(a.baseline,dest,variant)
        result[variant]={'path':str(path),'ps2_midi_sha256':EXPECTED[variant]}
    print(json.dumps(result,indent=2))

if __name__=='__main__':main()
