/*
** nl_compile for builds that do not link libnlc (NL_LIBNLC unset in the
** Makefile): every compile fails with one diagnostic. It is linked into the
** luajit executable and the shared library only, never into libluajit.a, so
** a host that links the static library links libnlc itself.
*/

#include <stdlib.h>
#include <string.h>

#include "nlc.h"

struct nlc_Result {
  nlc_Diagnostic diag;
  char file[1];
};

static const char nocomp_msg[] =
  "the NL compiler is not linked into this build (NL_LIBNLC)";

nlc_Result *nl_compile(const char *source, size_t size, const char *chunkname,
		       const nlc_Options *options, const nlc_Loader *loader)
{
  const char *file = !chunkname ? "?" :
		     (chunkname[0] == '@' || chunkname[0] == '=') ?
		     chunkname + 1 : chunkname;
  size_t len = strlen(file);
  nlc_Result *r = (nlc_Result *)malloc(sizeof(nlc_Result) + len);
  (void)source; (void)size; (void)options; (void)loader;
  if (!r) return NULL;
  memcpy(r->file, file, len + 1);
  r->diag.severity = NLC_DIAG_ERROR;
  r->diag.code = "no-compiler";
  r->diag.message = nocomp_msg;
  r->diag.file = r->file;
  r->diag.line = r->diag.column = r->diag.endline = r->diag.endcolumn = 1;
  return r;
}

const char *nlc_chunk(const nlc_Result *r, size_t *size)
{
  (void)r;
  if (size) *size = 0;
  return NULL;
}

size_t nlc_ndiagnostics(const nlc_Result *r)
{
  return r ? 1 : 0;
}

const nlc_Diagnostic *nlc_diagnostic(const nlc_Result *r, size_t i)
{
  return r && i == 0 ? &r->diag : NULL;
}

void nlc_free(nlc_Result *r)
{
  free(r);
}
