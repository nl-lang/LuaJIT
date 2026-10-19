/*
** NL extensions to the Lua C API.
**
** This header declares what NL adds to Lua. It extends the C API in the way
** luajit.h does: everything that Lua can express is done with lua.h and
** lauxlib.h, and nothing declared there changes meaning.
**
** Each function is annotated as in the Lua manual: [-o, +p, x] pops o values
** and pushes p; x is '-' (raises nothing), 'm' (memory errors only), 'e'
** (other errors) or 'v' (raises on purpose).
*/

#ifndef _NL_H
#define _NL_H

#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#define NL_VERSION	"NL 0.1"
#define NL_VERSION_NUM	100		/* major*10000 + minor*100 + patch */
#define NL_VERSION_SYM	nl_version_0_1

#ifdef __cplusplus
extern "C" {
#endif

/* Call from main: a link error if the library is another version of NL, or
** stock LuaJIT.
*/
LUA_API void NL_VERSION_SYM(void);

/* -- States and options -------------------------------------------------- */

#define NL_OPT_CHECKS		1	/* Runtime type checks; default 1. */
#define NL_OPT_STRIP		2	/* Strip debug info; default 0. */
#define NL_OPT_MAXWORKERS	3	/* Dedicated workers; default 64. */
#define NL_OPT_WORKERMEM	4	/* Bytes per worker, 0 = no limit. */
#define NL_OPT_GLUA		5	/* Accept GLua (Garry's Mod) syntax; default 0. */
#define NL_OPT_SANDBOX		6	/* Sandbox profile for untrusted mods; default 0. */
#define NL_OPT_LOCALIZE		7	/* Localized Lua paths: 0, 1 std, 2 all; default 1. */

LUALIB_API int luaopen_nl(lua_State *L);			/* [-0, +0, e] */
LUA_API int nl_setoption(lua_State *L, int opt, lua_Integer value); /* [-0, +0, -] */
LUA_API lua_Integer nl_getoption(lua_State *L, int opt);	/* [-0, +0, -] */
LUA_API int nl_isworker(lua_State *L);				/* [-0, +0, -] */
/* 1 when the workers' settings are fixed for L (L is a worker, or a main
** state with workers): nl_setloader then has no effect, nor nl_addpath and
** nl_adddecl on a main state (2.10).
*/
LUA_API int nl_workersstarted(lua_State *L);			/* [-0, +0, -] */
LUA_API void nl_sandboxenv(lua_State *L);			/* [-0, +1, m] */

/* -- Loading ------------------------------------------------------------- */

LUA_API int nl_loadbuffer(lua_State *L, const char *buf, size_t size,
			  const char *name);			/* [-0, +1, m] */
LUA_API int nl_loadfile(lua_State *L, const char *filename);	/* [-0, +1, m] */
LUA_API void nl_import(lua_State *L, const char *path);	/* [-0, +1, e] */
/* No effect on a main state whose workers started (nl_workersstarted(L) &&
** !nl_isworker(L)); see the end of this file.
*/
LUA_API void nl_addpath(lua_State *L, const char *dir);	/* [-0, +0, m] */
LUA_API void nl_adddecl(lua_State *L, const char *name);	/* [-0, +0, m] */

/* As in Lua, a name that starts with neither '@' nor '=' is the source
** itself: it is shown as [string "first line..."].
*/
#define nlL_loadstring(L, s)	nl_loadbuffer(L, s, strlen(s), s)
#define nlL_dostring(L, s)	(nlL_loadstring(L, s) || nl_pcall(L, 0, LUA_MULTRET))
#define nlL_dofile(L, fn)	(nl_loadfile(L, fn) || nl_pcall(L, 0, LUA_MULTRET))

typedef struct nl_Loader {
  int (*resolve)(void *data, const char *from, const char *path,
		 char *out, size_t size);
  const char *(*read)(void *data, const char *name, size_t *size);
  void (*release)(void *data, const char *name, const char *buf);
  void *data;
} nl_Loader;
/* No effect when nl_workersstarted(L), in a worker too. */
LUA_API void nl_setloader(lua_State *L, const nl_Loader *loader); /* [-0, +0, -] */

#define NL_DIAG_ERROR		0
#define NL_DIAG_WARNING		1
#define NL_DIAG_LINT		2

typedef struct nl_Diagnostic {
  int severity;			/* NL_DIAG_ERROR, NL_DIAG_WARNING, NL_DIAG_LINT */
  const char *code;		/* Stable identifier, such as "undeclared-name". */
  const char *message;
  const char *file;		/* Resolved name of the file. */
  int line, column;		/* 1-based. */
  int endline, endcolumn;
} nl_Diagnostic;
typedef void (*nl_Reporter)(void *data, const nl_Diagnostic *d);
/* No effect in a worker (nl_isworker(L)). */
LUA_API void nl_setreporter(lua_State *L, nl_Reporter fn, void *data); /* [-0, +0, -] */

/* -- Types and values ---------------------------------------------------- */

LUA_API const char *nl_typeof(lua_State *L, int idx);		/* [-0, +0, -] */
/* The name of the type at idx, as a TypeError writes it: a struct, enum,
** interface, host or built-in type (nl_typeof gives its values that name),
** or a combination such as `Number?`. NULL when the value is not a type (a
** module, an instance). The string lives as long as the state.
*/
LUA_API const char *nl_typename(lua_State *L, int idx);	/* [-0, +0, -] */
LUA_API int nl_isa(lua_State *L, int idx, int type);		/* [-0, +0, -] */
LUA_API void nl_gettype(lua_State *L, const char *name);	/* [-0, +1, -] */
LUA_API void nl_settype(lua_State *L, int idx, const char *name,
			int parent);				/* [-0, +0, e] */
LUA_API void nl_keyed(lua_State *L, int idx);		/* [-0, +0, e] */
LUA_API void nl_new(lua_State *L, int type);			/* [-1, +1, e] */
LUA_API void nl_pushrange(lua_State *L, lua_Number first, lua_Number last,
			  int exclusive);			/* [-0, +1, m] */
LUA_API int nl_torange(lua_State *L, int idx, lua_Number *first,
		       lua_Number *last, int *exclusive);	/* [-0, +0, -] */
LUALIB_API void nlL_checkisa(lua_State *L, int narg, int type);	/* [-0, +0, v] */
LUALIB_API int nlL_typeerror(lua_State *L, int narg,
			     const char *expected);		/* [-0, +0, v] */
LUALIB_API const char *nlL_tolstring(lua_State *L, int idx,
				     size_t *len);		/* [-0, +1, e] */

/* -- Methods and member calls -------------------------------------------- */

LUA_API void nl_markmethod(lua_State *L, int idx);		/* [-0, +0, -] */
LUA_API int nl_ismethod(lua_State *L, int idx);		/* [-0, +0, -] */
LUALIB_API void nlL_setmethods(lua_State *L, const luaL_Reg *l); /* [-0, +0, m] */
LUALIB_API void nlL_callmember(lua_State *L, const char *name, int nargs,
			       int nresults);	/* [-(nargs+1), +nresults, e] */

/* -- Operators ----------------------------------------------------------- */

#define NL_OPADD	0
#define NL_OPSUB	1
#define NL_OPMUL	2
#define NL_OPDIV	3
#define NL_OPMOD	4
#define NL_OPPOW	5
#define NL_OPIDIV	6
#define NL_OPBAND	7
#define NL_OPBOR	8
#define NL_OPBXOR	9
#define NL_OPSHL	10
#define NL_OPSHR	11
#define NL_OPUSHR	12
#define NL_OPCONCAT	13
#define NL_OPUNM	14	/* Unary: one operand. */
#define NL_OPBNOT	15	/* Unary: one operand. */

#define NL_OPEQ		0
#define NL_OPLT		1
#define NL_OPLE		2
#define NL_OPGT		3
#define NL_OPGE		4

LUA_API void nl_arith(lua_State *L, int op);		/* [-(2|1), +1, e] */
LUA_API int nl_compare(lua_State *L, int idx1, int idx2, int op); /* [-0, +0, e] */
LUA_API void nl_len(lua_State *L, int idx);			/* [-0, +1, e] */

/* -- Exceptions ---------------------------------------------------------- */

LUA_API int nl_pcall(lua_State *L, int nargs, int nresults); /* [-(nargs+1), +(nresults|1), -] */
LUA_API int nl_raise(lua_State *L);				/* [-1, +0, v] */
LUALIB_API int nlL_raise(lua_State *L, const char *type,
			 const char *fmt, ...);			/* [-0, +0, v] */
LUA_API void nl_settranslator(lua_State *L);			/* [-1, +0, -] */

/* -- Events -------------------------------------------------------------- */

#define NL_EVENTLIBNAME	"event"
LUALIB_API int luaopen_event(lua_State *L);

LUA_API void nl_runevent(lua_State *L, const char *name, int nargs,
			 int nresults);		/* [-nargs, +nresults, e] */
LUA_API int nl_prunevent(lua_State *L, const char *name, int nargs,
			 int nresults);		/* [-nargs, +(nresults|1), -] */
LUA_API void nl_onevent(lua_State *L, const char *name,
			const char *handler);	/* [-1, +0, m] */
LUA_API void nl_offevent(lua_State *L, const char *name,
			 const char *handler);	/* [-0, +0, -] */

/* -- Tasks --------------------------------------------------------------- */

#define NL_TASK_PENDING		0
#define NL_TASK_DONE		1
#define NL_TASK_FAILED		2

LUA_API void nl_newtask(lua_State *L);				/* [-0, +1, m] */
LUA_API void nl_resolve(lua_State *L, int idx);		/* [-1, +0, e] */
LUA_API void nl_reject(lua_State *L, int idx);			/* [-1, +0, e] */
LUA_API int nl_taskstate(lua_State *L, int idx);		/* [-0, +(0|1), -] */

typedef void (*nl_Callback)(lua_State *L, void *data);
typedef void (*nl_Timer)(void *data, lua_State *L, double seconds,
			 nl_Callback wake, void *arg);
LUA_API void nl_settimer(lua_State *L, nl_Timer fn, void *data); /* [-0, +0, -] */
LUA_API void nl_setunhandled(lua_State *L);			/* [-1, +0, -] */

/* -- Threads and workers -------------------------------------------------- */

/* nl_pump runs the callbacks posted so far, each in a protected call (job
** results and events from workers arrive this way), then, with no worker
** threads (nl_setworkers(L, 0)), a step of each worker on this thread, then
** what the runtime library has queued (reports of unhandled failures). It
** returns the number of posted callbacks run, plus 1 when the workers or
** the runtime did anything. A callback must not call lua_close.
*/
LUA_API int nl_pump(lua_State *L);				/* [-0, +0, -] */
/* Pumps, waiting, until every job and call started by L has been delivered.
** Returns the number nl_pump would have. With no worker threads it returns
** when nothing can move any more without a host timer or another thread.
*/
LUA_API int nl_drain(lua_State *L);				/* [-0, +0, -] */
/* Callable from any thread. Pass the main state (or any thread of it that
** stays alive): another thread uses L only to find the state's inbox.
*/
LUA_API int nl_post(lua_State *L, nl_Callback fn, void *data);	/* any thread */
typedef void (*nl_Wakeup)(void *data);
/* The wakeup is called, on a posting thread, when the inbox of L stops being
** empty. nl_setwakeup returns once the old function can no longer be called
** (it waits for calls in progress), so the old data may be freed then. A
** wakeup function may post, but must not wait for the thread that owns the
** state, and must not call nl_setwakeup.
*/
LUA_API void nl_setwakeup(lua_State *L, nl_Wakeup fn, void *data); /* [-0, +0, -] */

/* The number of pool workers; 0 runs jobs and dedicated workers on the
** caller's thread while it pumps. The default is the hardware threads minus
** one, at least one. Set before the first worker starts (below): no effect
** when nl_workersstarted(L).
*/
LUA_API void nl_setworkers(lua_State *L, int n);		/* [-0, +0, -] */
/* fn runs in every new worker state W as lua_cpcall(W, fn, data) does, on
** the worker's thread, before any NL file loads. An error fails the worker
** with WorkerError. data is shared by all workers. No effect when
** nl_workersstarted(L).
*/
LUA_API void nl_setworkerinit(lua_State *L, lua_CFunction fn,
			      void *data);			/* [-0, +0, -] */
/* Pops encode (below) and decode (on top): userdata of the host type `type`
** cross between states as encode(ud) in the sender, decode(value) in the
** receiver. Every state registers the pair under the same name.
*/
LUA_API void nl_settransfer(lua_State *L, const char *type);	/* [-2, +0, e] */
/* Worker.run(f, args...): the function and nargs arguments on top; pushes
** the Task. Raises SendError as spawn does.
*/
LUA_API void nl_spawn(lua_State *L, int nargs);	/* [-(nargs+1), +1, e] */

/* nl_setworkers, nl_setworkerinit, nl_setloader, nl_addpath, nl_adddecl and
** the options are set on the main state, before the first worker starts (a
** spawn, Worker.new, nl_spawn). From then on the workers' settings are fixed
** (nl_workersstarted is 1): nl_setoption returns 0, except that
** NL_OPT_SANDBOX can still be turned on, and every worker applies it before
** it handles anything more; nl_setworkers, nl_setworkerinit, nl_setloader,
** nl_addpath and nl_adddecl have no effect (they are void: a host that must
** know tests nl_workersstarted first).
** In a worker, nl_setoption returns 0 and nl_setworkers, nl_setworkerinit,
** nl_setloader and nl_setreporter have no effect, while nl_addpath and
** nl_adddecl widen that worker's own search (from its init function, for
** instance). A worker copies the options, the loader, the search path and
** the declaration files when it starts, but not the reporter. The loader's
** functions are then called from worker threads too, so they must be safe
** to call from several threads at once.
*/

#ifdef __cplusplus
}
#endif

#endif
