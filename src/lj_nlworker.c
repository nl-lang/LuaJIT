/*
** NL workers: the pool, its job queue, dedicated workers, their loops and
** timers, and the host functions nl_drain, nl_setworkers, nl_setworkerinit,
** nl_settransfer and nl_spawn.
**
** A worker is a state of its own (luaL_newstate, then luaL_openlibs, which
** opens NL and the runtime) with an optional thread. With threads (the
** default: hardware threads minus one, at least one) each worker's thread
** runs its loop. With nl_setworkers(L, 0) the same loop runs on the main
** state's thread, one step per worker each time it pumps. A
** worker is entered by its own thread only: its thread, or the main thread
** for a pumped worker.
**
** Everything here belongs to one main state (NLWorkers, ns->wk), which every
** worker it starts points to as well. lua_close of the main state stops the
** workers and waits for them (lj_nl_closeworkers) before its inbox is
** drained.
**
** Messages are encoded by lj_nltransfer.c and travel as envelopes (NLEnv)
** posted to the target state's inbox (nl_post), so they arrive in the order
** they were sent and are handled when that state pumps: a job's result, a
** call to a dedicated worker, an event, a request to stop. Jobs for the pool
** wait in a queue of their own (NLWorkers.qhead), from which an idle worker
** takes one; a worker that has loaded the job's file is preferred.
**
** A result goes to a port (NLPort): a reference-counted address that knows
** whether its state is still alive, so that a worker never posts to a state
** that has closed. A request whose state closes is answered with WorkerError.
**
** Locks, always taken in this order when nested: a port's lock (held while
** posting, which may call the target's wakeup function), then the workers'
** lock (wk->mtx, which a worker's wakeup takes), then a state's inbox lock
** (lj_nlstate.c). A port is never locked while wk->mtx is held; port
** references are counted with atomic operations, without a lock.
**
** The Lua side is runtime/nl_64_worker.lua. It gets the functions of
** host.worker (lj_nl_pushhostworker) and provides these, which the loop calls:
**
**   rt.workerstart(kind, from, path, failure)  a new worker state, after
**                                       the host's init function
**   rt.workerjob(msg, req, port)        run a job of the pool
**   rt.workerin(kind, ...)              a message from the inbox
**   rt.workerstep() -> deadline, busy   fire due timers; the next deadline
**                                       (or nil) and whether the runtime has
**                                       queued work
**   rt.workercount(n)                   Worker.count
*/

#define lj_nlworker_c
#define LUA_CORE

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

#include "lj_nlint.h"

#if LJ_NL

#include "lauxlib.h"
#include "lualib.h"

#if LJ_TARGET_WINDOWS
#include <process.h>
#else
#include <unistd.h>
#include <sys/time.h>
#endif

/* -- Threads, locks and the clock ---------------------------------------- */

#if LJ_TARGET_WINDOWS
typedef HANDLE NLThread;
static int mtx_init(NLMutex *m) { InitializeSRWLock(m); return 0; }
static void mtx_destroy(NLMutex *m) { UNUSED(m); }
static void mtx_lock(NLMutex *m) { AcquireSRWLockExclusive(m); }
static void mtx_unlock(NLMutex *m) { ReleaseSRWLockExclusive(m); }
int lj_nl_condinit(NLCond *c) { InitializeConditionVariable(c); return 0; }
static int cond_init(NLCond *c) { InitializeConditionVariable(c); return 0; }
static void cond_destroy(NLCond *c) { UNUSED(c); }
static void cond_signal(NLCond *c) { WakeConditionVariable(c); }

static void cond_wait(NLCond *c, NLMutex *m, double seconds)
{
  DWORD ms = seconds < 0 ? INFINITE :
	     seconds > 1e6 ? 1000000000u : (DWORD)(seconds * 1000.0 + 0.999);
  SleepConditionVariableSRW(c, m, ms, 0);
}
typedef struct ThreadStart { void *(*fn)(void *); void *arg; } ThreadStart;
static unsigned __stdcall thread_tramp(void *p)
{
  ThreadStart s = *(ThreadStart *)p;
  free(p);
  s.fn(s.arg);
  return 0;
}
static int thread_start(NLThread *t, void *(*fn)(void *), void *arg)
{
  ThreadStart *s = (ThreadStart *)malloc(sizeof(ThreadStart));
  uintptr_t h;
  if (!s) return -1;
  s->fn = fn;
  s->arg = arg;
  h = _beginthreadex(NULL, 8u << 20, thread_tramp, s, 0, NULL);
  if (!h) { free(s); return -1; }
  *t = (HANDLE)h;
  return 0;
}
static void thread_join(NLThread t)
{
  WaitForSingleObject(t, INFINITE);
  CloseHandle(t);
}
static double now_mono(void)
{
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart / (double)f.QuadPart;
}
static int hw_threads(void)
{
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return (int)si.dwNumberOfProcessors;
}
#else
typedef pthread_t NLThread;
static int mtx_init(NLMutex *m) { return pthread_mutex_init(m, NULL); }
static void mtx_destroy(NLMutex *m) { pthread_mutex_destroy(m); }
static void mtx_lock(NLMutex *m) { pthread_mutex_lock(m); }
static void mtx_unlock(NLMutex *m) { pthread_mutex_unlock(m); }
/* Timed waits measure time on the monotonic clock, as the deadlines of
** worker timers do (now_mono), so that a change of the wall clock moves
** neither: Linux and the BSDs set the condition variable's clock to
** CLOCK_MONOTONIC; macOS has a relative wait instead.
*/
#if defined(__APPLE__)
#define NL_COND_RELATIVE	1
#elif defined(CLOCK_MONOTONIC) && !defined(__ANDROID__)
#define NL_COND_MONOTONIC	1
#endif

int lj_nl_condinit(NLCond *c)
{
#ifdef NL_COND_MONOTONIC
  pthread_condattr_t a;
  int r;
  if (pthread_condattr_init(&a) != 0) return -1;
  pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
  r = pthread_cond_init(c, &a);
  pthread_condattr_destroy(&a);
  return r;
#else
  return pthread_cond_init(c, NULL);
#endif
}

static int cond_init(NLCond *c) { return lj_nl_condinit(c); }
static void cond_destroy(NLCond *c) { pthread_cond_destroy(c); }
static void cond_signal(NLCond *c) { pthread_cond_signal(c); }

/* Waits for c, at most `seconds` (forever when negative). c was made by
** lj_nl_condinit.
*/
static void cond_wait(NLCond *c, NLMutex *m, double seconds)
{
  if (seconds < 0) {
    pthread_cond_wait(c, m);
  } else {
    struct timespec ts;
    if (seconds > 1e6) seconds = 1e6;
#ifdef NL_COND_RELATIVE
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    pthread_cond_timedwait_relative_np(c, m, &ts);
#else
    {
      double s;
#ifdef NL_COND_MONOTONIC
      clock_gettime(CLOCK_MONOTONIC, &ts);
#else
      clock_gettime(CLOCK_REALTIME, &ts);
#endif
      s = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9 + seconds;
      ts.tv_sec = (time_t)s;
      ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
      if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
      if (ts.tv_nsec < 0) ts.tv_nsec = 0;
      pthread_cond_timedwait(c, m, &ts);
    }
#endif
  }
}
static int thread_start(NLThread *t, void *(*fn)(void *), void *arg)
{
  pthread_attr_t attr;
  int r;
  if (pthread_attr_init(&attr) != 0) return -1;
  /* The compiler (libnlc) runs on worker threads too: the stack of a main
  ** thread, not the smaller default of other threads.
  */
  pthread_attr_setstacksize(&attr, (size_t)8 << 20);
  r = pthread_create(t, &attr, fn, arg);
  pthread_attr_destroy(&attr);
  return r == 0 ? 0 : -1;
}
static void thread_join(NLThread t)
{
  pthread_join(t, NULL);
}
static double now_mono(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
static int hw_threads(void)
{
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (int)n : 1;
}
#endif

static char *str_dup(const char *s)
{
  size_t n;
  char *p;
  if (!s) return NULL;
  n = strlen(s) + 1;
  p = (char *)malloc(n);
  if (p) memcpy(p, s, n);
  return p;
}

/* -- Ports --------------------------------------------------------------- */

#if LJ_TARGET_WINDOWS
typedef volatile LONG NLRefs;
#define refs_inc(p)	InterlockedIncrement(p)
#define refs_dec(p)	InterlockedDecrement(p)
#else
typedef int NLRefs;
#define refs_inc(p)	__atomic_add_fetch((p), 1, __ATOMIC_ACQ_REL)
#define refs_dec(p)	__atomic_sub_fetch((p), 1, __ATOMIC_ACQ_REL)
#endif

/* The references are counted without the lock, so that taking one never
** waits for a post in progress (see the lock order above).
*/
struct NLPort {
  NLMutex mtx;
  NLRefs refs;
  lua_State *L;		/* Guarded by mtx; NULL once the state closes. */
};

static NLPort *port_new(lua_State *L)
{
  NLPort *p = (NLPort *)malloc(sizeof(NLPort));
  if (!p) return NULL;
  if (mtx_init(&p->mtx) != 0) { free(p); return NULL; }
  p->refs = 1;
  p->L = L;
  return p;
}

static void port_ref(NLPort *p)
{
  refs_inc(&p->refs);
}

void lj_nl_portunref(NLPort *p)
{
  if (!p) return;
  if (refs_dec(&p->refs) == 0) {
    mtx_destroy(&p->mtx);
    free(p);
  }
}

/* The state is closing: nothing more is posted to it. */
void lj_nl_portkill(NLPort *p)
{
  if (!p) return;
  mtx_lock(&p->mtx);
  p->L = NULL;
  mtx_unlock(&p->mtx);
}

/* Posts fn(data) to the port's state; 0 when it has closed. */
static int port_post(NLPort *p, nl_Callback fn, void *data)
{
  int ok = 0;
  if (!p) return 0;
  mtx_lock(&p->mtx);
  if (p->L) ok = nl_post(p->L, fn, data);
  mtx_unlock(&p->mtx);
  return ok;
}

/* The port of L's state, made the first time. NULL when out of memory. */
static NLPort *state_port(lua_State *L)
{
  NLState *ns = nl_state(L);
  if (!ns->port) ns->port = port_new(mainthread(G(L)));
  return ns->port;
}

/* -- Settings a worker copies from the main state ------------------------ */

typedef struct NLSettings {
  lua_Integer opt[NL_OPT__MAX+1];
  int hasloader;
  nl_Loader loader;
  char **path;
  size_t npath, declnpath;
  char **decl;
  size_t ndecl;
} NLSettings;

static void settings_free(NLSettings *s)
{
  lj_nl_strlist_free(s->path, s->npath);
  lj_nl_strlist_free(s->decl, s->ndecl);
  memset(s, 0, sizeof(*s));
}

static char **strs_copy(char **src, size_t n)
{
  size_t i;
  char **dst = (char **)calloc(n ? n : 1, sizeof(char *));
  if (!dst) return NULL;
  for (i = 0; i < n; i++)
    if (!(dst[i] = str_dup(src[i]))) {
      while (i > 0) free(dst[--i]);
      free(dst);
      return NULL;
    }
  return dst;
}

/* -- Requests and envelopes ---------------------------------------------- */

/* A job or a call waiting for its result: in the pool's queue (a job not
** taken yet), or in the running list of the worker handling it.
*/
typedef struct NLReq {
  struct NLReq *next, *prev;
  NLPort *reply;	/* Where the result goes (a reference). */
  double id;		/* The requester's number for it. */
  NLMsg *msg;		/* A job's message until a worker takes it. */
  char *file;		/* A job's file: the pool prefers a worker that has it. */
  int skips;		/* How often a later job was taken before it. */
} NLReq;

enum { ENV_RESULT, ENV_CALL, ENV_EMIT, ENV_STOP };
/* The status of a result. */
enum { RES_DONE, RES_FAILED, RES_WORKERERROR };

typedef struct NLEnv {
  int kind, status;
  double id;
  NLMsg *msg;
  char *text;		/* RES_WORKERERROR: the message. */
  NLPort *reply;	/* ENV_CALL: where the result goes (a reference). */
} NLEnv;

static void env_free(NLEnv *e)
{
  if (e->msg) lj_nl_msgfree(e->msg);
  free(e->text);
  lj_nl_portunref(e->reply);
  free(e);
}

static void env_deliver(lua_State *L, void *data);

/* Posts a result to `to`; frees msg when it cannot be posted. */
static void reply_post(NLPort *to, double id, int status, NLMsg *msg,
		       const char *text)
{
  NLEnv *e = (NLEnv *)calloc(1, sizeof(NLEnv));
  if (!e) {
    if (msg) lj_nl_msgfree(msg);
    return;
  }
  e->kind = ENV_RESULT;
  e->status = status;
  e->id = id;
  e->msg = msg;
  e->text = text ? str_dup(text) : NULL;
  if (!port_post(to, env_deliver, e)) env_free(e);
}

static void req_free(NLReq *r)
{
  if (r->msg) lj_nl_msgfree(r->msg);
  lj_nl_portunref(r->reply);
  free(r->file);
  free(r);
}

/* -- Workers ------------------------------------------------------------- */

enum { W_POOL, W_DEDICATED };

struct NLWorker {
  NLWorkers *wk;
  NLWorker *next;		/* In wk->workers. */
  int kind;			/* W_POOL, W_DEDICATED. */
  int threaded;
  NLThread thread;
  int joined, exited;		/* Guarded by wk->mtx. */
  int hrefs;			/* Handles (WorkerBox) that name it; guarded by
				** wk->mtx. A dedicated worker that has ended
				** is freed once none is left (workers_reap). */
  lua_State *L;			/* Its state: NULL once closed. */
  NLPort *port;			/* The state's port (ns->port of L). */
  NLPort *parentport;		/* Dedicated: the state that started it. */
  NLWorker *parent;		/* Dedicated: the worker that started it. */
  char *from, *path;		/* Dedicated: its file, as Worker.new gave it. */
  NLCond cond;			/* Signalled with wk->mtx held. */
  /* Guarded by wk->mtx. */
  int wake;			/* The inbox stopped being empty. */
  int stop;			/* Asked to end (Worker.stop, lua_close). */
  int idle;			/* Waiting for work. */
  /* Its own thread only. */
  int opened;			/* The state is set up. */
  int closed;			/* Ended for good. */
  int broken;			/* No Lua can run in it (the libraries failed). */
  int discard;			/* To be discarded (memory, a failure). */
  int served;			/* Pool: took a request since its state opened. */
  char *failure;		/* Why: the message of WorkerError. */
  char **files;			/* Files its state has loaded (pool). */
  size_t nfiles, szfiles;	/* Guarded by wk->mtx: read by enqueue. */
  NLReq *running;		/* Requests it has taken and not answered. */
  double deadline;		/* Its nearest timer, or -1. */
  /* The memory limit (NL_OPT_WORKERMEM): the allocator counts. */
  lua_Alloc of;
  void *oud;
  size_t mem, limit;
  int overlimit;
};

struct NLWorkers {
  NLMutex mtx;
  lua_State *mainL;
  int npool;			/* Pool size, -1 for the default. */
  int started;			/* The pool has started. */
  int threads;			/* Workers have threads (fixed at the start). */
  int threadmode_set;		/* threads is decided. */
  NLWorker *workers;		/* Every worker, pool and dedicated. */
  int npoolalive, ndedicated;
  NLReq *qhead, *qtail;		/* The job queue. */
  size_t qlen;
  lua_CFunction init;		/* nl_setworkerinit. */
  void *initdata;
  NLSettings set;		/* What a new worker copies (guarded). */
  int frozen;			/* The settings are fixed: the pool has started
				** or a dedicated worker exists. */
  int sandbox;			/* NL_OPT_SANDBOX is on: every worker applies it
				** before it loads anything more. */
  int closing;
};

static int pool_default(void)
{
  int n = hw_threads() - 1;
  return n < 1 ? 1 : n;
}

static int pool_size(NLWorkers *wk)
{
  return wk->npool < 0 ? pool_default() : wk->npool;
}

/* The workers of L's main state, made the first time (on the main state). */
static NLWorkers *workers_get(lua_State *L)
{
  NLState *ns = nl_state(L);
  NLWorkers *wk = ns->wk;
  if (wk) return wk;
  wk = (NLWorkers *)calloc(1, sizeof(NLWorkers));
  if (!wk) return NULL;
  if (mtx_init(&wk->mtx) != 0) { free(wk); return NULL; }
  wk->mainL = mainthread(G(L));
  wk->npool = -1;
  ns->wk = wk;
  return wk;
}

/* Copies the main state's settings for the workers started from now on.
** Only on the main state's thread, which owns them.
*/
static int settings_snapshot(lua_State *L, NLWorkers *wk)
{
  NLState *ns = nl_state(L);
  NLSettings s;
  int frozen;
  memset(&s, 0, sizeof(s));
  if (ns->worker) return 1;  /* A worker uses what it was given. */
  mtx_lock(&wk->mtx);
  frozen = wk->frozen;
  mtx_unlock(&wk->mtx);
  if (frozen) return 1;  /* Taken once, when the first worker started. */
  memcpy(s.opt, ns->opt, sizeof(s.opt));
  s.hasloader = ns->hasloader;
  s.loader = ns->loader;
  s.path = strs_copy(ns->path, ns->npath);
  s.decl = strs_copy(ns->decl, ns->ndecl);
  if (!s.path || !s.decl) {
    free(s.path); free(s.decl);
    return 0;
  }
  s.npath = ns->npath;
  s.declnpath = ns->declnpath;
  s.ndecl = ns->ndecl;
  mtx_lock(&wk->mtx);
  settings_free(&wk->set);
  wk->set = s;
  wk->frozen = 1;
  if (s.opt[NL_OPT_SANDBOX]) wk->sandbox = 1;
  mtx_unlock(&wk->mtx);
  return 1;
}

/* Whether the workers' settings are fixed (nl_setoption, lj_nlstate.c). */
int lj_nl_workersfrozen(NLState *ns)
{
  NLWorkers *wk = ns->wk;
  int f;
  if (!wk) return 0;
  mtx_lock(&wk->mtx);
  f = wk->frozen;
  mtx_unlock(&wk->mtx);
  return f;
}

/* NL_OPT_SANDBOX was turned on in the main state: every worker, running
** or not yet started, applies it before it loads anything more.
*/
void lj_nl_workerssandbox(NLState *ns)
{
  NLWorkers *wk = ns->wk;
  if (!wk) return;
  mtx_lock(&wk->mtx);
  /* As nl_setoption does for a state (lj_nlstate.c): the directories that
  ** feed declaration files in the sandbox are those added before it, here
  ** every one the workers copy.
  */
  if (!wk->set.opt[NL_OPT_SANDBOX]) wk->set.declnpath = wk->set.npath;
  wk->sandbox = 1;
  wk->set.opt[NL_OPT_SANDBOX] = 1;
  mtx_unlock(&wk->mtx);
}

/* In a worker: applies the sandbox when the main state has turned it on
** since the worker last looked. Called before each step, each job and each
** message, so that a job, a call or an event that arrives after the main
** state's nl_setoption has returned loads its files sandboxed. A step that
** was already running when the option went on can still load a file
** unsandboxed before it ends: a result that it decodes later in that step
** may name a file this worker lacks.
*/
static void worker_syncsandbox(NLWorker *w)
{
  NLState *ns;
  int on;
  if (!w->L) return;
  ns = nl_state(w->L);
  if (ns->opt[NL_OPT_SANDBOX]) return;
  mtx_lock(&w->wk->mtx);
  on = w->wk->sandbox;
  mtx_unlock(&w->wk->mtx);
  if (on) {
    ns->opt[NL_OPT_SANDBOX] = 1;
    ns->declnpath = ns->npath;  /* As nl_setoption does (lj_nlstate.c). */
  }
}

/* Counts delta bytes (the transfer encoder's buffer) toward the memory
** limit of the worker that L is. 0 when that would exceed it (the worker is
** then discarded, as for its own allocations); always 1 outside workers.
*/
int lj_nl_memcharge(lua_State *L, ptrdiff_t delta)
{
  NLWorker *w = nl_state(L)->worker;
  if (!w) return 1;
  if (delta < 0) {
    size_t d = (size_t)(-delta);
    w->mem -= d <= w->mem ? d : w->mem;
    return 1;
  }
  if (w->limit && w->mem + (size_t)delta > w->limit) {
    w->overlimit = 1;
    return 0;
  }
  w->mem += (size_t)delta;
  return 1;
}

/* The allocator of a worker state with a memory limit. */
static void *count_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
  NLWorker *w = (NLWorker *)ud;
  size_t old = ptr ? osize : 0;
  void *p;
  if (nsize == 0) {
    p = w->of(w->oud, ptr, osize, 0);
    w->mem -= old <= w->mem ? old : w->mem;
    return p;
  }
  if (w->limit && nsize > old && w->mem + (nsize - old) > w->limit) {
    w->overlimit = 1;
    return NULL;
  }
  p = w->of(w->oud, ptr, osize, nsize);
  if (p) w->mem = w->mem - old + nsize;
  return p;
}

static void worker_wakeup(void *data)
{
  NLWorker *w = (NLWorker *)data;
  NLWorkers *wk = w->wk;
  mtx_lock(&wk->mtx);
  w->wake = 1;
  cond_signal(&w->cond);
  mtx_unlock(&wk->mtx);
}

/* For tests only (tests/vm/real/test_workers.c: a pool whose workers cannot
** start fails the jobs given to it): when 0 or more, how many more
** worker states may be made; the next one fails as if out of memory. -1,
** the default, is no limit. Read and written under the workers' lock.
*/
int lj_nl_test_workerstates = -1;

/* A new state for w, with the settings copied from the main state. NULL when
** out of memory.
*/
static lua_State *worker_newstate(NLWorker *w)
{
  NLWorkers *wk = w->wk;
  lua_State *L;
  NLState *ns;
  size_t i;
  int refuse = 0;
  mtx_lock(&wk->mtx);
  if (lj_nl_test_workerstates == 0) refuse = 1;
  else if (lj_nl_test_workerstates > 0) lj_nl_test_workerstates--;
  mtx_unlock(&wk->mtx);
  if (refuse) return NULL;
  L = luaL_newstate();
  if (!L) return NULL;
  ns = nl_state(L);
  ns->wk = wk;
  ns->worker = w;
  w->of = lua_getallocf(L, &w->oud);
  w->mem = (size_t)lua_gc(L, LUA_GCCOUNT, 0) * 1024 +
	   (size_t)lua_gc(L, LUA_GCCOUNTB, 0);
  w->overlimit = 0;
  mtx_lock(&wk->mtx);
  w->limit = wk->set.opt[NL_OPT_WORKERMEM] > 0 ?
	     (size_t)wk->set.opt[NL_OPT_WORKERMEM] : 0;
  for (i = 0; i < wk->set.npath; i++) lj_nl_addpath(ns, wk->set.path[i]);
  ns->declnpath = wk->set.declnpath;
  for (i = 0; i < wk->set.ndecl; i++) lj_nl_adddecl(ns, wk->set.decl[i]);
  if (wk->set.hasloader) {
    ns->loader = wk->set.loader;
    ns->hasloader = 1;
  }
  mtx_unlock(&wk->mtx);
  lua_setallocf(L, count_alloc, w);
  {
    NLPort *p = state_port(L);
    if (!p) {
      lua_setallocf(L, w->of, w->oud);
      lua_close(L);
      return NULL;
    }
    port_ref(p);  /* One for ns->port, one for w->port. */
    mtx_lock(&wk->mtx);
    w->port = p;
    mtx_unlock(&wk->mtx);
  }
  nl_setwakeup(L, worker_wakeup, w);
  return L;
}

static void worker_setfailure(NLWorker *w, const char *msg)
{
  if (w->failure) return;
  w->failure = str_dup(msg ? msg : "the worker failed");
}

/* -- The Lua side of the loop -------------------------------------------- */

static int open_f(lua_State *L)
{
  NLWorker *w = (NLWorker *)lua_touserdata(L, 1);
  NLWorkers *wk = w->wk;
  NLState *ns = nl_state(L);
  int i;
  luaL_openlibs(L);
  /* The options (a worker gets a copy when it starts). */
  mtx_lock(&wk->mtx);
  for (i = 1; i <= NL_OPT__MAX; i++) {
    if (i == NL_OPT_SANDBOX) continue;
    ns->opt[i] = wk->set.opt[i];
  }
  if (wk->set.opt[NL_OPT_SANDBOX]) ns->opt[NL_OPT_SANDBOX] = 1;
  mtx_unlock(&wk->mtx);
  return 0;
}

static int start_f(lua_State *L)
{
  NLWorker *w = (NLWorker *)lua_touserdata(L, 1);
  lj_nl_pushrt(L, "workerstart");
  lua_pushstring(L, w->kind == W_POOL ? "pool" : "dedicated");
  if (w->from) lua_pushstring(L, w->from); else lua_pushnil(L);
  if (w->path) lua_pushstring(L, w->path); else lua_pushnil(L);
  if (w->failure) lua_pushstring(L, w->failure); else lua_pushnil(L);
  lua_call(L, 4, 0);
  return 0;
}

/* A full collection (finalizers may raise). */
static int collect_f(lua_State *L)
{
  lua_gc(L, LUA_GCCOLLECT, 0);
  return 0;
}

/* The message of the error on top, popped. */
static void pop_message(lua_State *L, char *buf, size_t size)
{
  const char *s = lua_tostring(L, -1);
  if (!s && lj_nl_getrt(L, "tostring")) {
    lua_pushvalue(L, -2);
    if (lua_pcall(L, 1, 1, 0) == 0) s = lua_tostring(L, -1);
    if (s) {
      snprintf(buf, size, "%s", s);
      lua_pop(L, 2);
      return;
    }
    lua_pop(L, 1);
  }
  snprintf(buf, size, "%s", s ? s : "(error object is not a string)");
  lua_pop(L, 1);
}

/* Sets the new state up: the libraries, the host's init function, then the
** runtime's start (a dedicated worker loads its file there).
*/
static void worker_open(NLWorker *w)
{
  lua_State *L = w->L;
  NLWorkers *wk = w->wk;
  lua_CFunction init;
  void *initdata;
  char msg[512];
  w->opened = 1;
  w->deadline = -1;
  if (lua_cpcall(L, open_f, w) != 0) {
    pop_message(L, msg, sizeof(msg));
    w->broken = 1;
    if (w->overlimit) {
      /* The limit is below what the libraries and the runtime need: the
      ** worker stays as it is, failing every request, rather than being
      ** replaced by one that fails the same way.
      */
      snprintf(msg, sizeof(msg), "the memory limit of a worker (NL_OPT_WORKERMEM, "
	       "%llu bytes) is too small to open its libraries",
	       (unsigned long long)w->limit);
      w->overlimit = 0;
    }
    worker_setfailure(w, msg);
    return;
  }
  mtx_lock(&wk->mtx);
  init = wk->init;
  initdata = wk->initdata;
  mtx_unlock(&wk->mtx);
  if (init) {
    /* The init function runs without the memory limit, so that a host
    ** binding that allocates outside a protected call (a Rust host's
    ** attachment to the state) is not failed halfway; the memory it leaves
    ** counts, and a worker over its limit afterwards fails below.
    */
    size_t limit = w->limit;
    w->limit = 0;
    if (lua_cpcall(L, init, initdata) != 0) {
      char full[600];
      pop_message(L, msg, sizeof(msg));
      snprintf(full, sizeof(full), "the worker-init function failed: %s", msg);
      worker_setfailure(w, full);
    }
    if (limit && w->mem > limit && lua_cpcall(L, collect_f, NULL) != 0)
      lua_pop(L, 1);  /* A finalizer's error: the count decides. */
    w->limit = limit;
    if (limit && w->mem > limit) w->overlimit = 1;
  }
  worker_syncsandbox(w);
  if (!w->overlimit && lua_cpcall(L, start_f, w) != 0) {
    pop_message(L, msg, sizeof(msg));
    w->broken = 1;
    worker_setfailure(w, msg);
  }
  if (w->overlimit) {
    /* Over the limit while it opened, the init function or its start
    ** included: the worker stays as it is, failing every request, rather
    ** than being replaced by one that fails the same way.
    */
    snprintf(msg, sizeof(msg), "the memory limit of a worker (NL_OPT_WORKERMEM, "
	     "%llu bytes) is too small for its libraries, the worker-init "
	     "function and its start",
	     (unsigned long long)w->limit);
    w->broken = 1;
    w->overlimit = 0;
    free(w->failure);
    w->failure = str_dup(msg);
  }
}

typedef struct StepCtx {
  double deadline;
  int busy;
} StepCtx;

static int step_f(lua_State *L)
{
  StepCtx *c = (StepCtx *)lua_touserdata(L, 1);
  c->deadline = -1;
  c->busy = 0;
  if (!lj_nl_getrt(L, "workerstep")) return 0;
  lua_call(L, 0, 2);
  if (lua_isnumber(L, -2)) c->deadline = lua_tonumber(L, -2);
  c->busy = lua_toboolean(L, -1);
  return 0;
}

/* Pushes a box for msg (a userdata that frees it when collected). */
static void push_msgbox(lua_State *L, NLMsg *m);
static void push_portbox(lua_State *L, NLPort *p);

typedef struct JobCtx {
  NLReq *r;
} JobCtx;

static int job_f(lua_State *L)
{
  JobCtx *c = (JobCtx *)lua_touserdata(L, 1);
  NLReq *r = c->r;
  NLMsg *m = r->msg;
  lj_nl_pushrt(L, "workerjob");
  r->msg = NULL;
  push_msgbox(L, m);
  lua_pushlightuserdata(L, r);
  push_portbox(L, r->reply);
  lua_call(L, 3, 0);
  return 0;
}

static void running_add(NLWorker *w, NLReq *r)
{
  r->prev = NULL;
  r->next = w->running;
  if (w->running) w->running->prev = r;
  w->running = r;
}

static void running_remove(NLWorker *w, NLReq *r)
{
  if (r->prev) r->prev->next = r->next; else w->running = r->next;
  if (r->next) r->next->prev = r->prev;
  r->next = r->prev = NULL;
}

static int running_has(NLWorker *w, NLReq *r)
{
  NLReq *x;
  for (x = w->running; x; x = x->next)
    if (x == r) return 1;
  return 0;
}

/* -- The job queue ------------------------------------------------------- */

static int worker_hasfile(NLWorker *w, const char *file)
{
  size_t i;
  if (!file) return 0;
  for (i = 0; i < w->nfiles; i++)
    if (strcmp(w->files[i], file) == 0) return 1;
  return 0;
}

/* Under wk->mtx: wakes an idle pool worker for job r, one that has loaded
** its file if there is one.
*/
static void queue_signal(NLWorkers *wk, NLReq *r)
{
  NLWorker *w, *any = NULL;
  for (w = wk->workers; w; w = w->next) {
    if (w->kind != W_POOL || !w->idle || w->stop) continue;
    if (worker_hasfile(w, r->file)) {
      w->idle = 0;
      cond_signal(&w->cond);
      return;
    }
    if (!any) any = w;
  }
  if (any) {
    any->idle = 0;
    cond_signal(&any->cond);
  }
}

/* Queues job r; 0 (r not queued) when no pool worker is alive to run it,
** decided in the same critical section that queues it.
*/
static int queue_push(NLWorkers *wk, NLReq *r)
{
  mtx_lock(&wk->mtx);
  if (wk->npoolalive == 0) {
    mtx_unlock(&wk->mtx);
    return 0;
  }
  r->next = NULL;
  r->prev = wk->qtail;
  if (wk->qtail) wk->qtail->next = r; else wk->qhead = r;
  wk->qtail = r;
  wk->qlen++;
  queue_signal(wk, r);
  mtx_unlock(&wk->mtx);
  return 1;
}

/* How far into the queue a worker looks for a job of a file it has, and
** how often the first job may be passed over before it is taken.
*/
#define QUEUE_LOOK	8

/* Under wk->mtx: the job w takes, or NULL. The first job, unless one of the
** next few is of a file w has loaded and the first has not been passed over
** too often (so that every job is taken in the end).
*/
static NLReq *queue_take(NLWorkers *wk, NLWorker *w)
{
  NLReq *r = wk->qhead, *pick = r;
  int k;
  if (!r) return NULL;
  if (!worker_hasfile(w, r->file) && r->skips < 2 * pool_size(wk) + 2) {
    NLReq *x = r->next;
    for (k = 1; x && k < QUEUE_LOOK; k++, x = x->next)
      if (worker_hasfile(w, x->file)) {
	pick = x;
	r->skips++;
	break;
      }
  }
  if (pick->prev) pick->prev->next = pick->next; else wk->qhead = pick->next;
  if (pick->next) pick->next->prev = pick->prev; else wk->qtail = pick->prev;
  pick->next = pick->prev = NULL;
  wk->qlen--;
  return pick;
}

/* -- One step of a worker's loop ----------------------------------------- */

static void worker_close(NLWorker *w, const char *why);

/* Runs one step: the inbox, the timers, one job (pool). Returns whether it
** did anything. Never raises: everything that runs Lua code runs protected,
** and an error that escapes it discards the worker.
*/
static int worker_step(NLWorker *w)
{
  NLWorkers *wk = w->wk;
  lua_State *L = w->L;
  int progress = 0, st;
  StepCtx sc;
  char msg[512];
  if (!w->opened) {
    worker_open(w);
    progress = 1;
  }
  mtx_lock(&wk->mtx);
  w->wake = 0;
  mtx_unlock(&wk->mtx);
  worker_syncsandbox(w);
  if (nl_pump(L) > 0) progress = 1;
  if (!w->broken) {
    st = lua_cpcall(L, step_f, &sc);
    if (st != 0) {
      pop_message(L, msg, sizeof(msg));
      w->discard = 1;
      worker_setfailure(w, msg);
    } else {
      w->deadline = sc.deadline;
      if (sc.busy) progress = 1;
    }
  }
  if (w->kind == W_POOL && !w->discard) {
    NLReq *r;
    int stop;
    mtx_lock(&wk->mtx);
    stop = w->stop;
    r = stop ? NULL : queue_take(wk, w);
    mtx_unlock(&wk->mtx);
    if (r) {
      progress = 1;
      w->served = 1;
      running_add(w, r);
      if (w->broken) {
	running_remove(w, r);
	reply_post(r->reply, r->id, RES_WORKERERROR, NULL,
		   w->failure ? w->failure : "the worker failed");
	req_free(r);
      } else {
	JobCtx jc;
	jc.r = r;
	worker_syncsandbox(w);
	st = lua_cpcall(L, job_f, &jc);
	if (st != 0) {
	  pop_message(L, msg, sizeof(msg));
	  w->discard = 1;
	  worker_setfailure(w, msg);
	  if (r->msg) { lj_nl_msgfree(r->msg); r->msg = NULL; }
	}
      }
    }
  }
  if (w->kind == W_POOL && !w->served && !w->broken &&
      (w->discard || w->overlimit)) {
    /* It failed before it took a request (over its limit while idle, right
    ** after it opened): a replacement would fail the same way, over and
    ** over, so it stays, failing every request, as one whose libraries do
    ** not fit.
    */
    if (w->overlimit) {
      snprintf(msg, sizeof(msg), "the memory limit of a worker (NL_OPT_WORKERMEM, "
	       "%llu bytes) is too small for its libraries, the worker-init "
	       "function and its start",
	       (unsigned long long)w->limit);
      free(w->failure);
      w->failure = str_dup(msg);
    }
    w->discard = 0;
    w->broken = 1;
  }
  if (w->overlimit && !w->broken) {
    char m[200];
    snprintf(m, sizeof(m), "the worker exceeded its memory limit of %llu "
	     "bytes (NL_OPT_WORKERMEM) and was discarded",
	     (unsigned long long)w->limit);
    w->discard = 1;
    free(w->failure);
    w->failure = str_dup(m);
  } else if (w->overlimit) {
    w->overlimit = 0;  /* A broken worker stays broken (worker_open). */
  }
  return progress;
}

/* Waits for work: a message in the inbox, a job (pool), a timer, a stop. */
static void worker_wait(NLWorker *w)
{
  NLWorkers *wk = w->wk;
  double deadline = w->deadline;
  mtx_lock(&wk->mtx);
  while (!w->wake && !w->stop && !(w->kind == W_POOL && wk->qhead)) {
    double left = -1;
    if (deadline >= 0) {
      left = deadline - now_mono();
      if (left <= 0) break;
    }
    w->idle = 1;
    cond_wait(&w->cond, &wk->mtx, left);
    w->idle = 0;
  }
  w->idle = 0;
  mtx_unlock(&wk->mtx);
}

/* Ends w's state: its port takes no more messages, the requests it has
** taken fail with WorkerError (why), the dedicated workers it started stop,
** and the state closes, which answers the calls still in its inbox.
*/
static void worker_close(NLWorker *w, const char *why)
{
  NLWorkers *wk = w->wk;
  lua_State *L = w->L;
  NLReq *r;
  NLWorker *c;
  if (!L) return;
  lj_nl_portkill(w->port);
  while ((r = w->running) != NULL) {
    running_remove(w, r);
    reply_post(r->reply, r->id, RES_WORKERERROR, NULL, why);
    req_free(r);
  }
  mtx_lock(&wk->mtx);
  for (c = wk->workers; c; c = c->next)
    if (c->kind == W_DEDICATED && c->parent == w && !c->stop) {
      c->stop = 1;
      cond_signal(&c->cond);
    }
  mtx_unlock(&wk->mtx);
  nl_setwakeup(L, NULL, NULL);
  lua_setallocf(L, w->of, w->oud);
  lua_close(L);  /* Answers what is still in its inbox (env_deliver). */
  w->L = NULL;
  mtx_lock(&wk->mtx);
  lj_nl_portunref(w->port);
  w->port = NULL;
  {
    size_t i;
    for (i = 0; i < w->nfiles; i++) free(w->files[i]);
    w->nfiles = 0;
  }
  mtx_unlock(&wk->mtx);
}

/* What a worker that ends answers its waiting requests with. */
static const char *close_reason(NLWorker *w)
{
  int closing;
  if (w->discard && w->failure) return w->failure;
  mtx_lock(&w->wk->mtx);
  closing = w->wk->closing;
  mtx_unlock(&w->wk->mtx);
  if (closing)
    return "the state that started the worker closed before the worker answered";
  return "the worker was stopped before it answered";
}

/* After a step: ends or replaces a worker that is to stop or be discarded.
** Returns 1 when the worker is gone for good.
*/
static int worker_after(NLWorker *w)
{
  NLWorkers *wk = w->wk;
  int stop;
  mtx_lock(&wk->mtx);
  stop = w->stop;
  mtx_unlock(&wk->mtx);
  if (!stop && !w->discard) return 0;
  worker_close(w, close_reason(w));
  if (!stop && w->discard && w->kind == W_POOL) {
    /* A pool worker is replaced. */
    w->discard = 0;
    w->served = 0;
    w->opened = 0;
    w->broken = 0;
    free(w->failure);
    w->failure = NULL;
    w->L = worker_newstate(w);
    if (w->L) return 0;
  }
  {
    NLReq *q = NULL;
    mtx_lock(&wk->mtx);
    w->closed = 1;
    if (w->kind == W_DEDICATED) {
      wk->ndedicated--;
    } else if (--wk->npoolalive == 0 && !wk->closing) {
      /* No pool worker is left to run what is queued: it fails. */
      q = wk->qhead;
      wk->qhead = wk->qtail = NULL;
      wk->qlen = 0;
    }
    mtx_unlock(&wk->mtx);
    while (q) {
      NLReq *n = q->next;
      reply_post(q->reply, q->id, RES_WORKERERROR, NULL,
		 "no pool worker is left: the last one could not be replaced");
      req_free(q);
      q = n;
    }
  }
  return 1;
}

static void *worker_thread(void *arg)
{
  NLWorker *w = (NLWorker *)arg;
  for (;;) {
    if (!worker_step(w)) worker_wait(w);
    if (worker_after(w)) break;
  }
  mtx_lock(&w->wk->mtx);
  w->exited = 1;
  mtx_unlock(&w->wk->mtx);
  return NULL;
}

/* A new worker, linked into wk, with its state; its thread started when
** wk has threads. NULL when out of memory or no thread could start.
*/
static NLWorker *worker_new(NLWorkers *wk, int kind)
{
  NLWorker *w = (NLWorker *)calloc(1, sizeof(NLWorker));
  if (!w) return NULL;
  w->wk = wk;
  w->kind = kind;
  w->deadline = -1;
  if (cond_init(&w->cond) != 0) { free(w); return NULL; }
  w->L = worker_newstate(w);
  if (!w->L) {
    cond_destroy(&w->cond);
    free(w);
    return NULL;
  }
  return w;
}

static void worker_free(NLWorker *w)
{
  size_t i;
  for (i = 0; i < w->nfiles; i++) free(w->files[i]);
  free(w->files);
  free(w->from);
  free(w->path);
  free(w->failure);
  lj_nl_portunref(w->parentport);
  cond_destroy(&w->cond);
  free(w);
}

/* Links w into wk and starts its thread. Under no lock. 0 on failure, when
** w has been freed. A dedicated worker is counted by hw_new beforehand.
*/
static int worker_launch(NLWorkers *wk, NLWorker *w)
{
  mtx_lock(&wk->mtx);
  w->threaded = wk->threads;
  w->next = wk->workers;
  wk->workers = w;
  if (w->kind == W_POOL) wk->npoolalive++;  /* Dedicated: hw_new counted it. */
  if (w->threaded && thread_start(&w->thread, worker_thread, w) != 0) {
    NLWorker **pp;
    for (pp = &wk->workers; *pp; pp = &(*pp)->next)
      if (*pp == w) { *pp = w->next; break; }
    if (w->kind == W_DEDICATED) wk->ndedicated--; else wk->npoolalive--;
    w->closed = 1;
    mtx_unlock(&wk->mtx);
    nl_setwakeup(w->L, NULL, NULL);
    lua_setallocf(w->L, w->of, w->oud);
    lj_nl_portkill(w->port);
    lua_close(w->L);
    lj_nl_portunref(w->port);
    worker_free(w);
    return 0;
  }
  mtx_unlock(&wk->mtx);
  return 1;
}

/* Decides whether workers have threads: before the first one starts, from
** the pool size then in effect.
*/
static void workers_mode(NLWorkers *wk)
{
  mtx_lock(&wk->mtx);
  if (!wk->threadmode_set) {
    wk->threadmode_set = 1;
    wk->threads = pool_size(wk) > 0;
  }
  mtx_unlock(&wk->mtx);
}

/* Starts the pool at the first spawn. 0 when it could not start. */
static int pool_start(lua_State *L, NLWorkers *wk)
{
  int n, i, started;
  mtx_lock(&wk->mtx);
  started = wk->started;
  wk->started = 1;
  n = pool_size(wk);
  mtx_unlock(&wk->mtx);
  if (started) return 1;
  if (!settings_snapshot(L, wk)) return 0;
  workers_mode(wk);
  /* Without threads one pumped state runs every job. A pool of none with
  ** threads already decided (a dedicated worker came first) gets one, so
  ** that jobs run.
  */
  if (!wk->threads || n < 1) n = 1;
  for (i = 0; i < n; i++) {
    NLWorker *w = worker_new(wk, W_POOL);
    if (!w || !worker_launch(wk, w)) break;
  }
  return i > 0;
}

/* Joins the threads of the workers that have ended, and frees the dedicated
** workers that have ended and that no handle names any more (their
** records would otherwise pile up until lua_close). On the main thread.
*/
static void workers_reap(NLWorkers *wk)
{
  NLWorker *w, **pp;
  for (;;) {
    NLWorker *found = NULL;
    mtx_lock(&wk->mtx);
    for (w = wk->workers; w; w = w->next)
      if (w->threaded && w->exited && !w->joined) {
	w->joined = 1;
	found = w;
	break;
      }
    mtx_unlock(&wk->mtx);
    if (!found) break;
    thread_join(found->thread);
  }
  for (;;) {
    NLWorker *found = NULL;
    mtx_lock(&wk->mtx);
    for (pp = &wk->workers; *pp; pp = &(*pp)->next) {
      w = *pp;
      if (w->kind == W_DEDICATED && w->closed && w->hrefs == 0 &&
	  (!w->threaded || w->joined)) {
	NLWorker *c;
	*pp = w->next;
	for (c = wk->workers; c; c = c->next)
	  if (c->parent == w) c->parent = NULL;
	found = w;
	break;
      }
    }
    mtx_unlock(&wk->mtx);
    if (!found) break;
    worker_free(found);
  }
}

/* -- Pumped workers (no threads) ----------------------------------------- */

/* At most this many steps of one pumped worker per pump of the main state. */
#define PUMP_STEPS	64

/* Steps the pumped workers of the main state L (nl_pump). Returns 1 when
** any of them did something.
*/
int lj_nl_stepworkers(lua_State *L)
{
  NLState *ns = nl_state(L);
  NLWorkers *wk = ns->wk;
  NLWorker **list;
  size_t n = 0, i, cap = 0;
  int progress = 0;
  NLWorker *w;
  if (!wk || ns->worker || ns->wpumping) return 0;
  if (wk->threads) {
    workers_reap(wk);
    return 0;
  }
  mtx_lock(&wk->mtx);
  for (w = wk->workers; w; w = w->next) cap++;
  list = cap ? (NLWorker **)malloc(cap * sizeof(NLWorker *)) : NULL;
  if (list)
    for (w = wk->workers; w; w = w->next)
      if (!w->threaded && !w->closed) list[n++] = w;
  mtx_unlock(&wk->mtx);
  if (!list) return 0;
  ns->wpumping = 1;
  /* Oldest first: the list is newest first. */
  for (i = n; i > 0; i--) {
    int k;
    w = list[i - 1];
    for (k = 0; k < PUMP_STEPS && !w->closed; k++) {
      int did = worker_step(w);
      if (worker_after(w)) { progress = 1; break; }
      if (!did) break;
      progress = 1;
    }
  }
  ns->wpumping = 0;
  free(list);
  workers_reap(wk);
  return progress;
}

/* The nearest timer of the pumped workers, or -1. */
static double pumped_deadline(NLWorkers *wk)
{
  NLWorker *w;
  double d = -1;
  mtx_lock(&wk->mtx);
  for (w = wk->workers; w; w = w->next)
    if (!w->threaded && !w->closed && w->deadline >= 0 &&
	(d < 0 || w->deadline < d))
      d = w->deadline;
  mtx_unlock(&wk->mtx);
  return d;
}

/* -- Closing ------------------------------------------------------------- */

/* lua_close of the main state (lj_nl_freestate): stops every worker, waits
** for the threads, closes the pumped states, frees the queue.
*/
void lj_nl_closeworkers(NLState *ns)
{
  NLWorkers *wk = ns->wk;
  NLWorker *w, *next;
  NLReq *q;
  if (!wk || ns->worker) return;
  mtx_lock(&wk->mtx);
  wk->closing = 1;
  for (w = wk->workers; w; w = w->next) {
    w->stop = 1;
    cond_signal(&w->cond);
  }
  mtx_unlock(&wk->mtx);
  for (;;) {
    NLWorker *found = NULL;
    mtx_lock(&wk->mtx);
    for (w = wk->workers; w; w = w->next)
      if (w->threaded && !w->joined) {
	w->joined = 1;
	found = w;
	break;
      }
    mtx_unlock(&wk->mtx);
    if (!found) break;
    thread_join(found->thread);
  }
  /* The pumped states, on this thread, which is theirs. */
  for (w = wk->workers; w; w = w->next)
    if (!w->threaded && w->L) {
      worker_close(w, close_reason(w));
      w->closed = 1;
    }
  mtx_lock(&wk->mtx);
  q = wk->qhead;
  wk->qhead = wk->qtail = NULL;
  wk->qlen = 0;
  w = wk->workers;
  wk->workers = NULL;
  mtx_unlock(&wk->mtx);
  while (q) {
    NLReq *n = q->next;
    req_free(q);
    q = n;
  }
  for (; w; w = next) {
    next = w->next;
    worker_free(w);
  }
  settings_free(&wk->set);
  mtx_destroy(&wk->mtx);
  free(wk);
  ns->wk = NULL;
}

/* -- Envelopes arriving -------------------------------------------------- */

/* A call that its worker will never handle. */
static void env_drop(NLEnv *e)
{
  if (e->kind == ENV_CALL && e->reply)
    reply_post(e->reply, e->id, RES_WORKERERROR, NULL,
	       "the worker ended before it handled the call");
  env_free(e);
}

/* The runtime's handler of messages, rt.workerin, gets the kind and what
** belongs to it. Runs inside nl_pump's protected call of the callback.
*/
static void env_deliver(lua_State *L, void *data)
{
  NLEnv *e = (NLEnv *)data;
  NLState *ns;
  NLWorker *w;
  if (!L) { env_drop(e); return; }
  ns = nl_state(L);
  w = ns->worker;
  switch (e->kind) {
  case ENV_RESULT: {
    double id = e->id;
    int status = e->status;
    NLMsg *m = e->msg;
    char *text = e->text;
    ns->outstanding--;
    e->msg = NULL;
    e->text = NULL;
    env_free(e);
    lj_nl_pushrt(L, "workerin");
    lua_pushliteral(L, "result");
    lua_pushnumber(L, id);
    lua_pushinteger(L, status);
    if (m) push_msgbox(L, m); else lua_pushnil(L);
    if (text) { lua_pushstring(L, text); free(text); } else lua_pushnil(L);
    lua_call(L, 5, 0);
    break;
  }
  case ENV_CALL: {
    NLReq *r;
    NLMsg *m;
    int stop = 1;
    if (w) {
      mtx_lock(&w->wk->mtx);
      stop = w->stop;
      mtx_unlock(&w->wk->mtx);
    }
    if (!w || stop || w->broken) {
      if (w && w->broken && e->reply) {
	reply_post(e->reply, e->id, RES_WORKERERROR, NULL,
		   w->failure ? w->failure : "the worker failed");
	env_free(e);
      } else {
	env_drop(e);
      }
      break;
    }
    r = (NLReq *)calloc(1, sizeof(NLReq));
    if (!r) { env_drop(e); break; }
    r->reply = e->reply;
    e->reply = NULL;
    r->id = e->id;
    m = e->msg;
    e->msg = NULL;
    env_free(e);
    running_add(w, r);
    worker_syncsandbox(w);
    lj_nl_pushrt(L, "workerin");
    lua_pushliteral(L, "call");
    lua_pushlightuserdata(L, r);
    push_msgbox(L, m);
    push_portbox(L, r->reply);
    lua_call(L, 4, 0);
    break;
  }
  case ENV_EMIT: {
    NLMsg *m = e->msg;
    e->msg = NULL;
    env_free(e);
    if (w) {
      int stop;
      mtx_lock(&w->wk->mtx);
      stop = w->stop;
      mtx_unlock(&w->wk->mtx);
      if (stop || w->broken) { lj_nl_msgfree(m); break; }
      worker_syncsandbox(w);
    }
    lj_nl_pushrt(L, "workerin");
    lua_pushliteral(L, "emit");
    push_msgbox(L, m);
    lua_call(L, 2, 0);
    break;
  }
  case ENV_STOP:
    env_free(e);
    if (w) {
      mtx_lock(&w->wk->mtx);
      w->stop = 1;
      mtx_unlock(&w->wk->mtx);
    }
    break;
  default:
    env_free(e);
  }
}

/* -- Boxes: a message, a port, held by Lua ------------------------------- */

#define MSGBOX_MT	"nl.worker.msg"
#define PORTBOX_MT	"nl.worker.port"
#define WORKERBOX_MT	"nl.worker.handle"

/* The metatables are under names in the registry, which a script can read
** (debug.getregistry) and put on a userdata of its own (debug.setmetatable)
** or on another kind of box. So a box is known by its metatable, its size
** and its magic number, all three, and every function that takes one, its
** __gc included, checks them (testbox): a userdata that is not the box is
** an argument error, and a __gc ignores it.
*/
#define MSGBOX_MAGIC	0x4e4c4d53u	/* "NLMS" */
#define PORTBOX_MAGIC	0x4e4c5054u	/* "NLPT" */
#define WORKERBOX_MAGIC	0x4e4c574bu	/* "NLWK" */

typedef struct MsgBox { uint32_t magic; NLMsg *m; } MsgBox;
typedef struct PortBox { uint32_t magic; NLPort *p; } PortBox;
/* The handle of a dedicated worker holds the record alive (hrefs): a
** handle to a worker that has ended reads its `closed`, never freed memory.
*/
typedef struct WorkerBox { uint32_t magic; NLWorker *w; } WorkerBox;

/* The box at idx with metatable mt, of `size` bytes, starting with magic;
** or NULL.
*/
static void *testbox(lua_State *L, int idx, const char *mt, size_t size,
		     uint32_t magic)
{
  void *b = luaL_testudata(L, idx, mt);
  if (b && lua_objlen(L, idx) == size && *(uint32_t *)b == magic) return b;
  return NULL;
}

static void *checkbox(lua_State *L, int idx, const char *mt, size_t size,
		      uint32_t magic)
{
  void *b = testbox(L, idx, mt, size, magic);
  if (!b) luaL_typerror(L, idx, mt);
  return b;
}

#define testmsgbox(L, i) \
  ((MsgBox *)testbox((L), (i), MSGBOX_MT, sizeof(MsgBox), MSGBOX_MAGIC))
#define checkmsgbox(L, i) \
  ((MsgBox *)checkbox((L), (i), MSGBOX_MT, sizeof(MsgBox), MSGBOX_MAGIC))
#define testportbox(L, i) \
  ((PortBox *)testbox((L), (i), PORTBOX_MT, sizeof(PortBox), PORTBOX_MAGIC))
#define checkportbox(L, i) \
  ((PortBox *)checkbox((L), (i), PORTBOX_MT, sizeof(PortBox), PORTBOX_MAGIC))
#define testworkerbox(L, i) \
  ((WorkerBox *)testbox((L), (i), WORKERBOX_MT, sizeof(WorkerBox), \
			WORKERBOX_MAGIC))
#define checkworkerbox(L, i) \
  ((WorkerBox *)checkbox((L), (i), WORKERBOX_MT, sizeof(WorkerBox), \
			 WORKERBOX_MAGIC))

static int msgbox_gc(lua_State *L)
{
  MsgBox *b = testmsgbox(L, 1);
  if (b && b->m) { lj_nl_msgfree(b->m); b->m = NULL; }
  return 0;
}

static int workerbox_gc(lua_State *L)
{
  WorkerBox *b = testworkerbox(L, 1);
  if (b && b->w) {
    NLWorkers *wk = b->w->wk;
    mtx_lock(&wk->mtx);
    b->w->hrefs--;
    mtx_unlock(&wk->mtx);
    b->w = NULL;
  }
  return 0;
}

/* An empty handle box: made before the worker, so that running out of
** memory while it is made leaves no count behind.
*/
static void push_workerbox(lua_State *L)
{
  WorkerBox *b = (WorkerBox *)lua_newuserdata(L, sizeof(WorkerBox));
  b->magic = WORKERBOX_MAGIC;
  b->w = NULL;
  if (luaL_newmetatable(L, WORKERBOX_MT)) {
    lua_pushcfunction(L, workerbox_gc);
    lua_setfield(L, -2, "__gc");
  }
  lua_setmetatable(L, -2);
}

static int portbox_gc(lua_State *L)
{
  PortBox *b = testportbox(L, 1);
  if (b && b->p) { lj_nl_portunref(b->p); b->p = NULL; }
  return 0;
}

/* The box is made before it owns m, so that a failure to make it (out of
** memory) leaks m rather than freeing it twice.
*/
static void push_msgbox(lua_State *L, NLMsg *m)
{
  MsgBox *b = (MsgBox *)lua_newuserdata(L, sizeof(MsgBox));
  b->magic = MSGBOX_MAGIC;
  b->m = NULL;
  if (luaL_newmetatable(L, MSGBOX_MT)) {
    lua_pushcfunction(L, msgbox_gc);
    lua_setfield(L, -2, "__gc");
  }
  lua_setmetatable(L, -2);
  b->m = m;
}

static void push_portbox(lua_State *L, NLPort *p)
{
  PortBox *b = (PortBox *)lua_newuserdata(L, sizeof(PortBox));
  b->magic = PORTBOX_MAGIC;
  b->p = NULL;
  if (luaL_newmetatable(L, PORTBOX_MT)) {
    lua_pushcfunction(L, portbox_gc);
    lua_setfield(L, -2, "__gc");
  }
  lua_setmetatable(L, -2);
  if (p) port_ref(p);
  b->p = p;
}

/* -- host.worker: the C side of runtime/nl_64_worker.lua ----------------- */

/* worker.setup(ctx): the transfer context (lj_nltransfer.c). */
static int hw_setup(lua_State *L)
{
  NLState *ns = nl_state(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  if (ns->wctxref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, ns->wctxref);
  lua_pushvalue(L, 1);
  ns->wctxref = luaL_ref(L, LUA_REGISTRYINDEX);
  return 0;
}

/* worker.info() -> kind | nil, count: what this state is ("pool",
** "dedicated" or nil in the main state), and the pool size (Worker.count).
*/
static int hw_info(lua_State *L)
{
  NLState *ns = nl_state(L);
  NLWorkers *wk = ns->wk;
  int n;
  if (ns->worker) lua_pushstring(L, ns->worker->kind == W_POOL ? "pool" : "dedicated");
  else lua_pushnil(L);
  if (wk) {
    mtx_lock(&wk->mtx);
    n = pool_size(wk);
    mtx_unlock(&wk->mtx);
  } else {
    n = pool_default();
  }
  lua_pushinteger(L, n);
  return 2;
}

/* worker.now() -> seconds, monotonic. */
static int hw_now(lua_State *L)
{
  lua_pushnumber(L, now_mono());
  return 1;
}

/* worker.spawn(labels, id, file, f, args...): the job f(args) for the pool,
** whose result comes back as rt.workerin("result", id, ...). Raises
** SendError for a value that cannot cross. Returns true, or nil and a
** message when the pool cannot start.
*/
static int hw_spawn(lua_State *L)
{
  NLState *ns = nl_state(L);
  double id = luaL_checknumber(L, 2);
  const char *file = luaL_checkstring(L, 3);
  int n = lua_gettop(L) - 3;
  NLWorkers *wk;
  NLPort *port;
  NLReq *r;
  NLMsg *m;
  if (n < 1) return luaL_error(L, "worker.spawn: no function");
  m = lj_nl_encode(L, 4, n, 1);
  wk = workers_get(L);
  port = state_port(L);
  r = (NLReq *)calloc(1, sizeof(NLReq));
  if (r) r->file = str_dup(file);
  if (!wk || !port || !r || !r->file) {
    lj_nl_msgfree(m);
    if (r) free(r->file);
    free(r);
    return luaL_error(L, "not enough memory");
  }
  r->msg = m;
  r->id = id;
  r->reply = port;
  port_ref(port);
  if (!pool_start(L, wk)) {
    req_free(r);
    lua_pushnil(L);
    lua_pushliteral(L, "the worker pool could not start (out of memory or threads)");
    return 2;
  }
  if (!queue_push(wk, r)) {
    req_free(r);
    lua_pushnil(L);
    lua_pushliteral(L, "no pool worker is alive: none could start or be replaced");
    return 2;
  }
  ns->outstanding++;
  lua_pushboolean(L, 1);
  return 1;
}

/* worker.new(from, path) -> worker | nil, message: a dedicated worker for the
** file path (resolved in the worker as import would from the file `from`).
*/
static int hw_new(lua_State *L)
{
  NLState *ns = nl_state(L);
  const char *from = luaL_optstring(L, 1, NULL);
  const char *path = luaL_checkstring(L, 2);
  NLWorkers *wk = workers_get(L);
  NLPort *port = state_port(L);
  NLWorker *w;
  WorkerBox *box;
  lua_Integer max;
  int nded, boxidx;
  if (!wk || !port) return luaL_error(L, "not enough memory");
  push_workerbox(L);
  boxidx = lua_gettop(L);
  if (!settings_snapshot(L, wk)) return luaL_error(L, "not enough memory");
  workers_mode(wk);
  if (!ns->worker) workers_reap(wk);
  mtx_lock(&wk->mtx);
  max = wk->set.opt[NL_OPT_MAXWORKERS];
  nded = wk->ndedicated;
  if ((lua_Integer)nded < max) wk->ndedicated++;  /* Reserved. */
  mtx_unlock(&wk->mtx);
  if ((lua_Integer)nded >= max) {
    lua_pushnil(L);
    lua_pushfstring(L, "too many dedicated workers: NL_OPT_MAXWORKERS allows %d "
		    "at once", (int)max);
    return 2;
  }
  w = worker_new(wk, W_DEDICATED);
  if (!w) {
    mtx_lock(&wk->mtx);
    wk->ndedicated--;
    mtx_unlock(&wk->mtx);
    lua_pushnil(L);
    lua_pushliteral(L, "a worker could not start (out of memory)");
    return 2;
  }
  box = (WorkerBox *)lua_touserdata(L, boxidx);
  w->from = str_dup(from);
  w->path = str_dup(path);
  w->parentport = port;
  port_ref(port);
  w->parent = ns->worker;
  if (!w->path || (from && !w->from)) {
    mtx_lock(&wk->mtx);
    wk->ndedicated--;
    mtx_unlock(&wk->mtx);
    nl_setwakeup(w->L, NULL, NULL);
    lua_setallocf(w->L, w->of, w->oud);
    lj_nl_portkill(w->port);
    lua_close(w->L);
    lj_nl_portunref(w->port);
    worker_free(w);
    return luaL_error(L, "not enough memory");
  }
  /* The handle names it before it runs: a worker with no handle may be
  ** freed once it ends.
  */
  mtx_lock(&wk->mtx);
  w->hrefs++;
  mtx_unlock(&wk->mtx);
  box->w = w;
  if (!worker_launch(wk, w)) {  /* w is freed. */
    box->w = NULL;
    lua_pushnil(L);
    lua_pushliteral(L, "a worker could not start (no thread)");
    return 2;
  }
  lua_pushvalue(L, boxidx);
  return 1;
}

static NLWorker *check_worker(lua_State *L, int idx)
{
  WorkerBox *b = checkworkerbox(L, idx);
  if (!b->w) luaL_argerror(L, idx, "a worker");
  return b->w;
}

/* Posts envelope e to worker w: 0 when w has ended. Under no lock: w->port
** is read under wk->mtx (the worker swaps it when it is replaced).
*/
static int post_to_worker(NLWorker *w, NLEnv *e)
{
  NLPort *p;
  int ok;
  mtx_lock(&w->wk->mtx);
  p = w->closed ? NULL : w->port;
  if (p) port_ref(p);
  mtx_unlock(&w->wk->mtx);
  if (!p) return 0;
  ok = port_post(p, env_deliver, e);
  lj_nl_portunref(p);
  return ok;
}

/* worker.call(w, labels, id, name, args...): the call w.name(args), whose
** result comes back as rt.workerin("result", id, ...). Raises SendError.
*/
static int hw_call(lua_State *L)
{
  NLState *ns = nl_state(L);
  NLWorker *w = check_worker(L, 1);
  double id = luaL_checknumber(L, 3);
  int n = lua_gettop(L) - 3;
  NLPort *port = state_port(L);
  NLEnv *e;
  NLMsg *m;
  if (!port) return luaL_error(L, "not enough memory");
  m = lj_nl_encode(L, 4, n, 2);
  e = (NLEnv *)calloc(1, sizeof(NLEnv));
  if (!e) { lj_nl_msgfree(m); return luaL_error(L, "not enough memory"); }
  e->kind = ENV_CALL;
  e->id = id;
  e->msg = m;
  e->reply = port;
  port_ref(port);
  ns->outstanding++;
  if (!post_to_worker(w, e)) {
    /* The worker has ended: the call fails at the next pump. */
    env_free(e);
    reply_post(port, id, RES_WORKERERROR, NULL,
	       "the worker has ended: it was stopped or discarded");
  }
  return 0;
}

/* worker.emit(target, labels, event, args...): Event.run(event, args) in the
** target: a worker, or a port box (Worker.parent). Raises SendError.
*/
static int hw_emit(lua_State *L)
{
  int n = lua_gettop(L) - 2;
  NLWorker *w = testworkerbox(L, 1) ? check_worker(L, 1) : NULL;
  NLPort *p = w ? NULL : checkportbox(L, 1)->p;
  NLEnv *e;
  NLMsg *m;
  int ok;
  m = lj_nl_encode(L, 3, n, 2);
  e = (NLEnv *)calloc(1, sizeof(NLEnv));
  if (!e) { lj_nl_msgfree(m); return luaL_error(L, "not enough memory"); }
  e->kind = ENV_EMIT;
  e->msg = m;
  ok = w ? post_to_worker(w, e) : port_post(p, env_deliver, e);
  if (!ok) env_free(e);  /* Nothing comes back from an event. */
  return 0;
}

/* worker.stop(w): w ends after the message it is handling. */
static int hw_stop(lua_State *L)
{
  NLWorker *w = check_worker(L, 1);
  NLEnv *e = (NLEnv *)calloc(1, sizeof(NLEnv));
  if (!e) return luaL_error(L, "not enough memory");
  e->kind = ENV_STOP;
  if (!post_to_worker(w, e)) env_free(e);
  return 0;
}

/* The request req of this worker state, or raises. */
static NLReq *check_req(lua_State *L, int idx)
{
  NLState *ns = nl_state(L);
  NLReq *r = (NLReq *)lua_touserdata(L, idx);
  if (!ns->worker || !r || lua_type(L, idx) != LUA_TLIGHTUSERDATA ||
      !running_has(ns->worker, r))
    luaL_argerror(L, idx, "a request of this worker that has no answer yet");
  return r;
}

/* worker.reply(req, failed, value): the answer to a job or a call. Raises
** SendError when the value cannot cross, and the request stays open.
*/
static int hw_reply(lua_State *L)
{
  NLState *ns = nl_state(L);
  NLReq *r = check_req(L, 1);
  int failed = lua_toboolean(L, 2);
  NLMsg *m;
  /* Over its memory limit, the worker is about to be discarded: the request
  ** stays open and fails with that WorkerError (worker_close).
  */
  if (ns->worker->overlimit) return 0;
  lua_settop(L, 3);
  lua_createtable(L, 1, 0);
  lua_pushstring(L, failed ? "exception" : "result");
  lua_rawseti(L, -2, 1);
  m = lj_nl_encode(L, 3, 1, 4);
  running_remove(ns->worker, r);
  reply_post(r->reply, r->id, failed ? RES_FAILED : RES_DONE, m, NULL);
  req_free(r);
  return 0;
}

/* worker.replyerror(req, message): the request fails with WorkerError. */
static int hw_replyerror(lua_State *L)
{
  NLState *ns = nl_state(L);
  NLReq *r = check_req(L, 1);
  const char *msg = luaL_checkstring(L, 2);
  if (ns->worker->overlimit) return 0;
  running_remove(ns->worker, r);
  reply_post(r->reply, r->id, RES_WORKERERROR, NULL, msg);
  req_free(r);
  return 0;
}

/* worker.decode(box) -> values: the values of a message, which is freed. */
static int hw_decode(lua_State *L)
{
  MsgBox *b = checkmsgbox(L, 1);
  int n;
  NLMsg *m = b->m;
  if (!m) return luaL_error(L, "worker.decode: the message was decoded already");
  n = lj_nl_decode(L, m);
  b->m = NULL;
  lj_nl_msgfree(m);
  return n;
}

/* worker.msgsize(box) -> bytes. */
static int hw_msgsize(lua_State *L)
{
  MsgBox *b = checkmsgbox(L, 1);
  lua_pushnumber(L, (lua_Number)lj_nl_msgsize(b->m));
  return 1;
}

/* worker.encode(labels, values...) -> box: for tests and measurements of the
** encoder (tests/vm/real/test_transfer.c).
*/
static int hw_encode(lua_State *L)
{
  int n = lua_gettop(L) - 1;
  NLMsg *m = lj_nl_encode(L, 2, n, 1);
  push_msgbox(L, m);
  return 1;
}

/* worker.parent() -> port box | nil: the state that started this dedicated
** worker.
*/
static int hw_parent(lua_State *L)
{
  NLWorker *w = nl_state(L)->worker;
  if (w && w->parentport) push_portbox(L, w->parentport); else lua_pushnil(L);
  return 1;
}

/* worker.isself(port box) -> boolean: the port is this state's own. */
static int hw_isself(lua_State *L)
{
  PortBox *b = checkportbox(L, 1);
  lua_pushboolean(L, b->p != NULL && b->p == nl_state(L)->port);
  return 1;
}

/* worker.loaded(name): this pool worker has loaded the file (the pool
** prefers it for the file's jobs).
*/
static int hw_loaded(lua_State *L)
{
  NLWorker *w = nl_state(L)->worker;
  const char *name = luaL_checkstring(L, 1);
  char *copy;
  if (!w || w->kind != W_POOL || worker_hasfile(w, name)) return 0;
  copy = str_dup(name);
  if (!copy) return 0;
  mtx_lock(&w->wk->mtx);
  if (w->nfiles == w->szfiles) {
    size_t nsz = w->szfiles ? w->szfiles * 2 : 8;
    char **p = (char **)realloc(w->files, nsz * sizeof(char *));
    if (!p) { mtx_unlock(&w->wk->mtx); free(copy); return 0; }
    w->files = p;
    w->szfiles = nsz;
  }
  w->files[w->nfiles++] = copy;
  mtx_unlock(&w->wk->mtx);
  return 0;
}

/* worker.memory() -> bytes counted, limit (0: none). In a worker. */
static int hw_memory(lua_State *L)
{
  NLWorker *w = nl_state(L)->worker;
  lua_pushnumber(L, w ? (lua_Number)w->mem : 0);
  lua_pushnumber(L, w ? (lua_Number)w->limit : 0);
  return 2;
}

void lj_nl_pushhostworker(lua_State *L)
{
  static const luaL_Reg fns[] = {
    {"setup", hw_setup}, {"info", hw_info}, {"now", hw_now},
    {"spawn", hw_spawn}, {"new", hw_new}, {"call", hw_call},
    {"emit", hw_emit}, {"stop", hw_stop}, {"reply", hw_reply},
    {"replyerror", hw_replyerror}, {"decode", hw_decode},
    {"encode", hw_encode}, {"msgsize", hw_msgsize}, {"parent", hw_parent},
    {"loaded", hw_loaded}, {"memory", hw_memory}, {"isself", hw_isself},
    {NULL, NULL}
  };
  const luaL_Reg *r;
  lua_createtable(L, 0, 16);
  for (r = fns; r->name; r++) {
    lua_pushcfunction(L, r->func);
    lua_setfield(L, -2, r->name);
  }
}

/* -- Host API ------------------------------------------------------------ */

static int count_f(lua_State *L, void *ud)
{
  int n = (int)(intptr_t)ud;
  if (!lj_nl_getrt(L, "workercount")) return 0;
  lua_pushinteger(L, n);
  lua_call(L, 1, 0);
  return 0;
}

LUA_API void nl_setworkers(lua_State *L, int n)
{
  NLState *ns = nl_state(L);
  NLWorkers *wk;
  int now;
  if (ns->worker) return;  /* No effect in a worker. */
  wk = workers_get(L);
  if (!wk) return;
  if (n < 0) n = 0;
  mtx_lock(&wk->mtx);
  /* Fixed once workers exist. */
  if (!wk->frozen) wk->npool = n;
  now = pool_size(wk);
  mtx_unlock(&wk->mtx);
  if (lj_nl_isopen(L) && lua_checkstack(L, 4)) {
    int st = lj_nl_protect(L, count_f, (void *)(intptr_t)now, 0, 0);
    if (st > 0) lua_pop(L, 1);
  }
}

LUA_API void nl_setworkerinit(lua_State *L, lua_CFunction fn, void *data)
{
  NLState *ns = nl_state(L);
  NLWorkers *wk;
  if (ns->worker) return;
  wk = workers_get(L);
  if (!wk) return;
  mtx_lock(&wk->mtx);
  if (!wk->frozen) {  /* Fixed once workers exist. */
    wk->init = fn;
    wk->initdata = fn ? data : NULL;
  }
  mtx_unlock(&wk->mtx);
}

/* encode below decode on the stack; both popped. */
LUA_API void nl_settransfer(lua_State *L, const char *type)
{
  lj_nl_pushrt(L, "settransfer");
  lua_pushstring(L, type);
  lua_pushvalue(L, -4);
  lua_pushvalue(L, -4);
  lua_call(L, 3, 0);
  lua_pop(L, 2);
}

/* Worker.run(f, args...): the function and nargs arguments on top. */
LUA_API void nl_spawn(lua_State *L, int nargs)
{
  NLState *ns = nl_state(L);
  int fidx = lua_gettop(L) - nargs;
  luaL_checkstack(L, 3, "nl_spawn");
  if (ns->rtref == LUA_NOREF) luaL_error(L, "NL is not open in this state (luaopen_nl)");
  lua_rawgeti(L, LUA_REGISTRYINDEX, ns->rtref);
  lua_getfield(L, -1, "Worker");
  lua_remove(L, -2);
  if (!lua_istable(L, -1)) luaL_error(L, "the NL runtime has no Worker library");
  lua_getfield(L, -1, "run");
  lua_remove(L, -2);
  lua_insert(L, fidx);
  lua_call(L, nargs + 1, 1);
}

LUA_API int nl_isworker(lua_State *L)
{
  return nl_state(L)->worker != NULL;
}

/* 1 when the workers' settings are fixed for L: L is a worker, or L is a
** main state whose pool has started or that has made a dedicated worker.
*/
LUA_API int nl_workersstarted(lua_State *L)
{
  NLState *ns = nl_state(L);
  return ns->worker != NULL || lj_nl_workersfrozen(ns);
}

/* Waits until the inbox of ns is not empty, at most `seconds` (forever when
** negative).
*/
static void inbox_wait(NLState *ns, double seconds)
{
  double until = seconds >= 0 ? now_mono() + seconds : -1;
  lj_nl_lock(ns);
  while (!ns->head) {
    double left = -1;
    if (until >= 0) {
      left = until - now_mono();
      if (left <= 0) break;
    }
    ns->drain_waiting = 1;
    cond_wait(&ns->posted, &ns->mtx, left);
  }
  ns->drain_waiting = 0;
  lj_nl_unlock(ns);
}

LUA_API int nl_drain(lua_State *L)
{
  NLState *ns = nl_state(L);
  int n = 0;
  for (;;) {
    int got = nl_pump(L);
    NLWorkers *wk = ns->wk;
    n += got;
    if (ns->outstanding <= 0) break;
    if (got > 0) continue;
    if (wk && !wk->threads && !ns->worker) {
      /* Pumped workers: only this thread moves them, so wait for the
      ** nearest timer, or stop when there is none to wait for.
      */
      double d = pumped_deadline(wk);
      int queued;
      lj_nl_lock(ns);
      queued = ns->head != NULL;
      lj_nl_unlock(ns);
      if (queued) continue;  /* Posted during the steps of this pump. */
      if (d < 0) break;
      d -= now_mono();
      if (d > 0) inbox_wait(ns, d);
    } else {
      inbox_wait(ns, -1);
    }
  }
  return n;
}

#endif
