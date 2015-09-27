/*
 * pllua.c: PL/Lua call handler, trusted
 * Author: Luis Carvalho <lexcarvalho at gmail.com>
 * Please check copyright notice at the bottom of pllua.h
 * $Id: pllua.c,v 1.15 2008/03/29 02:49:55 carvalho Exp $
 */

#include "pllua.h"

PG_MODULE_MAGIC;

static lua_State *L[2] = {NULL, NULL}; /* Lua VMs */

Datum _PG_init(PG_FUNCTION_ARGS);
Datum _PG_fini(PG_FUNCTION_ARGS);
Datum pllua_validator(PG_FUNCTION_ARGS);
Datum pllua_call_handler(PG_FUNCTION_ARGS);
Datum plluau_validator(PG_FUNCTION_ARGS);
Datum plluau_call_handler(PG_FUNCTION_ARGS);
#if PG_VERSION_NUM >= 90000
Datum pllua_inline_handler(PG_FUNCTION_ARGS);
Datum plluau_inline_handler(PG_FUNCTION_ARGS);
#endif

PG_FUNCTION_INFO_V1(_PG_init);
Datum _PG_init(PG_FUNCTION_ARGS) {
  L[0] = luaP_newstate(0); /* untrusted */
  L[1] = luaP_newstate(1); /* trusted */
  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(_PG_fini);
Datum _PG_fini(PG_FUNCTION_ARGS) {
  luaP_close(L[0]);
  luaP_close(L[1]);
  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(plluau_validator);
Datum plluau_validator(PG_FUNCTION_ARGS) {
  return luaP_validator(L[0], PG_GETARG_OID(0));
}

PG_FUNCTION_INFO_V1(plluau_call_handler);
Datum plluau_call_handler(PG_FUNCTION_ARGS) {
  return luaP_callhandler(L[0], fcinfo);
}

PG_FUNCTION_INFO_V1(pllua_validator);
Datum pllua_validator(PG_FUNCTION_ARGS) {
  return luaP_validator(L[1], PG_GETARG_OID(0));
}

PG_FUNCTION_INFO_V1(pllua_call_handler);
Datum pllua_call_handler(PG_FUNCTION_ARGS) {
  return luaP_callhandler(L[1], fcinfo);
}

#if PG_VERSION_NUM >= 90000
#define CODEBLOCK \
  ((InlineCodeBlock *) DatumGetPointer(PG_GETARG_DATUM(0)))->source_text

PG_FUNCTION_INFO_V1(plluau_inline_handler);
Datum plluau_inline_handler(PG_FUNCTION_ARGS) {
  return luaP_inlinehandler(L[0], CODEBLOCK);
}

PG_FUNCTION_INFO_V1(pllua_inline_handler);
Datum pllua_inline_handler(PG_FUNCTION_ARGS) {
  return luaP_inlinehandler(L[1], CODEBLOCK);
}
#endif


void p_lua_mem_cxt(void){}
void p_lua_master_state(void){}

MemoryContext luaP_getmemctxt(lua_State *L) {
    MemoryContext mcxt;
    lua_pushlightuserdata(L, p_lua_mem_cxt);
    lua_rawget(L, LUA_REGISTRYINDEX);
    mcxt = (MemoryContext) lua_touserdata(L, -1);
    lua_pop(L, 1);
    return mcxt;
}

lua_State * pllua_getmaster(lua_State *L) {
    lua_State *master;
    lua_pushlightuserdata(L, p_lua_master_state);
    lua_rawget(L, LUA_REGISTRYINDEX);
    master = (lua_State *) lua_touserdata(L, -1);
    lua_pop(L, 1);
    return master;
}

void luaL_setfuncs(lua_State *L, const luaL_Reg *l, int nup) {
    luaL_checkstack(L, nup+1, "too many upvalues");
    for (; l->name != NULL; l++) {  /* fill the table with given functions */
        int i;
        lua_pushstring(L, l->name);
        for (i = 0; i < nup; i++)  /* copy upvalues to the top */
            lua_pushvalue(L, -(nup+1));
        lua_pushcclosure(L, l->func, nup);  /* closure with those upvalues */
        lua_settable(L, -(nup + 3));
    }
    lua_pop(L, nup);  /* remove upvalues */
}



