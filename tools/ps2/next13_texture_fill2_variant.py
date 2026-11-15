#!/usr/bin/env python3
"""Exact unsigned source-column increments for the two remaining row fillers."""
import argparse
import hashlib
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
PATH=Path('ps2/hw/ps2_hw_tex.inc')
OLD_IDX='''	for (x = 0; x < c.W2; x++)
		dst[x] = s[src_col(&c, x)];
'''
NEW_IDX='''	if (!c.W2)
		return;
	// Emit the first centre sample before advancing the unsigned source column.
	const u32 step = 1u << c.dx;
	u32 sx = step >> 1;
	dst[0] = s[sx];
	for (x = 1; x < c.W2; x++)
	{
		sx += step;
		dst[x] = s[sx];
	}
'''
OLD_IA='''	for (x = 0; x < c.W2; x++)
	{
		const u8 *t = s + (size_t)src_col(&c, x) * 2;

		d[x] = (u32)t[0] * 0x010101u | (a8_to_gs(t[1]) << 24);
	}
'''
NEW_IA='''	if (!c.W2)
		return;
	// Emit the first centre sample before advancing the unsigned source column.
	const u32 step = 1u << c.dx;
	u32 sx = step >> 1;
	{
		const u8 *t = s + (size_t)sx * 2;

		d[0] = (u32)t[0] * 0x010101u | (a8_to_gs(t[1]) << 24);
	}
	for (x = 1; x < c.W2; x++)
	{
		sx += step;
		const u8 *t = s + (size_t)sx * 2;

		d[x] = (u32)t[0] * 0x010101u | (a8_to_gs(t[1]) << 24);
	}
'''



def rewrite(text):
    for old,new in ((OLD_IDX,NEW_IDX),(OLD_IA,NEW_IA)):
        if text.count(old)!=1 or new in text:raise RuntimeError('Exact row increment anchor changed')
        text=text.replace(old,new,1)
    return text


def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base-src',type=Path,default=ROOT/'build/nov15-next12/final/source/src')
    p.add_argument('--out',type=Path,required=True)
    a=p.parse_args()
    if a.out.exists():raise SystemExit('Fresh isolated filler variant required')
    target=a.out/PATH;target.parent.mkdir(parents=True)
    target.write_text(rewrite((a.base_src/PATH).read_text()))
    (a.out/'variant.json').write_text(json.dumps({'baseline_src':str(a.base_src.resolve()),
       'baseline_texture_sha256':sha(a.base_src/PATH),'candidate_texture_sha256':sha(target),
       'only_changed_functions':['fill_idx','fill_ia88'],'only_change':'first sample plus unsigned source-column induction'},indent=2)+'\n')


if __name__=='__main__':main()
