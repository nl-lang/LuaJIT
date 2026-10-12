/*
** The C side of NL's VM extensions: the intrinsic table of KINTR, the
** method test of ISMETH and MSELF, and the C API for methods and operators.
*/

#define lj_nlvm_c
#define LUA_CORE

#include "lj_nlint.h"

#if LJ_NL

#include "lauxlib.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_meta.h"
#if LJ_HASJIT
#include "lj_jit.h"
#include "lj_trace.h"
#endif

/* -- The intrinsic table ------------------------------------------------- */

static const char *const nl_intr_names[] = {
#define NLINTR_NAME(name)	#name,
  NLC_INTRINSICS(NLINTR_NAME)
#undef NLINTR_NAME
  NULL
};

/* Fills the intrinsic table from the runtime table on top of the stack
** (left there) and points g->nl_intr at its array part. The table is
** anchored in the registry and never written again, so the array part
** stays where it is and the JIT may treat the entries as constants.
*/
void lj_nl_openintr(lua_State *L)
{
  NLState *ns = nl_state(L);
  int n = (int)(NLINTR__MAX - 1), i;
  GCtab *t;
  lua_createtable(L, n, 0);  /* asize n+1: entries 1..n in the array part. */
  for (i = 1; i <= n; i++) {
    lua_getfield(L, -2, nl_intr_names[i-1]);  /* nil when the runtime lacks it. */
    lua_rawseti(L, -2, i);
  }
  t = tabV(L->top-1);
  lj_assertL(t->asize > (uint32_t)n, "intrinsic table has no array part");
  ns->intrref = luaL_ref(L, LUA_REGISTRYINDEX);
  /* A runtime without NL's String library (a test stand-in) gets no table:
  ** no NL dump loads then (lj_bcread.c), and the string paths of lj_meta.c
  ** stay Lua's.
  */
  if (tvistab(arrayslot(t, NLINTR_String)))
    setmref(G(L)->nl_intr, tvref(t->array));
}

/* rt.opgeneric[name of mm], the runtime's generic handler of an event, or
** NULL.
*/
cTValue *lj_nl_opgeneric(global_State *g, MMS mm)
{
  cTValue *t;
  if (!lj_nl_hasintr(g)) return NULL;
  t = lj_nl_intr(g, NLINTR_opgeneric);
  if (!tvistab(t)) return NULL;
  t = lj_tab_getstr(tabV(t), mmname_str(g, mm));
  return (t && tvisfunc(t)) ? t : NULL;
}

/* -- The method test ----------------------------------------------------- */

/* Whether a userdata's metatable has a true `__methods`. */
int lj_nl_ismethod_ud(global_State *g, GCudata *ud)
{
  GCtab *mt = tabref(ud->metatable);
  if (mt) {
    cTValue *mo = lj_tab_getstr(mt, mmname_str(g, MM_methods));
    if (mo && tvistruecond(mo)) return 1;
  }
  return 0;
}

/* The test of ISMETH and MSELF: a function that is marked, or
** any function found through a string, number, boolean or a userdata whose
** metatable has a true `__methods`. Pure: allocates nothing, raises nothing.
*/
int lj_nl_ismethod(global_State *g, cTValue *f, cTValue *recv)
{
  if (!tvisfunc(f)) return 0;
  if ((funcV(f)->c.nlflags & NLFUNC_METHOD)) return 1;
  if (tvisstr(recv) || tvisnumber(recv) || tvisbool(recv)) return 1;
  if (tvisudata(recv)) return lj_nl_ismethod_ud(g, udataV(recv));
  return 0;
}

/* -- Methods and member calls -------------------------------------------- */

/* The stack slot of an index, or NULL for a pseudo-index. */
static TValue *stackidx(lua_State *L, int idx)
{
  if (idx > 0) {
    TValue *o = L->base + (idx - 1);
    return o < L->top ? o : NULL;
  } else if (idx > LUA_REGISTRYINDEX) {
    return idx == 0 ? NULL : L->top + idx;
  }
  return NULL;
}

/* Marks a closure. The mark belongs to the function object:
** a Lua closure whose prototype has PROTO_METHOD already carries it, so
** this changes only functions the compiler did not mark. A trace may have
** read the old bit (the recorder folds the test for a known function), so
** when the bit changes while traces exist, they are flushed.
*/
LUA_API void nl_markmethod(lua_State *L, int idx)
{
  cTValue *o = stackidx(L, idx);
  if (o && tvisfunc(o)) {
    GCfunc *fn = funcV(o);
    if (!(fn->c.nlflags & NLFUNC_METHOD)) {
      fn->c.nlflags |= NLFUNC_METHOD;
#if LJ_HASJIT
      if (G2J(G(L))->state == LJ_TRACE_IDLE && G2J(G(L))->freetrace > 1)
	lj_trace_flushall(L);
#endif
    }
  }
}

LUA_API int nl_ismethod(lua_State *L, int idx)
{
  cTValue *o = stackidx(L, idx);
  return (o && tvisfunc(o) && (funcV(o)->c.nlflags & NLFUNC_METHOD)) ? 1 : 0;
}

LUALIB_API void nlL_setmethods(lua_State *L, const luaL_Reg *l)
{
  luaL_checkstack(L, 2, "nlL_setmethods");
  for (; l->name; l++) {
    lua_pushcfunction(L, l->func);
    nl_markmethod(L, -1);
    lua_setfield(L, -2, l->name);
  }
}

/* x.name(args) as NL code makes the call: the receiver
** and nargs arguments are on the stack. The member is read as NL code reads
** it (rt.index: a string receiver answers with NL's library), then called
** with the receiver when it passes the method test, without it otherwise.
*/
LUALIB_API void nlL_callmember(lua_State *L, const char *name, int nargs,
			       int nresults)
{
  int recv = lua_gettop(L) - nargs;  /* Stack index of the receiver. */
  luaL_checkstack(L, 4, "nlL_callmember");
  lj_nl_pushrt(L, "index");
  lua_pushvalue(L, recv);
  lua_pushstring(L, name);
  lua_call(L, 2, 1);  /* recv args... f */
  if (lj_nl_ismethod(G(L), L->top-1, L->base + (recv-1))) {
    lua_insert(L, recv);  /* f recv args... */
    lua_call(L, nargs + 1, nresults);
  } else {
    lua_insert(L, recv);  /* f recv args... */
    lua_remove(L, recv + 1);  /* f args... */
    lua_call(L, nargs, nresults);
  }
}

/* -- Operators ----------------------------------------------------------- */

static const char *const nl_opevents[] = {
  "__add", "__sub", "__mul", "__div", "__mod", "__pow", "__idiv",
  "__band", "__bor", "__bxor", "__shl", "__shr", "__ushr", "__concat",
  "__unm", "__bnot"
};

LUA_API void nl_arith(lua_State *L, int op)
{
  int unary = (op == NL_OPUNM || op == NL_OPBNOT);
  if (op < 0 || op >= (int)(sizeof(nl_opevents)/sizeof(nl_opevents[0])))
    luaL_error(L, "nl_arith: bad operator %d", op);
  luaL_checkstack(L, 3, "nl_arith");
  lj_nl_pushrt(L, "arith");
  lua_pushstring(L, nl_opevents[op]);
  if (unary) {
    lua_pushvalue(L, -3);
    lua_remove(L, -4);
    lua_call(L, 2, 1);
  } else {
    lua_pushvalue(L, -4);
    lua_pushvalue(L, -4);
    lua_call(L, 3, 1);
    lua_remove(L, -2);
    lua_remove(L, -2);
  }
}

LUA_API int nl_compare(lua_State *L, int idx1, int idx2, int op)
{
  static const char *const names[] = { "eq", "lt", "le", "gt", "ge" };
  int r;
  if (op < 0 || op > NL_OPGE) luaL_error(L, "nl_compare: bad operator %d", op);
  idx1 = lj_nl_absindex(L, idx1);
  idx2 = lj_nl_absindex(L, idx2);
  luaL_checkstack(L, 3, "nl_compare");
  lj_nl_pushrt(L, names[op]);
  lua_pushvalue(L, idx1);
  lua_pushvalue(L, idx2);
  lua_call(L, 2, 1);
  r = lua_toboolean(L, -1);
  lua_pop(L, 1);
  return r;
}

LUA_API void nl_len(lua_State *L, int idx)
{
  idx = lj_nl_absindex(L, idx);
  luaL_checkstack(L, 2, "nl_len");
  lj_nl_pushrt(L, "len");
  lua_pushvalue(L, idx);
  lua_call(L, 1, 1);
}

/* -- host table (runtime/nl.lua) ----------------------------------------- */

/* host.markmethod(f): the VM mark (rt.method). */
static int host_markmethod(lua_State *L)
{
  nl_markmethod(L, 1);
  lua_settop(L, 1);
  return 1;
}

/* host.ismethod(f): whether a function carries the mark. */
static int host_ismethod(lua_State *L)
{
  lua_pushboolean(L, nl_ismethod(L, 1));
  return 1;
}

/* Pushes the functions of the VM extensions for the host table: a table
** `vm` with markmethod and ismethod.
*/
void lj_nl_pushhostvm(lua_State *L)
{
  lua_createtable(L, 0, 2);
  lua_pushcfunction(L, host_markmethod);
  lua_setfield(L, -2, "markmethod");
  lua_pushcfunction(L, host_ismethod);
  lua_setfield(L, -2, "ismethod");
}

#endif
