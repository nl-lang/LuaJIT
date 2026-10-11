/*
** nltext.h: the UTF-8 string operations of libnlc (crates/nl_text).
**
** Internal: this is not part of the host API (nl.h). The VM's string glue
** (lib_nltext.c) calls these; libnlc implements them (src/abi.rs). The stub
** lj_nlnotext.c stands in when libnlc is not linked: nltext_available()
** then returns 0 and nothing else may be called.
**
** Nothing here calls into Lua or unwinds: a failure is a 0, NULL or
** NLTEXT_NONE result. Every function is total on any bytes; the glue checks
** validity (nltext_invalid_at) once per string and raises EncodingError
** before a character operation. Byte offsets are 0-based; the NL positions
** of nltext_slice and nltext_charat are 1-based and negative from the end.
*/

#ifndef _NLTEXT_H
#define _NLTEXT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NLTEXT_NONE	((size_t)-1)

/* 1 when the library is linked, 0 in the stub. */
int nltext_available(void);

/* The byte index of the first invalid UTF-8 sequence, or NLTEXT_NONE. */
size_t nltext_invalid_at(const char *s, size_t len);
/* The number of characters. */
size_t nltext_count(const char *s, size_t len);
/* The byte offset where the character with 0-based index pos starts; len
** when there is none.
*/
size_t nltext_offset(const char *s, size_t len, size_t pos);
/* The number of characters that start before byte offset byte. */
size_t nltext_charpos(const char *s, size_t len, size_t byte);
/* s[first..last] (exclusive: s[first...last]): 1 with *start and *end set
** to the byte range of the selected characters, or 0 when nothing is.
*/
int nltext_slice(const char *s, size_t len, int64_t first, int64_t last,
		 int exclusive, size_t *start, size_t *end);
/* s[pos]: 1 with the byte range of the character, or 0 out of range. */
int nltext_charat(const char *s, size_t len, int64_t pos,
		  size_t *start, size_t *end);
/* The case mapping mode (NLTEXT_CASE_*) of valid UTF-8 s[0..len): a new
** buffer of *outlen bytes, freed with nltext_free; NULL on invalid input.
*/
#define NLTEXT_CASE_UP		0
#define NLTEXT_CASE_DOWN	1
#define NLTEXT_CASE_CAPITALIZE	2
#define NLTEXT_CASE_SWAP	3
char *nltext_case(const char *s, size_t len, int mode, size_t *outlen);
void nltext_free(char *p, size_t len);
/* The code point starting at byte offset byte and its length in *clen; 0
** and *clen = 0 at the end or where no valid character starts.
*/
uint32_t nltext_decode(const char *s, size_t len, size_t byte, size_t *clen);
/* The UTF-8 bytes of cp into out (room for 4); their number, or 0 when cp
** is not a Unicode scalar value.
*/
size_t nltext_encode(uint32_t cp, char *out);

#ifdef __cplusplus
}
#endif

#endif
