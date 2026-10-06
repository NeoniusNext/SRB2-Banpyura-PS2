// SRB2 PS2 port: what the netcode needs from the platform and from subsystems that are restored elsewhere.
//
// PS2-01 (OPT7: PS2-120..139) The socket driver is the real src/netcode/i_tcp.c over the PS2 IP stack (src/ps2/ps2_net.c), the file transfers are
// the real src/netcode/d_netfil.c and the master server is the real src/netcode/mserv.c + http-mserv.c, whose libcurl calls land in the HTTP client
// of src/ps2/ps2_curl.c (PS2-130). What stays here is glue.

#include "../doomdef.h"
#include "../doomstat.h"
#include "../command.h"
#include "../d_main.h"
#include "../m_argv.h"
#include "../z_zone.h"
#include "../netcode/i_net.h"
#include "../netcode/d_net.h"
#include "../netcode/d_clisrv.h"
#include "../netcode/mserv.h"
#include "../netcode/protocol.h"

boolean I_InitNetwork(void) // only an external DOS-style driver would return true
{
	return false;
}

// Weak: the real Lua library (blua/liolib.c) and the add-on code (p_setup.c) replace them when they are linked.
__attribute__((weak)) void MakePathDirs(char *path)
{
	(void)path;
}

__attribute__((weak)) void RemoveLuaFileCallback(INT32 id)
{
	(void)id;
}

__attribute__((weak)) boolean P_AddFolder(const char *folderpath)
{
	(void)folderpath;
	return false;
}
