/*
** nlregex.h: the regex engine of libnlc (crates/nl_regex).
**
** Internal: this is not part of the host API (nl.h). The VM's regex glue
** (lib_regex.c) calls these; libnlc implements them (src/abi.rs). The stub
** lj_nlnotext.c stands in when libnlc is not linked: nlregex_available()
** then returns 0 and nothing else may be called.
**
** The engine never calls into Lua and never unwinds: a failure is a 0 or
** NULL result. Positions are 0-based byte offsets; the runtime converts
** them to character positions. Subjects are any bytes, but a pattern is
** compiled in UTF-8 mode, so on a valid UTF-8 subject a match never starts
** or ends inside a character.
*/

#ifndef _NLREGEX_H
#define _NLREGEX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nlregex nlregex;

/* A group that did not participate in a match (nlregex_captures). */
#define NLREGEX_NOSPAN	((size_t)-1)

/* 1 when the engine is linked, 0 in the stub. */
int nlregex_available(void);

/* Compiles pat[0..len). size_limit caps the compiled size in bytes (0: the
** engine's default). Returns the regex, or NULL with *err set to a message
** (freed with nlregex_freemsg; NULL when none could be made).
*/
nlregex *nlregex_compile(const char *pat, size_t len, size_t size_limit,
			 char **err);
void nlregex_free(nlregex *re);
void nlregex_freemsg(char *msg);

/* The heap the compiled regex takes, approximately. */
size_t nlregex_memory(const nlregex *re);
/* The number of capture groups, group 0 included. */
size_t nlregex_ngroups(const nlregex *re);
/* The name of group i and its length, or NULL (unnamed or no such group).
** Not NUL-terminated; valid until the regex is freed.
*/
const char *nlregex_groupname(const nlregex *re, size_t i, size_t *len);
/* The pattern and its length; not NUL-terminated. */
const char *nlregex_pattern(const nlregex *re, size_t *len);

/* 1 when s[0..len) contains a match. */
int nlregex_test(const nlregex *re, const char *s, size_t len);
/* The first match at or after byte offset start: 1 with *mstart and *mend
** set to its span (end exclusive), or 0.
*/
int nlregex_find(const nlregex *re, const char *s, size_t len, size_t start,
		 size_t *mstart, size_t *mend);
/* The groups of the first match at or after start: fills spans[0..n), n
** being the smaller of nspans and twice the group count, with the start
** and end of each group in turn (NLREGEX_NOSPAN for one that did not
** participate). Returns 1, or 0 for no match (spans untouched).
*/
int nlregex_captures(const nlregex *re, const char *s, size_t len,
		     size_t start, size_t *spans, size_t nspans);

#ifdef __cplusplus
}
#endif

#endif
