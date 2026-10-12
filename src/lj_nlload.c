/*
** NL loading: the bridge to the compiler (nl_compile, nlc.h), the default
** loader, the host table that the runtime library is opened with, and the
** load functions of API.md 2.3.
**
** Which compiler a file gets (DESIGN 1, 12; API.md 2.3):
** - luaL_loadfile(x), and through it loadfile, dofile, require and include,
**   choose by extension: a name ending in ".nl" is NL, anything else Lua.
** - luaL_loadbuffer(x), luaL_loadstring, lua_load, load and loadstring always
**   take Lua source or bytecode.
** - nl_loadfile and nl_loadbuffer always take NL source.
*/

#define lj_nlload_c
#define LUA_CORE

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#if LJ_TARGET_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <dirent.h>
#endif

#include "lj_nlint.h"
#include "nltext.h"
#include "nlregex.h"

#if LJ_NL

#include "lauxlib.h"
#include "lj_err.h"

#if LJ_TARGET_WINDOWS
#define NL_MAXPATH	4096
#else
#include <limits.h>
#define NL_MAXPATH	4096
#endif

/* Out of C memory. Raised as an ordinary error: lj_err_mem is not reachable
** from here when the core is built with "make amalg".
*/
static void nomem(lua_State *L)
{
  luaL_error(L, "not enough memory");
}

/* -- Files ---------------------------------------------------------------- */

int lj_nl_isnlfile(const char *filename)
{
  size_t len = strlen(filename);
  return len >= 3 && memcmp(filename + len - 3, ".nl", 3) == 0;
}

static int file_isregular(const char *name)
{
  struct stat st;
  return stat(name, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG;
}

/* Reads a whole file into a malloc'd buffer. NULL with errno set on failure. */
static char *file_read(const char *name, size_t *size)
{
  FILE *fp = fopen(name, "rb");
  char *buf = NULL;
  size_t len = 0, cap = 0;
  if (!fp) return NULL;
  for (;;) {
    size_t n;
    if (len == cap) {
      char *nb;
      cap = cap ? cap*2 : 16384;
      nb = (char *)realloc(buf, cap);
      if (!nb) { free(buf); fclose(fp); errno = ENOMEM; return NULL; }
      buf = nb;
    }
    n = fread(buf + len, 1, cap - len, fp);
    len += n;
    if (n == 0) break;
  }
  if (ferror(fp)) {
    int err = errno;
    free(buf);
    fclose(fp);
    errno = err ? err : EIO;
    return NULL;
  }
  fclose(fp);
  *size = len;
  return buf ? buf : (char *)malloc(1);
}

/* -- Default loader: the disk, in the search order of DESIGN 10 ----------- */

/* Copies dir/path.nl into out; 0 if it does not fit. dir may be NULL. */
static int join(char *out, size_t size, const char *dir, size_t dlen,
		const char *path)
{
  size_t plen = strlen(path);
  size_t need = (dir ? dlen + 1 : 0) + plen + 3 + 1;
  char *p = out;
  if (need > size) return 0;
  if (dir) {
    memcpy(p, dir, dlen);
    p += dlen;
    *p++ = '/';
  }
  memcpy(p, path, plen);
  p += plen;
  memcpy(p, ".nl", 4);
  return 1;
}

/* The canonical name of a file on disk is its real path, so that one file is
** one name however it was reached; it is shown relative to the working
** directory when the file is inside it, as nlc names files
** (crates/nlc/src/host.rs), and absolute otherwise.
*/
static int canonical(const char *name, char *out, size_t size)
{
#if LJ_TARGET_WINDOWS
  return _fullpath(out, name, size) != NULL;
#else
  char buf[PATH_MAX], cwd[PATH_MAX], rcwd[PATH_MAX];
  const char *shown = buf;
  size_t len;
  if (!realpath(name, buf)) return 0;
  if (getcwd(cwd, sizeof(cwd)) && realpath(cwd, rcwd)) {
    size_t clen = strlen(rcwd);
    if (clen > 1 && strncmp(buf, rcwd, clen) == 0 && buf[clen] == '/')
      shown = buf + clen + 1;
    else if (clen == 1 && buf[0] == '/' && buf[1])
      shown = buf + 1;  /* The working directory is the root. */
  }
  len = strlen(shown);
  if (len + 1 > size) return 0;
  memmove(out, shown, len + 1);
  return 1;
#endif
}

static int try_candidate(const char *cand, char *out, size_t size)
{
  return file_isregular(cand) && canonical(cand, out, size);
}

static int path_isabs(const char *p)
{
#if LJ_TARGET_WINDOWS
  return p[0] == '\\' || p[0] == '/' || (p[0] && p[1] == ':');
#else
  return p[0] == '/';
#endif
}

static int disk_resolve(void *data, const char *from, const char *path,
			char *out, size_t size)
{
  NLState *ns = (NLState *)data;
  char cand[NL_MAXPATH];
  size_t i;
  if (!path || !*path) return 0;
  if (path_isabs(path))
    return join(cand, sizeof(cand), NULL, 0, path) &&
	   try_candidate(cand, out, size);
  /* 1. Relative to the directory of the importing file. */
  if (from) {
    const char *slash = strrchr(from, '/');
#if LJ_TARGET_WINDOWS
    const char *bslash = strrchr(from, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    if (slash) {
      if (join(cand, sizeof(cand), from, (size_t)(slash - from), path) &&
	  try_candidate(cand, out, size))
	return 1;
    } else if (join(cand, sizeof(cand), NULL, 0, path) &&
	       try_candidate(cand, out, size)) {
      return 1;  /* The importing file is in the working directory. */
    }
  }
  /* 2. Relative to the working directory. */
  if (join(cand, sizeof(cand), NULL, 0, path) && try_candidate(cand, out, size))
    return 1;
  /* 3. Relative to each directory of the search path. */
  for (i = 0; i < ns->npath; i++) {
    const char *dir = ns->path[i];
    if (join(cand, sizeof(cand), dir, strlen(dir), path) &&
	try_candidate(cand, out, size))
      return 1;
  }
  return 0;
}

static const char *disk_read(void *data, const char *name, size_t *size)
{
  UNUSED(data);
  return file_read(name, size);
}

static void disk_release(void *data, const char *name, const char *buf)
{
  UNUSED(data); UNUSED(name);
  free((void *)buf);
}

/* -- Declaration files --------------------------------------------------- */

static int decl_push(char ***list, size_t *n, size_t *sz, const char *s)
{
  size_t len = strlen(s);
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
  memcpy(copy, s, len+1);
  (*list)[(*n)++] = copy;
  return 1;
}

static int decl_cmp(const void *a, const void *b)
{
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int is_decl_name(const char *name)
{
  size_t len = strlen(name);
  return len > 5 && strcmp(name + len - 5, ".d.nl") == 0;
}

/* Appends dir/name for every .d.nl in dir, in name order, to the list;
** a directory that cannot be read contributes nothing. 0 on OOM.
*/
static int decl_scan(char ***list, size_t *n, size_t *sz, const char *dir)
{
  size_t first = *n, dlen = strlen(dir);
  char cand[NL_MAXPATH];
#if LJ_TARGET_WINDOWS
  WIN32_FIND_DATAA fd;
  HANDLE h;
  if (dlen + 7 >= sizeof(cand)) return 1;
  memcpy(cand, dir, dlen);
  memcpy(cand + dlen, "\\*.d.nl", 8);
  h = FindFirstFileA(cand, &fd);
  if (h == INVALID_HANDLE_VALUE) return 1;
  do {
    if (!is_decl_name(fd.cFileName)) continue;
    if (dlen + 1 + strlen(fd.cFileName) >= sizeof(cand)) continue;
    memcpy(cand, dir, dlen);
    cand[dlen] = '/';
    strcpy(cand + dlen + 1, fd.cFileName);
    if (!decl_push(list, n, sz, cand)) { FindClose(h); return 0; }
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  DIR *d = opendir(dir);
  struct dirent *e;
  if (!d) return 1;
  while ((e = readdir(d)) != NULL) {
    if (!is_decl_name(e->d_name)) continue;
    if (dlen + 1 + strlen(e->d_name) >= sizeof(cand)) continue;
    memcpy(cand, dir, dlen);
    cand[dlen] = '/';
    strcpy(cand + dlen + 1, e->d_name);
    if (!file_isregular(cand)) continue;
    if (!decl_push(list, n, sz, cand)) { closedir(d); return 0; }
  }
  closedir(d);
#endif
  if (*n > first + 1) qsort(*list + first, *n - first, sizeof(char *), decl_cmp);
  return 1;
}

/* The declaration files of a compile (nlc.h, API.md 2.3): those given to
** nl_adddecl and, with the disk loader, every .d.nl in the directories of
** the search path, each directory's in name order. Returns a malloc'd list
** of malloc'd names to free with decls_free; NULL with *n == (size_t)-1 on
** OOM.
*/
static char **decls_collect(NLState *ns, size_t *n)
{
  char **list = NULL;
  size_t sz = 0, i;
  *n = 0;
  for (i = 0; i < ns->ndecl; i++)
    if (!decl_push(&list, n, &sz, ns->decl[i])) goto oom;
  if (!ns->hasloader) {
    for (i = 0; i < ns->npath; i++)
      if (!decl_scan(&list, n, &sz, ns->path[i])) goto oom;
  }
  return list;
oom:
  for (i = 0; i < *n; i++) free(list[i]);
  free(list);
  *n = (size_t)-1;
  return NULL;
}

static void decls_free(char **list, size_t n)
{
  size_t i;
  for (i = 0; i < n; i++) free(list[i]);
  free(list);
}

/* The loader in effect: the host's (nl_setloader) or the disk. */
static void current_loader(NLState *ns, nlc_Loader *ld)
{
  if (ns->hasloader) {
    ld->resolve = ns->loader.resolve;
    ld->read = ns->loader.read;
    ld->release = ns->loader.release;
    ld->data = ns->loader.data;
  } else {
    ld->resolve = disk_resolve;
    ld->read = disk_read;
    ld->release = disk_release;
    ld->data = ns;
  }
}

/* -- Guards: free C resources when a Lua error unwinds past them ----------- */

#define GUARD_MAGIC	0x4e4c4755u	/* "NLGU" */

typedef struct Guard {
  unsigned magic;	/* A script can give the guard metatable to another
			** userdata (debug.setmetatable): __gc checks this. */
  void (*cleanup)(struct Guard *g);
  nlc_Result *result;
  nlc_Loader loader;
  const char *buf;
  char name[1];		/* A copy of the name, for release. */
} Guard;

static int guard_gc(lua_State *L)
{
  Guard *g = (Guard *)lua_touserdata(L, 1);
  if (g && lua_type(L, 1) == LUA_TUSERDATA &&
      lua_objlen(L, 1) >= sizeof(Guard) && g->magic == GUARD_MAGIC &&
      g->cleanup) {
    void (*f)(Guard *) = g->cleanup;
    g->cleanup = NULL;
    f(g);
  }
  return 0;
}

static void guard_run(Guard *g)
{
  if (g->cleanup) {
    void (*f)(Guard *) = g->cleanup;
    g->cleanup = NULL;
    f(g);
  }
}

static void guard_freeresult(Guard *g) { nlc_free(g->result); }

static void guard_release(Guard *g)
{
  if (g->loader.release) g->loader.release(g->loader.data, g->name, g->buf);
}

/* Pushes a new guard userdata with a __gc that runs its cleanup. */
static Guard *guard_new(lua_State *L, const char *name)
{
  NLState *ns = nl_state(L);
  size_t len = name ? strlen(name) : 0;
  Guard *g = (Guard *)lua_newuserdata(L, sizeof(Guard) + len);
  memset(g, 0, sizeof(Guard));
  g->magic = GUARD_MAGIC;
  if (name) memcpy(g->name, name, len + 1);
  if (ns->guardref != LUA_NOREF) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, ns->guardref);
    if (!lua_istable(L, -1)) {  /* Damaged by a script: make a new one. */
      lua_pop(L, 1);
      ns->guardref = LUA_NOREF;
    }
  }
  if (ns->guardref == LUA_NOREF) {
    lua_newtable(L);
    lua_pushcfunction(L, guard_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushvalue(L, -1);
    ns->guardref = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  lua_setmetatable(L, -2);
  return g;
}

/* Reads a file through the loader into a Lua string. 0 with errno-style
** message pushed on failure.
*/
static int push_file(lua_State *L, const char *name)
{
  nlc_Loader ld;
  Guard *g;
  const char *buf;
  size_t size = 0;
  current_loader(nl_state(L), &ld);
  g = guard_new(L, name);
  errno = 0;
  buf = ld.read(ld.data, name, &size);
  if (!buf) {
    int err = errno;
    lua_pop(L, 1);
    if (err)
      lua_pushfstring(L, "cannot open %s: %s", name, strerror(err));
    else
      lua_pushfstring(L, "cannot open %s", name);
    return 0;
  }
  g->loader = ld;
  g->buf = buf;
  g->cleanup = guard_release;
  lua_pushlstring(L, buf, size);
  guard_run(g);
  lua_remove(L, -2);
  return 1;
}

/* -- The host table (runtime/nl.lua, HOST TABLE) -------------------------- */

static void report(NLState *ns, const nlc_Diagnostic *d)
{
  nl_Diagnostic nd;
  if (!ns->reporter) return;
  nd.severity = d->severity;
  nd.code = d->code;
  nd.message = d->message;
  nd.file = d->file;
  nd.line = d->line;
  nd.column = d->column;
  nd.endline = d->endline;
  nd.endcolumn = d->endcolumn;
  ns->reporter(ns->reporterdata, &nd);
}

/* host.compile(source, chunkname) -> chunk | nil, message */
static int host_compile(lua_State *L)
{
  NLState *ns = nl_state(L);
  size_t len, nd, i, nerr = 0;
  const char *src = luaL_checklstring(L, 1, &len);
  const char *name = luaL_checkstring(L, 2);
  const nlc_Diagnostic *first = NULL;
  nlc_Options opt;
  nlc_Loader ld;
  nlc_Result *r;
  const char *chunk;
  size_t chunklen = 0;
  Guard *g;
  char **decls;
  size_t ndecls;
  opt.checks = (int)ns->opt[NL_OPT_CHECKS];
  opt.strip = (int)ns->opt[NL_OPT_STRIP];
  opt.bytecode = 1;  /* The VM loads dumps (nlc.h); loadstring takes both. */
  opt.level = 1;     /* The fork's opcodes and events (IMPLEMENTATION_PLAN M5). */
  current_loader(ns, &ld);
  g = guard_new(L, NULL);
  decls = decls_collect(ns, &ndecls);
  if (ndecls == (size_t)-1) nomem(L);
  opt.decls = (const char *const *)decls;
  opt.ndecls = ndecls;
  r = nl_compile(src, len, name, &opt, &ld);
  decls_free(decls, ndecls);
  if (!r) nomem(L);
  g->result = r;
  g->cleanup = guard_freeresult;
  nd = nlc_ndiagnostics(r);
  for (i = 0; i < nd; i++) {
    const nlc_Diagnostic *d = nlc_diagnostic(r, i);
    if (!d) continue;
    report(ns, d);
    if (d->severity == NLC_DIAG_ERROR) {
      if (!first) first = d;
      nerr++;
    }
  }
  chunk = nlc_chunk(r, &chunklen);
  if (chunk) {
    lua_pushlstring(L, chunk, chunklen);
    guard_run(g);
    return 1;
  }
  lua_pushnil(L);
  if (first) {
    lua_pushfstring(L, "%s:%d:%d: %s", first->file ? first->file : "?",
		    first->line, first->column,
		    first->message ? first->message : "error");
    if (nerr > 1) {
      lua_pushfstring(L, " (and %d more error%s)", (int)(nerr - 1),
		      nerr > 2 ? "s" : "");
      lua_concat(L, 2);
    }
  } else {
    lua_pushfstring(L, "%s: the compiler returned no chunk", name);
  }
  guard_run(g);
  return 2;
}

/* host.resolve(from, path) -> name | nil */
static int host_resolve(lua_State *L)
{
  NLState *ns = nl_state(L);
  const char *from = luaL_optstring(L, 1, NULL);
  const char *path = luaL_checkstring(L, 2);
  char out[NL_MAXPATH];
  nlc_Loader ld;
  current_loader(ns, &ld);
  out[0] = '\0';
  if (ld.resolve(ld.data, from, path, out, sizeof(out))) {
    out[sizeof(out)-1] = '\0';
    lua_pushstring(L, out);
  } else {
    lua_pushnil(L);
  }
  return 1;
}

/* host.read(name) -> source | nil */
static int host_read(lua_State *L)
{
  const char *name = luaL_checkstring(L, 1);
  if (!push_file(L, name)) {
    lua_pop(L, 1);
    lua_pushnil(L);
  }
  return 1;
}

/* The canonical name of the file at `path`, taken as given (with its
** extension, relative to the working directory, no search), copied to out:
** the name resolve gives for that file. With a host loader, which has no
** such function, the path itself is the name (as nl_loadfile reads it).
*/
static int canonical_file(NLState *ns, const char *path, char *out, size_t size)
{
  if (ns->hasloader) {
    size_t len = strlen(path);
    if (len + 1 > size) return 0;
    memcpy(out, path, len + 1);
    return 1;
  }
  return file_isregular(path) && canonical(path, out, size);
}

/* host.canonical(path) -> name | nil (runtime/nl.lua, HOST TABLE) */
static int host_canonical(lua_State *L)
{
  const char *path = luaL_checkstring(L, 1);
  char out[NL_MAXPATH];
  if (canonical_file(nl_state(L), path, out, sizeof(out)))
    lua_pushstring(L, out);
  else
    lua_pushnil(L);
  return 1;
}

/* host.add_path(dir) */
static int host_addpath(lua_State *L)
{
  const char *dir = luaL_checkstring(L, 1);
  if (!lj_nl_addpath(nl_state(L), dir)) nomem(L);
  return 0;
}

void lj_nl_openhost(lua_State *L)
{
  lua_createtable(L, 0, 4);
  lua_pushcfunction(L, host_compile);
  lua_setfield(L, -2, "compile");
  lua_pushcfunction(L, host_resolve);
  lua_setfield(L, -2, "resolve");
  lua_pushcfunction(L, host_read);
  lua_setfield(L, -2, "read");
  lua_pushcfunction(L, host_addpath);
  lua_setfield(L, -2, "add_path");
  lua_pushcfunction(L, host_canonical);
  lua_setfield(L, -2, "canonical");
  /* The VM extensions (M5): host.vm.markmethod, host.vm.ismethod. */
  lj_nl_pushhostvm(L);
  lua_setfield(L, -2, "vm");
  /* The text and regex libraries of libnlc (M6), when it has them; without
  ** them the runtime keeps its Lua stand-ins (runtime/nl_80_string.lua).
  */
  if (nltext_available()) {
    lj_nl_opentext(L);
    lua_setfield(L, -2, "text");
  }
  if (nlregex_available()) {
    lj_nl_openregex(L);
    lua_setfield(L, -2, "regex");
  }
}

/* -- Loading (API.md 2.3) ------------------------------------------------- */

typedef struct LoadCtx {
  const char *buf;
  size_t size;
  const char *name;
} LoadCtx;

static int load_f(lua_State *L, void *ud)
{
  LoadCtx *c = (LoadCtx *)ud;
  lj_nl_pushrt(L, "load");
  lua_pushlstring(L, c->buf, c->size);
  lua_pushstring(L, c->name);
  lua_call(L, 2, 2);
  return 2;
}

/* Turns the value on top into a message string, in place. The string made
** by rt.tostring stays on the stack until it has replaced the value, so no
** collection can free it in between.
*/
static void tomessage(lua_State *L)
{
  if (lua_isstring(L, -1)) {
    lua_tostring(L, -1);  /* A number becomes a string in its own slot. */
    return;
  }
  if (lua_checkstack(L, 3) && lj_nl_getrt(L, "tostring")) {
    lua_pushvalue(L, -2);
    if (lua_pcall(L, 1, 1, 0) == 0 && lua_type(L, -1) == LUA_TSTRING) {
      lua_remove(L, -2);
      return;
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  lua_pushliteral(L, "error object is not a string");
}

/* rt.load(source, name), as a load function: status and one value pushed. */
static int load_source(lua_State *L, const char *buf, size_t size,
		       const char *name)
{
  LoadCtx c;
  int st;
  c.buf = buf;
  c.size = size;
  c.name = name;
  if (!lj_nl_isopen(L)) {
    lua_pushfstring(L, "cannot load %s: NL is not open in this state",
		    name);
    return LUA_ERRFILE;
  }
  st = lj_nl_protect(L, load_f, &c, 0, 2);
  if (st < 0) {
    lua_pushliteral(L, "not enough memory");
    return LUA_ERRMEM;
  }
  if (st == LUA_ERRMEM) return LUA_ERRMEM;
  if (st != 0) {
    tomessage(L);
    return LUA_ERRSYNTAX;
  }
  if (lua_isfunction(L, -2)) {
    lua_pop(L, 1);
    return 0;
  }
  lua_remove(L, -2);
  tomessage(L);
  return LUA_ERRSYNTAX;
}

/* A chunk name that starts with neither '@' nor '=' is the source itself,
** as in Lua (nlL_loadstring passes the source as the name). It is shown as
** Lua shows such names, [string "first line..."] (lj_debug_shortname), and
** that is the name the compiler and lua_load get: "=[string \"...\"]".
*/
static const char *source_name(const char *src, char *buf)
{
  char *out = buf;
  size_t len;
  *out++ = '=';
  for (len = 0; len < LUA_IDSIZE-12; len++)
    if (((const unsigned char *)src)[len] < ' ') break;
  memcpy(out, "[string \"", 9); out += 9;
  if (src[len] != '\0') {  /* Truncated. */
    if (len > LUA_IDSIZE-15) len = LUA_IDSIZE-15;
    memcpy(out, src, len); out += len;
    memcpy(out, "...", 3); out += 3;
  } else {
    memcpy(out, src, len); out += len;
  }
  memcpy(out, "\"]", 3);
  return buf;
}

LUA_API int nl_loadbuffer(lua_State *L, const char *buf, size_t size,
			  const char *name)
{
  char sname[LUA_IDSIZE+8];
  if (!name) name = "=?";
  else if (name[0] != '@' && name[0] != '=') name = source_name(name, sname);
  return load_source(L, buf, size, name);
}

typedef struct FileCtx {
  const char *filename;
  int status;
} FileCtx;

static int loadfile_f(lua_State *L, void *ud)
{
  FileCtx *c = (FileCtx *)ud;
  size_t size;
  const char *src;
  const char *name;
  char cname[NL_MAXPATH];
  /* The chunk is named by the file's canonical name, the one import gives
  ** it (runtime/nl.lua, LOADED CHUNKS), so that a file has one identity
  ** however its path is spelled: the handler name of an unnamed `on`, the
  ** importing file of its imports.
  */
  errno = 0;
  if (!canonical_file(nl_state(L), c->filename, cname, sizeof(cname))) {
    int err = errno;
    lua_pushfstring(L, "cannot open %s: %s", c->filename,
		    err ? strerror(err) : "no such file");
    c->status = LUA_ERRFILE;
    return 1;
  }
  if (!push_file(L, cname)) {
    c->status = LUA_ERRFILE;
    return 1;
  }
  src = lua_tolstring(L, -1, &size);
  name = lua_pushfstring(L, "@%s", cname);
  c->status = load_source(L, src, size, name);
  return 1;
}

/* Every .nl file a state loads is read through the loader (API.md 2.3,
** Loader). Without a host loader the file is named by its canonical name.
** With one, the file name is passed to its read function as the name,
** without resolving it: loadfile takes its path as given, with no search
** (DESIGN 10, include).
*/
LUA_API int nl_loadfile(lua_State *L, const char *filename)
{
  FileCtx c;
  int st;
  if (!filename) {
    lua_pushliteral(L, "nl_loadfile: no file name");
    return LUA_ERRFILE;
  }
  c.filename = filename;
  c.status = 0;
  if (!lj_nl_isopen(L)) {
    lua_pushfstring(L, "cannot load %s: NL is not open in this state",
		    filename);
    return LUA_ERRFILE;
  }
  st = lj_nl_protect(L, loadfile_f, &c, 0, 1);
  if (st < 0) {
    lua_pushliteral(L, "not enough memory");
    return LUA_ERRMEM;
  }
  if (st != 0) {  /* Out of memory while reading or pushing. */
    if (st != LUA_ERRMEM) tomessage(L);
    return st == LUA_ERRMEM ? LUA_ERRMEM : LUA_ERRFILE;
  }
  return c.status;
}

/* luaL_loadfilex for a name ending in ".nl". */
int lj_nl_loadfilex(lua_State *L, const char *filename, const char *mode)
{
  if (mode && !strchr(mode, 't')) {  /* NL source is text. */
    lua_pushstring(L, err2msg(LJ_ERR_XMODE));
    return LUA_ERRSYNTAX;
  }
  return nl_loadfile(L, filename);
}

LUA_API void nl_import(lua_State *L, const char *path)
{
  lj_nl_pushrt(L, "import");
  lua_pushstring(L, path);
  lua_call(L, 1, 1);
}

LUA_API void nl_adddecl(lua_State *L, const char *name)
{
  if (!lj_nl_adddecl(nl_state(L), name)) nomem(L);
}

LUA_API void nl_addpath(lua_State *L, const char *dir)
{
  if (lj_nl_isopen(L)) {
    lj_nl_pushrt(L, "addpath");
    lua_pushstring(L, dir);
    lua_call(L, 1, 0);
  } else if (!lj_nl_addpath(nl_state(L), dir)) {
    nomem(L);
  }
}

/* include(path): loadfile, then a call; errors from either propagate
** (DESIGN 10, IMPLEMENTATION_PLAN M4).
*/
int lj_nl_include(lua_State *L)
{
  const char *path = luaL_checkstring(L, 1);
  lua_settop(L, 1);
  if (luaL_loadfile(L, path) != 0) lua_error(L);
  lua_call(L, 0, LUA_MULTRET);
  return lua_gettop(L) - 1;
}

#endif
