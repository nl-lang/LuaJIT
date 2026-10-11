/*
** NL's UTF-8 string operations (DESIGN 13.5, IMPLEMENTATION_PLAN M6): the
** glue between the runtime's String library (runtime/nl_80_string.lua) and
** libnlc's text library (nltext.h, crates/nl_text).
**
** lj_nl_opentext pushes a table of C functions that lj_nl_openhost puts in
** the host table as host.text when the library is linked. The runtime takes
** its primitives from there and keeps its Lua stand-ins otherwise. These
** functions are not reachable from scripts; the runtime calls them with
** checked arguments, and they check again cheaply. The runtime raises
** EncodingError (through `invalid_at`) before any character operation, so
** the others may assume valid UTF-8; they stay memory-safe on any bytes.
**
** Validity is checked once per string: the answer is kept in two bits of
** GCstr.reserved (LJ_STR_UTF8CHECKED, LJ_STR_UTF8VALID; lj_obj.h), which the
** lexer's reserved-word numbers never reach. Strings are immutable and
** belong to one state, so the bits need no synchronization.
**
** Positions in the table's functions follow Lua: 1-based bytes and 1-based
** character positions; NL's negative positions are taken by `at` and
** `slice` as DESIGN 13.5 defines them.
*/

#define lib_nltext_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"

#include "lj_nlint.h"

#if LJ_NL

#include "lj_str.h"
#include "nltext.h"

/* The string argument idx as its GCstr. Numbers are not converted: the
** runtime passes strings only.
*/
static GCstr *strarg(lua_State *L, int idx)
{
  TValue *o = L->base + (idx - 1);
  if (o >= L->top || !tvisstr(o)) luaL_typerror(L, idx, "string");
  return strV(o);
}

int lj_nl_strutf8(GCstr *s)
{
  if (!(s->reserved & LJ_STR_UTF8CHECKED)) {
    int valid = nltext_invalid_at(strdata(s), s->len) == NLTEXT_NONE;
    s->reserved |= (uint8_t)(LJ_STR_UTF8CHECKED | (valid ? LJ_STR_UTF8VALID : 0));
  }
  return (s->reserved & LJ_STR_UTF8VALID) != 0;
}

/* A position argument as an int64, clamped to what a double holds exactly,
** so that a huge number cannot overflow a conversion.
*/
static int64_t posarg(lua_State *L, int idx)
{
  lua_Number n = luaL_checknumber(L, idx);
  if (n != n) return 0;  /* NaN: out of every range. */
  if (n >= 9007199254740992.0) return INT64_C(9007199254740992);
  if (n <= -9007199254740992.0) return -INT64_C(9007199254740992);
  return (int64_t)n;
}

/* A 1-based byte or character position argument, at least 1. */
static size_t index1(lua_State *L, int idx)
{
  int64_t n = posarg(L, idx);
  if (n < 1) luaL_argerror(L, idx, "position must be at least 1");
  return (size_t)n;
}

/* text.invalid_at(s) -> 1-based byte of the first invalid sequence | nil */
static int text_invalid_at(lua_State *L)
{
  GCstr *s = strarg(L, 1);
  if (lj_nl_strutf8(s)) {
    lua_pushnil(L);
  } else {
    size_t at = nltext_invalid_at(strdata(s), s->len);
    lua_pushinteger(L, at == NLTEXT_NONE ? 1 : (lua_Integer)at + 1);
  }
  return 1;
}

/* text.valid(s) -> boolean */
static int text_valid(lua_State *L)
{
  lua_pushboolean(L, lj_nl_strutf8(strarg(L, 1)));
  return 1;
}

/* text.count(s) -> number of characters */
static int text_count(lua_State *L)
{
  GCstr *s = strarg(L, 1);
  lua_pushinteger(L, (lua_Integer)nltext_count(strdata(s), s->len));
  return 1;
}

/* text.offset(s, p) -> the 1-based byte where character p (1 .. count+1)
** starts; #s + 1 past the end.
*/
static int text_offset(lua_State *L)
{
  GCstr *s = strarg(L, 1);
  size_t p = index1(L, 2);
  lua_pushinteger(L, (lua_Integer)nltext_offset(strdata(s), s->len, p - 1) + 1);
  return 1;
}

/* text.advance(s, i, k) -> from 1-based byte i, the start of a character,
** k characters forward; #s + 1 at the end.
*/
static int text_advance(lua_State *L)
{
  GCstr *s = strarg(L, 1);
  size_t i = index1(L, 2);
  int64_t k = posarg(L, 3);
  size_t from = i - 1;
  if (k < 0) k = 0;
  if (from > s->len) from = s->len;
  lua_pushinteger(L, (lua_Integer)(from + nltext_offset(strdata(s) + from, s->len - from, (size_t)k)) + 1);
  return 1;
}

/* text.charpos(s, b) -> the 1-based character position of the character
** that starts at 1-based byte b.
*/
static int text_charpos(lua_State *L)
{
  GCstr *s = strarg(L, 1);
  size_t b = index1(L, 2);
  lua_pushinteger(L, (lua_Integer)nltext_charpos(strdata(s), s->len, b - 1) + 1);
  return 1;
}

/* text.at(s, i) -> the character at NL position i, or nil */
static int text_at(lua_State *L)
{
  GCstr *s = strarg(L, 1);
  int64_t i = posarg(L, 2);
  size_t a, e;
  if (nltext_charat(strdata(s), s->len, i, &a, &e))
    lua_pushlstring(L, strdata(s) + a, e - a);
  else
    lua_pushnil(L);
  return 1;
}

/* text.slice(s, first, last, exclusive) -> s[first..last] */
static int text_slice(lua_State *L)
{
  GCstr *s = strarg(L, 1);
  int64_t first = posarg(L, 2), last = posarg(L, 3);
  int exclusive = lua_toboolean(L, 4);
  size_t a, e;
  if (nltext_slice(strdata(s), s->len, first, last, exclusive, &a, &e))
    lua_pushlstring(L, strdata(s) + a, e - a);
  else
    lua_pushliteral(L, "");
  return 1;
}

static int text_case(lua_State *L, int mode)
{
  GCstr *s = strarg(L, 1);
  size_t n = 0;
  char *out;
  if (s->len == 0) {
    lua_pushliteral(L, "");
    return 1;
  }
  out = nltext_case(strdata(s), s->len, mode, &n);
  if (out == NULL) {
    /* The runtime checks validity first; here only for the stub. */
    return nlL_raise(L, "EncodingError", "the string is not valid UTF-8");
  }
  /* Interning the result can only fail for memory, in which case the state
  ** is unwinding anyway; the buffer is then lost.
  */
  lua_pushlstring(L, out, n);
  nltext_free(out, n);
  return 1;
}

static int text_upcase(lua_State *L) { return text_case(L, NLTEXT_CASE_UP); }
static int text_downcase(lua_State *L) { return text_case(L, NLTEXT_CASE_DOWN); }
static int text_capitalize(lua_State *L) { return text_case(L, NLTEXT_CASE_CAPITALIZE); }
static int text_swapcase(lua_State *L) { return text_case(L, NLTEXT_CASE_SWAP); }

/* text.chars(s) -> list of the characters; text.codepoints(s) -> list of
** the code points.
*/
static int text_list(lua_State *L, int points)
{
  GCstr *s = strarg(L, 1);
  const char *p = strdata(s);
  size_t len = s->len, i = 0;
  lua_Integer k = 0;
  lua_createtable(L, (int)(len < 0x7fffffff ? len : 0x7fffffff), 0);
  while (i < len) {
    size_t clen;
    uint32_t cp = nltext_decode(p, len, i, &clen);
    if (clen == 0) clen = 1;  /* Not valid here: move on, never stall. */
    if (points) lua_pushinteger(L, (lua_Integer)cp);
    else lua_pushlstring(L, p + i, clen);
    lua_rawseti(L, -2, (int)++k);
    i += clen;
  }
  return 1;
}

static int text_chars(lua_State *L) { return text_list(L, 0); }
static int text_codepoints(lua_State *L) { return text_list(L, 1); }

/* text.bytes(s) -> list of the byte values */
static int text_bytes(lua_State *L)
{
  GCstr *s = strarg(L, 1);
  const uint8_t *p = (const uint8_t *)strdata(s);
  size_t i, len = s->len;
  lua_createtable(L, (int)(len < 0x7fffffff ? len : 0x7fffffff), 0);
  for (i = 0; i < len; i++) {
    lua_pushinteger(L, (lua_Integer)p[i]);
    lua_rawseti(L, -2, (int)(i + 1));
  }
  return 1;
}

/* text.decode(s, i) -> the code point starting at 1-based byte i, and the
** byte after it; nil at the end or where no character starts.
*/
static int text_decode(lua_State *L)
{
  GCstr *s = strarg(L, 1);
  size_t i = index1(L, 2);
  size_t clen;
  uint32_t cp;
  if (i > s->len) {
    lua_pushnil(L);
    return 1;
  }
  cp = nltext_decode(strdata(s), s->len, i - 1, &clen);
  if (clen == 0) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushinteger(L, (lua_Integer)cp);
  lua_pushinteger(L, (lua_Integer)(i + clen));
  return 2;
}

/* text.encode(cp) -> the UTF-8 string of a code point, or nil */
static int text_encode(lua_State *L)
{
  int64_t cp = posarg(L, 1);
  char buf[4];
  size_t n = (cp < 0 || cp > 0x10FFFF) ? 0 : nltext_encode((uint32_t)cp, buf);
  if (n == 0) lua_pushnil(L);
  else lua_pushlstring(L, buf, n);
  return 1;
}

static const luaL_Reg text_lib[] = {
  { "invalid_at",	text_invalid_at },
  { "valid",		text_valid },
  { "count",		text_count },
  { "offset",		text_offset },
  { "advance",		text_advance },
  { "charpos",		text_charpos },
  { "at",		text_at },
  { "slice",		text_slice },
  { "upcase",		text_upcase },
  { "downcase",		text_downcase },
  { "capitalize",	text_capitalize },
  { "swapcase",		text_swapcase },
  { "chars",		text_chars },
  { "codepoints",	text_codepoints },
  { "bytes",		text_bytes },
  { "decode",		text_decode },
  { "encode",		text_encode },
  { NULL,		NULL }
};

void lj_nl_opentext(lua_State *L)
{
  lua_createtable(L, 0, sizeof(text_lib) / sizeof(text_lib[0]));
  luaL_register(L, NULL, text_lib);
}

#endif
