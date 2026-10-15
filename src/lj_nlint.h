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

#define NL_OPT__MAX	7

/* lj_nltransfer.c and lj_nlworker.c. */
typedef struct NLMsg NLMsg;		/* An encoded message (lj_nltransfer.c). */
typedef struct NLPort NLPort;		/* Where results for a state go. */
typedef struct NLWorker NLWorker;	/* A worker state and its thread. */
typedef struct NLWorkers NLWorkers;	/* The workers of one main state. */

/* The kinds of the refs of a message (lj_nltransfer.c). */
#define NLT_REF_NAMED	1	/* A named function or struct: file, name. */
#define NLT_REF_BUILTIN	2	/* A built-in type of the runtime: name. */
#define NLT_REF_META	3	/* A metatable of Worker.register: name. */
#define NLT_REF_HOST	4	/* A host type's decode function: name. */

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
  NLPost *head, *tail;		/* The inbox. */
  nl_Wakeup wakeup;
  void *wakeupdata;
  int wakeup_calls;		/* Wakeup calls in progress. */
  NLCond posted;		/* Signalled by nl_post while nl_drain waits. */
  int drain_waiting;		/* nl_drain waits for the inbox. */
  /* -- Owner thread only. -- */
  int rtref;			/* Registry reference of rt, or LUA_NOREF. */
  int intrref;			/* Registry reference of the intrinsic table (lj_nl.h). */
  int trampref;			/* Registry reference of the trampoline. */
  int guardref;			/* Registry reference of the guard metatable. */
  int sandboxref;		/* Registry reference of the sandbox environment. */
  int sandboxpkgref;		/* Registry reference of the sandbox's package. */
  NLProt *prot;			/* The call the trampoline is to make. */
  const char *evname;		/* The event nl_prunevent is to run. */
  int hasloader;
  nl_Loader loader;		/* Host loader, when hasloader. */
  nl_Reporter reporter;
  void *reporterdata;
  lua_Integer opt[NL_OPT__MAX+1];
  char **path;			/* Import search path. */
  size_t npath, szpath;
  size_t declnpath;		/* Entries of path that feed declaration files
				** in the sandbox: those added before it. */
  char **decl;			/* Declaration files of nl_adddecl. */
  size_t ndecl, szdecl;
  /* Workers (lj_nlworker.c). */
  NLWorkers *wk;		/* The workers of the main state; the same in each
				** of its workers. NULL until the first is made. */
  NLWorker *worker;		/* In a worker state: the worker. */
  NLPort *port;			/* Where results for this state go, once made. */
  int wctxref;			/* Registry reference of the transfer context. */
  lua_Integer outstanding;	/* Jobs and calls sent, results not delivered. */
  int wpumping;			/* nl_pump is stepping the pumped workers. */
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

/* lj_nlvm.c: the C side of the VM extensions. */
LJ_FUNCA void lj_nl_openintr(lua_State *L);
LJ_FUNCA void lj_nl_pushhostvm(lua_State *L);

/* lj_nlsandbox.c: the sandbox profile (NL_OPT_SANDBOX).
** NL_SANDBOX_NOBC is the message of a bytecode chunk handed in.
*/
#define NL_SANDBOX_NOBC	"bytecode loading is off in the sandbox"
#define lj_nl_sandboxed(L)	(nl_state(L)->opt[NL_OPT_SANDBOX] != 0)
LJ_FUNCA int lj_nl_hostsandbox(lua_State *L);
LJ_FUNCA int lj_nl_hostloadfile(lua_State *L);

/* lj_nltransfer.c */
LJ_FUNCA NLMsg *lj_nl_encode(lua_State *L, int base, int n, int labidx);
LJ_FUNCA int lj_nl_decode(lua_State *L, const NLMsg *m);
LJ_FUNCA void lj_nl_msgfree(NLMsg *m);
LJ_FUNCA size_t lj_nl_msgsize(const NLMsg *m);

/* lj_nlworker.c */
LJ_FUNCA void lj_nl_pushhostworker(lua_State *L);
LJ_FUNCA int lj_nl_stepworkers(lua_State *L);
LJ_FUNCA void lj_nl_closeworkers(NLState *ns);
LJ_FUNCA void lj_nl_portkill(NLPort *p);
LJ_FUNCA int lj_nl_memcharge(lua_State *L, ptrdiff_t delta);
LJ_FUNCA int lj_nl_condinit(NLCond *c);
LJ_FUNCA int lj_nl_workersfrozen(NLState *ns);
LJ_FUNCA void lj_nl_workerssandbox(NLState *ns);
LJ_FUNCA void lj_nl_portunref(NLPort *p);

/* lj_nlload.c */
LJ_FUNCA void lj_nl_openhost(lua_State *L);
LJ_FUNCA int lj_nl_include(lua_State *L);

/* lib_nltext.c: NL's UTF-8 string operations, host.text.
** lj_nl_strutf8 is whether a string is valid UTF-8, checked once per string
** and remembered in it (LJ_STR_UTF8CHECKED, lj_obj.h).
*/
LJ_FUNCA void lj_nl_opentext(lua_State *L);
LJ_FUNCA int lj_nl_strutf8(GCstr *s);

/* lib_regex.c: NL's Regex, host.regex. */
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
