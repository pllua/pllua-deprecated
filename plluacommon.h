#ifndef PLLUACOMMON_H
#define PLLUACOMMON_H

/* PostgreSQL */
#include <postgres.h>
#include <fmgr.h>
#include <funcapi.h>
#include <access/heapam.h>
#if PG_VERSION_NUM >= 90300
#include <access/htup_details.h>
#endif
#include <catalog/namespace.h>
#include <catalog/pg_proc.h>
#include <catalog/pg_type.h>
#include <commands/trigger.h>
#include <executor/spi.h>
#include <nodes/makefuncs.h>
#include <parser/parse_type.h>
#include <utils/array.h>
#include <utils/builtins.h>
#include <utils/datum.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/rel.h>
#include <utils/syscache.h>
#include <utils/typcache.h>
/* Lua */
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#if LUA_VERSION_NUM <= 501
#define lua_pushglobaltable(L) lua_pushvalue(L, LUA_GLOBALSINDEX)
#define lua_setuservalue lua_setfenv
#define lua_getuservalue lua_getfenv
#define lua_rawlen lua_objlen
#define luaP_register(L,l) luaL_register(L, NULL, (l))
#else
#define luaP_register(L,l) luaL_setfuncs(L, (l), 0)
#endif

#if !defined LUA_VERSION_NUM || LUA_VERSION_NUM==501
/*
** Adapted from Lua 5.2.0
*/
void luaL_setfuncs (lua_State *L, const luaL_Reg *l, int nup);
#endif

#define PLLUA_VERSION "PL/Lua 1.0"

#if defined(PLLUA_DEBUG)
#include "pllua_debug.h"
#define BEGINLUA int ____stk=lua_gettop(L)
#define ENDLUA ____stk =lua_gettop(L)-____stk; if(0!=____stk) ereport(INFO, (errmsg("stk %s>%i",AT,____stk)))
#define ENDLUAV(v) ____stk =(lua_gettop(L)-____stk-v); if(0!=____stk) ereport(INFO, (errmsg("stk %s>%i",AT,____stk)))
#else
#define BEGINLUA
#define ENDLUA
#define ENDLUAV(v)
#endif

/* get MemoryContext for state L */
MemoryContext luaP_getmemctxt (lua_State *L);

lua_State *pllua_getmaster (lua_State *L);

#define MTOLUA(state) {MemoryContext ___mcxt,___m;\
    ___mcxt = luaP_getmemctxt(state); \
    ___m  = MemoryContextSwitchTo(___mcxt)

#define MTOPG MemoryContextSwitchTo(___m);}

#endif // PLLUACOMMON_H
