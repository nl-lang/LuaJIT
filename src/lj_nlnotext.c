/*
** nltext.h and nlregex.h for builds that do not link libnlc (NL_LIBNLC
** unset in the Makefile), and for a libnlc that lacks the libraries (the
** identity compiler of tests/vm): nltext_available and nlregex_available
** return 0, so lj_nl_openhost gives the runtime no host.text and no
** host.regex, and the runtime keeps its Lua stand-ins (String without
** Unicode case tables; Regex.new raises). The rest is never called and
** only satisfies the linker. Linked where lj_nlnocomp.c is, never into
** libluajit.a.
*/

#include "nltext.h"
#include "nlregex.h"

int nltext_available(void) { return 0; }

size_t nltext_invalid_at(const char *s, size_t len)
{
  (void)s; (void)len;
  return NLTEXT_NONE;
}

size_t nltext_count(const char *s, size_t len)
{
  (void)s;
  return len;
}

size_t nltext_offset(const char *s, size_t len, size_t pos)
{
  (void)s;
  return pos < len ? pos : len;
}

size_t nltext_charpos(const char *s, size_t len, size_t byte)
{
  (void)s;
  return byte < len ? byte : len;
}

int nltext_slice(const char *s, size_t len, int64_t first, int64_t last,
		 int exclusive, size_t *start, size_t *end)
{
  (void)s; (void)len; (void)first; (void)last; (void)exclusive;
  (void)start; (void)end;
  return 0;
}

int nltext_charat(const char *s, size_t len, int64_t pos,
		  size_t *start, size_t *end)
{
  (void)s; (void)len; (void)pos; (void)start; (void)end;
  return 0;
}

char *nltext_case(const char *s, size_t len, int mode, size_t *outlen)
{
  (void)s; (void)len; (void)mode;
  if (outlen) *outlen = 0;
  return NULL;
}

void nltext_free(char *p, size_t len) { (void)p; (void)len; }

uint32_t nltext_decode(const char *s, size_t len, size_t byte, size_t *clen)
{
  (void)s; (void)len; (void)byte;
  if (clen) *clen = 0;
  return 0;
}

size_t nltext_encode(uint32_t cp, char *out)
{
  (void)cp; (void)out;
  return 0;
}

int nlregex_available(void) { return 0; }

nlregex *nlregex_compile(const char *pat, size_t len, size_t size_limit,
			 char **err)
{
  (void)pat; (void)len; (void)size_limit;
  if (err) *err = NULL;
  return NULL;
}

void nlregex_free(nlregex *re) { (void)re; }
void nlregex_freemsg(char *msg) { (void)msg; }
size_t nlregex_memory(const nlregex *re) { (void)re; return 0; }
size_t nlregex_ngroups(const nlregex *re) { (void)re; return 0; }

const char *nlregex_groupname(const nlregex *re, size_t i, size_t *len)
{
  (void)re; (void)i;
  if (len) *len = 0;
  return NULL;
}

const char *nlregex_pattern(const nlregex *re, size_t *len)
{
  (void)re;
  if (len) *len = 0;
  return NULL;
}

int nlregex_test(const nlregex *re, const char *s, size_t len)
{
  (void)re; (void)s; (void)len;
  return 0;
}

int nlregex_find(const nlregex *re, const char *s, size_t len, size_t start,
		 size_t *mstart, size_t *mend)
{
  (void)re; (void)s; (void)len; (void)start; (void)mstart; (void)mend;
  return 0;
}

int nlregex_captures(const nlregex *re, const char *s, size_t len,
		     size_t start, size_t *spans, size_t nspans)
{
  (void)re; (void)s; (void)len; (void)start; (void)spans; (void)nspans;
  return 0;
}
