/*
** NL per-state data: the inbox (nl_post, nl_pump, nl_setwakeup), options,
** the loader and reporter hooks, and the import search path.
**
** The thread rule (DESIGN 14.4, API.md A5): every function here that takes L
** runs on the thread that owns L, except nl_post. nl_post reaches the inbox
** through G(L)->nl, which is set when the state is created and never changes
** until lua_close, so it only reads memory that no other thread writes.
*/

#define lj_nlstate_c
#define LUA_CORE

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "lj_nlint.h"

#if LJ_NL

#include "lauxlib.h"

#if LJ_TARGET_WINDOWS
#include <direct.h>
#define NL_PATHSEP	';'
#define nl_getcwd	_getcwd
#else
#include <unistd.h>
#define NL_PATHSEP	':'
#define nl_getcwd	getcwd
#endif

/* -- Mutex --------------------------------------------------------------- */

#if LJ_TARGET_WINDOWS
static int sync_init(NLState *ns)
{
  InitializeSRWLock(&ns->mtx);
  InitializeConditionVariable(&ns->idle);
  return 0;
}
static void sync_destroy(NLState *ns) { UNUSED(ns); }
void lj_nl_lock(NLState *ns) { AcquireSRWLockExclusive(&ns->mtx); }
void lj_nl_unlock(NLState *ns) { ReleaseSRWLockExclusive(&ns->mtx); }
static void idle_wait(NLState *ns)
{
  SleepConditionVariableSRW(&ns->idle, &ns->mtx, INFINITE, 0);
}
static void idle_signal(NLState *ns) { WakeAllConditionVariable(&ns->idle); }
#else
static int sync_init(NLState *ns)
{
  if (pthread_mutex_init(&ns->mtx, NULL) != 0) return -1;
  if (pthread_cond_init(&ns->idle, NULL) != 0) {
    pthread_mutex_destroy(&ns->mtx);
    return -1;
  }
  return 0;
}
static void sync_destroy(NLState *ns)
{
  pthread_cond_destroy(&ns->idle);
  pthread_mutex_destroy(&ns->mtx);
}
void lj_nl_lock(NLState *ns) { pthread_mutex_lock(&ns->mtx); }
void lj_nl_unlock(NLState *ns) { pthread_mutex_unlock(&ns->mtx); }
static void idle_wait(NLState *ns) { pthread_cond_wait(&ns->idle, &ns->mtx); }
static void idle_signal(NLState *ns) { pthread_cond_broadcast(&ns->idle); }
#endif

/* -- String lists -------------------------------------------------------- */

/* Appends a copy of s[0..len] to a growable list; 0 on OOM. */
static int strlist_push(char ***list, size_t *n, size_t *sz,
			const char *s, size_t len)
{
  char *copy;
  if (*n == *sz) {
    size_t nsz = *sz ? *sz*2 : 8;
    char **p = (char **)realloc(*list, nsz*sizeof(char *));
    if (!p) return 0;
    *list = p;
    *sz = nsz;
  }
  copy = (char *)malloc(len+1);
  if (!copy) return 0;
  memcpy(copy, s, len);
  copy[len] = '\0';
  (*list)[(*n)++] = copy;
  return 1;
}

static void strlist_free(char **list, size_t n)
{
  size_t i;
  for (i = 0; i < n; i++) free(list[i]);
  free(list);
}

/* -- Search path --------------------------------------------------------- */

static int path_push(NLState *ns, const char *dir, size_t len)
{
  if (len == 0) return 1;
  return strlist_push(&ns->path, &ns->npath, &ns->szpath, dir, len);
}

static int path_isabs(const char *p)
{
#if LJ_TARGET_WINDOWS
  return p[0] == '\\' || p[0] == '/' || (p[0] && p[1] == ':');
#else
  return p[0] == '/';
#endif
}

/* add_path(dir). R117: a relative directory is resolved against the working
** directory at the time of the call (the reading REVIEW.md assumes), so a
** later chdir does not move it.
*/
int lj_nl_addpath(NLState *ns, const char *dir)
{
  char cwd[4096];
  size_t len = strlen(dir);
  if (len == 0 || path_isabs(dir) || !nl_getcwd(cwd, sizeof(cwd)))
    return path_push(ns, dir, len);
  else {
    size_t clen = strlen(cwd);
    char *buf = (char *)malloc(clen + 1 + len + 1);
    int ok;
    if (!buf) return 0;
    memcpy(buf, cwd, clen);
    buf[clen] = '/';
    memcpy(buf+clen+1, dir, len+1);
    ok = path_push(ns, buf, clen+1+len);
    free(buf);
    return ok;
  }
}

/* -- Declaration files --------------------------------------------------- */

/* nl_adddecl (API.md 2.3): the name is kept as given; the loader in effect
** reads it when a file is compiled. A name given twice is kept once.
*/
int lj_nl_adddecl(NLState *ns, const char *name)
{
  size_t i, len = strlen(name);
  if (len == 0) return 1;
  for (i = 0; i < ns->ndecl; i++)
    if (strcmp(ns->decl[i], name) == 0) return 1;
  return strlist_push(&ns->decl, &ns->ndecl, &ns->szdecl, name, len);
}

/* The search path starts as NL_PATH, separated like the system's PATH;
** empty entries are skipped. A relative entry is resolved against the
** working directory when the state is created, as add_path resolves its
** directory when it is called (R117), so a later chdir moves neither.
*/
static int path_init(NLState *ns)
{
  const char *env = getenv("NL_PATH");
  if (env) {
    const char *p = env;
    for (;;) {
      const char *e = strchr(p, NL_PATHSEP);
      size_t len = e ? (size_t)(e - p) : strlen(p);
      if (len > 0) {
	char *dir = (char *)malloc(len + 1);
	int ok;
	if (!dir) return 0;
	memcpy(dir, p, len);
	dir[len] = '\0';
	ok = lj_nl_addpath(ns, dir);
	free(dir);
	if (!ok) return 0;
      }
      if (!e) break;
      p = e+1;
    }
  }
  return 1;
}

/* -- State lifecycle ----------------------------------------------------- */

int lj_nl_newstate(global_State *g)
{
  NLState *ns = (NLState *)calloc(1, sizeof(NLState));
  if (!ns) return -1;
  if (sync_init(ns) != 0) {
    free(ns);
    return -1;
  }
  ns->rtref = LUA_NOREF;
  ns->intrref = LUA_NOREF;
  setmref(g->nl_intr, NULL);
  ns->trampref = LUA_NOREF;
  ns->guardref = LUA_NOREF;
  ns->sandboxref = LUA_NOREF;
  ns->opt[NL_OPT_CHECKS] = 1;
  ns->opt[NL_OPT_STRIP] = 0;
  ns->opt[NL_OPT_MAXWORKERS] = 64;
  ns->opt[NL_OPT_WORKERMEM] = 0;
  ns->opt[NL_OPT_GLUA] = 0;
  ns->opt[NL_OPT_SANDBOX] = 0;
  g->nl = ns;
  if (!path_init(ns)) return -1;  /* lj_nl_freestate cleans up. */
  return 0;
}

void lj_nl_freestate(global_State *g)
{
  NLState *ns = (NLState *)g->nl;
  NLPost *p;
  if (!ns) return;
  /* A callback still queued when the state closes is called once with
  ** L == NULL, so that it can free its data (API.md 2.10). One that posts
  ** again from there gets its post called with NULL too: the inbox is
  ** drained until it stays empty.
  */
  for (;;) {
    lj_nl_lock(ns);
    p = ns->head;
    ns->head = ns->tail = NULL;
    lj_nl_unlock(ns);
    if (!p) break;
    while (p) {
      NLPost *next = p->next;
      p->fn(NULL, p->data);
      free(p);
      p = next;
    }
  }
  strlist_free(ns->path, ns->npath);
  strlist_free(ns->decl, ns->ndecl);
  sync_destroy(ns);
  free(ns);
  g->nl = NULL;
}

/* -- Inbox --------------------------------------------------------------- */

LUA_API int nl_post(lua_State *L, nl_Callback fn, void *data)
{
  NLState *ns;
  NLPost *p;
  int wasempty;
  nl_Wakeup wakeup;
  void *wakeupdata;
  if (!L || !fn || !(ns = nl_state(L))) return 0;
  p = (NLPost *)malloc(sizeof(NLPost));
  if (!p) return 0;
  p->fn = fn;
  p->data = data;
  p->next = NULL;
  lj_nl_lock(ns);
  wasempty = (ns->head == NULL);
  if (ns->tail) ns->tail->next = p; else ns->head = p;
  ns->tail = p;
  wakeup = wasempty ? ns->wakeup : NULL;
  wakeupdata = ns->wakeupdata;
  if (wakeup) ns->wakeup_calls++;
  lj_nl_unlock(ns);
  if (wakeup) {
    /* Called outside the lock, so that it may post. nl_setwakeup waits
    ** until no call of the old function is in progress.
    */
    wakeup(wakeupdata);
    lj_nl_lock(ns);
    if (--ns->wakeup_calls == 0) idle_signal(ns);
    lj_nl_unlock(ns);
  }
  return 1;
}

/* Returns once the old function can no longer be called: it waits for the
** calls in progress on other threads. So a wakeup function must not wait
** for the thread that owns the state (it may be blocked here), and must not
** call nl_setwakeup itself.
*/
LUA_API void nl_setwakeup(lua_State *L, nl_Wakeup fn, void *data)
{
  NLState *ns = nl_state(L);
  lj_nl_lock(ns);
  ns->wakeup = fn;
  ns->wakeupdata = fn ? data : NULL;
  while (ns->wakeup_calls > 0) idle_wait(ns);
  lj_nl_unlock(ns);
}

typedef struct PostCtx {
  nl_Callback fn;
  void *data;
} PostCtx;

static int post_call(lua_State *L)
{
  PostCtx *c = (PostCtx *)lua_touserdata(L, 1);
  lua_settop(L, 0);  /* The callback starts with an empty frame. */
  c->fn(L, c->data);
  return 0;
}

static int post_run(lua_State *L)
{
  PostCtx *c = (PostCtx *)lua_touserdata(L, 1);
  lua_pushcfunction(L, post_call);
  lua_pushlightuserdata(L, c);
  lj_nl_callhandled(L, 1);
  return 0;
}

/* Runs what is in the inbox, each callback in a protected call; an error goes
** to the unhandled hook. Callbacks posted while the pump runs, including by
** the callbacks themselves, wait for the next pump, so one pump always ends.
*/
LUA_API int nl_pump(lua_State *L)
{
  NLState *ns = nl_state(L);
  NLPost *p;
  int n = 0;
  lj_nl_lock(ns);
  p = ns->head;
  ns->head = ns->tail = NULL;
  lj_nl_unlock(ns);
  while (p) {
    NLPost *next = p->next;
    PostCtx c;
    c.fn = p->fn;
    c.data = p->data;
    free(p);
    /* Only an error outside the callback's own protected call, such as
    ** running out of memory before it starts, gets to lj_nl_report here.
    */
    if (lua_cpcall(L, post_run, &c) != 0)
      lj_nl_report(L);
    n++;
    p = next;
  }
  /* Then what the runtime has queued (runtime/nl.lua, rt.pump): results of
  ** the spawn stand-in, reports of unhandled failures, including those of
  ** the callbacks just run.
  */
  return n + lj_nl_pumprt(L);
}

/* -- Options ------------------------------------------------------------- */

LUA_API int nl_setoption(lua_State *L, int opt, lua_Integer value)
{
  NLState *ns = nl_state(L);
  switch (opt) {
  case NL_OPT_CHECKS: case NL_OPT_STRIP: case NL_OPT_GLUA:
    if (value != 0 && value != 1) return 0;
    break;
  case NL_OPT_SANDBOX:
    /* One way: a state that has loaded untrusted code keeps the sandbox, or
    ** that code's later calls of load_nl, include and require would load
    ** with the full environment.
    */
    if (value != 0 && value != 1) return 0;
    if (value == 0 && ns->opt[opt] != 0) return 0;
    break;
  case NL_OPT_MAXWORKERS: case NL_OPT_WORKERMEM:
    /* Stored for nl_getoption. Workers do not exist before M10. */
    if (value < 0) return 0;
    break;
  default:
    return 0;
  }
  ns->opt[opt] = value;
  return 1;
}

LUA_API lua_Integer nl_getoption(lua_State *L, int opt)
{
  NLState *ns = nl_state(L);
  if (opt < 1 || opt > NL_OPT__MAX) return 0;
  return ns->opt[opt];
}

/* Read by lj_lex_setup: whether the chunks loaded from now on are GLua. */
int lj_nl_glua(lua_State *L)
{
  return nl_state(L)->opt[NL_OPT_GLUA] != 0;
}

LUA_API int nl_isworker(lua_State *L)
{
  UNUSED(L);
  return 0;  /* No workers before M10. */
}

/* -- Loader and reporter ------------------------------------------------- */

LUA_API void nl_setloader(lua_State *L, const nl_Loader *loader)
{
  NLState *ns = nl_state(L);
  if (loader && loader->resolve && loader->read) {
    ns->loader = *loader;
    ns->hasloader = 1;
  } else {
    memset(&ns->loader, 0, sizeof(ns->loader));
    ns->hasloader = 0;
  }
}

LUA_API void nl_setreporter(lua_State *L, nl_Reporter fn, void *data)
{
  NLState *ns = nl_state(L);
  ns->reporter = fn;
  ns->reporterdata = fn ? data : NULL;
}

/* -- Version ------------------------------------------------------------- */

LUA_API void NL_VERSION_SYM(void)
{
}

#endif
