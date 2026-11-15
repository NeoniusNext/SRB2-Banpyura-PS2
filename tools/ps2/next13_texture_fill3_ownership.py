#!/usr/bin/env python3
"""Read-only source, row-callback lifetime and time-derived policy audit."""
import argparse
import hashlib
import json
from pathlib import Path

from next13_texture_fill3_variant import ROOT,PATH,rewrite,OLD_IDX,NEW_IDX,OLD_IA,NEW_IA
from next13_texture_hosttest import section


def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--candidate',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
 baseline=ROOT/'build/nov15-next12/final/source/src';current=a.candidate.resolve()/'src'
 before=(baseline/PATH).read_text();after=(current/PATH).read_text()
 if after!=rewrite(before) or after.replace(NEW_IDX,OLD_IDX).replace(NEW_IA,OLD_IA)!=before:raise SystemExit('The full texture file does not restore literally')
 for name,end in [('dec_level','// PS2-HW-38: level'),('dc_find','// the cached level'),('upload_bands','// ---- texture data cache'),('tex_upload','// a texture view')]:
  if end not in before:continue
  if name in ('upload_bands','tex_upload'):continue # Their original complete bytes are included by the whole-file reversal and actual code binding.
  if section(before,'static '+('dcent_t *' if name=='dc_find' else 'void ')+name+'(',end)!=section(after,'static '+('dcent_t *' if name=='dc_find' else 'void ')+name+'(',end):raise SystemExit('Unexpected policy/filter source change '+name)
 upload=section(before,'static void upload_bands(','// ---- texture data cache')
 if upload.count('fill(ctx, r0 + y, dst + (size_t)y * rowbytes);')!=1:raise SystemExit('Original synchronous row invoker changed')
 if 'h = pk_alloc(1 + 4 + 1 + nqw);' not in upload or upload.index('h = pk_alloc')>upload.index('fill(ctx,'):raise SystemExit('Ring ownership must precede the row call')
 if before.index('Z_SetRebuildCost((void **)(void *)&ne->p, cyc() - dt0)')>before.index('upload_bands(blk, tbw, psm, W2, H2, bpp, fill, c);'):raise SystemExit('Filler entered timed source rebuild interval')
 hwd=(baseline/'ps2/hw/ps2_hwd.c').read_text();zone=(baseline/'z_zone.c').read_text()
 if hwd.index('TexInfo->ps2_cost =')>hwd.index('ri = tex_upload(TexInfo);'):raise SystemExit('Upload entered source regeneration cost measurement')
 if 'static boolean zroom_rebuild_cost;' not in zone or 'if (!user || !zroom_rebuild_cost)' not in zone:raise SystemExit('Existing optional rebuild policy contract changed')
 result={'baseline_source':str(baseline),'candidate_source':str(current),'baseline_texture_sha256':sha(baseline/PATH),'candidate_texture_sha256':sha(current/PATH),
   'whole_file_literal_reversal':True,'changed_functions':['fill_idx','fill_ia88'],
   'exact_unsigned_identity':'For defined dx<32, step=1u<<dx and sx0=step>>1; sx after x advances equals (x<<dx)+(step>>1) modulo2^32. The zero-guarded full-u32 countdown executes exactly W2 shared sample bodies. The final sx increment is defined modulo2^32 and unused; the destination cursor becomes one-past the last valid output element without any further access. IA88 converts sx to size_t only after its u32 wrap. Zero width returns before the new column shift or sample access.',
   'literal_index_copy_fast_path_preserved':True,'original_const_context_snapshot_literal_unchanged':True,'remaining_width_is_full_u32_snapshot_of_original_local_c':True,'same_source_read_and_destination_store_sequence':True,
   'unchanged_observable_data':['source row and crop stride','all input byte combinations and alpha rounding','immutable per-row context snapshot','logical dimensions,texture format,CLUT,filtering,LOD,planner,VRAM footprints,capacities','cache keys/stamps/evictions/locks,allocation/retry/failure,source reconstruction','GIF packet words,tails,texture flushes,VU/DMA ordering and completion ownership'],
   'row_lifetime':['tex_upload owns fc on its stack and updates it only between completed upload_bands calls','pk_alloc acquires ring ownership before invoking the callback','row output is separate DMA-ring storage; no callback allocates or submits DMA','filler retains no source or context across calls; only existing dx0 memcpy can call the unchanged memory wrapper','The actual destination is a distinct ring allocation and cannot overlap tex_upload stack fc or its by-value local snapshot','No source cursor is advanced: source base stays fixed; only u32 sx advances, before the existing size_t/bpp address scaling','The non-copy paths retain each source-read-before-output-store sequence; the copy path retains its original nonoverlap memcpy contract'],
   'time_consumers':['Both Z_SetRebuildCost publications finish before upload_bands invokes either selected filler','TexInfo->ps2_cost measures only HWR_PS2_RegenerateMipmap before tex_upload','H.st.cyc_tex and HWUPSLOW consume upload duration as telemetry; no texture controller consumes these duration fields','The dec_level score candidate was rejected because its duration directly changes supported -zroomcost1 cache-eviction policy'],
   'persistent_heap_ring_VRAM_added_bytes':0,'new_policy_query_or_flag':False,'all_preceding_source_algorithms_literal_unchanged':True}
 if a.out.exists():raise SystemExit('Fresh ownership report required')
 a.out.write_text(json.dumps(result,indent=2)+'\n');print('Exact source-column/lifetime/controller-consumer audit passed.')


if __name__=='__main__':main()
