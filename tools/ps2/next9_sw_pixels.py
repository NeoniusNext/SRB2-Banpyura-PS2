"""Exercise actual old/new slope-light helpers through unchanged real drawers.

The existing complete framebuffer, cursor, lighting-array and live-alias proof
is reused. Each version now calls its own verbatim extracted lighting helpers.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import next7_render_fog_hosttest as fog

ROOT=Path(__file__).resolve().parents[2]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',required=True,type=Path)
    p.add_argument('--base-src',required=True,type=Path)
    p.add_argument('--cc',default='gcc')
    p.add_argument('--negative-controls',action='store_true')
    p.add_argument('--sanitize',action='store_true')
    a=p.parse_args();out=a.out.resolve();base=a.base_src.resolve()
    fog.prepare(out,base)
    helpers=['INT32 tiltlighting[MAXVIDWIDTH];\n']
    hashes={}
    for variant,directory in [('reference',base),('candidate',ROOT/'src')]:
        source=(directory/'r_draw.c').read_text()
        begin=source.index('static INT32 tiltlighting')
        end=source.index('#ifdef PS2_OPT_SLOPE\n// (UINT32)',begin)
        lighting=source[begin:end]
        lighting=re.sub(r'static INT32 tiltlighting\[MAXVIDWIDTH\];\s*','',lighting,count=1)
        for name in ('R_CalcTiltedLighting','R_CalcSlopeLight'):
            lighting=re.sub(r'\b'+name+r'\b',variant+'_'+name,lighting)
        helpers.append(lighting)
        for path in out.glob(variant+'*.inc'):
            path.write_text(re.sub(r'\bR_CalcSlopeLight\b',variant+'_R_CalcSlopeLight',path.read_text()))
        hashes[variant]={'actual_source_sha256':hashlib.sha256(source.encode()).hexdigest(),
                         'lighting_source_sha256':hashlib.sha256(lighting.encode()).hexdigest()}
    (out/'lighting.inc').write_text('\n'.join(helpers))
    (out/'next9-actual-helper-hashes.json').write_text(json.dumps(hashes,indent=2)+'\n')
    fog.host(out,a)


if __name__=='__main__':main()
