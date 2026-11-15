#!/usr/bin/env python3
"""Run exact-source implicit drawnode/control/opt-out host traces and mutations."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

import next12_cull_hosttest as old
from next13_ram_variant import ROOT, BASE, OUT, MAIN


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


EXTRA_COMMON = r'''
typedef struct {
 result_t semantic;
 uint64_t sort_hash, compare_hash, sorted_hash, pointers_hash;
 size_t qsort_calls, compare_calls, dn_bytes, di_bytes, dn_cap, di_cap;
 unsigned errors, unwinds, callbacks, callback_mask;
 uint64_t error_hash;
} ram_result_t;
void base_ram_run(const fixture_t *,ram_result_t *);
void cur_ram_run(const fixture_t *,ram_result_t *);
void off_ram_run(const fixture_t *,ram_result_t *);
'''

EXTRA_CONTEXT = r'''
#include <setjmp.h>
static ram_result_t *ram_res;
static jmp_buf ram_unwind;
static unsigned ram_abort_on_event,ram_callback_mask,ram_fail_index,ram_fail_node;
static size_t ram_original_counts[3];
static void *ram_old_bases[3];
static unsigned ram_mutated;
static void ram_mutate(void);
static uint64_t ram_hash(uint64_t h,const void *p,size_t n) {
 const unsigned char *s=p;while(n--)h=(h^*s++)*1099511628211ull;return h;
}
static void *ram_realloc(void *p,size_t n,const char *name) {
 if(!strcmp(name,"dn_buf")) {
  ram_res->dn_bytes=n;ram_res->dn_cap=n/(4*sizeof(void *));
  if(ram_fail_node) {ram_fail_node=0;return NULL;}
 } else if(!strcmp(name,"di_buf")) {
  ram_res->di_bytes=n;ram_res->di_cap=n/sizeof(size_t);
  if(ram_fail_index) {ram_fail_index=0;return NULL;}
 } else abort();
 return realloc(p,n);
}
static int (*ram_cmp)(const void *,const void *);
static int ram_compare(const void *a,const void *b) {
 size_t aa=*(const size_t*)a,bb=*(const size_t*)b;
 int value=ram_cmp(a,b);ram_res->compare_calls++;
 ram_res->compare_hash=ram_hash(ram_res->compare_hash,&aa,sizeof aa);
 ram_res->compare_hash=ram_hash(ram_res->compare_hash,&bb,sizeof bb);
 ram_res->compare_hash=ram_hash(ram_res->compare_hash,&value,sizeof value);
 return value;
}
static void ram_qsort(void *p,size_t n,size_t width,int(*cmp)(const void *,const void *)) {
 unsigned which=cmp==CompareDrawNodes?1u:2u;
 size_t offset=(size_t)((size_t*)p-sortindex);
 ram_res->qsort_calls++;
 ram_res->sort_hash=ram_hash(ram_res->sort_hash,&which,sizeof which);
 ram_res->sort_hash=ram_hash(ram_res->sort_hash,&offset,sizeof offset);
 ram_res->sort_hash=ram_hash(ram_res->sort_hash,&n,sizeof n);
 ram_res->sort_hash=ram_hash(ram_res->sort_hash,&width,sizeof width);
 ram_cmp=cmp;qsort(p,n,width,ram_compare);
 ram_res->sorted_hash=ram_hash(ram_res->sorted_hash,p,n*width);
}
#define realloc(p,n) ram_realloc(p,n,#p)
#define qsort ram_qsort
'''

EXTRA_BOTTOM = r'''
#undef realloc
#undef qsort
static void ram_mutate(void) {
 if(ram_mutated)return;ram_mutated=1;ram_res->callbacks++;
 const size_t counts[3]={ram_original_counts[0],ram_original_counts[1],ram_original_counts[2]};
 const size_t strides[3]={sizeof *planeinfo,sizeof *polyplaneinfo,sizeof *wallinfo};
 void **bases[3]={(void**)&planeinfo,(void**)&polyplaneinfo,(void**)&wallinfo};
 for(unsigned k=0;k<3;k++) {
  ram_old_bases[k]=*bases[k];
  if(counts[k]) {
   // Force a moved copy without invalidating the old source, as a callback can
   // rebind globals while the original drawnode tuples still name old storage.
   void *copy=realloc(NULL,counts[k]*strides[k]);if(!copy)abort();
   memcpy(copy,*bases[k],counts[k]*strides[k]);*bases[k]=copy;
   for(size_t j=0;j<counts[k];j++) {
    if(k==0)((planeinfo_t*)copy)[j].alpha^=1;
    else if(k==1)((polyplaneinfo_t*)copy)[j].alpha^=1;
    else ((wallinfo_t*)copy)[j].lightlevel^=127;
   }
  }
 }
 numplanes+=17;numpolyplanes+=23;numwalls+=31;
}
void TAG_ram_run(const fixture_t*f,ram_result_t*r) {
 memset(r,0,sizeof*r);ram_res=r;res=&r->semantic;input=f;
 r->sort_hash=r->compare_hash=r->sorted_hash=r->pointers_hash=1469598103934665603ull;
 ram_callback_mask=f->metadata_fault;ram_abort_on_event=f->reload_frame>0?(unsigned)f->reload_frame:0;
 ram_fail_index=f->evict_each_frame==2;ram_fail_node=f->evict_each_frame==3;ram_mutated=0;
 memset(ram_old_bases,0,sizeof ram_old_bases);memset(lut_ids,0,sizeof lut_ids);
 res->record_hash=2166136261u;hwr_texsig=5;hwr_geo_off=0;hwr_lt_epoch=7;hl_view=-1;hl_epoch=0;hl_id=0;
 fixture_palette=f->palette;Cubeapply=f->cube;cv_glshaders.value=f->shader;gl_shadersavailable=true;
 cv_secbright.value=f->secbright;r_renderwalls=f->renderwalls;currently_batching=false;hwr_grec_on=f->record;
 selected_texture=current_texture=NULL;viewz=0;memset(&gc,0,sizeof gc);fixture_frame=0;
 init_textures(f);drawcount=0;numwalls=numplanes=numpolyplanes=0;
 for(unsigned i=0;i<f->n;i++) {
  const node_input_t*n=&f->node[i];cmaps[i]=(extracolormap_t){(int)i,0x20406080u+(UINT32)i,0xff102030u,2,27};
  extracolormap_t*cm=n->cmap?&cmaps[i]:NULL;flats[i].id=n->texture;xsubs[i].id=polys[i].id=n->id;polys[i].flags=POF_SPLAT;sectors[i].id=n->id;
  if(n->kind==1)HWR_AddTransparentFloor(&flats[i],&xsubs[i],false,n->height,n->light,n->alpha,&sectors[i],n->flags,false,true,cm);
  else if(n->kind==2)HWR_AddTransparentPolyobjectFloor(&flats[i],&polys[i],false,n->height,n->light,n->alpha,&sectors[i],n->flags,cm);
  else {FSurfaceInfo s={0};s.PolyFlags=(FUINT)n->id;s.PolyColor.rgba=0xaabbccddu;s.PolyColor.s.alpha=n->alpha;HWR_AddTransparentWall((FOutVector*)n->v,&s,n->texture,n->flags,false,n->light,cm);}
 }
 ram_original_counts[0]=numplanes;ram_original_counts[1]=numpolyplanes;ram_original_counts[2]=numwalls;
 if(f->fr_off) {
  INT32 *first=NULL;
  if(numplanes)first=&planeinfo[0].drawcount;
  if(numpolyplanes) {
   if(first)polyplaneinfo[0].drawcount=*first;
   else first=&polyplaneinfo[0].drawcount;
  }
  if(numwalls&&first)wallinfo[0].drawcount=*first;
 }
 if(!setjmp(ram_unwind)) {
  // Both real renderer callers guard HWR_CreateDrawNodes this way. Calling its
  // internal unsigned p-1 loop directly at p=0 was never a supported path.
  if(numwalls||numplanes||numpolyplanes)HWR_CreateDrawNodes();
 } else {
  // Exact HWR_Shutdown count/drawcount reset block, no resumed stale access.
  numwalls = numplanes = numpolyplanes = 0;
  drawcount = 0;
  res->list_counts[0]=(unsigned)numwalls;res->list_counts[1]=(unsigned)numplanes;res->list_counts[2]=(unsigned)numpolyplanes;
  r->unwinds++;
 }
 if(numwalls||numplanes||numpolyplanes)abort();
 unsigned nw=(unsigned)ram_original_counts[2];res->wall_count[0]=nw;
 wallinfo_t *walls=ram_old_bases[2]?(wallinfo_t*)ram_old_bases[2]:wallinfo;
 for(unsigned i=0;i<nw;i++)res->wall_surf[0][i]=walls[i].Surf;
 snapshot_materials();res->texsig=hwr_texsig;
 for(unsigned k=0;k<3;k++)free(ram_old_bases[k]);
 free_textures();free(wallinfo);wallinfo=NULL;free(planeinfo);planeinfo=NULL;free(polyplaneinfo);polyplaneinfo=NULL;
}
'''


def prepare(work, negative=0):
    work.mkdir(parents=True, exist_ok=False)
    # Reuse only the already-reviewed host service-boundary declarations. Every
    # production type/function below is re-extracted from this pass's exact tree.
    common = old.COMMON.replace("#define MAXN 64", "#define MAXN 8192")
    actual = old.between((BASE / "src/hardware/hw_defs.h").read_text(),
                         "// Transformed vector, as passed to HWR API", "#ifdef GL_SHADERS")
    defs = (BASE / "src/hardware/hw_defs.h").read_text()
    actual += old.between(defs, "enum EPolyFlags", "enum ESurfFlags")
    actual += old.between(defs, "enum ETextureFlags", "struct FTextureInfo")
    actual += old.between(defs, "struct FLightInfo", "#define GL_DEFAULTMIX")
    actual += old.between((BASE / "src/hardware/hw_data.h").read_text(),
                          "typedef enum GLTextureFormat_e", "// Patch information for the hardware renderer.")
    actual += old.between((BASE / "src/ps2/hw/ps2_hw_fx2.h").read_text(),
                          "typedef struct\n{\n\tfloat r[4][4]", "extern const ps2cull_t")
    actual += "typedef struct { float a,b,c,d; } fr_plane_t;\ntypedef struct { INT32 a,b,c,nd;UINT32 ox,oy,oz; } hwr_fbox_t;\n"
    common = common.replace("typedef struct { int kind,id,texture,light,cmap;", actual + "typedef struct { int kind,id,texture,light,cmap;")
    end = common.rindex("#endif\n")
    common = common[:end] + EXTRA_COMMON + common[end:]
    (work / "common.h").write_text(common)
    pins = {}
    for tag, source in (("base", BASE), ("cur", OUT), ("off", OUT)):
        main = (source / MAIN).read_text()
        layout = old.between(main, "typedef struct\n{\n\tFOutVector    wallVerts[4];", "static int CompareDrawNodes(")
        layout = layout.replace("void HWR_RenderWall(", "static void HWR_RenderWall(")
        layout = layout.replace("void HWR_AddTransparentFloor(", "static void HWR_AddTransparentFloor(")
        layout = layout.replace("void HWR_AddTransparentPolyobjectFloor(", "static void HWR_AddTransparentPolyobjectFloor(")
        if tag == "base":
            layout += "#define HWR_SORTNODE(n) sortnode[n]\n"
        if tag == "cur":
            if negative == 1:
                layout = layout.replace("dn_source.plane[n]", "dn_source.plane[(n + 1) % dn_source.nplane]")
            if negative == 2:
                layout = layout.replace("dn_source.polyplane[n - dn_source.nplane]", "dn_source.polyplane[(n - dn_source.nplane + 1) % dn_source.npolyplane]")
            if negative == 3:
                layout = layout.replace("dn_source.wall[n - dn_source.nplane - dn_source.npolyplane]", "wallinfo[n - dn_source.nplane - dn_source.npolyplane]")
            if negative == 9:
                layout = layout.replace("dn_source.plane[n]", "planeinfo[n]")
            if negative == 10:
                layout = layout.replace("dn_source.polyplane[n - dn_source.nplane]", "polyplaneinfo[n - dn_source.nplane]")
        context = old.CONTEXT
        error = 'static void I_Error(const char *fmt,...) { va_list a;va_start(a,fmt);vfprintf(stderr,fmt,a);va_end(a);abort(); }'
        context = context.replace(error, 'static void I_Error(const char *fmt,...) { ram_res->error_hash=ram_hash(ram_res->error_hash,fmt,strlen(fmt));ram_res->errors++;longjmp(ram_unwind,1); }')
        # Put unwind declarations before the instrumented error function.
        extra_a, extra_b = EXTRA_CONTEXT.split("static int (*ram_cmp)", 1)
        context = extra_a + context
        funcs = []
        lighting = old.static(old.function(main, "HWR_Lighting"), "HWR_Lighting").replace("HWR_Lighting(", "HWR_LightingBody(", 1)
        funcs.append(lighting + "static void HWR_Lighting(FSurfaceInfo*s,INT32 l,extracolormap_t*c){res->lighting_calls++;HWR_LightingBody(s,l,c);}\n")
        batch = (source / "src/hardware/hw_batching.c").read_text()
        cache = (source / "src/hardware/hw_cache.c").read_text()
        funcs += [old.static(old.function(batch, "HWR_SetCurrentTexture"), "HWR_SetCurrentTexture"),
                  old.static(old.function(cache, "HWR_GetTexture"), "HWR_GetTexture"),
                  old.static(old.function(main, "HWR_AddTransparentWall"), "HWR_AddTransparentWall"),
                  old.static(old.function(main, "HWR_RenderWall"), "HWR_RenderWall"),
                  old.function(main, "CompareDrawNodes"), old.function(main, "CompareDrawNodePlanes")]
        create = old.function(main, "HWR_CreateDrawNodes")
        if tag == "cur" and negative == 4:
            create = create.replace("run_end - run_start + 1", "run_end - run_start")
        if tag == "cur" and negative == 5:
            create = create.replace("cap += cap / 2", "cap += cap")
        if tag == "cur" and negative == 6:
            create = create.replace("HWR_GetTexture(HWR_SORTNODE(sortindex[i]).wall->texnum, true);", "((void)0);")
            create = create.replace("HWR_GetTexture(HWR_DN_DRAW.wall->texnum, true);", "((void)0);")
        if tag == "cur" and negative == 7:
            create = create.replace("dn_source.nplane = numplanes;", "dn_source.nplane = (UINT8)numplanes;")
        if tag == "cur" and negative == 8:
            create = create.replace("dn_source.plane = planeinfo;", "dn_source.plane = planeinfo;")
            create = create.replace("const size_t need = numplanes + numpolyplanes + numwalls;", "const size_t need = numplanes + numpolyplanes + numwalls + 1;")
        bottom = old.BOTTOM[:old.BOTTOM.index("void TAG_run(")]
        bottom = bottom.replace("gl_drawnode_t *q=&sortnode[sortindex[i]];", "gl_drawnode_t node=HWR_SORTNODE(sortindex[i]);gl_drawnode_t *q=&node;")
        bottom = bottom.replace("unsigned n=(unsigned)(numwalls+numplanes+numpolyplanes);", "unsigned n=(unsigned)(ram_original_counts[0]+ram_original_counts[1]+ram_original_counts[2]);")
        bottom = bottom.replace("res->sorted[fixture_frame][i]=kind*10000u+id;", "res->sorted[fixture_frame][i]=kind*10000u+id;\n  uintptr_t ptr=(uintptr_t)(q->plane?(void*)q->plane:q->polyplane?(void*)q->polyplane:(void*)q->wall);\n  uintptr_t base=(uintptr_t)(q->plane?(void*)planeinfo:q->polyplane?(void*)polyplaneinfo:(void*)wallinfo);\n  size_t offset=(size_t)(ptr-base);ram_res->pointers_hash=ram_hash(ram_res->pointers_hash,&kind,sizeof kind);ram_res->pointers_hash=ram_hash(ram_res->pointers_hash,&offset,sizeof offset);")
        bottom = bottom.replace("\nstatic void event(", "\nstatic void event(", 1)
        bottom = bottom.replace("e->shader=shader;e->flags=flags;e->surf=*s;", "e->shader=shader;e->flags=flags;e->surf=*s;\n if(ram_callback_mask&2)ram_mutate();\n if(ram_abort_on_event && res->nevent==ram_abort_on_event){ram_res->errors++;longjmp(ram_unwind,1);}")
        bottom = bottom.replace(" }\n}\nstatic void event(", " }\n if(ram_callback_mask&1)ram_mutate();\n}\nstatic void event(", 1)
        context = context.replace("res->selections++; selected_texture=m;if(!m)return;", "res->selections++; selected_texture=m;if(ram_callback_mask&4)ram_mutate();if(!m)return;")
        prefix = '#include "common.h"\n' + ("#define PS2_NOOPT_IMPLICIT_DN 1\n" if tag == "off" else "")
        body = prefix + context + layout + "\n".join(funcs)
        body += "static int (*ram_cmp)" + extra_b + create + bottom + EXTRA_BOTTOM.replace("TAG_", tag + "_")
        body = re.sub(r"(?m)^gl_drawnode_t \*sortnode;", "static gl_drawnode_t *sortnode;", body)
        body = re.sub(r"(?m)^size_t \*sortindex;", "static size_t *sortindex;", body)
        path = work / (tag + "-production.c")
        path.write_text(body)
        pins[tag] = {"sha256": sha(path), "source_sha256": sha(source / MAIN)}
    (work / "extraction.json").write_text(json.dumps({"pins": pins, "negative": negative,
        "boundary": "Exact production types, collection, sort, material selection, lighting and RenderWall; libc qsort and instrumented HWD/zone/plane/lighttable sinks. No EE performance or full-driver claim."}, indent=2) + "\n")


def command(args, log, expected=0):
    proc = subprocess.run([str(x) for x in args], capture_output=True, text=True)
    log.write_text(proc.stdout + proc.stderr)
    if (expected is None and proc.returncode == 0) or (expected is not None and proc.returncode != expected):
        raise RuntimeError(str(log) + " rc=" + str(proc.returncode) + "\n" + proc.stdout[-2000:] + proc.stderr[-4000:])
    return proc


def build(work, negative=0, sanitize=False):
    prepare(work, negative)
    flags = ["-std=gnu17", "-O2", "-DPS2", "-DPS2_PROFILE", "-ffp-contract=off", "-fwrapv", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-parameter",
             "-Wno-missing-field-initializers", "-Wno-sign-compare", "-Wno-misleading-indentation"]
    if sanitize:
        flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
    binary = work / "test"
    command(["gcc", *flags, "-I" + str(work), *(work / (tag + "-production.c") for tag in ("base", "cur", "off")),
             ROOT / "tools/ps2/next13_ram_hosttest.c", "-o", binary, "-lm"], work / "build.log")
    result = command([binary], work / "test.log", None if negative else 0)
    rows = [{"mode": "main", "rc": result.returncode, "output": result.stdout.strip()}]
    if not negative:
        for mode in ("index-failure", "node-failure", "callback-unwind", "duplicate-drawcount"):
            proc = command([binary, mode], work / (mode + ".log"))
            rows.append({"mode": mode, "rc": proc.returncode, "output": proc.stdout.strip()})
    return {"negative": negative, "sanitize": sanitize, "binary_sha256": sha(binary), "rows": rows}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=ROOT / "build/nov15-next13/ram-host-v1")
    parser.add_argument("--candidate", type=Path, default=OUT)
    parser.add_argument("--skip-negative", action="store_true")
    args = parser.parse_args()
    global_candidate = args.candidate.resolve()
    globals()["OUT"] = global_candidate
    if args.out.exists():
        raise RuntimeError("Use a fresh immutable output directory")
    args.out.mkdir(parents=True)
    (args.out / "tools").mkdir()
    for p in (Path(__file__), ROOT / "tools/ps2/next13_ram_hosttest.c",
              ROOT / "tools/ps2/next13_ram_variant.py", ROOT / "tools/ps2/next12_cull_hosttest.py"):
        shutil.copyfile(p, args.out / "tools" / p.name)
    rows = [build(args.out / "positive"), build(args.out / "sanitize", sanitize=True)]
    if not args.skip_negative:
        rows += [build(args.out / ("negative-" + str(n)), negative=n) for n in range(1, 11)]
    report = {"status": "host_component_only", "target_or_emulator_used": False, "rows": rows,
              "tools": {str(p.relative_to(ROOT)): sha(p) for p in (Path(__file__),
                        ROOT / "tools/ps2/next13_ram_hosttest.c", ROOT / "tools/ps2/next13_ram_variant.py",
                        ROOT / "tools/ps2/next12_cull_hosttest.py")}}
    (args.out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
