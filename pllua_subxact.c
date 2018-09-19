#include "pllua_subxact.h"

#include <utils/resowner.h>
#include <access/xact.h>
#include <executor/spi.h>
#include "pllua_debug.h"
#include "plluacommon.h"
#include "rtupdescstk.h"
#include "pllua_errors.h"


typedef struct
{
    ResourceOwner		resowner;
    MemoryContext		mcontext;

} SubTransactionBlock;


static SubTransactionBlock	stb_SubTranBlock(){
    SubTransactionBlock stb;
    stb.mcontext = NULL;
    stb.resowner = NULL;
    return stb;
}

static void stb_enter(lua_State *L, SubTransactionBlock *block){
    if (!IsTransactionOrTransactionBlock())
        luaL_error(L, "out of transaction");

    block->resowner = CurrentResourceOwner;
    block->mcontext = CurrentMemoryContext;
    BeginInternalSubTransaction(NULL);
    /* Do not want to leave the previous memory context */
    MemoryContextSwitchTo(block->mcontext);
}

static void stb_exit(SubTransactionBlock *block, bool success){
    if (success)
        ReleaseCurrentSubTransaction();
    else
        RollbackAndReleaseCurrentSubTransaction();

    MemoryContextSwitchTo(block->mcontext);
    CurrentResourceOwner = block->resowner;

    /*
     * AtEOSubXact_SPI() should not have popped any SPI context, but just
     * in case it did, make sure we remain connected.
     */
    SPI_restore_connection();
}

/* all exceptions should be thrown only through luaL_error
 * we might me be here if there is a postgres unhandled exception
 * and lua migth be in an inconsistent state that's why the process aborted
 */
#define WRAP_SUBTRANSACTION(source_code)  do\
{\
    RTupDescStack funcxt;\
    RTupDescStack prev;\
    SubTransactionBlock		subtran;\
    funcxt = rtds_initStack(L);\
    rtds_inuse(funcxt);\
    prev = rtds_set_current(funcxt);\
    subtran = stb_SubTranBlock();\
    stb_enter(L, &subtran);\
    PG_TRY();\
{\
    source_code\
    }\
    PG_CATCH();\
{\
    ErrorData  *edata;\
    edata = CopyErrorData();\
    ereport(FATAL, (errmsg("Unhandled exception: %s", edata->message)));\
 }\
    PG_END_TRY();\
    stb_exit(&subtran, status == 0);\
    if (status)  rtds_unref(funcxt);\
    rtds_set_current(prev);\
}while(0)


int subt_luaB_pcall (lua_State *L) {
    int status = 0;

    luaL_checkany(L, 1);

    WRAP_SUBTRANSACTION(
                status = lua_pcall(L, lua_gettop(L) - 1, LUA_MULTRET, 0);
            );

    lua_pushboolean(L, (status == 0));
    lua_insert(L, 1);
    return lua_gettop(L);  /* return status + all results */
}

int subt_luaB_xpcall (lua_State *L) {
    int status = 0;

    luaL_checkany(L, 2);
    lua_settop(L, 2);
    lua_insert(L, 1);  /* put error function under function to be called */

    WRAP_SUBTRANSACTION(
                status = lua_pcall(L, 0, LUA_MULTRET, 1);
            );

    lua_pushboolean(L, (status == 0));
    lua_replace(L, 1);
    return lua_gettop(L);  /* return status + all results */
}

int use_subtransaction(lua_State *L){
    int status = 0;


    if (lua_gettop(L) < 1){
        return luaL_error(L, "subtransaction has no function argument");
    }
    if (lua_type(L, 1) != LUA_TFUNCTION){
        return luaL_error(L, "subtransaction first arg must be a lua function");
    }

    WRAP_SUBTRANSACTION(
                status = lua_pcall(L, lua_gettop(L) - 1, LUA_MULTRET, 0);
            );


    lua_pushboolean(L, (status == 0));
    lua_insert(L, 1);
    return lua_gettop(L);  /* return status + all results */
}
