/*
 * pllua.c: PL/Lua call handler, trusted
 * Author: Luis Carvalho <lexcarvalho at gmail.com>
 * Please check copyright notice at the bottom of pllua.h
 * $Id: pllua.c,v 1.15 2008/03/29 02:49:55 carvalho Exp $
 */

#include "pllua.h"

PG_MODULE_MAGIC;

static lua_State *L[2] = {NULL, NULL}; /* Lua VMs */

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
  pllua_init_common_ctx();
  L[0] = luaP_newstate(0); /* untrusted */
  L[1] = luaP_newstate(1); /* trusted */
  RegisterXactCallback(pllua_xact_cb, NULL);
  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(_PG_fini);
Datum _PG_fini(PG_FUNCTION_ARGS) {
  luaP_close(L[0]);
  luaP_close(L[1]);
  pllua_delete_common_ctx();
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

/* p_lua_mem_cxt and p_lua_master_state addresses used as keys for storing
 * values in lua states. In case when this functions are equal then
 * visual studio compiler with default settings "optimize" it and as
 * result both functions have equal addresses which make them unusable as keys,
 * thats why return 1; return 2;
*/

int p_lua_mem_cxt(void){return 2;}
int p_lua_master_state(void){return 1;}


void push_spi_error(lua_State *L, MemoryContext oldcontext)
{
    ErrorData  *edata;
    /* Save error info */
    MemoryContextSwitchTo(oldcontext);
    edata = CopyErrorData();
    FlushErrorState();
    lua_pushstring(L, edata->message);
    FreeErrorData(edata);
}

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
