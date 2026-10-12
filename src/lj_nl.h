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
#include "lj_frame.h"

#if LJ_NL

#include "nlc.h"

/* -- VM extensions ------------------------------------------------------- */

/* The intrinsic table (nlc.h, NLC_INTRINSICS): KINTR reads it from
** g->nl_intr, the array part of a table that luaopen_nl fills and anchors
** in the registry. Index 0 is unused.
*/
enum {
  NLINTR__0,
#define NLINTR_ENUM(name)	NLINTR_##name,
  NLC_INTRINSICS(NLINTR_ENUM)
#undef NLINTR_ENUM
  NLINTR__MAX
};
#define lj_nl_intr(g, i)	(&mref((g)->nl_intr, TValue)[(i)])
#define lj_nl_hasintr(g)	(mref((g)->nl_intr, TValue) != NULL)
#define lj_nl_strlib(g)		tabV(lj_nl_intr((g), NLINTR_String))

/* Whether the running function was compiled from NL (PROTO_NL). Only for
** the slow paths the interpreter calls with a Lua frame on top; a C frame
** (the C API) gets Lua's rules.
*/
static LJ_AINLINE int lj_nl_curnl(lua_State *L)
{
  GCfunc *fn = curr_func(L);
  return isluafunc(fn) && (funcproto(fn)->nlflags & PROTO_NL);
}

/* lj_nlvm.c */
LJ_FUNCA int lj_nl_ismethod(global_State *g, cTValue *f, cTValue *recv);
LJ_FUNCA int lj_nl_ismethod_ud(global_State *g, GCudata *ud);
LJ_FUNCA cTValue *lj_nl_opgeneric(global_State *g, MMS mm);

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
