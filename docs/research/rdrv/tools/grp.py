import re,os,sys
txt=open(sys.argv[1]).read().splitlines()
rows=[]
for l in txt:
    m=re.match(r'^(\S+)\s+(\d+)\s+([\d.]+)\s+([\d.]+)$',l)
    if m: rows.append((m.group(1).split('.')[0],float(m.group(3))))
d={}
for n,k in rows: d[n]=d.get(n,0)+k
groups={
'A copy/chunk':['PS2HWD_DrawBlocks','vu_pbegin','vu_chunk_end','vu_chunk_open','vu_cut_blk_run','vu_poly','blk_fov','vc_put','vu_poly_blk','vu_fans'],
'B water/EE emit':['water_sweep','clip_poly','emit_piece','emit_poly','lit_fast_poly','emit_reglist_fan','emit_reglist_fan_pv','lf_class_w_slow','ripple_table','emit_water_poly','lf_fog','cut_and_emit','emit_fan','water_generic','emit_tris','emit_fast_fan'],
'C plan/state':['begin_draw_inner','vu_plan_setup','vu_key_make','PS2HWD_PalLit','pass_setup','m_mul','state_flush','tex_regs','vu_consts_send','begin_draw','vu_retarget','vu_consts_build','xf_update','xf_set_transform','vu_state_rec'],
'D planner':['plan_part','poly_plane','plan_measure_blk','plan_clip','plan_xform_vu0','PS2HWD_PlanEnd','PS2HWD_PlanBlock','plan_measure_after','plan_key','plan_measure','plan_xform_vu0_blk','PS2HWD_PlanBegin'],
'E texture bookkeeping':['tex_footprint','plan_tex_touch','hw_SetTexture','PS2HWD_TouchTexture','settex_now','clut_get','imm_prepare','dc_find','tex_view','lru_touch'],
'F upload':['dec_level','tex_upload','fill_ap88','fill_idx','upload_bands','upload_ref','vram_alloc','vram_alloc_evicting','best_window','tex_drop'],
'G sky':['hw_RenderSkyDome','sky_strip','sky_dome_fast'],
'H singles':['hw_DrawPolygon','single_draw'],
'I quadhidden':['PS2HWD_QuadHidden'],
'J cull setup':['PS2HWD_CullSetup'],
}
tot=0
print(sys.argv[1])
for g,fs in groups.items():
    s=sum(d.get(f,0) for f in fs); tot+=s
    top=sorted([(d.get(f,0),f) for f in fs if d.get(f,0)>0],reverse=True)[:6]
    print('%-22s %6.0f  %s'%(g,s,', '.join('%s %.0f'%(f,k) for k,f in top)))
print('sum',tot)
# other notable
for n in ['HWR_ProcessPolygon','HWR_PBAdd','HWR_PBSlow','HWR_PBNew','HWR_RenderBatches','HWR_ProjectPlain','HWR_Subsector','HWR_ProcessSegC','HWR_RenderPlane','HWR_DrawSprite','HWR_DrawDropShadowGen','gc_replay','LZ4_decompress_safe','HWR_GenerateTexture','HWR_DrawTexturePatchInCache','Patch_CreateFromDoomPatch','Patch_CreateGL','W_ReadLumpHeaderPwad','memcpy','memset','ReadBlock','fread','Z_Malloc','Z_Free']:
    for k,v in d.items():
        if k==n or k.startswith(n): print('   %-34s %6.1f'%(k,v))
