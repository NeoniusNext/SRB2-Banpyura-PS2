/* Actual HWR_GetLevelFlat/HWR_FreeMapTextures and real PS2 Zone are above. */
static int zone_live(const void *p)
{
    for(zablock_t *b=ZA_First();b;b=ZA_Next(b))if(!ZA_ISFREE(b)&&ZA_PAYLOAD(b)==p)return 1;
    return 0;
}
int main(void)
{
    for(int cycle=0;cycle<12;cycle++)for(steal_child=0;steal_child<2;steal_child++)
    {
        arena_reset(8u<<20,0,0,0);Z_Init();driver_owned=NULL;retired_child=NULL;retired_storage=NULL;
        texture.width=cycle&1?1024:64;texture.height=cycle&1?1024:64;
        const size_t bytes=(size_t)texture.width*texture.height;
        flat_source=malloc(bytes);check(flat_source!=NULL,"fixture raw flat allocation");for(size_t i=0;i<bytes;i++)flat_source[i]=(UINT8)(i*37u+cycle);
        HWR_InitMapTextures();HWR_LoadMapTextures(1);
        levelflat_t flat={1,0};HWR_GetLevelFlat(&flat,false);HWR_GetLevelFlat(&flat,true);
#ifdef HWR_TEXTURE_PAGES
        GLMipmap_t *parent=HWR_FindFlatMipmap(0,false);
#elif !defined(PS2_NOOPT_FLAT_METADATA)
        GLMipmap_t *parent=HWR_FlatMipmap(0);
#else
        GLMipmap_t *parent=&gl_flats[0].mipmap;
#endif
        GLMipmap_t *child=parent->nextcolormap;check(child&&child->ps2_twin==parent&&child->regen_kind==2,"actual keyed flat creation owns a recipe");
        void *old=steal_child?driver_owned:child->data;
        check(parent->data&&old&&old!=parent->data,"profile flat child owns an independent allocation");
        check(!memcmp(old,flat_source,bytes),"actual recipe copy bytes");
        retired_child=child;
#ifdef OWNER_REUSE
        hold_retired=1;
#else
        hold_retired=0;
#endif
        HWR_FreeMapTextures();
#ifdef EXPECT_ORPHAN
        if(!steal_child)
        {
            check(zone_live(old),"baseline must retain the child Zone allocation after metadata retirement");
#ifdef OWNER_REUSE
            expecting_error=1;
            if(setjmp(error_jump)==0){Z_CheckHeap(-1140);check(0,"baseline must reproduce proper-user error");}
            expecting_error=0;check(strstr(error_text,"doesn't have a proper user")!=NULL,"exact Zone user-contract diagnostic");
            printf("FLAT EXPECTED RED %s\n",error_text);Z_Free(old);free(retired_storage);retired_storage=NULL;
#else
            printf("FLAT EXPECTED RED live Zone child survives actual engine metadata free\n");
            // Do not ask Zone to dereference the freed libc metadata in this mode.
#endif
            free(flat_source);puts("FLAT LIFETIME COMPLETE expected_orphan=1");return 0;
        }
#else
        check(!zone_live(old),"no live Zone block may retain a retired child user");
#endif
        check(!gl_textures&&!gl_flats&&!gl_numtextures&&!driver_owned,"engine and simulated driver source teardown");
        Z_CheckHeap(-1140);consistent();
        free(retired_storage);retired_storage=NULL;free(flat_source);
    }
    printf("FLAT LIFETIME COMPLETE checks=%u cycles=24 source_copy_steal=PASS\n",checks);return 0;
}
