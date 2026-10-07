/* OPT10-S: link shim of the host ASan profile (tools/ps2/host_asan.sh). The CMake host profile does not compile the net/file-transfer units of the EE build
 * (d_netfil.c, mserv.c, http-mserv.c are compiled by hand there); what those and the engine call from the EE-only network layer is stubbed here:
 * no network exists in the host profile runs. */
#include <unistd.h>

int PS2Net_Unlink(const char *p) { return unlink(p); }
void *PS2HttpGet_Open(const char *u, long a, int b, const char *c) { (void)u; (void)a; (void)b; (void)c; return 0; }
int PS2HttpGet_Step(void *g, void *w, void *d, long *s, long *t, long *got, char *e, unsigned long es)
{ (void)g; (void)w; (void)d; (void)s; (void)t; (void)got; (void)e; (void)es; return -1; }
void PS2HttpGet_Close(void *g) { (void)g; }
int PS2Net_Up(void) { return 0; }
int Net_IsNodeIPv6(int n) { (void)n; return 0; }
void StoreLuaFileCallback(int id) { (void)id; }
int I_InitTcpNetwork(void) { return 0; }
