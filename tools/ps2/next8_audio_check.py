#!/usr/bin/env python3
"""Full field/PCM/read equivalence of private MIDI layouts against frozen source.

The canonical serializer preserves every private field and all cache bytes at
old widths, including failed state and input-callback boundaries. Only C layout
padding is excluded. Production reference code is never normalized.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import zipfile

from audio_lossless_check import MUSIC_APIS, MUSIC_PROBE, ROOT, run
from next8_audio_variants import EXPECTED, prepare_variant

MIDI_PROBE = r'''
// Stable old-width layout on both sides; assign each complete field.
typedef struct {
    size_t start, end, pos, cacheoff, cached;
    uint64_t tick;
    unsigned running, ended;
    uint8_t cache[256];
} canonical_track;
typedef struct { unsigned program,volume,expression,pan,sustain,bend; } canonical_channel;
typedef struct {
    unsigned active,channel,key,velocity,released,held,env,age;
    uint32_t phase,step,noise;
} canonical_note;
typedef struct {
    ps2_audio_input in;
    canonical_track tracks[TRACKS];
    canonical_channel channels[16];
    canonical_note notes[NOTES];
    uint32_t steps[128]; int16_t sine[256];
    unsigned count,division,tempo,failed,events;
    uint64_t tick,us,remainder,frame,length_frames;
} midi_canonical_state;
size_t PS2_TestMidiState(const ps2_midi *m, unsigned char *bytes) {
    midi_canonical_state state, *out = &state;
    unsigned i;
    memset(out,0,sizeof *out);
#define COPY(x) out->x=m->x
    out->in=m->in;
    for(i=0;i<m->count;i++) {
#define TRACK(x) out->tracks[i].x=m->tracks[i].x
        TRACK(start); TRACK(end); TRACK(pos); TRACK(cacheoff); TRACK(cached);
        TRACK(tick); TRACK(running); TRACK(ended);
        memcpy(out->tracks[i].cache,m->tracks[i].cache,sizeof out->tracks[i].cache);
#undef TRACK
        if (m->tracks[i].running > 239 || m->tracks[i].ended > 1) abort();
    }
    for(i=0;i<16;i++) {
#define CHANNEL(x) out->channels[i].x=m->channels[i].x
        CHANNEL(program); CHANNEL(volume); CHANNEL(expression); CHANNEL(pan); CHANNEL(sustain); CHANNEL(bend);
#undef CHANNEL
        if(m->channels[i].program>127||m->channels[i].volume>127||m->channels[i].expression>127||m->channels[i].pan>127||m->channels[i].sustain>1||m->channels[i].bend>16383) abort();
    }
    for(i=0;i<NOTES;i++) {
#define NOTE(x) out->notes[i].x=m->notes[i].x
        NOTE(active); NOTE(channel); NOTE(key); NOTE(velocity); NOTE(released); NOTE(held); NOTE(env); NOTE(age);
        NOTE(phase); NOTE(step); NOTE(noise);
#undef NOTE
        if(m->notes[i].active>1||m->notes[i].channel>15||m->notes[i].key>127||m->notes[i].velocity>127||m->notes[i].released>1||m->notes[i].held>1||m->notes[i].env>32992) abort();
    }
    memcpy(out->steps,m->steps,sizeof out->steps); memcpy(out->sine,m->sine,sizeof out->sine);
    COPY(count); COPY(division); COPY(tempo); COPY(failed); COPY(events);
    COPY(tick); COPY(us); COPY(remainder); COPY(frame); COPY(length_frames);
#undef COPY
    memcpy(bytes,out,sizeof *out);
    return sizeof *out;
}
size_t PS2_TestMidiBytes(unsigned count) { return sizeof(ps2_midi)+count*sizeof(midi_track); }
size_t PS2_TestMidiTrackBytes(void) { return sizeof(midi_track); }
size_t PS2_TestMidiChannelBytes(void) { return sizeof(midi_channel); }
size_t PS2_TestMidiNoteBytes(void) { return sizeof(midi_note); }
void PS2_TestMidiFlipPhase(ps2_midi *m) { m->notes[0].phase ^= 1; }
unsigned PS2_TestMidiEnvelope(const ps2_midi *m) { return m->notes[0].env; }
'''
MUSIC_PROBE += r'''
void PS2_TestMusicFlipCursor(ps2_music *m) { m->cursor ^= 1; }
unsigned PS2_TestMusicStep(const ps2_music *m) { return m->step; }
'''


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline',type=Path,required=True)
    ap.add_argument('--candidate',type=Path,default=ROOT)
    ap.add_argument('--variant',choices=tuple(EXPECTED),help='reconstruct a recorded rejected source under --out before testing')
    ap.add_argument('--out',type=Path,default=ROOT/'build/nov15-next8/audio/host')
    ap.add_argument('--assets',type=Path,default=Path('/workspace/.onboarding/srb2-assets'))
    ap.add_argument('--sdk',type=Path,default=Path(os.environ.get('PS2SDK','/workspace/.onboarding/ps2dev-v2/ps2dev/ps2sdk')))
    ap.add_argument('--sanitize',action='store_true')
    ap.add_argument('--ee-only',action='store_true',help='build real-codec EE proof and strict objects without starting emulator')
    ap.add_argument('--mutations-only',action='store_true',help='compile and execute five deliberate candidate source failures')
    a=ap.parse_args(); a.out=a.out.resolve(); assert a.out.is_relative_to(ROOT/'build')
    a.out.mkdir(parents=True,exist_ok=True)
    if a.variant:a.candidate=prepare_variant(a.baseline,a.out/'source-candidate',a.variant)
    reference=a.baseline.resolve()/'src/ps2'; candidate=a.candidate.resolve()/'src/ps2'
    names=('ps2_audio.c','ps2_audio.h','ps2_music.c','ps2_music.h','ps2_midi.c','ps2_midi.h','ps2_athread.c','ps2_athread.h','ps2_pcmconv.h')
    manifest={kind:{n:hashlib.sha256((p/n).read_bytes()).hexdigest() for n in names} for kind,p in [('reference',reference),('candidate',candidate)]}
    for n in names:
        if n!='ps2_midi.c': assert manifest['reference'][n]==manifest['candidate'][n],n
    manifest['baseline_path']=str(a.baseline.resolve());manifest['candidate_path']=str(a.candidate.resolve())
    manifest['reference_normalization']='none; complete old-width field serialization excludes only layout padding'
    manifest['fixtures']={n:hashlib.sha256((ROOT/'tools/ps2'/n).read_bytes()).hexdigest() for n in ('next8_audio_check.py','next8_audio_hosttest.c','next8_audio_variants.py','audio_lossless_hosttest.c','audio_refill_next6_hosttest.c')}
    (a.out/'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    headers=''.join((candidate/n).read_text() for n in ('ps2_music.h','ps2_midi.h'))
    apis=sorted(set(re.findall(r'\bPS2_(?:Music|MIDI)\w+(?=\s*\()',headers)))
    decl=[]
    for n in ('ps2_music.h','ps2_midi.h'):
        for line in (candidate/n).read_text().splitlines():
            if re.match(r'^[^/].*\bPS2_(?:Music|MIDI)\w+\(.*;$',line): decl.append(MUSIC_APIS.sub(lambda m:m[0].replace('PS2_','Ref_',1),line))
    (a.out/'reference.h').write_text('\n'.join(decl)+'\n')
    sources=[]
    for kind,p in [('reference',reference),('candidate',candidate)]:
        for unit,probe in [('ps2_music.c',MUSIC_PROBE),('ps2_midi.c',MIDI_PROBE)]:
            production=(p/unit).read_text()
            if kind=='reference':
                production=MUSIC_APIS.sub(lambda m:m[0].replace('PS2_','Ref_',1),production)
                production='\n'.join('#define %s %s'%(api,api.replace('PS2_','Ref_',1)) for api in apis)+'\n'+production
                probe=probe.replace('PS2_Test','Ref_Test')
            path=a.out/(kind+'_'+unit);path.write_text(production+probe);sources.append(path)
    if a.ee_only:
        ee=a.out/'ee';ee.mkdir(exist_ok=True)
        assets=[]
        with zipfile.ZipFile(a.assets/'music.pk3') as archive:
            for magic in (b'MThd',b'OggS'):
                songs=sorted((len(data),n) for n in archive.namelist() if not n.endswith('/') and (data:=archive.read(n)).startswith(magic))
                for selected in (0,len(songs)//2,len(songs)-1):
                    n=songs[selected][1];assets.append((n,archive.read(n)))
        for channels in (1,2):
            p=a.out/('tone%d.mp3'%channels)
            run(['ffmpeg','-hide_banner','-loglevel','error','-y','-f','lavfi','-i','sine=frequency=337:sample_rate=44100:duration=3.25','-ac',channels,'-codec:a','libmp3lame','-b:a','128k',p]);assets.append((p.name,p.read_bytes()))
        with (a.out/'next8_audio_assets.h').open('w') as h:
            for i,(n,data) in enumerate(assets):h.write('static const unsigned char audio8_%d[]={%s};\n'%(i,','.join(map(str,data))))
            h.write('static const struct { const char *name; const unsigned char *bytes; size_t length; } next8_audio_assets[]={\n')
            for i,(n,data) in enumerate(assets):h.write('{%s,audio8_%d,sizeof audio8_%d},\n'%(json.dumps(n),i,i))
            h.write('};\n')
        (a.out/'ee-assets.json').write_text(json.dumps([{'name':n,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()} for n,data in assets],indent=2)+'\n')
        flags=['-O2','-std=gnu17','-G0','-Wall','-Wextra','-Werror','-D_EE','-DPS2','-DPS2_AUDIO_VORBIS','-DPS2_AUDIO_MP3','-DNEXT8_AUDIO_ASSETS',
               '-I'+str(candidate),'-I'+str(ROOT/'tools/ps2'),'-I'+str(a.out),'-I'+str(a.sdk/'ee/include'),'-I'+str(a.sdk/'common/include'),'-I'+str(a.sdk/'ports/include')]
        sizes=[]
        for i,src in enumerate(sources):
            text=src.read_text();probe=MUSIC_PROBE if i%2==0 else MIDI_PROBE
            if i<2:probe=probe.replace('PS2_Test','Ref_Test')
            assert text.endswith(probe)
            clean=ee/('strict_'+src.name);clean.write_text(text[:-len(probe)])
            obj=clean.with_suffix('.o');run(['mips64r5900el-ps2-elf-gcc',*flags,'-c',clean,'-o',obj]);sizes.append(obj)
            obj.with_suffix('.dis').write_text(run(['mips64r5900el-ps2-elf-objdump','-dr',obj]))
        objects=[]
        for src in [*sources,candidate/'ps2_audio.c',ROOT/'tools/ps2/next8_audio_hosttest.c']:
            obj=ee/('bench_'+src.stem+'.o');run(['mips64r5900el-ps2-elf-gcc',*flags,'-c',src,'-o',obj]);objects.append(obj)
        elf=ee/'AUDIO8.ELF'
        link=['mips64r5900el-ps2-elf-gcc','-T'+str(a.sdk/'ee/startup/linkfile'),'-L'+str(a.sdk/'ee/lib'),'-L'+str(a.sdk/'ports/lib'),
              '-Wl,-zmax-page-size=128','-Wl,--defsym,_stack_size=0x60000','-Wl,--wrap=calloc','-Wl,--wrap=free']
        libs=['-ldebug','-lpatches','-lvorbisfile','-lvorbis','-logg','-lmpg123','-lm']
        run([*link,*objects,'-o',elf,*libs])
        size=run(['mips64r5900el-ps2-elf-size',*sizes]);(a.out/'sizes.txt').write_text(size);print(size)
        (a.out/'ee-build.json').write_text(json.dumps({'elf':str(elf),'elf_sha256':hashlib.sha256(elf.read_bytes()).hexdigest(),'flags':flags},indent=2)+'\n')
        print('EE real-codec proof built:',elf);return
    flags=['-O2','-g','-std=gnu17','-Wall','-Wextra','-Werror','-I'+str(candidate),'-I'+str(ROOT/'tools/ps2'),'-I'+str(a.out),'-I'+str(a.sdk/'ports/include'),'-DPS2_AUDIO_VORBIS','-DPS2_AUDIO_MP3']
    if a.sanitize: flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie']
    exe=a.out/'audio8';libs=['-l:libvorbisfile.so.3','-l:libvorbis.so.0','-l:libogg.so.0','-l:libmpg123.so.0','-lm']
    command=['gcc',*flags,*sources,candidate/'ps2_audio.c',ROOT/'tools/ps2/next8_audio_hosttest.c','-Wl,--wrap=calloc','-Wl,--wrap=free','-o',exe,*libs]
    if a.mutations_only:
        mutations=[('bend-high','c->bend = a + b * 128;','c->bend = (a + b * 128) & 255;'),
                   ('env-high','n->env += 256;','n->env = (n->env + 256) & 32767;'),
                   ('cached-count','t->cached = PS2_AudioRead','t->cached = (uint16_t)PS2_AudioRead'),
                   ('track-running','t->running = s;','t->running = s & 127;'),
                   ('allocation-size','sizeof *m + count * sizeof m->tracks[0]','sizeof *m + count * sizeof m->tracks[0] - 16')]
        env=os.environ.copy();env['PS2_AUDIO_NO_BENCH']='1';results=[]
        for label,old,new in mutations:
            text=sources[3].read_text();assert old in text,label
            changed=a.out/(label+'.c');changed.write_text(text.replace(old,new,1))
            target=a.out/(label+'-negative');cmd=[str(x) for x in command]
            cmd[cmd.index(str(sources[3]))]=str(changed);cmd[cmd.index(str(exe))]=str(target);run(cmd)
            result=subprocess.run([target],capture_output=True,text=True,env=env,timeout=60)
            (a.out/(label+'-negative.log')).write_text(result.stdout+result.stderr)
            assert result.returncode==1 and 'audio-lossless:' in result.stderr,(label,result.stdout,result.stderr)
            results.append({'mutation':label,'status':result.returncode,'assertion':result.stderr.splitlines()[0]})
            print(label,'source mutation detected')
        (a.out/'mutations.json').write_text(json.dumps(results,indent=2)+'\n');return
    log=[run(command)];files=[]
    with zipfile.ZipFile(a.assets/'music.pk3') as archive:
        for n in sorted(archive.namelist()):
            if n.endswith('/'):continue
            data=archive.read(n)
            if data.startswith((b'MThd',b'OggS')):
                p=a.out/'assets'/Path(n).name;p.parent.mkdir(exist_ok=True);p.write_bytes(data);files.append(p)
    for channels in (1,2):
        p=a.out/('tone%d.mp3'%channels)
        run(['ffmpeg','-hide_banner','-loglevel','error','-y','-f','lavfi','-i','sine=frequency=337:sample_rate=44100:duration=3.25','-ac',channels,'-codec:a','libmp3lame','-b:a','128k',p]);files.append(p)
    env=os.environ.copy()
    if a.sanitize:env['PS2_AUDIO_NO_BENCH']='1'
    output=run([exe,*files],env=env);print(output,end='');log.append(output)
    for control,expected in [('--negative-control','na == nb && !memcmp(a, b, sizeof a)'),('--state-negative-control','na == nb && na <= sizeof sa && !memcmp(sa, sb, na)')]:
        result=subprocess.run([exe,control],capture_output=True,text=True,env=env)
        (a.out/(control[2:]+'.log')).write_text(result.stdout+result.stderr)
        assert result.returncode==1 and expected in result.stderr,(control,result.stdout,result.stderr)
        log.append(control+' detected');print(log[-1])
    (a.out/'report.txt').write_text('\n'.join(log))

if __name__=='__main__':main()
