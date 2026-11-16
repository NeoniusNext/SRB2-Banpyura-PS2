static int live_payload(const void *p)
{
    for(zablock_t *b=ZA_First();b;b=ZA_Next(b))if(!ZA_ISFREE(b)&&ZA_PAYLOAD(b)==p)return 1;
    return 0;
}
int main(void)
{
    arena_reset(8u<<20,0,0,0);Z_Init();memset(&gc,0,sizeof gc);
    for(int cycle=0;cycle<64;cycle++)
    {
        gc.kb_user=(cycle&1)!=0;gc.kb=640;gc.dead=true;gc.tries=3;gc.on=true;
        gc.bypass=gc.bypass_len=gc.w_views=gc.w_q=gc.w_hit=gc.w_full=17;
        void *old=Z_TryMallocAlign(818176,PU_HWRPLANE,&gc.blk,4);check(old!=NULL,"old geometry block");sticky=(UINT8*)old+64;
        if(cycle%3==1){Z_FreeTags(PU_HWRPLANE,PU_HWRPLANE);check(!gc.blk,"real level teardown clears gc owner");}
        const unsigned before=detaches;HWR_GCacheLevel();
        check(!gc.blk&&!sticky&&detaches==before+1,"reset detaches state and clears owner");
        check(!gc.dead&&!gc.tries&&!gc.on&&!gc.bypass&&!gc.bypass_len&&!gc.w_views&&!gc.w_q&&!gc.w_hit&&!gc.w_full,"cache reset policy is unchanged");
        check(gc.kb==(gc.kb_user?640u:GC_DEFAULT_KB),"cache sizing policy is unchanged");
#ifdef EXPECT_ORPHAN
        check(live_payload(old),"baseline old owned block must survive the reset");
#else
        check(!live_payload(old),"reset must release any still-live old owner");
        Z_CheckHeap(-1139);
#endif
        void *now=Z_TryMallocAlign(490496,PU_HWRPLANE,&gc.blk,4);check(now!=NULL&&gc.blk==now,"replacement publishes current Zone owner");
#ifdef EXPECT_ORPHAN
        expecting_error=1;
        if(setjmp(error_jump)==0){Z_CheckHeap(-1140);check(0,"baseline must reproduce gc proper-user error");}
        expecting_error=0;check(strstr(error_text,"doesn't have a proper user")!=NULL,"exact gc Zone diagnostic");
        printf("GC EXPECTED RED old=%p new=%p user=%p %s\n",old,now,(void*)&gc.blk,error_text);
        puts("GC LIFETIME COMPLETE expected_orphan=1");return 0;
#else
        Z_CheckHeap(-1140);consistent();Z_Free(gc.blk);check(!gc.blk,"replacement Zone teardown clears owner");
#endif
    }
    printf("GC LIFETIME COMPLETE checks=%u resets=64 live_and_zone_freed=PASS\n",checks);return 0;
}
