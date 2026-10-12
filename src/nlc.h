/*
** nlc.h: the boundary between the VM and the NL compiler library (libnlc).
**
** Internal: this is not part of the host API (API.md 2.3). The VM calls
** nl_compile; libnlc (crates/nl_capi in the NL repository, src/capi.rs)
** implements it. The struct layouts here must match the #[repr(C)] types
** there.
**
** The compiler is pure. It takes no lua_State, never calls into Lua, reads
** imported files only through the loader it is given, and can be called from
** any thread. No Lua error and no Rust panic crosses this boundary: a panic in
** the compiler is caught and reported as a diagnostic with the code
** "internal-error".
**
** The chunk. nlc_chunk() returns input for lua_load: a bytecode dump when
** nlc_Options.bytecode is set (IMPLEMENTATION_PLAN M3, crates/nl_codegen),
** Lua source text otherwise (the oracle backend, crates/nl_luagen). Both are
** loaded in two steps: lua_load gives a function P, and P(rt), where rt is
** the runtime table of the state, returns the file's main function
** (crates/nl_capi/src/driver.rs). The VM asks for bytecode; the Lua backend
** stays reachable through `nlc build --emit=lua`.
*/

#ifndef _NLC_H
#define _NLC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Same layout as nl_Loader in nl.h. */
typedef struct nlc_Loader {
  /* Copies the canonical name of the file that `path` (written in an import
  ** of `from`, or of no file when `from` is NULL) means into out[size].
  ** Returns 1, or 0 when there is no such file.
  */
  int (*resolve)(void *data, const char *from, const char *path,
		 char *out, size_t size);
  /* The contents of a canonical name, or NULL. */
  const char *(*read)(void *data, const char *name, size_t *size);
  /* Called once for each successful read, when the compiler is done. */
  void (*release)(void *data, const char *name, const char *buf);
  void *data;
} nlc_Loader;

typedef struct nlc_Options {
  int checks;		/* NL_OPT_CHECKS */
  int strip;		/* NL_OPT_STRIP */
  int bytecode;		/* 1: a bytecode dump; 0: Lua source (see above). */
  /* The feature level of a bytecode dump (IMPLEMENTATION_PLAN M5): 0 uses
  ** stock opcodes only and loads in a stock LuaJIT; 1 uses the fork's
  ** opcodes, prototype flags and events (DESIGN 14.2) and is marked
  ** BCDUMP_F_NL. The VM asks for 1. Ignored for Lua source. */
  int level;
  /* The declaration files in scope (GRAMMAR.md 10, API.md 2.3): ndecls
  ** names, each read through the loader's `read` when the compile starts.
  ** A name that cannot be read is an error of the compile. */
  const char *const *decls;
  size_t ndecls;
} nlc_Options;

#define NLC_DIAG_ERROR		0
#define NLC_DIAG_WARNING	1
#define NLC_DIAG_LINT		2

/* Same layout as nl_Diagnostic in nl.h. */
typedef struct nlc_Diagnostic {
  int severity;
  const char *code;
  const char *message;
  const char *file;
  int line, column;
  int endline, endcolumn;
} nlc_Diagnostic;

typedef struct nlc_Result nlc_Result;

/* The intrinsic table of KINTR (DESIGN 14.2, IMPLEMENTATION_PLAN M5). Entry
** i, counting from 1 in this order, is rt[name_i]: the value of that field of
** the runtime table when the nl library opened (lj_nlvm.c). Level-1 code
** loads an intrinsic with KINTR and its index, so the compiler
** (crates/nl_codegen/src/intrinsics.rs) and the VM must agree on this list;
** a test there checks it against this header. The first three entries are
** what the VM itself uses (lj_nl.h): NL's String library, the character
** indexing of strings, and the runtime's generic operator handlers by event
** name. New names go at the end.
*/
#define NLC_INTRINSICS(_) \
  _(String) _(strindex) _(opgeneric) \
  _(as) _(async) _(band) _(bnot) _(bor) _(bxor) _(check) _(enum) _(eq) \
  _(ge) _(gettype) _(gt) _(idiv) _(implement) _(import_from) _(index) \
  _(inrange) _(isa) _(iskw) _(istable) _(iter) _(keyed) _(kwlast) _(kwpop) \
  _(kwtake) _(lazy) _(le) _(len) _(listcat) _(lmethod) _(lt) _(matcherror) \
  _(mcall) _(member) _(method) _(mget) _(module) _(negate) _(new) _(nokw) \
  _(nokwrest) _(on) _(opdef) _(optional) _(owner) _(primmethod) _(primop) \
  _(range) _(record) _(register) _(rest) _(ret) _(retn) _(shl) _(shr) \
  _(spawn) _(step) _(submodule) _(tostring) _(try) _(union) _(unpack) \
  _(unpackkw) _(ushr) _(when) \
  _(nfloor) _(nband) _(nbor) _(nbxor) _(nlshift) _(narshift) _(nrshift)

/* Compiles one NL source file. `chunkname` follows Lua's chunk names: "@name"
** for a file with canonical name `name`, "=text" for anything else. `loader`
** must not be NULL. Returns NULL only when the result cannot be built at all
** (libnlc, being Rust, aborts the process when memory runs out instead).
*/
nlc_Result *nl_compile(const char *source, size_t size, const char *chunkname,
		       const nlc_Options *options, const nlc_Loader *loader);

/* The chunk, or NULL when a diagnostic is an error. */
const char *nlc_chunk(const nlc_Result *r, size_t *size);

/* Every diagnostic of the compile, errors first within a file. Pointers stay
** valid until nlc_free.
*/
size_t nlc_ndiagnostics(const nlc_Result *r);
const nlc_Diagnostic *nlc_diagnostic(const nlc_Result *r, size_t i);

void nlc_free(nlc_Result *r);

#ifdef __cplusplus
}
#endif

#endif
