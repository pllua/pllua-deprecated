/*
   pgfunc works as a wrapper for postgres functions and as a loader as module for
 pllua (internal)->internal  functions.

 pgfunc(text - function singnature)
 pgfunc(text - function singnature, table - options)

options =
{
only_internal = true --false,
--[[if only_internal is false, pgfunc accepts any function]]

throwable = true --false
--[[ throwable makes PG_TRY PG_CATCH for non internal functions]]

]]

}

Note:
Set returning functions are not supported.
In/Out functions are not supported.
No checks if function is strict.

 */

#include "pllua_pgfunc.h"

#include "plluacommon.h"
#include "pllua.h"
#include "pllua_xact_cleanup.h"

#include <catalog/pg_language.h>
#include <lib/stringinfo.h>

#include "pllua_errors.h"

#define RESET_CONTEXT_AFTER 1000

static const char pg_func_type_name[] = "pg_func";

static Oid find_lang_oids(const char* lang){
    HeapTuple		tuple;
    tuple = SearchSysCache(LANGNAME, CStringGetDatum(lang), 0, 0, 0);
    if (HeapTupleIsValid(tuple))
    {
        Oid langtupoid = HeapTupleGetOid(tuple);
        ReleaseSysCache(tuple);
        return langtupoid;
    }
    return 0;
}

static Oid pllua_oid = 0;
static Oid plluau_oid = 0;
static Oid get_pllua_oid(){
    if (pllua_oid !=0)
        return pllua_oid;
    return find_lang_oids("pllua");
}

static Oid get_plluau_oid(){
    if (plluau_oid !=0)
        return plluau_oid;
    return find_lang_oids("plluau");
}

typedef struct{
    bool only_internal;
    bool throwable;
} Pgfunc_options;

typedef struct{
    Oid         funcid;
    int			numargs;
    Oid		   *argtypes;
    char	  **argnames;
    char	   *argmodes;
    lua_CFunction  callfunc;
    Oid         prorettype;

    FmgrInfo	fi;
    Pgfunc_options options;
} PgFuncInfo, Lua_pgfunc;

#define freeandnil(p)    do{ if (p){\
    pfree(p);\
    p = NULL;\
    }}while(0)

static void clean_pgfuncinfo(Lua_pgfunc *data){
    freeandnil (data->argtypes);
    freeandnil (data->argnames);
    freeandnil (data->argmodes);
}

static MemoryContext get_tmpcontext(){
    MemoryContext mc;
    mc = AllocSetContextCreate(TopMemoryContext,
                                                     "pgfunc temporary context",
                                                     ALLOCSET_DEFAULT_MINSIZE,
                                                     ALLOCSET_DEFAULT_INITSIZE,
                                                     ALLOCSET_DEFAULT_MAXSIZE);
    return mc;
}

static MemoryContext tmpcontext;
static int tmpcontext_usage = 0;

static int pg_callable_func(lua_State *L)
{
    MemoryContext m;
    int i;
    FunctionCallInfoData fcinfo;
    Lua_pgfunc *fi = (Lua_pgfunc *) lua_touserdata(L, lua_upvalueindex(1));
    InitFunctionCallInfoData(fcinfo, &fi->fi, fi->numargs, InvalidOid, NULL, NULL);

    if(tmpcontext_usage> RESET_CONTEXT_AFTER ){
        MemoryContextReset(tmpcontext);
        tmpcontext_usage = 0;
    }
    ++tmpcontext_usage;

    m = MemoryContextSwitchTo(tmpcontext);

    for (i=0; i<fi->numargs; ++i){
        fcinfo.arg[i] = luaP_todatum(L, fi->argtypes[i], 0, &fcinfo.argnull[i], i+1);
    }

    if(!fi->options.only_internal && fi->options.throwable){\
        SPI_push();
        PG_TRY();
        {
            Datum d = FunctionCallInvoke(&fcinfo);
            MemoryContextSwitchTo(m);
            luaP_pushdatum(L, d, fi->prorettype);
            SPI_pop();
        }
        PG_CATCH();
        {
            lua_pop(L, lua_gettop(L));
            push_spi_error(L, m); /*context switch to m inside push_spi_error*/
            SPI_pop();
            return lua_error(L);
        }PG_END_TRY();
    }else{
        Datum d = FunctionCallInvoke(&fcinfo);
        MemoryContextSwitchTo(m);
        luaP_pushdatum(L, d, fi->prorettype);
    }

    return 1;
}

static void parse_options(lua_State *L, Pgfunc_options *opt){
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING){
            const char *key = lua_tostring(L, -2);

            if (strcmp(key, "only_internal") == 0){
                opt->only_internal =  lua_toboolean(L, -1);
            } else if (strcmp(key, "throwable") == 0){
                opt->throwable =  lua_toboolean(L, -1);
            } else {
                luaL_error(L, "pgfunc unknown option \"%s\"", key);
            }

        }
        lua_pop(L, 1);
    }
}

int get_pgfunc(lua_State *L)
{
    Lua_pgfunc *lf;
    Pgfunc_options opt;
    MemoryContext mcxt;
    MemoryContext m;
    const char* reg_name = NULL;
    HeapTuple	proctup;

    Form_pg_proc proc;
    const char* reg_error = NULL;
    int i;
    int luasrc = 0;
    Oid funcid = 0;

    BEGINLUA;

    opt.only_internal = true;
    opt.throwable = true;

    if (lua_gettop(L) == 2){
        luaL_checktype(L, 2, LUA_TTABLE);
        parse_options(L, &opt);
    }else if (lua_gettop(L) != 1){
        return luaL_error(L, "pgfunc(text): wrong arguments");
    }
    if(lua_type(L, 1) == LUA_TSTRING){
        reg_name = luaL_checkstring(L, 1);

        PG_TRY();
        {
            funcid = DatumGetObjectId(DirectFunctionCall1(regprocedurein, CStringGetDatum(reg_name)));
        }
        PG_CATCH();{}
        PG_END_TRY();
    }else if (lua_type(L, 1) == LUA_TNUMBER){
        funcid = luaL_checkinteger(L, 1);
    }

    if (!OidIsValid(funcid)){
        if (reg_name)
            return luaL_error(L,"failed to register %s", reg_name);
        return luaL_error(L,"failed to register function with oid %d", funcid);
    }

    proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
    if (!HeapTupleIsValid(proctup)){
        return luaL_error(L,"cache lookup failed for function %d", funcid);
    }

    proc = (Form_pg_proc) GETSTRUCT(proctup);

    luasrc = ((proc->prolang == get_pllua_oid())
              || (proc->prolang == get_plluau_oid()));
    if ( opt.only_internal
            &&(proc->prolang != INTERNALlanguageId)
            &&(!luasrc) ){
        ReleaseSysCache(proctup);
        return luaL_error(L, "supported only SQL/internal functions");
    }

    lf = (Lua_pgfunc *)lua_newuserdata(L, sizeof(Lua_pgfunc));

    /*make it g/collected*/
    luaP_getfield(L, pg_func_type_name);
    lua_setmetatable(L, -2);


    lf->prorettype = proc->prorettype;
    lf->funcid = funcid;
    lf->options = opt;

    mcxt = get_common_ctx();
    m  = MemoryContextSwitchTo(mcxt);

    lf->numargs = get_func_arg_info(proctup,
                                    &lf->argtypes, &lf->argnames, &lf->argmodes);

    m  = MemoryContextSwitchTo(m);

    if (luasrc){
        bool isnull;
        text *t;
        const char *s;
        luaL_Buffer b;
        int pcall_result;
        Datum prosrc;

        if((lf->numargs != 1)
                || (lf->argtypes[0] != INTERNALOID)
                || (lf->prorettype != INTERNALOID)){
            luaL_error(L, "pgfunc accepts only 'internal' pllua/u functions with internal argument");
        }

        prosrc = SysCacheGetAttr(PROCOID, proctup, Anum_pg_proc_prosrc, &isnull);
        if (isnull) elog(ERROR, "[pgfunc]: null lua prosrc");
        luaL_buffinit(L, &b);

        luaL_addstring(&b,"do ");
        t = DatumGetTextP(prosrc);
        luaL_addlstring(&b, VARDATA(t), VARSIZE(t) - VARHDRSZ);
        luaL_addstring(&b, " end");
        luaL_pushresult(&b);
        s = lua_tostring(L, -1);

        ReleaseSysCache(proctup);
        clean_pgfuncinfo(lf);

        if (luaL_loadbuffer(L, s, strlen(s), "pgfunc chunk"))
                  luaL_error(L, "compile");

        lua_remove(L, -2); /*delete source element*/

        pcall_result = lua_pcall(L, 0, 1, 0);
        lua_remove(L, -2); /*delete chunk*/
        if(pcall_result == 0){
            ENDLUAV(1);
            return 1;
        }

        if( pcall_result == LUA_ERRRUN)
            luaL_error(L,"%s %s","Runtime error:",lua_tostring(L, -1));
        else if(pcall_result == LUA_ERRMEM)
            luaL_error(L,"%s %s","Memory error:",lua_tostring(L, -1));
        else if(pcall_result == LUA_ERRERR)
            luaL_error(L,"%s %s","Error:",lua_tostring(L, -1));

        return luaL_error(L, "pgfunc unknown error");
    }


    if (lf->numargs >9){
        reg_error = "not supported function with more than 9 arguments";
    }else {
        for (i = 0; i < lf->numargs; i++){
            char		argmode = lf->argmodes ? lf->argmodes[i] : PROARGMODE_IN;
            if (argmode != PROARGMODE_IN){
                reg_error = "only input parameters supported";
                break;
            }
        }
    }
    if (reg_error){
        ReleaseSysCache(proctup);
        clean_pgfuncinfo(lf);
        return luaL_error(L, "pgfunc error: %s",reg_error);
    }

    fmgr_info(funcid, &lf->fi);

    lua_pushcclosure(L, pg_callable_func, 1);

    ReleaseSysCache(proctup);

    ENDLUAV(1);
    return 1;
}

static void __newmetatable (lua_State *L, const char *tname)
{
    lua_newtable(L);
    lua_pushlightuserdata(L, (void *) tname);
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

static int gc_pg_func(lua_State *L)
{
    Lua_pgfunc *lf = lua_touserdata(L, 1);
    clean_pgfuncinfo(lf);
    return 0;
}

static luaL_Reg regs[] =
{
    {"__gc", gc_pg_func},
    { NULL, NULL }
};

void register_funcinfo_mt(lua_State *L)
{
    __newmetatable(L, pg_func_type_name);
    luaP_register(L, regs);
    lua_pop(L, 1);
    tmpcontext = get_tmpcontext();
}
