#ifndef __LUA_HTTPLIB_H__
#define __LUA_HTTPLIB_H__

#include "lua_script.h"

#ifndef HAS_LUA
static inline void LUA_HTTPProcessCallbacks(void) {}
#else
int LUA_HTTPLib(lua_State *L);
void LUA_HTTPProcessCallbacks(void);
#endif

#endif