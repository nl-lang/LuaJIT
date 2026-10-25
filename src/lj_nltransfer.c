/*
** NL transfer encoder: copies values from one state into a buffer outside
** every heap, and rebuilds them in another.
** Modeled on lj_serialize.c, which it leaves untouched; the format is its own.
**
** A message is one malloc'd block (NLMsg), written in place by the encoder
** (no final copy):
**
**   "NLT2", u64 header offset, the values, then the header:
**   u32 nfiles   then per file:  u32 len, bytes          (canonical names)
**   u32 nrefs    then per ref:   u8 kind, u32 file, u32 len, bytes
**   u32 nvalues
**
** A ref is something that crosses by name: a named function, struct, enum,
** module or interface (kind NAMED: file and registry name), a
** built-in type of the runtime such as Range (BUILTIN), a metatable
** registered with Worker.register (META), the transfer pair of a host type
** (HOST; nl_settransfer). The receiver reads the header first: it loads
** every file it lists that it lacks, as import does, and looks every ref
** up; then it builds the values without running script code (only the
** decode function of a host type runs).
**
** Values (one tag byte, then the operands, in native byte order: a message
** never leaves the process):
**
**   NIL FALSE TRUE
**   NUM  f64 (the bits as they are: -0 stays -0, NaN stays NaN)
**   INT  i32 (an integer slot of a DUALNUM build)
**   STR  u32 len, bytes        a short string
**   OSTR u32 len, bytes        a long string: an object, so that a string
**                              sent many times is written once
**   TAB  u32 narr, u32 nhash, entries      a table: an object
**   TABMT u32 ref, u32 narr, u32 nhash, entries      with that metatable
**   REF  u32 ref               the receiver's value of the ref
**   BACK u32 object            an object of this message written before
**   HOST u32 ref, value        a userdata: decode(value), an object
**   entries: [ARR u32 n, n values for keys 1..n, nil allowed], then
**            key value pairs, then END.
**
** Objects are numbered in the order their tags appear, from 1, so a table
** referenced twice arrives as one table and a cycle as a cycle. Between
** messages there is no identity. The tag of an object that a BACK names has
** TT_KEEP added (the encoder sets it when it writes the BACK), so that the
** decoder keeps only those objects where a BACK finds them.
**
** Both sides walk with explicit stacks of their own, never by recursion,
** so a structure of any depth or length is copied (or refused) without
** growing the C stack or Lua's. Both run inside a protected call of their
** own, so that every error (a SendError, a callback's error, running out of
** memory) frees the encoder's or decoder's buffers before it goes on. In a
** worker the encoder's buffer counts toward NL_OPT_WORKERMEM while it is
** being written.
**
** The rules of what crosses come from the runtime (runtime/nl_64_worker.lua),
** through the transfer context: the table at registry[ns->wctxref].
**
**   ctx.named        value -> { file, name } (I.named, weak keys)
**   ctx.other_info   type -> description, for the types that are not structs
**   ctx.classify(mt, v) -> kind, a, b   what a value with metatable mt is
**                    (v is the first such value; the answer is kept per
**                    metatable and per kind of value, table or userdata):
**                    "named" file name (a struct instance), "builtin" name,
**                    "meta" name (Worker.register), "host" name encode
**                    (userdata only), "type" (v is a struct itself),
**                    "bad" message
**   ctx.typevalue(v) -> kind, a, b      the same for a type value v
**   ctx.fail(code, value, part, keys, detail)   raises SendError
**   ctx.recvfail(code, name, detail)    raises SendError on arrival
**   ctx.load(file)                      loads a file the receiver lacks
**   ctx.resolve(kind, file, name) -> value
*/

#define lj_nltransfer_c
#define LUA_CORE

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "lj_nlint.h"

#if LJ_NL

#include "lauxlib.h"
#include "lj_obj.h"
#include "lj_tab.h"
#include "lj_str.h"
#include "lj_state.h"

/* -- Format -------------------------------------------------------------- */

enum {
  TT_NIL, TT_FALSE, TT_TRUE, TT_NUM, TT_INT, TT_STR, TT_TAB, TT_TABMT,
  TT_REF, TT_BACK, TT_HOST, TT_END, TT_ARR, TT_OSTR
};
#define TT_KEEP		0x80	/* Added to the tag of an object a BACK names. */

/* Strings this long or longer are objects (TT_OSTR). */
#define OSTR_MIN	32

#define MSG_MAGIC	"NLT2"
#define MSG_PREFIX	12	/* The magic and the header's offset. */

struct NLMsg {
  size_t size;		/* Bytes in data. */
  char data[1];
};

#define MSG_DATA	offsetof(NLMsg, data)

void lj_nl_msgfree(NLMsg *m)
{
  free(m);
}

size_t lj_nl_msgsize(const NLMsg *m)
{
  return m ? m->size : 0;
}

/* -- Encoder ------------------------------------------------------------- */

/* A table being written, or a host userdata whose payload is being written. */
typedef struct EFrame {
  GCtab *t;		/* NULL: a host frame. */
  uint32_t phase;	/* 0: the array run, 1: key 0, 2: the hash part. */
  uint32_t i;		/* The current array index or node. */
  uint32_t narr;	/* The array run covers keys 1..narr-1. */
  uint32_t sub;		/* Hash part: 0 the key comes next, 1 the value. */
  uint32_t adv;		/* The item at i is written (its own frames may be
			** open above): move on when the frame is next on top. */
  uint32_t hostid;	/* Host frame: the object. */
  uint32_t hostanchor;	/* Host frame: where the payload is anchored, or 0
			** once it is written. */
} EFrame;

typedef struct ERef {
  uint8_t kind;
  uint32_t file;
  GCstr *name;
} ERef;

/* The class of a metatable for tables or for userdata, kept for the rest of
** the message (a hash table: a message may hold many metatables).
*/
typedef struct EClass {
  GCtab *mt;		/* NULL: an empty slot. */
  int isud;
  int kind;		/* EC_* */
  uint32_t ref;		/* The ref; EC_BAD: the anchor of the message. */
  uint32_t encref;	/* EC_HOST: the anchor of the encode function. */
} EClass;

enum { EC_NAMED, EC_BUILTIN, EC_META, EC_HOST, EC_TYPE, EC_BAD };

typedef struct Enc {
  /* The message being written: an NLMsg header, then MSG_PREFIX bytes,
  ** then the values; the header of refs is appended at the end.
  */
  char *buf;
  size_t n, cap;
  size_t charged;	/* Bytes counted toward the worker's memory limit. */
  NLMsg *result;
  /* The header. */
  GCstr **files;
  uint32_t nfiles, szfiles;
  ERef *refs;
  uint32_t nrefs, szrefs;
  /* Objects written: pointer -> id (open addressing). */
  uintptr_t *skeys;
  uint32_t *svals;
  uint32_t smask, scount;
  uint32_t nobj;
  size_t *objat;	/* Where each object's tag is in buf (index id-1). */
  TValue *objtv;	/* Each object, for anchoring (index id-1). */
  uint32_t szobj;
  uint32_t nanchoredobj;	/* Objects anchored so far (a watermark). */
  /* Host objects whose payload is still being written. */
  uint32_t *hostopen;
  uint32_t nhostopen, szhostopen;
  EFrame *fr;
  uint32_t nfr, szfr;
  EClass *cls;
  uint32_t clsmask, ncls;
  /* ctx.named and ctx.other_info, read once (both stay alive in ctx). */
  GCtab *named, *other_info;
  /* Stack indices. */
  int ctx, anchor, part;
  uint32_t nanchor;
} Enc;

static void enc_release(lua_State *L, Enc *e)
{
  if (e->charged) lj_nl_memcharge(L, -(ptrdiff_t)e->charged);
  e->charged = 0;
  free(e->buf); e->buf = NULL;
  free(e->files); e->files = NULL;
  free(e->refs); e->refs = NULL;
  free(e->skeys); e->skeys = NULL;
  free(e->svals); e->svals = NULL;
  free(e->objat); e->objat = NULL;
  free(e->objtv); e->objtv = NULL;
  free(e->hostopen); e->hostopen = NULL;
  free(e->fr); e->fr = NULL;
  free(e->cls); e->cls = NULL;
}

static void enc_nomem(lua_State *L)
{
  luaL_error(L, "not enough memory");
}

/* Counts delta bytes of the encoder's own memory (the buffer and its
** bookkeeping: the seen table, the objects, the classes, the frames) toward
** the memory limit of the worker it runs in; raises when that is over.
*/
static void enc_charge(lua_State *L, Enc *e, ptrdiff_t delta)
{
  if (delta > 0) {
    if (!lj_nl_memcharge(L, delta)) enc_nomem(L);
    e->charged += (size_t)delta;
  } else if (delta < 0) {
    size_t d = (size_t)(-delta);
    lj_nl_memcharge(L, delta);
    e->charged -= d <= e->charged ? d : e->charged;
  }
}

/* Room for `more` bytes in the message. In a worker the growth counts
** toward its memory limit.
*/
static void enc_need(lua_State *L, Enc *e, size_t more)
{
  size_t need = e->n + more, cap;
  char *p;
  if (need <= e->cap) return;
  if (need < e->n) enc_nomem(L);
  cap = e->cap ? e->cap : 256;
  while (cap < need) {
    if (cap > ((size_t)-1) / 2) enc_nomem(L);
    cap *= 2;
  }
  enc_charge(L, e, (ptrdiff_t)(cap - e->cap));
  p = (char *)realloc(e->buf, cap);
  if (!p) enc_nomem(L);
  e->buf = p;
  e->cap = cap;
}

static LJ_AINLINE void enc_byte(Enc *e, int b)
{
  e->buf[e->n++] = (char)b;
}

static LJ_AINLINE void enc_u32(Enc *e, uint32_t v)
{
  memcpy(e->buf + e->n, &v, 4);
  e->n += 4;
}

/* A slot of a growable array: grows *p (of *sz elements) to hold n+1. */
static void *enc_slot(lua_State *L, Enc *e, void **p, uint32_t n,
		      uint32_t *sz, size_t esize)
{
  if (n >= *sz) {
    uint32_t nsz = *sz ? *sz * 2 : 8;
    void *np;
    if (nsz < *sz) enc_nomem(L);
    enc_charge(L, e, (ptrdiff_t)((size_t)(nsz - *sz) * esize));
    np = realloc(*p, (size_t)nsz * esize);
    if (!np) enc_nomem(L);
    *p = np;
    *sz = nsz;
  }
  return (char *)*p + (size_t)n * esize;
}

static uint32_t ptr_hash(uintptr_t k)
{
  k ^= k >> 17;
  k *= (uintptr_t)0x9e3779b97f4a7c15ull;
  return (uint32_t)(k >> 32) ^ (uint32_t)k;
}

/* The object id of p, or 0. */
static uint32_t seen_get(Enc *e, const void *p)
{
  uintptr_t k = (uintptr_t)p;
  uint32_t i;
  if (!e->skeys) return 0;
  for (i = ptr_hash(k) & e->smask; e->skeys[i]; i = (i + 1) & e->smask)
    if (e->skeys[i] == k) return e->svals[i];
  return 0;
}

static void seen_put(lua_State *L, Enc *e, const void *p, uint32_t id)
{
  uintptr_t k = (uintptr_t)p;
  uint32_t i;
  if (!e->skeys || (e->scount + 1) * 4 > (e->smask + 1) * 3) {
    uint32_t nsize = e->skeys ? (e->smask + 1) * 2 : 64, j;
    uintptr_t *nk;
    uint32_t *nv;
    enc_charge(L, e, (ptrdiff_t)((size_t)(nsize - (e->skeys ? e->smask + 1 : 0)) *
				 (sizeof(uintptr_t) + sizeof(uint32_t))));
    nk = (uintptr_t *)calloc(nsize, sizeof(uintptr_t));
    nv = (uint32_t *)malloc(nsize * sizeof(uint32_t));
    if (!nk || !nv || nsize == 0) { free(nk); free(nv); enc_nomem(L); }
    if (e->skeys) {
      for (j = 0; j <= e->smask; j++) {
	uintptr_t ok = e->skeys[j];
	if (ok) {
	  uint32_t x = ptr_hash(ok) & (nsize - 1);
	  while (nk[x]) x = (x + 1) & (nsize - 1);
	  nk[x] = ok;
	  nv[x] = e->svals[j];
	}
      }
    }
    free(e->skeys);
    free(e->svals);
    e->skeys = nk;
    e->svals = nv;
    e->smask = nsize - 1;
  }
  for (i = ptr_hash(k) & e->smask; e->skeys[i]; i = (i + 1) & e->smask) ;
  e->skeys[i] = k;
  e->svals[i] = id;
  e->scount++;
}

/* Keeps the value on top alive for the rest of the encode; pops it. The
** table of anchors is made the first time (its slot is reserved).
*/
static uint32_t enc_anchor(lua_State *L, Enc *e)
{
  if (!lua_istable(L, e->anchor)) {
    lua_newtable(L);
    lua_replace(L, e->anchor);
  }
  lua_rawseti(L, e->anchor, (int)++e->nanchor);
  return e->nanchor;
}

/* The names of refs need no anchor: they are held by the runtime's tables
** (the registry, the classification) for as long as the values in the
** message, which name them, are alive.
*/
static uint32_t file_index(lua_State *L, Enc *e, GCstr *file)
{
  uint32_t i;
  GCstr **slot;
  for (i = 0; i < e->nfiles; i++)
    if (e->files[i] == file) return i;
  slot = (GCstr **)enc_slot(L, e, (void **)&e->files, e->nfiles, &e->szfiles,
			    sizeof(GCstr *));
  *slot = file;
  return e->nfiles++;
}

/* The index of a ref, added once. file is NULL for the kinds without one. */
static uint32_t ref_index(lua_State *L, Enc *e, int kind, GCstr *file,
			  GCstr *name)
{
  uint32_t i, fi = file ? file_index(L, e, file) : 0xffffffffu;
  ERef *r;
  for (i = 0; i < e->nrefs; i++)
    if (e->refs[i].kind == kind && e->refs[i].file == fi &&
	e->refs[i].name == name)
      return i;
  r = (ERef *)enc_slot(L, e, (void **)&e->refs, e->nrefs, &e->szrefs,
		       sizeof(ERef));
  r->kind = (uint8_t)kind;
  r->file = fi;
  r->name = name;
  return e->nrefs++;
}

/* Raises SendError through ctx.fail(code, value, part, keys, detail): the
** value at idx, the label of the value being written, and the keys from the
** outermost table to the one being written. detail is the stack index of a
** detail value, or 0. Does not return; the protected call around the
** encoder frees its buffers.
*/
static void enc_fail(lua_State *L, Enc *e, const char *code, int idx,
		     int detail)
{
  uint32_t k, n = 0;
  idx = lj_nl_absindex(L, idx);
  if (detail) detail = lj_nl_absindex(L, detail);
  luaL_checkstack(L, 8, "SendError");
  lua_getfield(L, e->ctx, "fail");
  lua_pushstring(L, code);
  lua_pushvalue(L, idx);
  lua_pushvalue(L, e->part);
  lua_newtable(L);
  for (k = 0; k < e->nfr; k++) {
    EFrame *f = &e->fr[k];
    GCtab *t = f->t;
    if (!t) continue;
    if (f->phase == 0) {
      lua_pushnumber(L, (lua_Number)f->i);
    } else if (f->phase == 1) {
      lua_pushnumber(L, 0);
    } else {
      Node *node = noderef(t->node);
      if (f->i > t->hmask) continue;
      copyTV(L, L->top, &node[f->i].key);
      incr_top(L);
    }
    lua_rawseti(L, -2, (int)++n);
  }
  if (detail) lua_pushvalue(L, detail); else lua_pushnil(L);
  lua_call(L, 5, 0);
  luaL_error(L, "cannot send a value");  /* ctx.fail raises; not reached. */
}

/* Calls ctx[fname] with the nargs values on top -> 3 results. */
static void enc_ask(lua_State *L, Enc *e, const char *fname, int nargs)
{
  lua_getfield(L, e->ctx, fname);
  lua_insert(L, -(nargs + 1));
  lua_call(L, nargs, 3);
}

static EClass *cls_find(Enc *e, GCtab *mt, int isud)
{
  uint32_t i;
  if (!e->cls) return NULL;
  for (i = ptr_hash((uintptr_t)mt + (uintptr_t)isud) & e->clsmask; e->cls[i].mt;
       i = (i + 1) & e->clsmask)
    if (e->cls[i].mt == mt && e->cls[i].isud == isud) return &e->cls[i];
  return NULL;
}

static EClass *cls_add(lua_State *L, Enc *e, GCtab *mt, int isud)
{
  uint32_t i;
  if (!e->cls || (e->ncls + 1) * 2 > e->clsmask + 1) {
    uint32_t nsize = e->cls ? (e->clsmask + 1) * 2 : 16, j;
    EClass *nc;
    enc_charge(L, e, (ptrdiff_t)((size_t)(nsize - (e->cls ? e->clsmask + 1 : 0)) *
				 sizeof(EClass)));
    nc = (EClass *)calloc(nsize, sizeof(EClass));
    if (!nc) enc_nomem(L);
    if (e->cls) {
      for (j = 0; j <= e->clsmask; j++) {
	EClass *c = &e->cls[j];
	if (c->mt) {
	  uint32_t x = ptr_hash((uintptr_t)c->mt + (uintptr_t)c->isud) & (nsize - 1);
	  while (nc[x].mt) x = (x + 1) & (nsize - 1);
	  nc[x] = *c;
	}
      }
    }
    free(e->cls);
    e->cls = nc;
    e->clsmask = nsize - 1;
  }
  for (i = ptr_hash((uintptr_t)mt + (uintptr_t)isud) & e->clsmask; e->cls[i].mt;
       i = (i + 1) & e->clsmask) ;
  e->ncls++;
  e->cls[i].mt = mt;
  e->cls[i].isud = isud;
  return &e->cls[i];
}

/* The class of metatable mt for a table or a userdata, found for value o.
** Kept for the message.
*/
static EClass *enc_classify(lua_State *L, Enc *e, GCtab *mt, cTValue *o)
{
  int isud = tvisudata(o);
  EClass *c = cls_find(e, mt, isud);
  const char *kind;
  if (c) return c;
  luaL_checkstack(L, 6, "transfer");
  settabV(L, L->top, mt);
  incr_top(L);
  copyTV(L, L->top, o);
  incr_top(L);
  enc_ask(L, e, "classify", 2);  /* kind, a, b */
  kind = lua_tostring(L, -3);
  c = cls_add(L, e, mt, isud);
  c->encref = 0;
  c->ref = 0;
  if (kind && strcmp(kind, "named") == 0 && lua_type(L, -2) == LUA_TSTRING &&
      lua_type(L, -1) == LUA_TSTRING && !isud) {
    c->kind = EC_NAMED;
    c->ref = ref_index(L, e, NLT_REF_NAMED, strV(L->top - 2), strV(L->top - 1));
  } else if (kind && strcmp(kind, "builtin") == 0 &&
	     lua_type(L, -2) == LUA_TSTRING && !isud) {
    c->kind = EC_BUILTIN;
    c->ref = ref_index(L, e, NLT_REF_BUILTIN, NULL, strV(L->top - 2));
  } else if (kind && strcmp(kind, "meta") == 0 &&
	     lua_type(L, -2) == LUA_TSTRING && !isud) {
    c->kind = EC_META;
    c->ref = ref_index(L, e, NLT_REF_META, NULL, strV(L->top - 2));
  } else if (kind && strcmp(kind, "host") == 0 && isud &&
	     lua_type(L, -2) == LUA_TSTRING && lua_isfunction(L, -1)) {
    c->kind = EC_HOST;
    c->ref = ref_index(L, e, NLT_REF_HOST, NULL, strV(L->top - 2));
    lua_pushvalue(L, -1);
    c->encref = enc_anchor(L, e);
  } else if (kind && strcmp(kind, "type") == 0 && !isud) {
    c->kind = EC_TYPE;
  } else {
    c->kind = EC_BAD;
    if (lua_type(L, -2) == LUA_TSTRING && kind && strcmp(kind, "bad") == 0)
      lua_pushvalue(L, -2);
    else if (isud)
      lua_pushliteral(L, "a userdata of a type that cannot cross");
    else  /* The stand-in's message (runtime/nl_64_worker.lua). */
      lua_pushliteral(L, "a table with a metatable that is not a struct");
    c->ref = enc_anchor(L, e);
  }
  lua_pop(L, 3);
  return c;
}

/* Fails with the message a classification gave. */
static void enc_failclass(lua_State *L, Enc *e, EClass *c)
{
  lua_rawgeti(L, e->anchor, (int)c->ref);
  enc_fail(L, e, "classified", -1, 0);
}

static void enc_pushframe(lua_State *L, Enc *e, GCtab *t, uint32_t hostid,
			  uint32_t hostanchor)
{
  EFrame *f = (EFrame *)enc_slot(L, e, (void **)&e->fr, e->nfr, &e->szfr,
				 sizeof(EFrame));
  memset(f, 0, sizeof(*f));
  f->t = t;
  f->hostid = hostid;
  f->hostanchor = hostanchor;
  e->nfr++;
}

static void enc_writeref(lua_State *L, Enc *e, uint32_t r)
{
  enc_need(L, e, 5);
  enc_byte(e, TT_REF);
  enc_u32(e, r);
}

/* Writes a reference to the ref of a named value found in ctx.named, or
** fails with code. o is the value.
*/
static void enc_named(lua_State *L, Enc *e, cTValue *o, const char *code)
{
  cTValue *info = e->named ? lj_tab_get(L, e->named, o) : NULL;
  if (info && tvistab(info)) {
    GCtab *it = tabV(info);
    cTValue *f = lj_tab_getint(it, 1), *n = lj_tab_getint(it, 2);
    if (f && n && tvisstr(f) && tvisstr(n)) {
      enc_writeref(L, e, ref_index(L, e, NLT_REF_NAMED, strV(f), strV(n)));
      return;
    }
  }
  copyTV(L, L->top, o);
  incr_top(L);
  enc_fail(L, e, code, -1, 0);
}

/* A type value (a struct, enum, module, interface or built-in type itself):
** crosses by name. o is a table.
*/
static void enc_typevalue(lua_State *L, Enc *e, cTValue *o)
{
  const char *kind;
  luaL_checkstack(L, 6, "transfer");
  copyTV(L, L->top, o);
  incr_top(L);
  enc_ask(L, e, "typevalue", 1);
  kind = lua_tostring(L, -3);
  if (kind && strcmp(kind, "named") == 0 && lua_type(L, -2) == LUA_TSTRING &&
      lua_type(L, -1) == LUA_TSTRING) {
    uint32_t r = ref_index(L, e, NLT_REF_NAMED, strV(L->top - 2),
			   strV(L->top - 1));
    lua_pop(L, 3);
    enc_writeref(L, e, r);
  } else if (kind && strcmp(kind, "builtin") == 0 &&
	     lua_type(L, -2) == LUA_TSTRING) {
    uint32_t r = ref_index(L, e, NLT_REF_BUILTIN, NULL, strV(L->top - 2));
    lua_pop(L, 3);
    enc_writeref(L, e, r);
  } else {
    lua_pop(L, 1);  /* kind, message */
    lua_remove(L, -2);
    enc_fail(L, e, "classified", -1, 0);
  }
}

/* A new object (o: the value), with the place of its tag (written next). */
static uint32_t enc_newobj(lua_State *L, Enc *e, cTValue *o)
{
  if (e->nobj >= e->szobj) {
    uint32_t nsz = e->szobj ? e->szobj * 2 : 64;
    size_t *na;
    TValue *nt;
    if (nsz < e->szobj) enc_nomem(L);
    enc_charge(L, e, (ptrdiff_t)((size_t)(nsz - e->szobj) *
				 (sizeof(size_t) + sizeof(TValue))));
    na = (size_t *)realloc(e->objat, (size_t)nsz * sizeof(size_t));
    if (!na) enc_nomem(L);
    e->objat = na;
    nt = (TValue *)realloc(e->objtv, (size_t)nsz * sizeof(TValue));
    if (!nt) enc_nomem(L);
    e->objtv = nt;
    e->szobj = nsz;
  }
  e->objat[e->nobj] = e->n;
  copyTV(L, &e->objtv[e->nobj], o);
  return ++e->nobj;
}

/* A BACK to object id: its tag gets TT_KEEP. */
static void enc_back(lua_State *L, Enc *e, uint32_t id)
{
  if (id >= 1 && id <= e->nobj) e->buf[e->objat[id - 1]] |= (char)TT_KEEP;
  enc_need(L, e, 5);
  enc_byte(e, TT_BACK);
  enc_u32(e, id);
}

/* Before a host callback (which may change and collect what is being
** sent): anchors the objects written since the last time, so that none of
** them is freed while its address is in the seen table. Linear over the
** message.
*/
static void enc_anchorobjects(lua_State *L, Enc *e)
{
  luaL_checkstack(L, 2, "transfer");
  for (; e->nanchoredobj < e->nobj; e->nanchoredobj++) {
    copyTV(L, L->top, &e->objtv[e->nanchoredobj]);
    incr_top(L);
    enc_anchor(L, e);
  }
}

/* Whether host object id still has its payload being written. */
static int enc_hostopen(Enc *e, uint32_t id)
{
  uint32_t i;
  for (i = 0; i < e->nhostopen; i++)
    if (e->hostopen[i] == id) return 1;
  return 0;
}

/* encode(userdata) for a host type, protected: the value sent in its place
** is left on top. A callback that raises, or returns nothing, is a
** SendError with the path of the userdata.
*/
static void enc_callhost(lua_State *L, Enc *e, EClass *c, cTValue *o)
{
  int top, st;
  enc_anchorobjects(L, e);
  luaL_checkstack(L, 6, "transfer");
  top = lua_gettop(L);
  lua_rawgeti(L, e->anchor, (int)c->encref);
  copyTV(L, L->top, o);
  incr_top(L);
  st = lua_pcall(L, 1, LUA_MULTRET, 0);
  if (st != 0) {
    copyTV(L, L->top, o);
    incr_top(L);
    enc_fail(L, e, "callback", -1, -2);
  }
  if (lua_gettop(L) == top) {
    copyTV(L, L->top, o);
    incr_top(L);
    enc_fail(L, e, "nothing", -1, 0);
  }
  lua_settop(L, top + 1);
}

/* Writes o. A table or a host userdata opens a frame that enc_run fills. */
static void enc_put(lua_State *L, Enc *e, cTValue *o)
{
  if (tvisstr(o)) {
    GCstr *s = strV(o);
    MSize len = s->len;
    if (len >= OSTR_MIN) {
      uint32_t id = seen_get(e, s);
      if (id) {
	enc_back(L, e, id);
	return;
      }
      enc_need(L, e, 5 + (size_t)len);
      id = enc_newobj(L, e, o);
      seen_put(L, e, s, id);
      enc_byte(e, TT_OSTR);
    } else {
      enc_need(L, e, 5 + (size_t)len);
      enc_byte(e, TT_STR);
    }
    enc_u32(e, (uint32_t)len);
    memcpy(e->buf + e->n, strdata(s), len);
    e->n += len;
  } else if (tvisint(o)) {
    int32_t x = intV(o);
    enc_need(L, e, 5);
    enc_byte(e, TT_INT);
    memcpy(e->buf + e->n, &x, 4);
    e->n += 4;
  } else if (tvisnum(o)) {
    enc_need(L, e, 9);
    enc_byte(e, TT_NUM);
    memcpy(e->buf + e->n, &o->u64, 8);
    e->n += 8;
  } else if (tvisnil(o)) {
    enc_need(L, e, 1);
    enc_byte(e, TT_NIL);
  } else if (tvisfalse(o)) {
    enc_need(L, e, 1);
    enc_byte(e, TT_FALSE);
  } else if (tvistrue(o)) {
    enc_need(L, e, 1);
    enc_byte(e, TT_TRUE);
  } else if (tvistab(o)) {
    GCtab *t = tabV(o), *mt;
    uint32_t id = seen_get(e, t), ref = 0;
    int withmt = 0;
    cTValue *oi;
    if (id) {
      enc_back(L, e, id);
      return;
    }
    /* A type that is not a struct (an enum, a module, an interface, a
    ** built-in type) is a description in ctx.other_info.
    */
    oi = e->other_info ? lj_tab_get(L, e->other_info, o) : NULL;
    if (oi && !tvisnil(oi)) {
      enc_typevalue(L, e, o);
      return;
    }
    mt = tabref(t->metatable);
    if (mt) {
      EClass *c = enc_classify(L, e, mt, o);
      switch (c->kind) {
      case EC_NAMED: case EC_BUILTIN: case EC_META:
	withmt = 1;
	ref = c->ref;
	break;
      case EC_TYPE:
	enc_typevalue(L, e, o);
	return;
      default:
	enc_failclass(L, e, c);
	return;
      }
    }
    enc_need(L, e, 18);
    id = enc_newobj(L, e, o);
    seen_put(L, e, t, id);
    if (withmt) {
      enc_byte(e, TT_TABMT);
      enc_u32(e, ref);
    } else {
      enc_byte(e, TT_TAB);
    }
    {
      uint32_t narr = 0, nhash = 0, j;
      EFrame *f;
      if (t->asize > 0) {
	TValue *array = tvref(t->array);
	int64_t i;
	for (i = (int64_t)t->asize - 1; i >= 1; i--)
	  if (!tvisnil(&array[i])) break;
	narr = i >= 1 ? (uint32_t)i + 1 : 0;
      }
      if (t->hmask > 0) {
	Node *node = noderef(t->node);
	for (j = 0; j <= t->hmask; j++)
	  nhash += !tvisnil(&node[j].val);
      }
      enc_u32(e, narr ? narr - 1 : 0);
      enc_u32(e, nhash);
      if (narr > 1) {
	enc_byte(e, TT_ARR);
	enc_u32(e, narr - 1);
      }
      enc_pushframe(L, e, t, 0, 0);
      f = &e->fr[e->nfr - 1];
      f->narr = narr;
      f->i = 1;
      if (narr <= 1) f->phase = 1;
    }
  } else if (tvisfunc(o)) {
    enc_named(L, e, o, "function");
  } else if (tvisudata(o)) {
    GCudata *ud = udataV(o);
    GCtab *mt = tabref(ud->metatable);
    uint32_t id = seen_get(e, ud);
    EClass *c;
    if (id) {
      if (enc_hostopen(e, id)) {
	copyTV(L, L->top, o);
	incr_top(L);
	enc_fail(L, e, "selfhost", -1, 0);
      }
      enc_back(L, e, id);
      return;
    }
    if (!mt) {
      copyTV(L, L->top, o);
      incr_top(L);
      enc_fail(L, e, "userdata", -1, 0);
    }
    c = enc_classify(L, e, mt, o);
    if (c->kind != EC_HOST) {
      if (c->kind == EC_BAD) enc_failclass(L, e, c);
      copyTV(L, L->top, o);
      incr_top(L);
      enc_fail(L, e, "userdata", -1, 0);
    }
    enc_callhost(L, e, c, o);  /* The value sent in its place, on top. */
    {
      uint32_t a = enc_anchor(L, e);
      uint32_t *slot;
      enc_need(L, e, 5);
      id = enc_newobj(L, e, o);
      seen_put(L, e, ud, id);
      enc_byte(e, TT_HOST);
      enc_u32(e, c->ref);
      slot = (uint32_t *)enc_slot(L, e, (void **)&e->hostopen, e->nhostopen,
				  &e->szhostopen, sizeof(uint32_t));
      *slot = id;
      e->nhostopen++;
      enc_pushframe(L, e, NULL, id, a);
    }
  } else if (tvisthread(o)) {
    copyTV(L, L->top, o);
    incr_top(L);
    enc_fail(L, e, "coroutine", -1, 0);
  } else {
    /* Light userdata, cdata. */
    copyTV(L, L->top, o);
    incr_top(L);
    enc_fail(L, e, "userdata", -1, 0);
  }
}

/* Writes the entries of the frames until none is left. A frame moves past
** an item only when it is next on top (f->adv), so that while the frames of
** a nested table are open, each frame below still names the key it is in,
** for the path of a SendError.
*/
static void enc_run(lua_State *L, Enc *e)
{
  while (e->nfr > 0) {
    EFrame *f = &e->fr[e->nfr - 1];
    GCtab *t = f->t;
    TValue v;
    if (!t) {  /* A host frame. */
      if (f->hostanchor) {
	uint32_t a = f->hostanchor;
	f->hostanchor = 0;
	lua_rawgeti(L, e->anchor, (int)a);
	copyTV(L, &v, L->top - 1);
	L->top--;  /* Anchored. */
	enc_put(L, e, &v);
      } else {
	e->nhostopen--;  /* Opened last, closed first. */
	e->nfr--;
      }
      continue;
    }
    if (f->adv) {
      f->adv = 0;
      if (f->phase == 0) {
	f->i++;
      } else if (f->phase == 1) {
	f->phase = 2;
	f->i = 0;
	f->sub = 0;
      } else {
	f->i++;
	f->sub = 0;
      }
    }
    if (f->phase == 0) {
      if (f->i < f->narr) {
	f->adv = 1;
	if (f->i < t->asize) {
	  copyTV(L, &v, arrayslot(t, f->i));
	  enc_put(L, e, &v);
	} else {  /* The table shrank (a host encode function). */
	  enc_need(L, e, 1);
	  enc_byte(e, TT_NIL);
	}
	continue;
      }
      f->phase = 1;
    }
    if (f->phase == 1) {
      if (t->asize > 0 && !tvisnil(arrayslot(t, 0))) {
	TValue k;
	setnumV(&k, 0);
	enc_need(L, e, 9);
	enc_byte(e, TT_NUM);
	memcpy(e->buf + e->n, &k.u64, 8);
	e->n += 8;
	f->adv = 1;
	copyTV(L, &v, arrayslot(t, 0));
	enc_put(L, e, &v);
	continue;
      }
      f->phase = 2;
      f->i = 0;
      f->sub = 0;
    }
    {  /* The hash part. */
      Node *node = noderef(t->node);
      if (f->sub == 0)
	while (f->i <= t->hmask && tvisnil(&node[f->i].val)) f->i++;
      if (f->i > t->hmask) {
	enc_need(L, e, 1);
	enc_byte(e, TT_END);
	e->nfr--;
	continue;
      }
      if (f->sub == 0) {
	f->sub = 1;
	copyTV(L, &v, &node[f->i].key);
      } else {
	f->adv = 1;
	copyTV(L, &v, &node[f->i].val);
      }
      enc_put(L, e, &v);
    }
  }
}

static void hdr_u32(Enc *e, uint32_t v)
{
  memcpy(e->buf + e->n, &v, 4);
  e->n += 4;
}

/* Appends the header and makes the buffer the message. */
static void enc_finish(lua_State *L, Enc *e, uint32_t nvalues)
{
  uint32_t i;
  size_t hsize = 4 + 4 + 4, at;
  uint64_t off;
  NLMsg *m;
  for (i = 0; i < e->nfiles; i++) hsize += 4 + e->files[i]->len;
  for (i = 0; i < e->nrefs; i++) hsize += 1 + 4 + 4 + e->refs[i].name->len;
  enc_need(L, e, hsize);
  at = e->n;
  hdr_u32(e, e->nfiles);
  for (i = 0; i < e->nfiles; i++) {
    GCstr *s = e->files[i];
    hdr_u32(e, s->len);
    memcpy(e->buf + e->n, strdata(s), s->len);
    e->n += s->len;
  }
  hdr_u32(e, e->nrefs);
  for (i = 0; i < e->nrefs; i++) {
    ERef *r = &e->refs[i];
    e->buf[e->n++] = (char)r->kind;
    hdr_u32(e, r->file);
    hdr_u32(e, r->name->len);
    memcpy(e->buf + e->n, strdata(r->name), r->name->len);
    e->n += r->name->len;
  }
  hdr_u32(e, nvalues);
  off = (uint64_t)(at - MSG_DATA);
  memcpy(e->buf + MSG_DATA + 4, &off, 8);
  m = (NLMsg *)(void *)e->buf;
  m->size = e->n - MSG_DATA;
  e->result = m;
  e->buf = NULL;  /* The message owns it now. */
}

/* Pushes the transfer context, or raises. */
static void push_ctx(lua_State *L)
{
  NLState *ns = nl_state(L);
  if (ns->wctxref != LUA_NOREF) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, ns->wctxref);
    if (lua_istable(L, -1)) return;
    lua_pop(L, 1);
  }
  luaL_error(L, "the NL runtime has set up no transfer context (workers)");
}

/* The encoder's protected call: (enc, labels | nil, values...). */
static int enc_main(lua_State *L)
{
  Enc *e = (Enc *)lua_touserdata(L, 1);
  int n = lua_gettop(L) - 2, i;
  luaL_checkstack(L, 12, "transfer");
  push_ctx(L);
  e->ctx = lua_gettop(L);
  lua_getfield(L, e->ctx, "named");
  e->named = lua_istable(L, -1) ? tabV(L->top - 1) : NULL;
  lua_getfield(L, e->ctx, "other_info");
  e->other_info = lua_istable(L, -1) ? tabV(L->top - 1) : NULL;
  lua_pop(L, 2);
  lua_pushnil(L);  /* The anchors, made when needed (enc_anchor). */
  e->anchor = lua_gettop(L);
  lua_pushnil(L);  /* The label of the value being written. */
  e->part = lua_gettop(L);
  enc_need(L, e, MSG_DATA + MSG_PREFIX);
  e->n = MSG_DATA;
  memcpy(e->buf + e->n, MSG_MAGIC, 4);
  e->n += MSG_PREFIX;
  for (i = 0; i < n; i++) {
    TValue v;
    if (lua_istable(L, 2)) lua_rawgeti(L, 2, i + 1); else lua_pushnil(L);
    if (lua_type(L, -1) != LUA_TSTRING) {
      lua_pop(L, 1);
      lua_pushfstring(L, "argument %d", i + 1);
    }
    lua_replace(L, e->part);
    copyTV(L, &v, L->base + 2 + i);  /* Stays on the stack. */
    enc_put(L, e, &v);
    enc_run(L, e);
  }
  enc_finish(L, e, (uint32_t)n);
  return 0;
}

/* Encodes the n values from stack index base on. The label of value i is
** labels[i] of the table at labidx when it has one, else "argument i".
** Raises SendError (through ctx.fail) for a value that cannot cross, after
** freeing what it had built.
*/
NLMsg *lj_nl_encode(lua_State *L, int base, int n, int labidx)
{
  Enc *e;
  int i, st;
  NLMsg *m;
  base = lj_nl_absindex(L, base);
  if (labidx) labidx = lj_nl_absindex(L, labidx);
  if (!lua_checkstack(L, n + 4)) luaL_error(L, "too many values to send");
  e = (Enc *)calloc(1, sizeof(Enc));
  if (!e) enc_nomem(L);
  lua_pushcfunction(L, enc_main);
  lua_pushlightuserdata(L, e);
  if (labidx) lua_pushvalue(L, labidx); else lua_pushnil(L);
  for (i = 0; i < n; i++) lua_pushvalue(L, base + i);
  st = lua_pcall(L, n + 2, 0, 0);
  m = e->result;
  enc_release(L, e);
  free(e);
  if (st != 0) {
    lj_nl_msgfree(m);
    lua_error(L);
  }
  return m;
}

/* -- Decoder ------------------------------------------------------------- */

typedef struct DVal {
  int tag;		/* DV_* */
  int32_t i;
  double n;
  const char *s;
  size_t len;
  uint32_t id;		/* DV_OBJ: object; DV_REF: ref. */
} DVal;

enum { DV_NIL, DV_FALSE, DV_TRUE, DV_NUM, DV_INT, DV_STR, DV_OBJ, DV_REF };

typedef struct DFrame {
  uint32_t obj;		/* The table or host object. */
  int slot;		/* The table's stack slot, or 0: it is in objs only. */
  int keep;		/* It is in objs too (a BACK names it). */
  int host;		/* 1: a host frame waiting for its payload. */
  uint32_t ref;		/* Host frame: the decode function's ref. */
  uint32_t arrleft;	/* Array run values left. */
  uint32_t arrnext;	/* The key of the next one. */
  int haskey;		/* A key read, waiting for its value. */
  DVal key;
} DFrame;

/* Open tables stay on the Lua stack this deep; deeper ones are found in
** objs (the stack of a C function is limited to LUAI_MAXCSTACK slots).
*/
#define DEC_STACKDEPTH	2000

typedef struct Dec {
  const char *p, *end;
  DFrame *fr;
  uint32_t nfr, szfr;
  uint32_t nobj;
  uint32_t nrefs;
  int ctx, objs, refs;	/* Stack indices. */
  uint32_t ntop, sztop;
} Dec;

static void dec_bad(lua_State *L)
{
  luaL_error(L, "NL transfer: a damaged message");
}

static uint32_t dec_u32(lua_State *L, Dec *d)
{
  uint32_t v;
  if ((size_t)(d->end - d->p) < 4) dec_bad(L);
  memcpy(&v, d->p, 4);
  d->p += 4;
  return v;
}

static int dec_byte(lua_State *L, Dec *d)
{
  if (d->p >= d->end) dec_bad(L);
  return (unsigned char)*d->p++;
}

static void dec_pushval(lua_State *L, Dec *d, const DVal *v)
{
  switch (v->tag) {
  case DV_NIL: lua_pushnil(L); break;
  case DV_FALSE: lua_pushboolean(L, 0); break;
  case DV_TRUE: lua_pushboolean(L, 1); break;
  case DV_INT: lua_pushinteger(L, (lua_Integer)v->i); break;
  case DV_NUM: lua_pushnumber(L, (lua_Number)v->n); break;
  case DV_STR: lua_pushlstring(L, v->s, v->len); break;
  case DV_OBJ: lua_rawgeti(L, d->objs, (int)v->id); break;
  default: lua_rawgeti(L, d->refs, (int)v->id); break;
  }
}

static DFrame *dec_pushframe(lua_State *L, Dec *d)
{
  DFrame *f;
  if (d->nfr >= d->szfr) {
    uint32_t nsz = d->szfr ? d->szfr * 2 : 16;
    DFrame *nf = (DFrame *)realloc(d->fr, (size_t)nsz * sizeof(DFrame));
    if (!nf || nsz < d->szfr) enc_nomem(L);
    d->fr = nf;
    d->szfr = nsz;
  }
  f = &d->fr[d->nfr++];
  memset(f, 0, sizeof(*f));
  return f;
}

/* decode(payload) for a host type, protected: the userdata it makes is left
** on top. A function that raises or returns something other than a
** userdata is a SendError on arrival (ctx.recvfail).
*/
static void dec_callhost(lua_State *L, Dec *d, uint32_t ref, const DVal *v)
{
  int top = lua_gettop(L), failed;
  luaL_checkstack(L, 6, "transfer");
  lua_rawgeti(L, d->refs, (int)ref);
  dec_pushval(L, d, v);
  failed = lua_pcall(L, 1, 1, 0) != 0;
  if (failed || lua_type(L, -1) != LUA_TUSERDATA) {
    lua_getfield(L, d->ctx, "recvfail");
    lua_pushstring(L, failed ? "failed" : "returned");
    lua_rawgeti(L, d->refs, -(int)ref);  /* The type's name (dec_main). */
    lua_pushvalue(L, top + 1);
    lua_call(L, 3, 0);
    dec_bad(L);  /* ctx.recvfail raises; not reached. */
  }
}

/* A value is complete: it goes to the frame on top, or to the top level. A
** host frame takes it as its payload and completes in turn with the
** userdata that its decode function makes of it.
*/
static void dec_complete(lua_State *L, Dec *d, DVal v)
{
  for (;;) {
    DFrame *f;
    if (d->nfr == 0) {
      /* A top-level value: pushed, in order (no frame is open above). */
      if (d->ntop >= d->sztop) dec_bad(L);
      dec_pushval(L, d, &v);
      d->ntop++;
      return;
    }
    f = &d->fr[d->nfr - 1];
    if (f->host) {
      uint32_t obj = f->obj, ref = f->ref;
      d->nfr--;
      dec_callhost(L, d, ref, &v);
      lua_rawseti(L, d->objs, (int)obj);
      v.tag = DV_OBJ;
      v.id = obj;
      continue;
    }
    if (f->arrleft > 0) {
      if (v.tag != DV_NIL) {
	if (f->slot) {
	  dec_pushval(L, d, &v);
	  lua_rawseti(L, f->slot, (int)f->arrnext);
	} else {
	  lua_rawgeti(L, d->objs, (int)f->obj);
	  dec_pushval(L, d, &v);
	  lua_rawseti(L, -2, (int)f->arrnext);
	  lua_pop(L, 1);
	}
      }
      f->arrnext++;
      f->arrleft--;
    } else if (!f->haskey) {
      if (v.tag == DV_NIL) dec_bad(L);
      f->key = v;
      f->haskey = 1;
    } else {
      if (f->slot) {
	dec_pushval(L, d, &f->key);
	dec_pushval(L, d, &v);
	lua_rawset(L, f->slot);
      } else {
	lua_rawgeti(L, d->objs, (int)f->obj);
	dec_pushval(L, d, &f->key);
	dec_pushval(L, d, &v);
	lua_rawset(L, -3);
	lua_pop(L, 1);
      }
      f->haskey = 0;
    }
    return;
  }
}

/* Reads the values. */
static void dec_values(lua_State *L, Dec *d, uint32_t nvalues)
{
  uint32_t nrefs = d->nrefs;
  while (d->ntop < nvalues || d->nfr > 0) {
    int tag = dec_byte(L, d), keep = tag & TT_KEEP;
    DVal v;
    tag &= ~TT_KEEP;
    v.tag = DV_NIL;
    switch (tag) {
    case TT_NIL: v.tag = DV_NIL; break;
    case TT_FALSE: v.tag = DV_FALSE; break;
    case TT_TRUE: v.tag = DV_TRUE; break;
    case TT_NUM:
      if ((size_t)(d->end - d->p) < 8) dec_bad(L);
      memcpy(&v.n, d->p, 8);
      d->p += 8;
      v.tag = DV_NUM;
      break;
    case TT_INT:
      if ((size_t)(d->end - d->p) < 4) dec_bad(L);
      memcpy(&v.i, d->p, 4);
      d->p += 4;
      v.tag = DV_INT;
      break;
    case TT_STR: case TT_OSTR: {
      size_t len = dec_u32(L, d);
      if ((size_t)(d->end - d->p) < len) dec_bad(L);
      v.tag = DV_STR;
      v.s = d->p;
      v.len = len;
      d->p += len;
      if (tag == TT_OSTR) {
	uint32_t id = ++d->nobj;
	if (keep) {
	  /* Kept where a BACK finds it; the value is that object. */
	  lua_pushlstring(L, v.s, v.len);
	  lua_rawseti(L, d->objs, (int)id);
	  v.tag = DV_OBJ;
	  v.id = id;
	}
      }
      break;
    }
    case TT_REF:
      v.id = dec_u32(L, d) + 1;
      if (v.id > nrefs) dec_bad(L);
      v.tag = DV_REF;
      break;
    case TT_BACK:
      v.id = dec_u32(L, d);
      if (v.id == 0 || v.id > d->nobj) dec_bad(L);
      v.tag = DV_OBJ;
      break;
    case TT_TAB: case TT_TABMT: {
      uint32_t ref = 0, narr, nhash, id;
      DFrame *f;
      if (tag == TT_TABMT) {
	ref = dec_u32(L, d) + 1;
	if (ref > nrefs) dec_bad(L);
      }
      narr = dec_u32(L, d);
      nhash = dec_u32(L, d);
      id = ++d->nobj;
      lua_createtable(L, (int)(narr > (1u << 26) ? 0 : narr),
		      (int)(nhash > (1u << 26) ? 0 : nhash));
      if (ref) {
	lua_rawgeti(L, d->refs, (int)ref);
	if (!lua_istable(L, -1)) dec_bad(L);
	lua_setmetatable(L, -2);
      }
      /* Open tables stay on the stack (popped with the frame, TT_END); a
      ** table a BACK names is kept in objs as well.
      */
      if (d->nfr < DEC_STACKDEPTH) {
	if (keep) {
	  lua_pushvalue(L, -1);
	  lua_rawseti(L, d->objs, (int)id);
	}
	f = dec_pushframe(L, d);
	f->slot = lua_gettop(L);
	f->keep = keep;
      } else {
	lua_rawseti(L, d->objs, (int)id);
	f = dec_pushframe(L, d);
	f->keep = 1;
      }
      f->obj = id;
      /* An array run may follow at once. */
      if (d->p < d->end && (unsigned char)*d->p == TT_ARR) {
	d->p++;
	f->arrleft = dec_u32(L, d);
	f->arrnext = 1;
      }
      continue;
    }
    case TT_END:
      if (d->nfr == 0 || d->fr[d->nfr - 1].host || d->fr[d->nfr - 1].haskey ||
	  d->fr[d->nfr - 1].arrleft)
	dec_bad(L);
      {
	DFrame *f = &d->fr[d->nfr - 1];
	v.tag = DV_OBJ;
	v.id = f->obj;
	if (f->slot) {
	  /* The table is on top. Straight into a parent that waits for a
	  ** value and is on the stack too; otherwise through objs.
	  */
	  DFrame *p = d->nfr >= 2 ? &d->fr[d->nfr - 2] : NULL;
	  if (p && !p->host && p->slot && (p->arrleft > 0 || p->haskey)) {
	    if (p->arrleft > 0) {
	      lua_rawseti(L, p->slot, (int)p->arrnext);
	      p->arrnext++;
	      p->arrleft--;
	    } else {
	      dec_pushval(L, d, &p->key);
	      lua_insert(L, -2);
	      lua_rawset(L, p->slot);
	      p->haskey = 0;
	    }
	    d->nfr--;
	    continue;
	  }
	  if (!p) {
	    /* A top-level value: it stays where it is, on top. */
	    if (d->ntop >= d->sztop) dec_bad(L);
	    d->nfr--;
	    d->ntop++;
	    continue;
	  }
	  if (f->keep) lua_pop(L, 1); else lua_rawseti(L, d->objs, (int)f->obj);
	}
	d->nfr--;
      }
      break;
    case TT_HOST: {
      DFrame *f;
      uint32_t ref = dec_u32(L, d) + 1;
      if (ref > nrefs) dec_bad(L);
      f = dec_pushframe(L, d);
      f->host = 1;
      f->ref = ref;
      f->obj = ++d->nobj;
      continue;
    }
    default:
      dec_bad(L);
    }
    dec_complete(L, d, v);
  }
}

/* The decoder's protected call: (dec, msg). Leaves the values. */
static int dec_main(lua_State *L)
{
  Dec *d = (Dec *)lua_touserdata(L, 1);
  const NLMsg *m = (const NLMsg *)lua_touserdata(L, 2);
  uint32_t nfiles, nrefs, nvalues, i;
  uint64_t off;
  const char *data = m->data, *hdr, *filesat;
  int base;
  lua_settop(L, 0);
  luaL_checkstack(L, 12 + DEC_STACKDEPTH, "transfer");
  push_ctx(L);
  d->ctx = 1;
  if (m->size < MSG_PREFIX || memcmp(data, MSG_MAGIC, 4) != 0) dec_bad(L);
  memcpy(&off, data + 4, 8);
  if (off < MSG_PREFIX || off > (uint64_t)m->size) dec_bad(L);
  hdr = data + (size_t)off;
  /* The header first. */
  d->p = hdr;
  d->end = data + m->size;
  nfiles = dec_u32(L, d);
  filesat = d->p;
  /* The files: loaded first, so that their registries hold the refs. */
  for (i = 0; i < nfiles; i++) {
    uint32_t len = dec_u32(L, d);
    if ((size_t)(d->end - d->p) < len) dec_bad(L);
    lua_getfield(L, d->ctx, "load");
    lua_pushlstring(L, d->p, len);
    lua_call(L, 1, 0);
    d->p += len;
  }
  nrefs = dec_u32(L, d);
  d->nrefs = nrefs;
  lua_createtable(L, (int)(nrefs < 1024 ? nrefs : 1024), 0);
  d->refs = lua_gettop(L);
  for (i = 0; i < nrefs; i++) {
    int kind = dec_byte(L, d);
    uint32_t file = dec_u32(L, d), len;
    lua_getfield(L, d->ctx, "resolve");
    lua_pushinteger(L, kind);
    if (file == 0xffffffffu) {
      lua_pushnil(L);
    } else {
      /* The file's name, found again in the list. */
      const char *q = filesat;
      uint32_t k, flen = 0;
      if (file >= nfiles) dec_bad(L);
      for (k = 0; k <= file; k++) {
	memcpy(&flen, q, 4);
	q += 4;
	if (k < file) q += flen;
      }
      lua_pushlstring(L, q, flen);
    }
    len = dec_u32(L, d);
    if ((size_t)(d->end - d->p) < len) dec_bad(L);
    lua_pushlstring(L, d->p, len);
    /* The name, for the messages of a decode function: refs[-i]. */
    lua_pushvalue(L, -1);
    lua_rawseti(L, d->refs, -(int)(i + 1));
    d->p += len;
    lua_call(L, 3, 1);
    lua_rawseti(L, d->refs, (int)i + 1);
  }
  nvalues = dec_u32(L, d);
  /* Then the values. */
  d->p = data + MSG_PREFIX;
  d->end = hdr;
  if ((size_t)nvalues > (size_t)(d->end - d->p)) dec_bad(L);
  lua_newtable(L);
  d->objs = lua_gettop(L);
  base = d->objs;
  d->sztop = nvalues;
  luaL_checkstack(L, (int)nvalues + 4, "too many values in a message");
  dec_values(L, d, nvalues);
  return lua_gettop(L) - base;
}

/* Decodes m in L: loads the files it lists that L lacks (ctx.load), looks
** its refs up (ctx.resolve), then builds the values and pushes them. Returns
** their number. Raises when a file cannot be loaded or a ref is missing,
** after freeing what it had built; the message is not freed.
*/
int lj_nl_decode(lua_State *L, const NLMsg *m)
{
  Dec *d;
  int top = lua_gettop(L), st;
  if (!lua_checkstack(L, 4)) luaL_error(L, "stack overflow (transfer)");
  d = (Dec *)calloc(1, sizeof(Dec));
  if (!d) enc_nomem(L);
  lua_pushcfunction(L, dec_main);
  lua_pushlightuserdata(L, d);
  lua_pushlightuserdata(L, (void *)m);
  st = lua_pcall(L, 2, LUA_MULTRET, 0);
  free(d->fr);
  free(d);
  if (st != 0) lua_error(L);
  return lua_gettop(L) - top;
}

#endif
