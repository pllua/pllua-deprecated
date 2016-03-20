/*
 * pllua.c: PL/Lua call handler, trusted
 * Author: Luis Carvalho <lexcarvalho at gmail.com>
 * Please check copyright notice at the bottom of pllua.h
 * $Id: pllua.c,v 1.15 2008/03/29 02:49:55 carvalho Exp $
 */

#include "pllua.h"
#include "pllua_errors.h"

PG_MODULE_MAGIC;

static lua_State *LuaVM[2] = {NULL, NULL}; /* Lua VMs */

LVMInfo lvm_info[2];

static void init_vmstructs(){
  LVMInfo lvm0;
  LVMInfo lvm1;

  lvm0.name = "untrusted";
  lvm0.hasTraceback = 0;

  lvm1.name = "trusted";
  lvm1.hasTraceback = 0;

  lvm_info[0] = lvm0;
  lvm_info[1] = lvm1;
}



PGDLLEXPORT Datum _PG_init(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum _PG_fini(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum pllua_validator(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum pllua_call_handler(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum plluau_validator(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum plluau_call_handler(PG_FUNCTION_ARGS);
#if PG_VERSION_NUM >= 90000
PGDLLEXPORT Datum pllua_inline_handler(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum plluau_inline_handler(PG_FUNCTION_ARGS);
#endif

#include "pllua_xact_cleanup.h"
PG_FUNCTION_INFO_V1(_PG_init);
Datum _PG_init(PG_FUNCTION_ARGS) {
  init_procedure_caches();
  init_vmstructs();
  pllua_init_common_ctx();
  LuaVM[0] = luaP_newstate(0); /* untrusted */
  LuaVM[1] = luaP_newstate(1); /* trusted */
  RegisterXactCallback(pllua_xact_cb, NULL);
  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(_PG_fini);
Datum _PG_fini(PG_FUNCTION_ARGS) {
  luaP_close(LuaVM[0]);
  luaP_close(LuaVM[1]);
  pllua_delete_common_ctx();
  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(plluau_validator);
Datum plluau_validator(PG_FUNCTION_ARGS) {
  return luaP_validator(LuaVM[0], PG_GETARG_OID(0));
}

PG_FUNCTION_INFO_V1(plluau_call_handler);
Datum plluau_call_handler(PG_FUNCTION_ARGS) {
  lvm_info[0].hasTraceback = false;
  return luaP_callhandler(LuaVM[0], fcinfo);
}

PG_FUNCTION_INFO_V1(pllua_validator);
Datum pllua_validator(PG_FUNCTION_ARGS) {
  return luaP_validator(LuaVM[1], PG_GETARG_OID(0));
}

PG_FUNCTION_INFO_V1(pllua_call_handler);
Datum pllua_call_handler(PG_FUNCTION_ARGS) {
  lvm_info[1].hasTraceback =  false;
  return luaP_callhandler(LuaVM[1], fcinfo);
}

#if PG_VERSION_NUM >= 90000
#define CODEBLOCK \
  ((InlineCodeBlock *) DatumGetPointer(PG_GETARG_DATUM(0)))->source_text

PG_FUNCTION_INFO_V1(plluau_inline_handler);
Datum plluau_inline_handler(PG_FUNCTION_ARGS) {
  lvm_info[0].hasTraceback = false;
  return luaP_inlinehandler(LuaVM[0], CODEBLOCK);
}

PG_FUNCTION_INFO_V1(pllua_inline_handler);
Datum pllua_inline_handler(PG_FUNCTION_ARGS) {
  lvm_info[1].hasTraceback = false;
  return luaP_inlinehandler(LuaVM[1], CODEBLOCK);
}
#endif

/* p_lua_mem_cxt and p_lua_master_state addresses used as keys for storing
 * values in lua states. In case when this functions are equal then
 * visual studio compiler with default settings "optimize" it and as
 * result both functions have equal addresses which make them unusable as keys,
 * thats why return 1; return 2;
*/

int p_lua_mem_cxt(void){return 2;}
int p_lua_master_state(void){return 1;}

//------------------------------------------------------------------------------------------------------

MemoryContext luaP_getmemctxt(lua_State *L) {
    MemoryContext mcxt;
    lua_pushlightuserdata(L, p_lua_mem_cxt);
    lua_rawget(L, LUA_REGISTRYINDEX);
    mcxt = lua_touserdata(L, -1);
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

int pllua_getmaster_index(lua_State *L) {
    if (pllua_getmaster(L) == LuaVM[0])
      return 0;
    return 1;
}

#if LUA_VERSION_NUM < 502
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
#endif

int pg_to_regtype(char *typ_name)
{

    Oid			result;
    int32		typmod;

    /*
     * Invoke the full parser to deal with special cases such as array syntax.
     */

#if PG_VERSION_NUM < 90400
           parseTypeString(typ_name, &result, &typmod);
#else
            parseTypeString(typ_name, &result, &typmod, true);
#endif


    if (OidIsValid(result))
        return result;
    else
        return -1;
}
