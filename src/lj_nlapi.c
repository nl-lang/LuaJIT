/*
** NL C API functions that are carried out by the runtime library.
**
** runtime/nl.lua returns a table rt with fixed names (its "host interface");
** the functions here call them. rt lives in the registry from luaopen_nl on.
**
** Functions annotated '-' in nl.h must not raise. They allocate nothing before
** entering a protected call, which is what lj_nl_protect is for: it calls a C
** function through a trampoline that luaopen_nl created once, so that the
** call itself needs no allocation.
*/

#define lj_nlapi_c
#define LUA_CORE

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "lj_nlint.h"

#if LJ_NL

#include "lauxlib.h"

/* -- Helpers ------------------------------------------------------------- */

int lj_nl_absindex(lua_State *L, int idx)
{
  return (idx > 0 || idx <= LUA_REGISTRYINDEX) ? idx : lua_gettop(L) + idx + 1;
}

int lj_nl_isopen(lua_State *L)
{
  return nl_state(L)->rtref != LUA_NOREF;
}

/* Pushes rt[name] and returns 1 when NL is open and rt[name] is a function;
** otherwise pushes nothing and returns 0. Raises nothing, also when a script
** has damaged the registry: registry[rtref] is checked to be a table. Looking
** up a name that rt holds allocates nothing (the key string is interned).
** Needs two free stack slots.
*/
int lj_nl_getrt(lua_State *L, const char *name)
{
  NLState *ns = nl_state(L);
  if (ns->rtref == LUA_NOREF) return 0;
  lua_rawgeti(L, LUA_REGISTRYINDEX, ns->rtref);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return 0;
  }
  lua_pushstring(L, name);
  lua_rawget(L, -2);
  lua_remove(L, -2);
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 1);
    return 0;
  }
  return 1;
}

/* Pushes rt[name], or raises. */
void lj_nl_pushrt(lua_State *L, const char *name)
{
  luaL_checkstack(L, 2, "NL runtime call");
  if (!lj_nl_getrt(L, name)) {
    if (!lj_nl_isopen(L))
      luaL_error(L, "NL is not open in this state (luaopen_nl)");
    luaL_error(L, "the NL runtime has no function '%s'", name);
  }
}

/* The trampoline takes the call to make from the state (ns->prot), never
** from its arguments: a script that reaches it through the registry can
** only make it raise.
*/
static int nl_trampoline(lua_State *L)
{
  NLState *ns = nl_state(L);
  NLProt *p = ns->prot;
  if (!p) return luaL_error(L, "an internal function of NL was called");
  ns->prot = NULL;
  return p->f(L, p->ud);
}

/* Pops the error value that lj_nl_protect left, if any; true on failure. */
#define protect_failed(L, st) \
  ((st) != 0 ? (((st) > 0 ? lua_pop(L, 1) : (void)0), 1) : 0)

/* Calls f(L, ud) in a protected call, with the nargs values on top of the
** stack as its stack. Returns the status of lua_pcall, with nresults results
** or the error value pushed, or -1 with nothing pushed when NL is not open or
** the stack cannot grow. The nargs values are popped in every case.
*/
int lj_nl_protect(lua_State *L, NLProtF f, void *ud, int nargs, int nresults)
{
  NLState *ns = nl_state(L);
  NLProt p;
  int st;
  if (ns->trampref == LUA_NOREF || !lua_checkstack(L, 2 + (nresults > 0 ? nresults : 0))) {
    lua_pop(L, nargs);
    return -1;
  }
  p.f = f;
  p.ud = ud;
  lua_rawgeti(L, LUA_REGISTRYINDEX, ns->trampref);
  lua_insert(L, -(nargs+1));
  ns->prot = &p;
  st = lua_pcall(L, nargs, nresults, 0);
  ns->prot = NULL;  /* When the call never reached the trampoline. */
  return st;
}

/* Reports the error value on top through the unhandled hook (rt.unhandled),
** and pops it. Without NL, or when the hook itself fails, the value is
** printed to stderr.
*/
void lj_nl_report(lua_State *L)
{
  if (lua_checkstack(L, 3) && lj_nl_getrt(L, "unhandled")) {
    lua_insert(L, -2);
    if (lua_pcall(L, 1, 0, 0) == 0) return;
    fputs("NL: the unhandled hook failed: ", stderr);
  } else {
    fputs("NL: unhandled error: ", stderr);
  }
  {
    const char *msg = lua_tostring(L, -1);
    fputs(msg ? msg : "(error object is not a string)", stderr);
    fputs("\n", stderr);
    fflush(stderr);
  }
  lua_pop(L, 1);
}

/* Calls the function below the nargs values on top with the handler that
** try uses (rt.handler), and sends an error to the unhandled hook. Leaves
** nothing on the stack. Called inside a protected call.
*/
void lj_nl_callhandled(lua_State *L, int nargs)
{
  int fidx = lua_gettop(L) - nargs;
  int hidx = 0;
  luaL_checkstack(L, 2, "NL callback");
  if (lj_nl_getrt(L, "handler")) {
    lua_insert(L, fidx);
    hidx = fidx++;
  }
  if (lua_pcall(L, nargs, 0, hidx) != 0)
    lj_nl_report(L);
  if (hidx) lua_remove(L, hidx);
}

/* -- Types and values (API.md 2.5) --------------------------------------- */

static char typenames_key;

static int typeof_f(lua_State *L, void *ud)
{
  UNUSED(ud);
  lj_nl_pushrt(L, "typeof");
  lua_pushvalue(L, 1);
  lua_call(L, 1, 1);
  /* A string only: a number would be converted in a slot nobody anchors. */
  if (lua_type(L, -1) != LUA_TSTRING) return 0;
  /* The name belongs to the state: anchor it for the state's lifetime. Type
  ** names are few, so a strong set is fine.
  */
  lua_pushlightuserdata(L, &typenames_key);
  lua_rawget(L, LUA_REGISTRYINDEX);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushlightuserdata(L, &typenames_key);
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
  }
  lua_pushvalue(L, -2);
  lua_pushboolean(L, 1);
  lua_rawset(L, -3);
  lua_pop(L, 1);
  return 1;
}

/* NULL when the runtime fails, which a correct runtime never does. */
LUA_API const char *nl_typeof(lua_State *L, int idx)
{
  const char *s;
  int st;
  if (!lua_checkstack(L, 3)) return NULL;
  lua_pushvalue(L, idx);
  st = lj_nl_protect(L, typeof_f, NULL, 1, 1);
  if (protect_failed(L, st)) return NULL;
  s = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : NULL;
  lua_pop(L, 1);
  return s;
}

static int isa_f(lua_State *L, void *ud)
{
  UNUSED(ud);
  lj_nl_pushrt(L, "isa");
  lua_insert(L, 1);
  lua_call(L, 2, 1);
  return 1;
}

LUA_API int nl_isa(lua_State *L, int idx, int type)
{
  int r, st;
  if (!lua_checkstack(L, 4)) return 0;
  type = lj_nl_absindex(L, type);
  lua_pushvalue(L, idx);
  lua_pushvalue(L, type);
  st = lj_nl_protect(L, isa_f, NULL, 2, 1);
  if (protect_failed(L, st)) return 0;
  r = lua_toboolean(L, -1);
  lua_pop(L, 1);
  return r;
}

static int gettype_f(lua_State *L, void *ud)
{
  lj_nl_pushrt(L, "gettype");
  lua_pushstring(L, (const char *)ud);
  lua_call(L, 1, 1);
  return 1;
}

LUA_API void nl_gettype(lua_State *L, const char *name)
{
  int st;
  if (!lua_checkstack(L, 3)) {
    lua_pushnil(L);  /* The one slot every C function may use. */
    return;
  }
  st = lj_nl_protect(L, gettype_f, (void *)name, 0, 1);
  if (protect_failed(L, st)) lua_pushnil(L);
}

LUA_API void nl_settype(lua_State *L, int idx, const char *name, int parent)
{
  idx = lj_nl_absindex(L, idx);
  if (parent) parent = lj_nl_absindex(L, parent);
  lj_nl_pushrt(L, "settype");
  lua_pushvalue(L, idx);
  lua_pushstring(L, name);
  if (parent) lua_pushvalue(L, parent); else lua_pushnil(L);
  lua_call(L, 3, 0);
}

/* keyed(t) (DESIGN 12, 13.1): replaces the table at idx with keyed
** arguments made from its string keys, for a call of an NL function that
** takes `**opts`.
*/
LUA_API void nl_keyed(lua_State *L, int idx)
{
  idx = lj_nl_absindex(L, idx);
  lj_nl_pushrt(L, "keyed_copy");
  lua_pushvalue(L, idx);
  lua_call(L, 1, 1);
  lua_replace(L, idx);
}

LUA_API void nl_new(lua_State *L, int type)
{
  type = lj_nl_absindex(L, type);
  lj_nl_pushrt(L, "new");
  lua_pushvalue(L, type);
  lua_pushvalue(L, -3);  /* The fields. */
  lua_call(L, 2, 1);
  lua_remove(L, -2);
}

LUA_API void nl_pushrange(lua_State *L, lua_Number first, lua_Number last,
			  int exclusive)
{
  lj_nl_pushrt(L, "range");
  lua_pushnumber(L, first);
  lua_pushnumber(L, last);
  lua_pushboolean(L, exclusive);
  lua_call(L, 3, 1);
}

static int torange_f(lua_State *L, void *ud)
{
  UNUSED(ud);
  lj_nl_pushrt(L, "torange");
  lua_insert(L, 1);
  lua_call(L, 1, 3);
  return 3;
}

LUA_API int nl_torange(lua_State *L, int idx, lua_Number *first,
		       lua_Number *last, int *exclusive)
{
  int ok = 0, st;
  if (!lua_checkstack(L, 5)) return 0;
  lua_pushvalue(L, idx);
  st = lj_nl_protect(L, torange_f, NULL, 1, 3);
  if (protect_failed(L, st)) return 0;
  if (lua_type(L, -3) == LUA_TNUMBER && lua_type(L, -2) == LUA_TNUMBER) {
    if (first) *first = lua_tonumber(L, -3);
    if (last) *last = lua_tonumber(L, -2);
    if (exclusive) *exclusive = lua_toboolean(L, -1);
    ok = 1;
  }
  lua_pop(L, 3);
  return ok;
}

LUALIB_API int nlL_typeerror(lua_State *L, int narg, const char *expected)
{
  narg = lj_nl_absindex(L, narg);
  if (!lj_nl_isopen(L))
    return luaL_error(L, "bad argument #%d (%s expected, got %s)", narg,
		      expected, luaL_typename(L, narg));
  lj_nl_pushrt(L, "typeerror");
  lua_pushinteger(L, narg);
  lua_pushstring(L, expected);
  lua_pushvalue(L, narg);
  lua_call(L, 3, 0);
  return luaL_error(L, "bad argument #%d (%s expected)", narg, expected);
}

LUALIB_API void nlL_checkisa(lua_State *L, int narg, int type)
{
  narg = lj_nl_absindex(L, narg);
  type = lj_nl_absindex(L, type);
  if (!nl_isa(L, narg, type)) {
    lj_nl_pushrt(L, "typeerror");
    lua_pushinteger(L, narg);
    lua_pushvalue(L, type);
    lua_pushvalue(L, narg);
    lua_call(L, 3, 0);
    luaL_argerror(L, narg, "wrong type");
  }
}

LUALIB_API const char *nlL_tolstring(lua_State *L, int idx, size_t *len)
{
  idx = lj_nl_absindex(L, idx);
  lj_nl_pushrt(L, "tostring");
  lua_pushvalue(L, idx);
  lua_call(L, 1, 1);
  return lua_tolstring(L, -1, len);
}

/* -- Exceptions (API.md 2.7) --------------------------------------------- */

/* Without the runtime (NL not open, or rt damaged) the call is a plain
** lua_pcall. When the stack cannot grow by the two slots the handler needs,
** the call is not made: the function and arguments are popped and a message
** is left, with LUA_ERRERR.
*/
LUA_API int nl_pcall(lua_State *L, int nargs, int nresults)
{
  int fidx = lua_gettop(L) - nargs;
  int status;
  if (!lua_checkstack(L, 2)) {
    lua_settop(L, fidx - 1);
    lua_pushliteral(L, "stack overflow (nl_pcall)");
    return LUA_ERRERR;
  }
  if (!lj_nl_getrt(L, "handler"))  /* An existing key: allocates nothing. */
    return lua_pcall(L, nargs, nresults, 0);
  lua_insert(L, fidx);
  status = lua_pcall(L, nargs, nresults, fidx);
  lua_remove(L, fidx);
  return status;
}

LUA_API int nl_raise(lua_State *L)
{
  if (!lj_nl_isopen(L)) return lua_error(L);
  lj_nl_pushrt(L, "raise");
  lua_insert(L, -2);
  lua_call(L, 1, 0);
  return lua_error(L);  /* Not reached with a correct runtime. */
}

LUALIB_API int nlL_raise(lua_State *L, const char *type, const char *fmt, ...)
{
  va_list argp;
  va_start(argp, fmt);
  lua_pushvfstring(L, fmt, argp);
  va_end(argp);
  if (!lj_nl_isopen(L)) return lua_error(L);
  lj_nl_pushrt(L, "exception");
  if (type) lua_pushstring(L, type); else lua_pushnil(L);
  lua_pushvalue(L, -3);
  lua_call(L, 2, 1);
  lua_remove(L, -2);
  return nl_raise(L);
}

static int setter_f(lua_State *L, void *ud)
{
  lj_nl_pushrt(L, (const char *)ud);
  lua_insert(L, 1);
  lua_call(L, 1, 0);
  return 0;
}

/* Pops a value and passes it to rt[name]; raises nothing. */
static void nl_setter(lua_State *L, const char *name)
{
  int st = lj_nl_protect(L, setter_f, (void *)name, 1, 0);
  (void)protect_failed(L, st);
}

LUA_API void nl_settranslator(lua_State *L)
{
  nl_setter(L, "settranslator");
}

/* -- Tasks (API.md 2.9) -------------------------------------------------- */

LUA_API void nl_newtask(lua_State *L)
{
  lj_nl_pushrt(L, "newtask");
  lua_call(L, 0, 1);
}

/* R119: the task's waiters and on_done callbacks run inside rt.resolve and
** rt.reject, so before nl_resolve and nl_reject return (the reading REVIEW.md
** assumes, and API.md 2.9). The C side adds no queueing of its own.
*/
static void nl_finish(lua_State *L, int idx, const char *name)
{
  idx = lj_nl_absindex(L, idx);
  lj_nl_pushrt(L, name);
  lua_pushvalue(L, idx);
  lua_pushvalue(L, -3);
  lua_call(L, 2, 0);
  lua_pop(L, 1);
}

LUA_API void nl_resolve(lua_State *L, int idx)
{
  nl_finish(L, idx, "resolve");
}

LUA_API void nl_reject(lua_State *L, int idx)
{
  nl_finish(L, idx, "reject");
}

static int taskstate_f(lua_State *L, void *ud)
{
  UNUSED(ud);
  lj_nl_pushrt(L, "taskstate");
  lua_insert(L, 1);
  lua_call(L, 1, 2);
  return 2;
}

/* -1, with nothing pushed, when the runtime fails (the value at idx is not a
** task). API.md 2.9 does not say what happens then.
*/
LUA_API int nl_taskstate(lua_State *L, int idx)
{
  const char *s;
  int st;
  if (!lua_checkstack(L, 4)) return -1;
  lua_pushvalue(L, idx);
  st = lj_nl_protect(L, taskstate_f, NULL, 1, 2);
  if (protect_failed(L, st)) return -1;
  s = lua_tostring(L, -2);
  if (s && strcmp(s, "done") == 0) {
    lua_remove(L, -2);
    return NL_TASK_DONE;
  } else if (s && strcmp(s, "failed") == 0) {
    lua_remove(L, -2);
    return NL_TASK_FAILED;
  } else if (s && strcmp(s, "pending") == 0) {
    lua_pop(L, 2);
    return NL_TASK_PENDING;
  }
  lua_pop(L, 2);
  return -1;
}

LUA_API void nl_setunhandled(lua_State *L)
{
  nl_setter(L, "setunhandled");
}

/* Timer for Task.sleep. rt.settimer(f) gets f(seconds, wake); the host gets
** nl_Timer(data, L, seconds, timer_wake, arg), where arg holds a registry
** reference to wake. The host calls timer_wake(L, arg) once, on the state's
** thread, or posts it; with L == NULL (posted, then the state closed) it only
** frees arg.
*/
typedef struct TimerHook {
  nl_Timer fn;
  void *data;
} TimerHook;

typedef struct TimerWake {
  int ref;
} TimerWake;

static int timer_wake_run(lua_State *L)
{
  TimerWake *w = (TimerWake *)lua_touserdata(L, 1);
  lua_rawgeti(L, LUA_REGISTRYINDEX, w->ref);
  luaL_unref(L, LUA_REGISTRYINDEX, w->ref);
  lj_nl_callhandled(L, 0);
  return 0;
}

static void timer_wake(lua_State *L, void *arg)
{
  TimerWake *w = (TimerWake *)arg;
  if (L && lua_cpcall(L, timer_wake_run, w) != 0) {
    lj_nl_report(L);
  }
  free(w);
}

static int timer_call(lua_State *L)
{
  TimerHook *h = (TimerHook *)lua_touserdata(L, lua_upvalueindex(1));
  lua_Number seconds = luaL_checknumber(L, 1);
  TimerWake *w;
  int ref;
  luaL_checktype(L, 2, LUA_TFUNCTION);
  lua_pushvalue(L, 2);
  ref = luaL_ref(L, LUA_REGISTRYINDEX);  /* May raise: before the malloc. */
  w = (TimerWake *)malloc(sizeof(TimerWake));
  if (!w) {
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    return luaL_error(L, "not enough memory");
  }
  w->ref = ref;
  /* The main thread: the caller may be a task's coroutine, which is suspended
  ** when the host wakes it.
  */
  h->fn(h->data, mainthread(G(L)), (double)seconds, timer_wake, w);
  return 0;
}

static int settimer_f(lua_State *L, void *ud)
{
  TimerHook *src = (TimerHook *)ud;
  lj_nl_pushrt(L, "settimer");
  if (src->fn) {
    TimerHook *h = (TimerHook *)lua_newuserdata(L, sizeof(TimerHook));
    *h = *src;
    lua_pushcclosure(L, timer_call, 1);
  } else {
    lua_pushnil(L);
  }
  lua_call(L, 1, 0);
  return 0;
}

LUA_API void nl_settimer(lua_State *L, nl_Timer fn, void *data)
{
  TimerHook h;
  int st;
  h.fn = fn;
  h.data = data;
  st = lj_nl_protect(L, settimer_f, &h, 0, 0);
  (void)protect_failed(L, st);
}

static int pumprt_f(lua_State *L, void *ud)
{
  UNUSED(ud);
  if (!lj_nl_getrt(L, "pump")) {  /* A runtime without a queue. */
    lua_pushboolean(L, 0);
    return 1;
  }
  lua_call(L, 0, 1);
  return 1;
}

/* rt.pump(): 1 when the runtime delivered something, else 0. Raises
** nothing; an error (rt.pump itself raises nothing) goes to the unhandled
** hook.
*/
int lj_nl_pumprt(lua_State *L)
{
  int st, r;
  if (!lj_nl_isopen(L) || !lua_checkstack(L, 4)) return 0;
  st = lj_nl_protect(L, pumprt_f, NULL, 0, 1);
  if (st < 0) return 0;
  if (st > 0) {
    lj_nl_report(L);
    return 0;
  }
  r = lua_toboolean(L, -1);
  lua_pop(L, 1);
  return r;
}

/* Created once by luaopen_nl (lib_nl.c). */
void lj_nl_inittrampoline(lua_State *L)
{
  NLState *ns = nl_state(L);
  if (ns->trampref == LUA_NOREF) {
    lua_pushcfunction(L, nl_trampoline);
    ns->trampref = luaL_ref(L, LUA_REGISTRYINDEX);
  }
}

#endif
