/*
** NL hooks called from upstream files.
**
** Everything NL adds to the VM sits in the lj_nl*.c, lib_nl.c and
** lib_event.c files; their shared declarations are in lj_nlint.h. Upstream
** files only get small hooks guarded by LJ_NL (see lj_arch.h): lj_obj.h (one
** global_State field), lj_state.c (create/free), lj_load.c (luaL_loadfilex
** by extension), lj_lex.c (the NL_OPT_GLUA option) and lib_init.c
** (luaL_openlibs opens NL). This header is all they include, so they see no
** system header of NL's.
*/

#ifndef _LJ_NL_H
#define _LJ_NL_H

#include "lj_obj.h"

#if LJ_NL

/* Functions here use LJ_FUNCA, not LJ_FUNC: the files that call them may be
** compiled into ljamalg.c, where LJ_FUNC means static, while the NL files are
** always separate objects.
*/

/* lj_nlstate.c */
LJ_FUNCA int lj_nl_newstate(global_State *g);
LJ_FUNCA void lj_nl_freestate(global_State *g);
LJ_FUNCA int lj_nl_glua(lua_State *L);

/* lj_nlload.c */
LJ_FUNCA int lj_nl_isnlfile(const char *filename);
LJ_FUNCA int lj_nl_loadfilex(lua_State *L, const char *filename,
			    const char *mode);

#endif

#endif
