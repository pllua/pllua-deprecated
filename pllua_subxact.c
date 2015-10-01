#include "pllua_subxact.h"

#include <utils/resowner.h>
#include <access/xact.h>
#include <executor/spi.h>
#include "pllua_debug.h"
#include "plluacommon.h"
#include "rtupdescstk.h"


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


int use_subtransaction(lua_State *L){

    SubTransactionBlock		subtran;

    RTupDescStack funcxt;
    RTupDescStack prev;
    int pcall_result = 0;


    if (lua_gettop(L) < 1){
        return luaL_error(L, "subtransaction has no function argument");
    }
    if (lua_type(L, 1) != LUA_TFUNCTION){
        return luaL_error(L, "subtransaction first arg must be a lua function");
    }

    funcxt = rtds_initStack(L);
    rtds_inuse(funcxt);
    prev = rtds_set_current(funcxt);

    subtran = stb_SubTranBlock();
    stb_enter(L, &subtran);

    PG_TRY();
    {
        pcall_result = lua_pcall(L, lua_gettop(L) - 1, LUA_MULTRET, 0);
    }
    PG_CATCH();
    {
        /* all exceptions should be thrown only through luaL_error
         * we might me be here if there is a postgres unhandled exception
         * and lua migth be in an inconsistant state that's why the process aborted
         */
        ErrorData  *edata;
        edata = CopyErrorData();
        ereport(FATAL, (errmsg("Unhandled exception: %s", edata->message)));
    }
    PG_END_TRY();
    stb_exit(&subtran, pcall_result == 0);

    funcxt = rtds_unref(funcxt);
    rtds_set_current(prev);

    if (pcall_result == 0){
        return lua_gettop(L);
    }else{
        lua_pushnil(L);
        lua_insert(L, 1);
        return 2;
    }

    return 0;
}
