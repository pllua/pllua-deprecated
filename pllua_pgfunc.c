#include "pllua_pgfunc.h"

#include "plluacommon.h"
#include "pllua.h"
#include "pllua_xact_cleanup.h"

#include <catalog/pg_language.h>
#include <lib/stringinfo.h>

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
    Oid         funcid;
    int			numargs;
    Oid		   *argtypes;
    char	  **argnames;
    char	   *argmodes;
    lua_CFunction  callfunc;
    Oid         prorettype;
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


static int call_0(lua_State *L){
    Lua_pgfunc *fi = (Lua_pgfunc *) lua_touserdata(L, lua_upvalueindex(1));
    luaP_pushdatum(L, OidFunctionCall0(fi->funcid), fi->prorettype);
    return 1;
}

#define g_datum(v) luaP_todatum(L, fi->argtypes[v], 0, &isnull, v+1)
static int call_1(lua_State *L)
{
    Lua_pgfunc *fi = (Lua_pgfunc *) lua_touserdata(L, lua_upvalueindex(1));
    bool isnull;

    luaP_pushdatum(L, OidFunctionCall1(fi->funcid,
                                       g_datum(0)), fi->prorettype);
    return 1;
}
static int call_2(lua_State *L)
{
    Lua_pgfunc *fi = (Lua_pgfunc *) lua_touserdata(L, lua_upvalueindex(1));
    bool isnull;

    luaP_pushdatum(L, OidFunctionCall2(fi->funcid,
                                       g_datum(0),
                                       g_datum(1)), fi->prorettype);
    return 1;
}
static int call_3(lua_State *L)
{
    Lua_pgfunc *fi = (Lua_pgfunc *) lua_touserdata(L, lua_upvalueindex(1));
    bool isnull;

    luaP_pushdatum(L, OidFunctionCall3(fi->funcid,
                                       g_datum(0),
                                       g_datum(1),
                                       g_datum(2)), fi->prorettype);
    return 1;
}
static int call_4(lua_State *L)
{
    Lua_pgfunc *fi = (Lua_pgfunc *) lua_touserdata(L, lua_upvalueindex(1));
    bool isnull;

    luaP_pushdatum(L, OidFunctionCall4(fi->funcid,
                                       g_datum(0),
                                       g_datum(1),
                                       g_datum(2),
                                       g_datum(3)), fi->prorettype);
    return 1;
}
static int call_5(lua_State *L)
{
    Lua_pgfunc *fi = (Lua_pgfunc *) lua_touserdata(L, lua_upvalueindex(1));
    bool isnull;

    luaP_pushdatum(L, OidFunctionCall5(fi->funcid,
                                       g_datum(0),
                                       g_datum(1),
                                       g_datum(2),
                                       g_datum(3),
                                       g_datum(4)), fi->prorettype);
    return 1;
}
static int call_6(lua_State *L)
{
    Lua_pgfunc *fi = (Lua_pgfunc *) lua_touserdata(L, lua_upvalueindex(1));
    bool isnull;

    luaP_pushdatum(L, OidFunctionCall6(fi->funcid,
                                       g_datum(0),
                                       g_datum(1),
                                       g_datum(2),
                                       g_datum(3),
                                       g_datum(4),
                                       g_datum(5)), fi->prorettype);
    return 1;
}
static int call_7(lua_State *L)
{
    Lua_pgfunc *fi = (Lua_pgfunc *) lua_touserdata(L, lua_upvalueindex(1));
    bool isnull;

    luaP_pushdatum(L, OidFunctionCall7(fi->funcid,
                                       g_datum(0),
                                       g_datum(1),
                                       g_datum(2),
                                       g_datum(3),
                                       g_datum(4),
                                       g_datum(5),
                                       g_datum(6)), fi->prorettype);
    return 1;
}
static int call_8(lua_State *L)
{
    Lua_pgfunc *fi = (Lua_pgfunc *) lua_touserdata(L, lua_upvalueindex(1));
    bool isnull;

    luaP_pushdatum(L, OidFunctionCall8(fi->funcid,
                                       g_datum(0),
                                       g_datum(1),
                                       g_datum(2),
                                       g_datum(3),
                                       g_datum(4),
                                       g_datum(5),
                                       g_datum(6),
                                       g_datum(7)), fi->prorettype);
    return 1;
}
static int call_9(lua_State *L)
{
    Lua_pgfunc *fi = (Lua_pgfunc *) lua_touserdata(L, lua_upvalueindex(1));
    bool isnull;

    luaP_pushdatum(L, OidFunctionCall9(fi->funcid,
                                       g_datum(0),
                                       g_datum(1),
                                       g_datum(2),
                                       g_datum(3),
                                       g_datum(4),
                                       g_datum(5),
                                       g_datum(6),
                                       g_datum(7),
                                       g_datum(8)), fi->prorettype);
    return 1;
}
#undef g_datum



static lua_CFunction pg_callable_funcs[] =
{
    call_0,
    call_1,
    call_2,
    call_3,
    call_4,
    call_5,
    call_6,
    call_7,
    call_8,
    call_9,
    NULL
};

#define luaP_getfield(L, s) \
    lua_pushlightuserdata((L), (void *)(s)); \
    lua_rawget((L), LUA_REGISTRYINDEX)

int get_pgfunc(lua_State *L)
{
    Lua_pgfunc *lf;
    MemoryContext mcxt;
    MemoryContext m;
    const char* reg_name;
    HeapTuple	proctup;

    Form_pg_proc proc;
    const char* reg_error = NULL;
    int i;
    int luasrc = 0;
    Oid funcid = 0;

    BEGINLUA;
    if (lua_gettop(L) != 1){
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
        return luaL_error(L,"failed to register %s", reg_name);
    }

    proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
    if (!HeapTupleIsValid(proctup)){
        return luaL_error(L,"cache lookup failed for function %d", funcid);
    }

    proc = (Form_pg_proc) GETSTRUCT(proctup);

    luasrc = ((proc->prolang == get_pllua_oid())
              || (proc->prolang == get_plluau_oid()));
    if ( (proc->prolang != INTERNALlanguageId)
            &&(proc->prolang != SQLlanguageId)
            &&(proc->prolang != ClanguageId)
            &&(!luasrc) ){
        ReleaseSysCache(proctup);
        return luaL_error(L, "supported only SQL/internal functions");
    }



    lf = (Lua_pgfunc *)lua_newuserdata(L, sizeof(Lua_pgfunc));

    //make it g/collected
    luaP_getfield(L, pg_func_type_name);
    lua_setmetatable(L, -2);


    lf->prorettype = proc->prorettype;
    lf->funcid = funcid;


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

    lua_pushcclosure(L, pg_callable_funcs[lf->numargs], 1);

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
}
