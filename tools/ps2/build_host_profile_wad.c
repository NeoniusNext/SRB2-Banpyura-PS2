// Native PS2_PROFILE platform glue: locate existing packs, not the stock pk3.
// The SDL system backend otherwise remains intact (clock, video, sound, home).
#include "doomdef.h"
#include "i_system.h"

const char *I_LocateWad(void)
{
	const char *directory = I_GetEnv("SRB2WADDIR");
	char path[1024];
	FILE *pack;
	int length = snprintf(path, sizeof(path), "%s/SRB2.PAK", directory ? directory : ".");
	if (length < 0 || (size_t)length >= sizeof(path))
		I_Error("Host PS2_PROFILE pack directory path is too long");
	pack = fopen(path, "rb");
	if (!pack)
		I_Error("Host PS2_PROFILE: %s not found; set SRB2WADDIR to the pack directory", path);
	fclose(pack);
	I_OutputMsg("Host PS2_PROFILE packs: %s\n", directory ? directory : ".");
	return directory;
}

// OPT10-SW: netcode files call PS2Net_Unlink under PS2_PROFILE (EE: src/ps2/ps2_net.c); on the host it is plain unlink
#include <unistd.h>
int PS2Net_Unlink(const char *path) { return unlink(path); }
// host stubs of the EE network/HTTP layer (the demos never touch the network)
#include "ps2/ps2_curl.h"
boolean PS2Net_Up(void) { return false; }
ps2_httpget_t *PS2HttpGet_Open(const char *url, long stall_seconds, int maxredirs, const char *useragent) { (void)url; (void)stall_seconds; (void)maxredirs; (void)useragent; return NULL; }
int PS2HttpGet_Step(ps2_httpget_t *g, ps2curl_write_fn write_fn, void *userdata, long *status, long *total, long *got, char *errbuf, size_t errsize) { (void)g; (void)write_fn; (void)userdata; (void)status; (void)total; (void)got; (void)errbuf; (void)errsize; return -1; }
void PS2HttpGet_Close(ps2_httpget_t *g) { (void)g; }
void StoreLuaFileCallback(INT32 id) { (void)id; }
INT32 ps2_fxfrac; // EE: src/ps2/i_video.c (-fxfrac); d_main.c reads it under PS2_PROFILE

// RTICK: link fixes of the host profile at main 4bce0a6 (OPT12 merges): NetLat_* are EE-only sources; ps2_nearest.c is compiled into the host binary here
#include "netcode/netlat.h"
#undef NetLat_TicMade
#undef NetLat_ClientPacket
#undef NetLat_ServerTics
#undef NetLat_TicRun
#undef NetLat_Poll
#undef NetLat_CmdMissed
#undef NetLat_TicHole
#undef NetLat_Frame
#undef NetLat_Sent
void NetLat_TicMade(tic_t t) { (void)t; }
void NetLat_ClientPacket(INT32 n, tic_t w, tic_t a) { (void)n; (void)w; (void)a; }
void NetLat_ServerTics(tic_t f, tic_t e, tic_t n) { (void)f; (void)e; (void)n; }
void NetLat_TicRun(tic_t t, INT32 b) { (void)t; (void)b; }
void NetLat_Poll(void) {}
void NetLat_CmdMissed(INT32 p) { (void)p; }
void NetLat_TicHole(void) {}
void NetLat_Frame(void) {}
void NetLat_Sent(INT32 t, INT32 b) { (void)t; (void)b; }
#include "ps2/ps2_nearest.c"
