/* OPT13-RCACHE: link shim for the host cachegrind build (HEAD's host profile misses NetLat_* and ps2_fxfrac; the demos never use them). Not part of the product. */
typedef unsigned int u32; typedef int s32; typedef unsigned long long u64;
s32 ps2_fxfrac;
void NetLat_TicMade(u32 t) { (void)t; }
void NetLat_ClientPacket(s32 n, u32 w, u32 a) { (void)n; (void)w; (void)a; }
void NetLat_ServerTics(u32 f, u32 e, u32 n) { (void)f; (void)e; (void)n; }
void NetLat_TicRun(u32 t, s32 b) { (void)t; (void)b; }
void NetLat_Poll(void) {}
void NetLat_RxAge(u64 a) { (void)a; }
void NetLat_RunPass(void) {}
void NetLat_Pass(s32 t, s32 r) { (void)t; (void)r; }
void NetLat_CmdMissed(s32 p) { (void)p; }
void NetLat_TicHole(void) {}
void NetLat_Frame(void) {}
void NetLat_Sent(s32 t, s32 b) { (void)t; (void)b; }
