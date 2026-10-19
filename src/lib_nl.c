/*
** luaopen_nl: opens everything NL adds to a state.
** luaL_openlibs calls it after the stock libraries.
**
** Order: the C event library first, so the runtime finds the global `event`
** and does not install its Lua stand-in; then the runtime chunk, called as
** `rt = chunk(host)` (runtime/nl.lua, header). It installs the prelude globals
** and returns rt, which the C API keeps in the registry.
*/

#define lib_nl_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"

#include "lj_nlint.h"

#if LJ_NL


LUALIB_API int luaopen_nl(lua_State *L)
{
  NLState *ns = nl_state(L);
  if (ns->rtref != LUA_NOREF) return 0;  /* Already open. */
  lua_pushcfunction(L, luaopen_event);
  lua_call(L, 0, 0);
  lj_nl_inittrampoline(L);
  if (lj_nlrt_size == 0) return 0;  /* Bootstrap build: no runtime yet. */
  if (luaL_loadbuffer(L, (const char *)lj_nlrt_bc, lj_nlrt_size,
		      "=[nl runtime]") != 0)
    lua_error(L);
  lj_nl_openhost(L);
  if (ns->hostinit) {  /* The embedder's fields (nl_sethostinit). */
    lua_pushcfunction(L, ns->hostinit);
    lua_pushvalue(L, -2);
    lua_call(L, 1, 1);
    lua_insert(L, -3);  /* What to call with rt, below the chunk. */
  }
  lua_call(L, 1, 1);
  if (!lua_istable(L, -1))
    luaL_error(L, "the NL runtime returned %s, not a table",
	       luaL_typename(L, -1));
  lj_nl_openintr(L);  /* The intrinsic table of KINTR, from rt. */
  ns->rtref = luaL_ref(L, LUA_REGISTRYINDEX);
  if (ns->hostinit) {
    if (lua_isfunction(L, -1)) {
      lua_rawgeti(L, LUA_REGISTRYINDEX, ns->rtref);
      lua_call(L, 1, 0);
    } else {
      lua_pop(L, 1);
    }
  }
  /* include(path) is loadfile and a call. The runtime may define its own;
  ** it is installed only when it did not.
  */
  lua_getglobal(L, "include");
  if (lua_isnil(L, -1)) {
    lua_pushcfunction(L, lj_nl_include);
    lua_setglobal(L, "include");
  }
  lua_pop(L, 1);
  return 0;
}

#endif
