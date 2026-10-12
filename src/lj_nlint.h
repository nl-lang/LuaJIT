/*
** NL per-state data and the interfaces shared by the NL files.
*/

#ifndef _LJ_NLINT_H
#define _LJ_NLINT_H

#include "lj_nl.h"

#if LJ_NL

#include "lua.h"
#include "nl.h"
#include "nlc.h"

#if LJ_TARGET_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef SRWLOCK NLMutex;
typedef CONDITION_VARIABLE NLCond;
#else
#include <pthread.h>
typedef pthread_mutex_t NLMutex;
typedef pthread_cond_t NLCond;
#endif

typedef struct NLPost {
  nl_Callback fn;
  void *data;
  struct NLPost *next;
} NLPost;

#define NL_OPT__MAX	5

/* A pending protected call (lj_nl_protect). */
typedef int (*NLProtF)(lua_State *L, void *ud);
typedef struct NLProt {
  NLProtF f;
  void *ud;
} NLProt;

/* Per-state NL data, hung off global_State. It is allocated with malloc and
** not with the state's allocator, because nl_post reaches it from other
** threads.
*/
typedef struct NLState {
  /* -- Shared with other threads, guarded by mtx. -- */
  NLMutex mtx;
  NLCond idle;			/* Signalled when wakeup_calls drops to 0. */
  NLPost *head, *tail;		/* The inbox (DESIGN 14.4). */
  nl_Wakeup wakeup;
  void *wakeupdata;
  int wakeup_calls;		/* Wakeup calls in progress. */
  /* -- Owner thread only. -- */
  int rtref;			/* Registry reference of rt, or LUA_NOREF. */
  int intrref;			/* Registry reference of the intrinsic table (lj_nl.h). */
  int trampref;			/* Registry reference of the trampoline. */
  int guardref;			/* Registry reference of the guard metatable. */
  NLProt *prot;			/* The call the trampoline is to make. */
  const char *evname;		/* The event nl_prunevent is to run. */
  int hasloader;
  nl_Loader loader;		/* Host loader, when hasloader. */
  nl_Reporter reporter;
  void *reporterdata;
  lua_Integer opt[NL_OPT__MAX+1];
  char **path;			/* Import search path (DESIGN 10). */
  size_t npath, szpath;
  char **decl;			/* Declaration files of nl_adddecl (API.md 2.3). */
  size_t ndecl, szdecl;
} NLState;

#define nl_state(L)	((NLState *)G(L)->nl)

/* lj_nlstate.c */
LJ_FUNCA void lj_nl_lock(NLState *ns);
LJ_FUNCA void lj_nl_unlock(NLState *ns);
LJ_FUNCA int lj_nl_addpath(NLState *ns, const char *dir);
LJ_FUNCA int lj_nl_adddecl(NLState *ns, const char *name);

/* lj_nlapi.c */
LJ_FUNCA int lj_nl_isopen(lua_State *L);
LJ_FUNCA int lj_nl_getrt(lua_State *L, const char *name);
LJ_FUNCA void lj_nl_pushrt(lua_State *L, const char *name);
LJ_FUNCA int lj_nl_protect(lua_State *L, NLProtF f, void *ud, int nargs,
			  int nresults);
LJ_FUNCA void lj_nl_callhandled(lua_State *L, int nargs);
LJ_FUNCA void lj_nl_report(lua_State *L);
LJ_FUNCA int lj_nl_absindex(lua_State *L, int idx);
LJ_FUNCA void lj_nl_inittrampoline(lua_State *L);
LJ_FUNCA int lj_nl_pumprt(lua_State *L);

/* lj_nlvm.c: the C side of the VM extensions (IMPLEMENTATION_PLAN M5). */
LJ_FUNCA void lj_nl_openintr(lua_State *L);
LJ_FUNCA void lj_nl_pushhostvm(lua_State *L);

/* lj_nlload.c */
LJ_FUNCA void lj_nl_openhost(lua_State *L);
LJ_FUNCA int lj_nl_include(lua_State *L);

/* lib_nltext.c: NL's UTF-8 string operations (DESIGN 13.5), host.text.
** lj_nl_strutf8 is whether a string is valid UTF-8, checked once per string
** and remembered in it (LJ_STR_UTF8CHECKED, lj_obj.h).
*/
LJ_FUNCA void lj_nl_opentext(lua_State *L);
LJ_FUNCA int lj_nl_strutf8(GCstr *s);

/* lib_regex.c: NL's Regex (DESIGN 13.3), host.regex. */
LJ_FUNCA void lj_nl_openregex(lua_State *L);

/* lj_nlrt.c: the runtime chunk (the .lua files of runtime/) and the run
** loop of the event library (lj_nlevent.lua), as bytecode.
*/
LJ_DATA const unsigned char *const lj_nlrt_bc;
LJ_DATA const size_t lj_nlrt_size;
LJ_DATA const unsigned char *const lj_nlev_bc;
LJ_DATA const size_t lj_nlev_size;

#endif

#endif
