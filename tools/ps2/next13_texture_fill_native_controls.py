#!/usr/bin/env python3
"""Execute native-object instruction/ordered-RELA/GP/frame negative controls."""
import argparse
import copy
import json
from pathlib import Path
import struct

from audio_storage_next7_finalbind import Object
from next10_gs_low import verify_object
from next7_render_fog_codecheck import Elf32
from next12_pb_scratch_codecheck import encoded_frame
from next10_sw_bounds_codecheck import frame


def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--work',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
 if a.out.exists():raise SystemExit('Fresh binary-control evidence required')
 a.out.mkdir(parents=True);g=json.loads((a.work/'cur-graph.json').read_text());engine=Path(g['engine']);emitted=a.work/'cur-graph.o';results=[]
 verify_object(engine,emitted,g['nodes'])
 selected=next(sec for sec in g['nodes'] if sec.startswith('.text.__wrap_memcpy'))
 mutations=[]
 for name,change in [('missing-RELA',lambda r:r[1:]),('reordered-RELAs',lambda r:list(reversed(r))),
                     ('wrong-RELA-addend',lambda r:[dict(r[0],addend=r[0]['addend']+4),*r[1:]])]:
  nodes=copy.deepcopy(g['nodes']);nodes[selected]['ordered_relocations']=change(nodes[selected]['ordered_relocations']);mutations.append((name,emitted,nodes))
 obj=Object(emitted)
 for name,section,offset in [('instruction-bit','.text.cur_fill_ia88',0),('input-GP0','.reginfo',20)]:
  body=bytearray(obj.data);body[obj.sections[section][4]+offset]^=1;dest=a.out/(name+'.o');dest.write_bytes(body);mutations.append((name,dest,g['nodes']))
 for name,current,nodes in mutations:
  try:verify_object(engine,current,nodes)
  except RuntimeError as error:results.append({'mutation':name,'detected':True,'reason':str(error)})
  else:raise SystemExit('Native binary mutation escaped '+name)
 actual=Elf32(engine);metadata=frame(actual,'fill_ia88');raw=actual.bytes(actual.sections[actual.index['.text.fill_ia88']])
 fake=copy.deepcopy(metadata);record=bytearray.fromhex(fake['frame_record_hex']);struct.pack_into('<I',record,20,16);fake['frame_record_hex']=record.hex()
 try:encoded_frame(raw,fake)
 except SystemExit as error:results.append({'mutation':'invented-stack-frame','detected':True,'reason':str(error)})
 else:raise SystemExit('Invented native stack reservation escaped')
 (a.out/'controls.json').write_text(json.dumps({'positive_object_binding_passed':True,'executed_binary_controls':results},indent=2)+'\n')
 print('Executed6 native object/RELA/GP/frame corruptions; all detected.')


if __name__=='__main__':main()
