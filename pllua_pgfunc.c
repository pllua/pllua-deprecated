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

}

Note:
Set returning functions are not supported.
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

static Oid
find_lang_oids(const char* lang)
{
	HeapTuple tuple;
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

static Oid
get_pllua_oid()
{
	if (pllua_oid !=0)
		return pllua_oid;
	return find_lang_oids("pllua");
}

static Oid
get_plluau_oid()
{
	if (plluau_oid !=0)
		return plluau_oid;
	return find_lang_oids("plluau");
}

typedef struct{
	bool only_internal;
	bool throwable;
} Pgfunc_options;

typedef struct{
	Oid funcid;
	int numargs;
	Oid *argtypes;
	lua_CFunction callfunc;
	Oid prorettype;

	FmgrInfo fi;
	Pgfunc_options options;
} PgFuncInfo, Lua_pgfunc;

typedef struct{
	FmgrInfo fi;
	ExprContext econtext;
	ReturnSetInfo rsinfo;
	FunctionCallInfoData fcinfo;
	Oid prorettype;
} Lua_pgfunc_srf;

#define freeandnil(p) do{ if (p){\
	pfree(p);\
	p = NULL;\
	}}while(0)

static void
clean_pgfuncinfo(Lua_pgfunc *data)
{
	freeandnil (data->argtypes);
}

#ifdef PGFUNC_CLEANUP
static MemoryContext
get_tmpcontext()
{
	MemoryContext mc;
	mc = pg_create_context("pgfunc temporary context");
	return mc;
}
#endif

static MemoryContext tmpcontext;

#ifdef PGFUNC_CLEANUP
static int tmpcontext_usage = 0;
#endif

static int
pg_callable_func(lua_State *L)
{
	MemoryContext m;
	int i;
	FunctionCallInfoData fcinfo;
	Lua_pgfunc *fi;

#ifndef PGFUNC_CLEANUP
	tmpcontext = CurTransactionContext;
#endif

	fi = (Lua_pgfunc *) lua_touserdata(L, lua_upvalueindex(1));

	InitFunctionCallInfoData(fcinfo, &fi->fi, fi->numargs, InvalidOid, NULL, NULL);

#ifdef PGFUNC_CLEANUP
	if(tmpcontext_usage> RESET_CONTEXT_AFTER ){
		MemoryContextReset(tmpcontext);
		tmpcontext_usage = 0;
	}
	++tmpcontext_usage;
#endif

	m = MemoryContextSwitchTo(tmpcontext);

	for (i=0; i<fi->numargs; ++i){
		fcinfo.arg[i] = luaP_todatum(L, fi->argtypes[i], 0, &fcinfo.argnull[i], i+1);
	}

	if(!fi->options.only_internal && fi->options.throwable){
		SPI_push();
		PG_TRY();
		{
			Datum d = FunctionCallInvoke(&fcinfo);
			MemoryContextSwitchTo(m);
			if (fcinfo.isnull) {
				lua_pushnil(L);
			} else {
				luaP_pushdatum(L, d, fi->prorettype);
			}
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
		if (fcinfo.isnull) {
			lua_pushnil(L);
		} else {
			luaP_pushdatum(L, d, fi->prorettype);
		}
	}

	return 1;
}

static void
parse_options(lua_State *L, Pgfunc_options *opt){
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

static int pgfunc_rowsaux (lua_State *L) {
	ReturnSetInfo *rsinfo;

	FunctionCallInfoData *fcinfo;
	Datum d;
	Oid prorettype;
	Lua_pgfunc_srf *srfi;

	srfi = (Lua_pgfunc_srf *) lua_touserdata(L, lua_upvalueindex(1));

	rsinfo = &srfi->rsinfo;
	fcinfo = &srfi->fcinfo;
	prorettype = srfi->prorettype;

	d = FunctionCallInvoke(fcinfo);
	if ((!fcinfo->isnull)&&(rsinfo->isDone != ExprEndResult)){

		luaP_pushdatum(L, d, prorettype);
		return 1;
	}

	lua_pushnil(L);
	return 1;

}

static int
pgfunc_rows (lua_State *L) {
	int i;

	Lua_pgfunc *fi;

	ReturnSetInfo *rsinfo;
	ExprContext *econtext;
	FunctionCallInfoData *fcinfo;
	Lua_pgfunc_srf *srfi;
	int argc;

	BEGINLUA;

	argc = lua_gettop(L);
	fi = (Lua_pgfunc *) lua_touserdata(L, lua_upvalueindex(1));

	srfi = (Lua_pgfunc_srf *)lua_newuserdata(L, sizeof(Lua_pgfunc_srf));

	econtext = &srfi->econtext;
	rsinfo = &srfi->rsinfo;
	fcinfo = &srfi->fcinfo;
	srfi->prorettype = fi->prorettype;

	fmgr_info(fi->funcid, &srfi->fi);

	memset(econtext, 0, sizeof(ExprContext));
	econtext->ecxt_per_query_memory = CurrentMemoryContext;

	rsinfo->type = T_ReturnSetInfo;
	rsinfo->econtext = econtext;
	rsinfo->allowedModes = (int)(SFRM_ValuePerCall /*| SFRM_Materialize*/);
	rsinfo->returnMode = SFRM_ValuePerCall;//SFRM_Materialize;
	rsinfo->setResult = NULL;
	rsinfo->setDesc = NULL;

	InitFunctionCallInfoData((*fcinfo), &srfi->fi, fi->numargs, InvalidOid, NULL, (fmNodePtr)rsinfo);

	for (i=0; i<fi->numargs; ++i){
		if(i>=argc){
			for (i = argc; i<fi->numargs; ++i){
				fcinfo->arg[i] = 0 ;//Datum;
				fcinfo->argnull[i] = true;
			}
			break;
		}
		fcinfo->arg[i] = luaP_todatum(L, fi->argtypes[i], 0, &fcinfo->argnull[i], i+1);
	}

	lua_pushcclosure(L, pgfunc_rowsaux, 1);
	ENDLUAV(1);
	return 1;

}

int
get_pgfunc(lua_State *L)
{
	Lua_pgfunc *lf;
	Pgfunc_options opt;
	MemoryContext m;
	const char* reg_name = NULL;
	HeapTuple proctup;
	Form_pg_proc proc;
	int luasrc = 0;
	Oid funcid = 0;

	BEGINLUA;

#ifndef PGFUNC_CLEANUP
	tmpcontext = CurTransactionContext;
#endif

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
		m = MemoryContextSwitchTo(tmpcontext);
		PG_TRY();
		{
			funcid = DatumGetObjectId(DirectFunctionCall1(regprocedurein, CStringGetDatum(reg_name)));
		}
		PG_CATCH();{}
		PG_END_TRY();
		MemoryContextSwitchTo(m);
#ifdef PGFUNC_CLEANUP
		MemoryContextReset(tmpcontext);
#endif
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

	{
		Oid *argtypes;
		char **argnames;
		char *argmodes;
		int argc;
		MemoryContext cur = CurrentMemoryContext;

		MemoryContextSwitchTo(tmpcontext);

		argc = get_func_arg_info(proctup,
					 &argtypes, &argnames, &argmodes);

		MemoryContextSwitchTo(get_common_ctx());

		lf->numargs = argc;
		lf->argtypes = (Oid*)palloc(argc * sizeof(Oid));
		memcpy(lf->argtypes, argtypes, argc * sizeof(Oid));
		MemoryContextSwitchTo(cur);
#ifdef PGFUNC_CLEANUP
		MemoryContextReset(tmpcontext);
#endif
	}

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

	if(proc->proretset) {
		lua_pushcclosure(L, pgfunc_rows, 1);
	} else {
		fmgr_info(funcid, &lf->fi);
		lua_pushcclosure(L, pg_callable_func, 1);
	}



	ReleaseSysCache(proctup);

	ENDLUAV(1);
	return 1;
}

static void
__newmetatable (lua_State *L, const char *tname)
{
	lua_newtable(L);
	lua_pushlightuserdata(L, (void *) tname);
	lua_pushvalue(L, -2);
	lua_rawset(L, LUA_REGISTRYINDEX);
}

static int
gc_pg_func(lua_State *L)
{
	Lua_pgfunc *lf = lua_touserdata(L, 1);
	clean_pgfuncinfo(lf);
	return 0;
}

static luaL_Reg regs[] = {
	{"__gc", gc_pg_func},
	{ NULL, NULL }
};

#ifdef PGFUNC_CLEANUP
static bool cxt_initialized = false;
#endif

void
register_funcinfo_mt(lua_State *L)
{
	__newmetatable(L, pg_func_type_name);
	luaP_register(L, regs);
	lua_pop(L, 1);
#ifdef PGFUNC_CLEANUP
	if (!cxt_initialized){
		tmpcontext = get_tmpcontext();
		cxt_initialized = true;
	}
#endif
}
