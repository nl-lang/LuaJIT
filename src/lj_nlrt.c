/*
** Lua code of NL embedded as bytecode, compiled at build time (Makefile):
** - the runtime library: the concatenation of the .lua files of runtime/ in
**   file-name order (lj_nlrt.h);
** - the run loop of the event library, lj_nlevent.lua (lj_nlev.h).
** Built with LJ_NLRT_BOOT for the bootstrap interpreter that compiles them,
** which has neither.
*/

#define lj_nlrt_c

#include "lj_nlint.h"

#if LJ_NL

#ifdef LJ_NLRT_BOOT
LJ_DATADEF const unsigned char *const lj_nlrt_bc = NULL;
LJ_DATADEF const size_t lj_nlrt_size = 0;
LJ_DATADEF const unsigned char *const lj_nlev_bc = NULL;
LJ_DATADEF const size_t lj_nlev_size = 0;
#else
#include "lj_nlrt.h"
#include "lj_nlev.h"
LJ_DATADEF const unsigned char *const lj_nlrt_bc = luaJIT_BC_nlrt;
LJ_DATADEF const size_t lj_nlrt_size = luaJIT_BC_nlrt_SIZE;
LJ_DATADEF const unsigned char *const lj_nlev_bc = luaJIT_BC_nlev;
LJ_DATADEF const size_t lj_nlev_size = luaJIT_BC_nlev_SIZE;
#endif

#endif
