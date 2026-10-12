/*
** NL sandbox profile for untrusted mods (NL_OPT_SANDBOX, API.md 2.2,
** IMPLEMENTATION_PLAN M9): the restricted global environment, and the
** text-only loaders it hands to the code that runs in it.
**
** While the option is 1, every chunk loaded through NL's entry points
** (nl_loadbuffer, nl_loadfile, nl_import, require of a .nl file, include,
** load_nl, and the load functions below) gets the sandbox environment as its
** function environment, and none of them takes a bytecode chunk. The
** environment is one table per state, shared by all sandboxed chunks, so
** mods see each other's globals. It is filled additively: each call of
** nl_sandboxenv (and so each sandboxed load) adds the names it lacks, so a
** table made before luaL_openlibs gets the libraries once they exist.
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

/* Taken from the state's globals as they are; absent ones are skipped. */
static const char *const sandbox_globals[] = {
  /* The safe part of the base library. print is safe: it writes, and it
  ** finds tostring in the state's globals, not in the caller's.
  */
  "assert", "error", "ipairs", "next", "pairs", "pcall", "print", "select",
  "tonumber", "tostring", "type", "unpack", "xpcall", "rawequal", "rawget",
  "rawset", "rawlen", "setmetatable", "_VERSION",
  /* What NL adds (DESIGN 1, 13.1): the prelude functions, String and the
  ** event library (a bus shared with the host, API.md 2.2).
  */
  "String", "typeof", "raise", "include", "add_path", "load_nl", "keyed",
  "event",
  NULL
};

/* Library tables the environment gets copies of, so that a mod cannot
** change what the host and the runtime call. A copy reads the fields the
** state's table gets later through __index: primitive methods defined on
** Number go into math (rt.primmethod).
*/
static const char *const sandbox_libs[] = {
  "table", "math", "bit", "coroutine", NULL
};

/* Modules that require refuses in the sandbox whatever the preload holds:
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
** its message is replaced by the sandbox's. (A bytecode chunk behind a "#"
** line or a BOM is refused by LuaJIT's lexer with its own message.)
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

/* A Lua source file for the sandbox, text only, with the environment at
** envidx (an absolute or upvalue index). With a host loader (nl_setloader)
** the file is read through it, by the name as given, as include reads .nl
** files (API.md 2.3); otherwise from disk. A name ending in ".nl" goes to
** nl_loadfile (lj_nl_loadfilex), which applies the sandbox itself.
** Returns 1 (the function pushed), 2 (nil and a message), or 0 when there
** is no such file (nothing pushed).
*/
static int sandbox_loadlua(lua_State *L, const char *fname, int envidx)
{
  NLState *ns = nl_state(L);
  if (lj_nl_isnlfile(fname)) {
    int st;
    if (!ns->hasloader) {
      FILE *fp = fopen(fname, "r");
      if (!fp) return 0;
      fclose(fp);
    }
    st = luaL_loadfilex(L, fname, "t");
    if (st == LUA_ERRFILE && ns->hasloader) {
      lua_pop(L, 1);
      return 0;
    }
    return sandbox_finish(L, st, envidx);
  } else if (ns->hasloader) {
    nl_Loader ld = ns->loader;
    const char *chunkname = lua_pushfstring(L, "@%s", fname);
    size_t size = 0;
    const char *buf = ld.read(ld.data, fname, &size);
    int st;
    if (!buf) {
      lua_pop(L, 1);
      return 0;
    }
    st = luaL_loadbufferx(L, buf, size, chunkname, "t");  /* Protected. */
    if (ld.release) ld.release(ld.data, fname, buf);
    lua_remove(L, -2);
    return sandbox_finish(L, st, envidx);
  } else {
    FILE *fp = fopen(fname, "r");
    if (!fp) return 0;
    fclose(fp);
    return sandbox_finish(L, luaL_loadfilex(L, fname, "t"), envidx);
  }
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

/* getmetatable in the sandbox: nil for strings, numbers and booleans, whose
** metatables are shared with the host (a mod could replace the methods the
** host's own code calls); Lua's for anything else.
*/
static int sandbox_getmetatable(lua_State *L)
{
  int t;
  luaL_checkany(L, 1);
  t = lua_type(L, 1);
  if (t == LUA_TSTRING || t == LUA_TNUMBER || t == LUA_TBOOLEAN ||
      !lua_getmetatable(L, 1)) {
    lua_pushnil(L);
    return 1;
  }
  luaL_getmetafield(L, 1, "__metatable");  /* Pushes it when present. */
  return 1;
}

/* -- require -------------------------------------------------------------- */

/* The mark of a module being loaded; only its address is used. */
static const char sandbox_loading = 0;
#define SANDBOX_LOADING	((void *)&sandbox_loading)

/* Each searcher pushes the module's loader and returns 1, or pushes a
** message (or nothing) and returns 0. Upvalues of require: 1 the sandbox
** environment, 2 the sandbox's package table, 3 its loaded table.
*/

static int search_preload(lua_State *L, const char *name)
{
  lua_getfield(L, lua_upvalueindex(2), "preload");
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

/* Lua source files on the sandbox's package.path (sandbox_loadlua). The
** loader is the loaded chunk itself. The messages are joined as they come,
** so that a long path needs no stack.
*/
static int search_lua(lua_State *L, const char *name)
{
  const char *path, *p;
  int top;
  lua_getfield(L, lua_upvalueindex(2), "path");
  path = lua_tostring(L, -1);
  if (!path) {
    lua_pop(L, 1);
    return 0;
  }
  name = luaL_gsub(L, name, ".", LUA_DIRSEP);
  lua_pushliteral(L, "");
  top = lua_gettop(L);  /* path, name, the message so far. */
  for (p = path; ; ) {
    const char *e, *fname;
    int r;
    while (*p == *LUA_PATHSEP) p++;
    if (*p == '\0') break;
    e = strchr(p, *LUA_PATHSEP);
    if (!e) e = p + strlen(p);
    lua_pushlstring(L, p, (size_t)(e - p));
    fname = luaL_gsub(L, lua_tostring(L, -1), LUA_PATH_MARK, name);
    lua_remove(L, -2);
    p = e;
    r = sandbox_loadlua(L, fname, lua_upvalueindex(1));
    if (r == 2)
      luaL_error(L, "error loading module " LUA_QS " from file " LUA_QS
		 ":\n\t%s", lua_tostring(L, 1), fname, lua_tostring(L, -1));
    if (r == 1) {
      lua_replace(L, top - 2);
      lua_settop(L, top - 2);
      return 1;
    }
    lua_pushfstring(L, "\n\tno file " LUA_QS, fname);
    lua_remove(L, -2);
    lua_concat(L, 2);
  }
  lua_replace(L, top - 2);
  lua_settop(L, top - 2);
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
  lua_getfield(L, lua_upvalueindex(3), name);
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
    lua_setfield(L, lua_upvalueindex(3), name);
    lua_pushvalue(L, 1);
    lua_call(L, 1, 1);
    if (!lua_isnil(L, -1))
      lua_setfield(L, lua_upvalueindex(3), name);
    lua_settop(L, 1);
    lua_getfield(L, lua_upvalueindex(3), name);
    if (lua_touserdata(L, -1) == SANDBOX_LOADING) {
      lua_pushboolean(L, 1);
      lua_pushvalue(L, -1);
      lua_setfield(L, lua_upvalueindex(3), name);
    }
    return 1;
  }
  lua_pushfstring(L, "module " LUA_QS " not found:", name);
  lua_insert(L, 2);
  lua_concat(L, lua_gettop(L) - 1);
  return lua_error(L);
}

/* -- The environment ------------------------------------------------------ */

/* Whether env (an absolute index) has a value at k, raw. */
static int env_has(lua_State *L, int env, const char *k)
{
  int has;
  lua_pushstring(L, k);
  lua_rawget(L, env);
  has = !lua_isnil(L, -1);
  lua_pop(L, 1);
  return has;
}

static void env_set(lua_State *L, int env, const char *k)
{
  lua_pushstring(L, k);
  lua_insert(L, -2);
  lua_rawset(L, env);
}

/* Replaces the table on top by a copy of its fields, minus `omit`. With
** `through`, fields the original gets later are read through __index,
** behind a metatable that getmetatable does not show.
*/
static void sandbox_copy(lua_State *L, const char *omit, int through)
{
  int src = lua_gettop(L);
  lua_newtable(L);
  lua_pushnil(L);
  while (lua_next(L, src)) {
    if (omit && lua_type(L, -2) == LUA_TSTRING &&
	strcmp(lua_tostring(L, -2), omit) == 0) {
      lua_pop(L, 1);
      continue;
    }
    lua_pushvalue(L, -2);
    lua_insert(L, -2);
    lua_rawset(L, src + 1);
  }
  if (through) {
    lua_createtable(L, 0, 2);
    lua_pushvalue(L, src);
    lua_setfield(L, -2, "__index");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, src + 1);
  }
  lua_replace(L, src);
}

/* Pushes the sandbox's package table: preload (the host's to fill),
** loaded, and a copy of the state's package.path; nothing else.
*/
static void sandbox_pushpackage(lua_State *L)
{
  NLState *ns = nl_state(L);
  if (ns->sandboxpkgref != LUA_NOREF) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, ns->sandboxpkgref);
    if (lua_istable(L, -1)) return;
    lua_pop(L, 1);
    ns->sandboxpkgref = LUA_NOREF;
  }
  lua_createtable(L, 0, 3);
  lua_newtable(L);
  lua_setfield(L, -2, "preload");
  lua_newtable(L);
  lua_setfield(L, -2, "loaded");
  lua_pushvalue(L, -1);
  ns->sandboxpkgref = luaL_ref(L, LUA_REGISTRYINDEX);
}

/* Adds to env (an absolute index) every name it lacks. */
static void sandbox_fill(lua_State *L, int env)
{
  int i, pkg;
  for (i = 0; sandbox_globals[i]; i++) {
    if (env_has(L, env, sandbox_globals[i])) continue;
    lua_getfield(L, LUA_GLOBALSINDEX, sandbox_globals[i]);
    env_set(L, env, sandbox_globals[i]);
  }
  for (i = 0; sandbox_libs[i]; i++) {
    if (env_has(L, env, sandbox_libs[i])) continue;
    lua_getfield(L, LUA_GLOBALSINDEX, sandbox_libs[i]);
    if (lua_istable(L, -1)) {
      sandbox_copy(L, NULL, 1);
      env_set(L, env, sandbox_libs[i]);
    } else {
      lua_pop(L, 1);
    }
  }
  /* string without dump. Strings still index the full table through their
  ** metatable; a dump made that way cannot be loaded here.
  */
  if (!env_has(L, env, "string")) {
    lua_getfield(L, LUA_GLOBALSINDEX, "string");
    if (lua_istable(L, -1)) {
      sandbox_copy(L, "dump", 0);
      env_set(L, env, "string");
    } else {
      lua_pop(L, 1);
    }
  }
  if (!env_has(L, env, "getmetatable")) {
    lua_pushcfunction(L, sandbox_getmetatable);
    env_set(L, env, "getmetatable");
  }
  if (!env_has(L, env, "load")) {
    lua_pushvalue(L, env);
    lua_pushcclosure(L, sandbox_load, 1);
    env_set(L, env, "load");
  }
  if (!env_has(L, env, "loadstring")) {
    lua_pushvalue(L, env);
    lua_pushcclosure(L, sandbox_load, 1);
    env_set(L, env, "loadstring");
  }
  sandbox_pushpackage(L);
  pkg = lua_gettop(L);
  if (!env_has(L, env, "package")) {
    lua_pushvalue(L, pkg);
    env_set(L, env, "package");
  }
  lua_getfield(L, pkg, "path");
  if (!lua_isstring(L, -1)) {
    lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
    if (lua_istable(L, -1)) {
      lua_getfield(L, -1, "package");
      if (lua_istable(L, -1)) {
	lua_getfield(L, -1, "path");
	if (lua_isstring(L, -1)) lua_setfield(L, pkg, "path");
	else lua_pop(L, 1);
      }
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  if (!env_has(L, env, "require")) {
    lua_pushvalue(L, env);
    lua_pushvalue(L, pkg);
    lua_getfield(L, pkg, "loaded");
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      lua_newtable(L);
      lua_pushvalue(L, -1);
      lua_setfield(L, pkg, "loaded");
    }
    lua_pushcclosure(L, sandbox_require, 3);
    env_set(L, env, "require");
  }
  lua_pop(L, 1);  /* pkg */
  if (!env_has(L, env, "_G")) {
    lua_pushvalue(L, env);
    env_set(L, env, "_G");
  }
}

LUA_API void nl_sandboxenv(lua_State *L)
{
  NLState *ns = nl_state(L);
  if (ns->sandboxref != LUA_NOREF) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, ns->sandboxref);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      ns->sandboxref = LUA_NOREF;
    }
  }
  if (ns->sandboxref == LUA_NOREF) {
    lua_createtable(L, 0, 48);
    lua_pushvalue(L, -1);
    ns->sandboxref = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  sandbox_fill(L, lua_gettop(L));
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
** sandbox text only with the sandbox environment, through the host's
** loader when it has one (include of a Lua file).
*/
int lj_nl_hostloadfile(lua_State *L)
{
  const char *path = luaL_checkstring(L, 1);
  lua_settop(L, 1);
  if (lj_nl_sandboxed(L)) {
    int r;
    nl_sandboxenv(L);
    r = sandbox_loadlua(L, path, 2);
    if (r != 0) return r;
    lua_pushnil(L);
    lua_pushfstring(L, "cannot open %s", path);
    return 2;
  }
  if (luaL_loadfile(L, path) == 0) return 1;
  lua_pushnil(L);
  lua_insert(L, -2);
  return 2;
}

#endif
