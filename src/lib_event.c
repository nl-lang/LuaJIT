/*
** The event library (DESIGN 11): event.run, event.on_run, event.remove, and
** the C entry points nl_runevent, nl_prunevent, nl_onevent, nl_offevent.
** NL names the table Event; Lua sees the global `event`. NL, Lua and the host
** share one set of handlers per state.
**
** event.run is written in Lua (lj_nlevent.lua, embedded as bytecode) so that
** handlers can yield; this file keeps the store, on_run, remove and the C
** entry points.
**
** Store. registry[&store_key] maps an event name to a record:
**   rec.head, rec.tail  first and last entry of a doubly linked list, in
**                       insertion order
**   rec.byname          handler name -> entry
**   rec.seq             sequence number of the last add
** and an entry is { f = callback, name = handler name, seq = n, next, prev,
** dead = true once removed }. Adding appends an entry (O(1)); removing
** unlinks it (O(1)) and keeps its `next`, so that a run standing on it goes
** on; an event left without handlers loses its record. Removed entries and
** empty records are then held by nothing but runs in progress.
**
** The rules (shared with the stand-in, runtime/nl_62_event.lua):
** - Event names and handler names are strings; a number is converted to its
**   string form; any other value is an error.
** - The callback is a function, checked when it is added (a callable table
**   is refused).
** - on_run and remove return nothing.
** - Any value a handler returns, nil included, ends the run (R105,
**   lj_nlevent.lua).
** - A handler added during a run is not called by that run; one removed
**   before the run reaches it is skipped; one replaced before the run
**   reaches it runs as replaced; re-adding a name keeps its place (R105).
** - package.loaded.event is set (luaL_register), as for the stock libraries.
** - No limit on arguments or nesting beyond the VM's own.
**
** A script that reaches the store through debug.getregistry() and damages
** it gets an error, never a crash: every table read here is checked.
*/

#define lib_event_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"

#include "lj_nlint.h"

#if LJ_NL

static char store_key;
static char run_key;
static char prun_key;

static int damaged(lua_State *L)
{
  return luaL_error(L, "the event store is damaged");
}

/* Pushes the store, creating it when create is set; 0 if there is none. */
static int ev_store(lua_State *L, int create)
{
  lua_pushlightuserdata(L, &store_key);
  lua_rawget(L, LUA_REGISTRYINDEX);
  if (lua_istable(L, -1)) return 1;
  if (!lua_isnil(L, -1)) damaged(L);
  lua_pop(L, 1);
  if (!create) return 0;
  lua_newtable(L);
  lua_pushlightuserdata(L, &store_key);
  lua_pushvalue(L, -2);
  lua_rawset(L, LUA_REGISTRYINDEX);
  return 1;
}

/* Pushes field k of the table at idx (raw). */
static void getf(lua_State *L, int idx, const char *k)
{
  lua_pushstring(L, k);
  lua_rawget(L, idx < 0 ? idx - 1 : idx);
}

/* Sets field k of the table at idx to the value on top, and pops it. */
static void setf(lua_State *L, int idx, const char *k)
{
  lua_pushstring(L, k);
  lua_insert(L, -2);
  lua_rawset(L, idx < 0 ? idx - 1 : idx);
}

/* Event and handler names: strings, or numbers in their string form. */
static void checkname(lua_State *L, int narg)
{
  int t = lua_type(L, narg);
  if (t == LUA_TNUMBER) lua_tostring(L, narg);  /* Converted in place. */
  else if (t != LUA_TSTRING) luaL_typerror(L, narg, "string");
}

/* Adds the function at fidx under event nidx and handler name hidx. */
static void ev_add(lua_State *L, int nidx, int hidx, int fidx)
{
  int store, rec, byname, e;
  luaL_checkstack(L, 8, "event.on_run");
  ev_store(L, 1);
  store = lua_gettop(L);
  lua_pushvalue(L, nidx);
  lua_rawget(L, store);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_createtable(L, 0, 4);
    lua_newtable(L);
    setf(L, -2, "byname");
    lua_pushinteger(L, 0);
    setf(L, -2, "seq");
    lua_pushvalue(L, nidx);
    lua_pushvalue(L, -2);
    lua_rawset(L, store);
  } else if (!lua_istable(L, -1)) {
    damaged(L);
  }
  rec = lua_gettop(L);
  getf(L, rec, "byname");
  if (!lua_istable(L, -1)) damaged(L);
  byname = lua_gettop(L);
  lua_pushvalue(L, hidx);
  lua_rawget(L, byname);
  if (lua_istable(L, -1)) {
    /* R105: re-adding an existing handler name replaces its callback in
    ** place, so the handler keeps its position in the order (the reading
    ** IMPLEMENTATION_PLAN M4 gives and REVIEW.md assumes).
    */
    lua_pushvalue(L, fidx);
    setf(L, -2, "f");
    lua_settop(L, store - 1);
    return;
  } else if (!lua_isnil(L, -1)) {
    damaged(L);
  }
  lua_pop(L, 1);
  getf(L, rec, "seq");
  if (lua_type(L, -1) != LUA_TNUMBER) damaged(L);
  lua_pushinteger(L, lua_tointeger(L, -1) + 1);
  lua_remove(L, -2);
  lua_pushvalue(L, -1);
  setf(L, rec, "seq");
  lua_createtable(L, 0, 5);
  lua_insert(L, -2);
  e = lua_gettop(L) - 1;
  setf(L, e, "seq");
  lua_pushvalue(L, fidx);
  setf(L, e, "f");
  lua_pushvalue(L, hidx);
  setf(L, e, "name");
  getf(L, rec, "tail");
  if (lua_istable(L, -1)) {
    lua_pushvalue(L, -1);
    setf(L, e, "prev");
    lua_pushvalue(L, e);
    setf(L, -2, "next");
    lua_pop(L, 1);
  } else if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_pushvalue(L, e);
    setf(L, rec, "head");
  } else {
    damaged(L);
  }
  lua_pushvalue(L, e);
  setf(L, rec, "tail");
  lua_pushvalue(L, hidx);
  lua_pushvalue(L, e);
  lua_rawset(L, byname);
  lua_settop(L, store - 1);
}

/* Removes handler hidx of event nidx. */
static void ev_remove(lua_State *L, int nidx, int hidx)
{
  int store, rec, e;
  luaL_checkstack(L, 8, "event.remove");
  if (!ev_store(L, 0)) return;
  store = lua_gettop(L);
  lua_pushvalue(L, nidx);
  lua_rawget(L, store);
  if (lua_isnil(L, -1)) { lua_settop(L, store - 1); return; }
  if (!lua_istable(L, -1)) damaged(L);
  rec = lua_gettop(L);
  getf(L, rec, "byname");
  if (!lua_istable(L, -1)) damaged(L);
  lua_pushvalue(L, hidx);
  lua_rawget(L, -2);
  if (lua_isnil(L, -1)) { lua_settop(L, store - 1); return; }
  if (!lua_istable(L, -1)) damaged(L);
  e = lua_gettop(L);
  /* Out of byname; marked; the callback released. */
  lua_pushvalue(L, hidx);
  lua_pushnil(L);
  lua_rawset(L, e - 1);
  lua_pushboolean(L, 1);
  setf(L, e, "dead");
  lua_pushnil(L);
  setf(L, e, "f");
  /* Unlinked: prev.next = next, next.prev = prev. e.next stays. */
  getf(L, e, "prev");
  getf(L, e, "next");
  if (lua_istable(L, -2)) {		/* prev.next = next */
    lua_pushvalue(L, -1);
    setf(L, -3, "next");
  } else {				/* head = next */
    lua_pushvalue(L, -1);
    setf(L, rec, "head");
  }
  if (lua_istable(L, -1)) {		/* next.prev = prev */
    lua_pushvalue(L, -2);
    setf(L, -2, "prev");
  } else {				/* tail = prev */
    lua_pushvalue(L, -2);
    setf(L, rec, "tail");
  }
  lua_pushnil(L);
  setf(L, e, "prev");
  /* An event without handlers loses its record. */
  getf(L, rec, "head");
  if (lua_isnil(L, -1)) {
    lua_pushvalue(L, nidx);
    lua_pushnil(L);
    lua_rawset(L, store);
  }
  lua_settop(L, store - 1);
}

/* event.on_run(name, handler_name, callback) */
static int ev_onrun(lua_State *L)
{
  checkname(L, 1);
  checkname(L, 2);
  luaL_checktype(L, 3, LUA_TFUNCTION);
  lua_settop(L, 3);
  ev_add(L, 1, 2, 3);
  return 0;
}

/* event.remove(name, handler_name) */
static int ev_removef(lua_State *L)
{
  checkname(L, 1);
  checkname(L, 2);
  lua_settop(L, 2);
  ev_remove(L, 1, 2);
  return 0;
}

/* event.run in the bootstrap interpreter, which has no embedded run loop. */
static int ev_norun(lua_State *L)
{
  return luaL_error(L, "event.run is not available in the bootstrap interpreter");
}

/* Pushes the run function. */
static void ev_pushrun(lua_State *L)
{
  lua_pushlightuserdata(L, &run_key);
  lua_rawget(L, LUA_REGISTRYINDEX);
}

/* The body of nl_prunevent inside its protected call: the arguments are on
** the stack, the event name in the state (so that a script that calls this
** function through the registry gets an error).
*/
static int ev_prun(lua_State *L)
{
  NLState *ns = nl_state(L);
  const char *name = ns->evname;
  int nargs = lua_gettop(L);
  if (!name) return luaL_error(L, "an internal function of NL was called");
  ns->evname = NULL;
  luaL_checkstack(L, 2, "event arguments");
  ev_pushrun(L);
  lua_insert(L, 1);
  lua_pushstring(L, name);
  lua_insert(L, 2);
  lua_call(L, nargs + 1, LUA_MULTRET);
  return lua_gettop(L);
}

static const luaL_Reg ev_lib[] = {
  { "on_run",	ev_onrun },
  { "remove",	ev_removef },
  { NULL,	NULL }
};

LUALIB_API int luaopen_event(lua_State *L)
{
  lua_pushlightuserdata(L, &store_key);
  lua_rawget(L, LUA_REGISTRYINDEX);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushlightuserdata(L, &store_key);
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
  }
  /* The run loop: chunk(store) returns run. */
  if (lj_nlev_size) {
    if (luaL_loadbuffer(L, (const char *)lj_nlev_bc, lj_nlev_size,
			"=[nl event]") != 0)
      lua_error(L);
    lua_insert(L, -2);
    lua_call(L, 1, 1);
  } else {
    lua_pop(L, 1);
    lua_pushcfunction(L, ev_norun);
  }
  lua_pushlightuserdata(L, &run_key);
  lua_pushvalue(L, -2);
  lua_rawset(L, LUA_REGISTRYINDEX);
  lua_pushlightuserdata(L, &prun_key);
  lua_pushcfunction(L, ev_prun);
  lua_rawset(L, LUA_REGISTRYINDEX);
  luaL_register(L, NL_EVENTLIBNAME, ev_lib);
  lua_insert(L, -2);
  lua_setfield(L, -2, "run");
  return 1;
}

/* -- C entry points (API.md 2.8) ------------------------------------------ */

LUA_API void nl_runevent(lua_State *L, const char *name, int nargs,
			 int nresults)
{
  luaL_checkstack(L, 2, "nl_runevent");
  ev_pushrun(L);
  lua_insert(L, -(nargs+1));
  lua_pushstring(L, name);
  lua_insert(L, -(nargs+1));
  lua_call(L, nargs + 1, nresults);
}

/* Raises nothing: everything that can fail, interning the name included,
** happens inside the protected call. When the stack cannot grow, the
** arguments are popped and a message is left, with LUA_ERRERR.
*/
LUA_API int nl_prunevent(lua_State *L, const char *name, int nargs,
			 int nresults)
{
  NLState *ns = nl_state(L);
  int st;
  if (!lua_checkstack(L, 3)) {
    lua_pop(L, nargs);
    lua_pushliteral(L, "stack overflow (nl_prunevent)");
    return LUA_ERRERR;
  }
  lua_pushlightuserdata(L, &prun_key);
  lua_rawget(L, LUA_REGISTRYINDEX);
  lua_insert(L, -(nargs+1));
  ns->evname = name;
  st = nl_pcall(L, nargs, nresults);
  ns->evname = NULL;
  return st;
}

/* Raises a type error for a value that is not a function, besides memory
** errors (API.md 2.8 says only memory errors; see the M4 report).
*/
LUA_API void nl_onevent(lua_State *L, const char *name, const char *handler)
{
  int f = lua_gettop(L);
  if (f < 1) luaL_error(L, "nl_onevent: no function on the stack");
  if (!name || !handler) luaL_error(L, "nl_onevent: NULL name");
  if (!lua_isfunction(L, f))
    luaL_error(L, "nl_onevent: a function expected, got %s", luaL_typename(L, f));
  luaL_checkstack(L, 2, "nl_onevent");
  lua_pushstring(L, name);
  lua_pushstring(L, handler);
  ev_add(L, f + 1, f + 2, f);
  lua_settop(L, f - 1);
}

typedef struct OffCtx {
  const char *name, *handler;
} OffCtx;

static int ev_off(lua_State *L)
{
  OffCtx *c = (OffCtx *)lua_touserdata(L, 1);
  lua_pushstring(L, c->name);
  lua_pushstring(L, c->handler);
  ev_remove(L, 2, 3);
  return 0;
}

/* Raises nothing. The removal runs in a protected call; if it fails (out of
** memory while interning the names, or a damaged store) the handler stays,
** and nothing reports it: the function returns no status (API.md 2.8).
*/
LUA_API void nl_offevent(lua_State *L, const char *name, const char *handler)
{
  OffCtx c;
  if (!name || !handler) return;
  c.name = name;
  c.handler = handler;
  if (lua_cpcall(L, ev_off, &c) != 0) lua_pop(L, 1);
}

#endif
