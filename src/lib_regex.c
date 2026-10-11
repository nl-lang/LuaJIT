/*
** NL's Regex: the glue between the runtime's Regex library
** (runtime/nl_82_regex.lua) and libnlc's regex engine (nlregex.h,
** crates/nl_regex).
**
** lj_nl_openregex pushes a table of C functions that lj_nl_openhost puts in
** the host table as host.regex when the engine is linked. A compiled
** pattern is a userdata (RegexUD) with the metatable registered as
** REGEX_MT, which the table exposes as `metatable`: the runtime fills it in
** (__index, the methods, __methods so that `re.test(s)` is a method call
** under the runtime's rule for userdata) and makes it the type
** `Regex` (rt.gettype, typeof). __gc is set here. Match tables, the
** replacement syntax, the pattern cache and the byte-to-character
** conversion of positions are Lua, in the runtime.
**
** Memory. A compiled regex lives outside the Lua heap. So that a state that
** compiles many of them still collects on time, its approximate size is
** charged to the collector's count of allocated bytes (g->gc.total, the
** quantity lj_gc_check compares with the threshold) when it is made, and
** taken off again when it is finalized. No memory is really allocated for
** it in the Lua heap: the alternative, a userdata padded by that many
** bytes, would double the memory of every pattern. The charge is kept in
** the userdata so that exactly what was added is taken off; lua_close runs
** the finalizers before it checks that the count is back to its initial
** value.
**
** Positions here are bytes: `find` and `captures` take a 1-based start byte
** and return 1-based spans whose end is the byte after the match (so the
** matched text is sub(s, a, e - 1) and the next search starts at e).
*/

#define lib_regex_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"

#include "lj_nlint.h"

#if LJ_NL

#include "lj_gc.h"
#include "nlregex.h"

#define REGEX_MT	"nl.Regex"

/* The compiled size cap (nlregex_compile): 0 is the engine's default. */
#define REGEX_SIZE_LIMIT	0

typedef struct RegexUD {
  nlregex *re;		/* NULL once freed, or when the compile failed. */
  size_t charge;	/* Bytes charged to g->gc.total. */
} RegexUD;

static RegexUD *checkregex(lua_State *L, int idx)
{
  RegexUD *ud = (RegexUD *)luaL_checkudata(L, idx, REGEX_MT);
  if (ud->re == NULL) luaL_argerror(L, idx, "regex has been freed");
  return ud;
}

static const char *subject(lua_State *L, int idx, size_t *len)
{
  TValue *o = L->base + (idx - 1);
  if (o >= L->top || !tvisstr(o)) luaL_typerror(L, idx, "string");
  *len = strV(o)->len;
  return strVdata(o);
}

/* A 1-based byte position as a 0-based offset; past the end stays past. */
static size_t startarg(lua_State *L, int idx, size_t len)
{
  lua_Number n = luaL_optnumber(L, idx, 1);
  if (n < 1) return 0;
  if (n > (lua_Number)len + 1) return len + 1;
  return (size_t)n - 1;
}

/* regex.compile(pattern) -> userdata | nil, message */
static int regex_compile(lua_State *L)
{
  size_t len;
  const char *pat = luaL_checklstring(L, 1, &len);
  RegexUD *ud = (RegexUD *)lua_newuserdata(L, sizeof(RegexUD));
  char *err = NULL;
  ud->re = NULL;
  ud->charge = 0;
  luaL_getmetatable(L, REGEX_MT);
  lua_setmetatable(L, -2);
  ud->re = nlregex_compile(pat, len, REGEX_SIZE_LIMIT, &err);
  if (ud->re == NULL) {
    lua_pushnil(L);
    lua_pushstring(L, err ? err : "the regex engine failed");
    nlregex_freemsg(err);
    return 2;
  }
  ud->charge = nlregex_memory(ud->re);
  G(L)->gc.total += (GCSize)ud->charge;
  return 1;
}

static int regex_gc(lua_State *L)
{
  RegexUD *ud = (RegexUD *)luaL_checkudata(L, 1, REGEX_MT);
  global_State *g = G(L);
  if (ud->re != NULL) {
    nlregex_free(ud->re);
    ud->re = NULL;
  }
  if (g->gc.total >= (GCSize)ud->charge) g->gc.total -= (GCSize)ud->charge;
  ud->charge = 0;
  return 0;
}

/* regex.pattern(re) -> string */
static int regex_pattern(lua_State *L)
{
  RegexUD *ud = checkregex(L, 1);
  size_t len = 0;
  const char *p = nlregex_pattern(ud->re, &len);
  lua_pushlstring(L, p ? p : "", len);
  return 1;
}

/* regex.memory(re) -> bytes charged (for tests) */
static int regex_memory(lua_State *L)
{
  lua_pushinteger(L, (lua_Integer)checkregex(L, 1)->charge);
  return 1;
}

/* regex.ngroups(re) -> number of groups, group 0 included */
static int regex_ngroups(lua_State *L)
{
  lua_pushinteger(L, (lua_Integer)nlregex_ngroups(checkregex(L, 1)->re));
  return 1;
}

/* regex.names(re) -> { [i] = name } for the named groups */
static int regex_names(lua_State *L)
{
  RegexUD *ud = checkregex(L, 1);
  size_t i, n = nlregex_ngroups(ud->re);
  lua_newtable(L);
  for (i = 1; i < n; i++) {
    size_t len = 0;
    const char *name = nlregex_groupname(ud->re, i, &len);
    if (name != NULL) {
      lua_pushlstring(L, name, len);
      lua_rawseti(L, -2, (int)i);
    }
  }
  return 1;
}

/* regex.test(re, s) -> boolean */
static int regex_test(lua_State *L)
{
  RegexUD *ud = checkregex(L, 1);
  size_t len;
  const char *s = subject(L, 2, &len);
  lua_pushboolean(L, nlregex_test(ud->re, s, len));
  return 1;
}

/* regex.find(re, s, start = 1) -> a, e | nil */
static int regex_find(lua_State *L)
{
  RegexUD *ud = checkregex(L, 1);
  size_t len, a, e;
  const char *s = subject(L, 2, &len);
  size_t start = startarg(L, 3, len);
  if (!nlregex_find(ud->re, s, len, start, &a, &e)) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushinteger(L, (lua_Integer)a + 1);
  lua_pushinteger(L, (lua_Integer)e + 1);
  return 2;
}

/* regex.captures(re, s, start = 1) -> a0, e0, a1, e1, ... | nothing.
** A group that did not participate gives nil, nil.
*/
static int regex_captures(lua_State *L)
{
  RegexUD *ud = checkregex(L, 1);
  size_t len, i, n;
  const char *s = subject(L, 2, &len);
  size_t start = startarg(L, 3, len);
  size_t local[2 * 32];
  size_t *spans = local;
  n = 2 * nlregex_ngroups(ud->re);
  if (n > sizeof(local) / sizeof(local[0]))
    spans = (size_t *)lua_newuserdata(L, n * sizeof(size_t));
  if (!nlregex_captures(ud->re, s, len, start, spans, n)) return 0;
  luaL_checkstack(L, (int)n + 1, "too many capture groups");
  for (i = 0; i < n; i++) {
    if (spans[i] == NLREGEX_NOSPAN) lua_pushnil(L);
    else lua_pushinteger(L, (lua_Integer)spans[i] + 1);
  }
  return (int)n;
}

static const luaL_Reg regex_lib[] = {
  { "compile",		regex_compile },
  { "pattern",		regex_pattern },
  { "memory",		regex_memory },
  { "ngroups",		regex_ngroups },
  { "names",		regex_names },
  { "test",		regex_test },
  { "find",		regex_find },
  { "captures",		regex_captures },
  { NULL,		NULL }
};

void lj_nl_openregex(lua_State *L)
{
  lua_createtable(L, 0, sizeof(regex_lib) / sizeof(regex_lib[0]));
  luaL_register(L, NULL, regex_lib);
  if (luaL_newmetatable(L, REGEX_MT)) {
    lua_pushcfunction(L, regex_gc);
    lua_setfield(L, -2, "__gc");
  }
  lua_setfield(L, -2, "metatable");
}

#endif
