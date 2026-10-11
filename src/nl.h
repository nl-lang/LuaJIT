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

LUALIB_API int luaopen_nl(lua_State *L);			/* [-0, +0, e] */
LUA_API int nl_setoption(lua_State *L, int opt, lua_Integer value); /* [-0, +0, -] */
LUA_API lua_Integer nl_getoption(lua_State *L, int opt);	/* [-0, +0, -] */
LUA_API int nl_isworker(lua_State *L);				/* [-0, +0, -] */

/* -- Loading ------------------------------------------------------------- */

LUA_API int nl_loadbuffer(lua_State *L, const char *buf, size_t size,
			  const char *name);			/* [-0, +1, m] */
LUA_API int nl_loadfile(lua_State *L, const char *filename);	/* [-0, +1, m] */
LUA_API void nl_import(lua_State *L, const char *path);	/* [-0, +1, e] */
LUA_API void nl_addpath(lua_State *L, const char *dir);	/* [-0, +0, m] */

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
LUA_API void nl_setreporter(lua_State *L, nl_Reporter fn, void *data); /* [-0, +0, -] */

/* -- Types and values ---------------------------------------------------- */

LUA_API const char *nl_typeof(lua_State *L, int idx);		/* [-0, +0, -] */
LUA_API int nl_isa(lua_State *L, int idx, int type);		/* [-0, +0, -] */
LUA_API void nl_gettype(lua_State *L, const char *name);	/* [-0, +1, -] */
LUA_API void nl_settype(lua_State *L, int idx, const char *name,
			int parent);				/* [-0, +0, e] */
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

/* -- Threads ------------------------------------------------------------- */

/* nl_pump runs the callbacks posted so far, each in a protected call, then
** what the runtime library has queued (results of the spawn stand-in,
** reports of unhandled failures). It returns the number of posted callbacks
** run, plus 1 when the runtime delivered anything. A callback must not call
** lua_close.
*/
LUA_API int nl_pump(lua_State *L);				/* [-0, +0, -] */
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

#ifdef __cplusplus
}
#endif

#endif
