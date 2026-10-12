/*
** NL sandbox profile for untrusted mods (NL_OPT_SANDBOX, API.md 2.2,
** IMPLEMENTATION_PLAN M9): the restricted global environment, and the
** text-only loaders it hands to the code that runs in it.
**
** While the option is 1, every chunk loaded through NL's entry points
** (nl_loadbuffer, nl_loadfile, nl_import, require of a .nl file, include,
** load_nl, and the load functions below) gets the sandbox environment as its
** function environment, and none of them takes a bytecode chunk. The
** environment is made once per state, the first time it is needed, and
** shared by all sandboxed chunks, so mods see each other's globals.
**
** What generated NL code reads as globals is the LuaGlobal half of the
** prelude (crates/nl_sema/src/prelude.rs): Lua's library tables under their
** NL names and Lua's free functions. Everything else NL code uses comes
** from the runtime table, which a chunk gets as its argument (runtime/nl.lua,
** LOADED CHUNKS) or through KINTR, so it does not depend on the environment.
*/

#define lj_nlsandbox_c
#define LUA_CORE

#include <stdio.h>
#include <string.h>

#include "lj_nlint.h"

#if LJ_NL

#include "lauxlib.h"
#include "lj_err.h"

/* Taken from the globals as they are when the environment is made. Absent
** ones are skipped.
*/
static const char *const sandbox_globals[] = {
  /* The safe part of the base library. print is safe: it writes, and it
  ** finds tostring in the state's globals, not in the caller's.
  */
  "assert", "error", "ipairs", "next", "pairs", "pcall", "print", "select",
  "tonumber", "tostring", "type", "unpack", "xpcall", "rawequal", "rawget",
  "rawset", "rawlen", "getmetatable", "setmetatable", "_VERSION",
  /* Library tables, shared with the state as they are (math is NL's Number,
  ** and the number metatable's __index).
  */
  "table", "math", "bit", "coroutine",
  /* What NL adds (DESIGN 1, 13.1): the prelude functions, String, and the
  ** event and worker libraries.
  */
  "String", "Regex", "typeof", "raise", "include", "add_path", "load_nl",
  "keyed", "event", "worker",
  NULL
};

/* Modules that require refuses in the sandbox even from package.preload:
** the FFI, the jit modules, and string.buffer, whose buf:ref() hands out a
** pointer that indexes raw memory without the ffi module.
*/
static int sandbox_denied(const char *name)
{
  return strcmp(name, "ffi") == 0 || strcmp(name, "jit") == 0 ||
	 strncmp(name, "jit.", 4) == 0 || strcmp(name, "string.buffer") == 0;
}

/* -- Text-only loading ---------------------------------------------------- */

/* Ends a load: the function with the environment at envidx, or nil and the
** message. A bytecode chunk was refused by the mode "t" check of lua_loadx;
** its message is replaced by the sandbox's.
*/
static int sandbox_finish(lua_State *L, int status, int envidx)
{
  if (status == 0) {
    lua_pushvalue(L, envidx);
    lua_setfenv(L, -2);
    return 1;
  } else {
    const char *msg = lua_tostring(L, -1);
    if (status == LUA_ERRSYNTAX && msg &&
	strcmp(msg, err2msg(LJ_ERR_XMODE)) == 0) {
      lua_pop(L, 1);
      lua_pushliteral(L, NL_SANDBOX_NOBC);
    }
    lua_pushnil(L);
    lua_insert(L, -2);
    return 2;
  }
}

/* A Lua file, as loadfile loads it but text only, with the environment at
** envidx. A name ending in ".nl" goes to nl_loadfile (lj_nl_loadfilex),
** which applies the sandbox itself.
*/
static int sandbox_loadfile(lua_State *L, const char *filename, int envidx)
{
  return sandbox_finish(L, luaL_loadfilex(L, filename, "t"), envidx);
}

/* The reader of load(f): as lib_base.c's, with the string anchored in
** slot 5.
*/
static const char *sandbox_reader(lua_State *L, void *ud, size_t *size)
{
  UNUSED(ud);
  luaL_checkstack(L, 2, "too many nested functions");
  lua_pushvalue(L, 1);
  lua_call(L, 0, 1);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    *size = 0;
    return NULL;
  }
  if (!lua_isstring(L, -1))
    luaL_error(L, "reader function must return a string");
  lua_replace(L, 5);
  return lua_tolstring(L, 5, size);
}

/* load(chunk [, name [, mode [, env]]]) and loadstring in the sandbox, with
** the sandbox environment as upvalue 1: Lua's, except that the mode is
** always "t" and the chunk gets the sandbox environment unless env is a
** table.
*/
static int sandbox_load(lua_State *L)
{
  const char *name = luaL_optstring(L, 2, NULL);
  int status;
  lua_settop(L, 4);
  if (lua_type(L, 1) == LUA_TSTRING || lua_type(L, 1) == LUA_TNUMBER) {
    size_t len;
    const char *s = lua_tolstring(L, 1, &len);
    status = luaL_loadbufferx(L, s, len, name ? name : s, "t");
  } else {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushnil(L);  /* Slot 5: the reader's string. */
    status = lua_loadx(L, sandbox_reader, NULL, name ? name : "=(load)", "t");
  }
  return sandbox_finish(L, status,
			lua_istable(L, 4) ? 4 : lua_upvalueindex(1));
}

/* -- require -------------------------------------------------------------- */

/* The mark of a module being loaded; only its address is used. */
static const char sandbox_loading = 0;
#define SANDBOX_LOADING	((void *)&sandbox_loading)

/* Each searcher pushes the module's loader and returns 1, or pushes a
** message (or nothing) and returns 0. Upvalues of require: 1 the sandbox
** environment, 2 its own loaded table, 3 the state's package table or nil.
*/

static int search_preload(lua_State *L, const char *name)
{
  if (!lua_istable(L, lua_upvalueindex(3))) return 0;
  lua_getfield(L, lua_upvalueindex(3), "preload");
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, name);
    if (lua_isfunction(L, -1)) {
      lua_remove(L, -2);
      return 1;
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  lua_pushfstring(L, "\n\tno field package.preload['%s']", name);
  return 0;
}

/* Lua files on package.path, loaded text only with the sandbox
** environment. The loader is the loaded chunk itself.
*/
static int search_lua(lua_State *L, const char *name)
{
  const char *path, *p;
  int top;
  if (!lua_istable(L, lua_upvalueindex(3))) return 0;
  lua_getfield(L, lua_upvalueindex(3), "path");
  path = lua_tostring(L, -1);
  if (!path) {
    lua_pop(L, 1);
    return 0;
  }
  name = luaL_gsub(L, name, ".", LUA_DIRSEP);
  top = lua_gettop(L);  /* path, name; the messages go above. */
  for (p = path; ; ) {
    const char *e, *fname;
    FILE *fp;
    while (*p == *LUA_PATHSEP) p++;
    if (*p == '\0') break;
    e = strchr(p, *LUA_PATHSEP);
    if (!e) e = p + strlen(p);
    lua_pushlstring(L, p, (size_t)(e - p));
    fname = luaL_gsub(L, lua_tostring(L, -1), LUA_PATH_MARK, name);
    lua_remove(L, -2);
    p = e;
    fp = fopen(fname, "r");
    if (fp) {
      fclose(fp);
      if (sandbox_loadfile(L, fname, lua_upvalueindex(1)) != 1)
	luaL_error(L, "error loading module " LUA_QS " from file " LUA_QS
		   ":\n\t%s", lua_tostring(L, 1), fname, lua_tostring(L, -1));
      lua_replace(L, top - 1);
      lua_settop(L, top - 1);
      return 1;
    }
    lua_pushfstring(L, "\n\tno file " LUA_QS, fname);
    lua_remove(L, -2);
  }
  lua_concat(L, lua_gettop(L) - top);
  lua_replace(L, top - 1);
  lua_settop(L, top - 1);
  return 0;
}

/* .nl files: the runtime's searcher (rt.searcher, runtime/nl_70_module.lua),
** which loads through rt.load and so under the sandbox.
*/
static int search_nl(lua_State *L, const char *name)
{
  if (!lj_nl_getrt(L, "searcher")) return 0;
  lua_pushstring(L, name);
  lua_call(L, 1, 1);
  if (lua_isfunction(L, -1)) return 1;
  if (!lua_isstring(L, -1)) lua_pop(L, 1);
  return 0;
}

static int sandbox_require(lua_State *L)
{
  const char *name = luaL_checkstring(L, 1);
  lua_settop(L, 1);
  lua_getfield(L, lua_upvalueindex(2), name);
  if (lua_toboolean(L, -1)) {
    if (lua_touserdata(L, -1) == SANDBOX_LOADING)
      luaL_error(L, "loop or previous error loading module " LUA_QS, name);
    return 1;
  }
  lua_pop(L, 1);
  if (sandbox_denied(name))
    luaL_error(L, "module " LUA_QS " is not available in the sandbox", name);
  if (search_preload(L, name) || search_lua(L, name) || search_nl(L, name)) {
    lua_insert(L, 2);  /* The loader; the messages above it go. */
    lua_settop(L, 2);
    lua_pushlightuserdata(L, SANDBOX_LOADING);
    lua_setfield(L, lua_upvalueindex(2), name);
    lua_pushvalue(L, 1);
    lua_call(L, 1, 1);
    if (!lua_isnil(L, -1))
      lua_setfield(L, lua_upvalueindex(2), name);
    lua_settop(L, 1);
    lua_getfield(L, lua_upvalueindex(2), name);
    if (lua_touserdata(L, -1) == SANDBOX_LOADING) {
      lua_pushboolean(L, 1);
      lua_pushvalue(L, -1);
      lua_setfield(L, lua_upvalueindex(2), name);
    }
    return 1;
  }
  lua_pushfstring(L, "module " LUA_QS " not found:", name);
  lua_insert(L, 2);
  lua_concat(L, lua_gettop(L) - 1);
  return lua_error(L);
}

/* -- The environment ------------------------------------------------------ */

static void sandbox_new(lua_State *L)
{
  int env, i;
  lua_createtable(L, 0, 48);
  env = lua_gettop(L);
  for (i = 0; sandbox_globals[i]; i++) {
    lua_getfield(L, LUA_GLOBALSINDEX, sandbox_globals[i]);
    lua_setfield(L, env, sandbox_globals[i]);
  }
  /* string without dump. Strings still index the full table through their
  ** metatable; a dump made that way cannot be loaded here.
  */
  lua_getfield(L, LUA_GLOBALSINDEX, "string");
  if (lua_istable(L, -1)) {
    lua_newtable(L);
    lua_pushnil(L);
    while (lua_next(L, -3)) {
      if (lua_type(L, -2) == LUA_TSTRING &&
	  strcmp(lua_tostring(L, -2), "dump") == 0) {
	lua_pop(L, 1);
      } else {
	lua_pushvalue(L, -2);
	lua_insert(L, -2);
	lua_rawset(L, -4);
      }
    }
    lua_setfield(L, env, "string");
  }
  lua_pop(L, 1);
  lua_pushvalue(L, env);
  lua_pushcclosure(L, sandbox_load, 1);
  lua_pushvalue(L, -1);
  lua_setfield(L, env, "load");
  lua_setfield(L, env, "loadstring");
  lua_pushvalue(L, env);
  lua_newtable(L);  /* The sandbox's own package.loaded. */
  lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
  if (lua_istable(L, -1))
    lua_getfield(L, -1, "package");
  else
    lua_pushnil(L);
  lua_remove(L, -2);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_pushnil(L);
  }
  lua_pushcclosure(L, sandbox_require, 3);
  lua_setfield(L, env, "require");
  lua_pushvalue(L, env);
  lua_setfield(L, env, "_G");
}

LUA_API void nl_sandboxenv(lua_State *L)
{
  NLState *ns = nl_state(L);
  if (ns->sandboxref != LUA_NOREF) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, ns->sandboxref);
    if (lua_istable(L, -1)) return;
    lua_pop(L, 1);
    ns->sandboxref = LUA_NOREF;
  }
  sandbox_new(L);
  lua_pushvalue(L, -1);
  ns->sandboxref = luaL_ref(L, LUA_REGISTRYINDEX);
}

/* -- The host table (runtime/nl.lua, HOST TABLE) -------------------------- */

/* host.sandbox(f) -> f: in the sandbox, f gets the sandbox environment.
** rt.load calls it on each chunk from the compiler before running it, so
** the file's functions inherit the environment.
*/
int lj_nl_hostsandbox(lua_State *L)
{
  luaL_checktype(L, 1, LUA_TFUNCTION);
  lua_settop(L, 1);
  if (lj_nl_sandboxed(L)) {
    nl_sandboxenv(L);
    lua_setfenv(L, 1);
  }
  return 1;
}

/* host.loadfile(path) -> function | nil, message: loadfile, and in the
** sandbox text only with the sandbox environment (include of a Lua file).
*/
int lj_nl_hostloadfile(lua_State *L)
{
  const char *path = luaL_checkstring(L, 1);
  lua_settop(L, 1);
  if (lj_nl_sandboxed(L)) {
    nl_sandboxenv(L);
    return sandbox_loadfile(L, path, 2);
  }
  if (luaL_loadfile(L, path) == 0) return 1;
  lua_pushnil(L);
  lua_insert(L, -2);
  return 2;
}

#endif
