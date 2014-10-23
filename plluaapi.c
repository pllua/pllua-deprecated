/*
 * plluaapi.c: PL/Lua API
 * Author: Luis Carvalho <lexcarvalho at gmail.com>
 * Please check copyright notice at the bottom of pllua.h
 */

#include "pllua.h"
#include "rowstamp.h"

/*
 * [[ Uses of REGISTRY ]]
 * [general]
 * REG[light(L)] = memcontext
 * [type]
 * REG[oid(type)] = typeinfo
 * REG[PLLUA_TYPEINFO] = typeinfo_MT
 * REG[PLLUA_DATUM] = datum_MT
 * [trigger]
 * REG[rel_id] = desc_table
 * REG[relname] = rel_table
 * [call handler]
 * REG[light(info)] = func
 * REG[oid(func)] = info
 * REG[light(thread)] = thread
 */

/* extended function info */
typedef struct luaP_Info {
  int oid;
  int vararg;
  Oid result;
  bool result_isset;
  struct RowStamp stamp; /* detect pg_proc row changes */
  lua_State *L; /* thread for SETOF iterator */
  Oid arg[1];
} luaP_Info;

/* extended type info */
typedef struct luaP_Typeinfo {
  int oid;
  int2 len;
  char type;
  char align;
  bool byval;
  Oid elem;
  FmgrInfo input;
  FmgrInfo output;
  TupleDesc tupdesc;
} luaP_Typeinfo;

/* raw datum */
typedef struct luaP_Datum {
  int issaved;
  Datum datum;
  luaP_Typeinfo *ti;
} luaP_Datum;

static const char PLLUA_TYPEINFO[] = "typeinfo";
static const char PLLUA_DATUM[] = "datum";

#define PLLUA_LOCALVAR "_U"
#define PLLUA_SHAREDVAR "shared"
#define PLLUA_SPIVAR "server"
#define PLLUA_TRIGGERVAR "trigger"
#define PLLUA_CHUNKNAME "pllua chunk"
#define PLLUA_INIT_EXIST \
  "select 1 from pg_catalog.pg_tables where schemaname='pllua'" \
  "and tablename='init'"
#define PLLUA_INIT_LIST "select module from pllua.init"

#define MaxArraySize ((Size) (MaxAllocSize / sizeof(Datum)))

/* back compatibility to 8.2 */
#if PG_VERSION_NUM < 80300
#define SET_VARSIZE(ptr, len) VARATT_SIZEP(ptr) = len
#define att_addlength_pointer(len, typ, ptr) \
  att_addlength(len, typ, PointerGetDatum(ptr))
#define att_align_nominal(len, typ) att_align(len, typ)
#endif

#define info(msg) ereport(INFO, (errmsg("%s", msg)))
#define argerror(type) \
  elog(ERROR, "[pllua]: type '%s' (%d) not supported as argument", \
      format_type_be(type), (type))
#define resulterror(type) \
  elog(ERROR, "[pllua]: type '%s' (%d) not supported as result", \
      format_type_be(type), (type))
#define luaP_error(L, tag) \
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), \
             errmsg("[pllua]: " tag " error"), \
             errdetail("%s", lua_tostring((L), -1))))

#define datum2string(d, f) \
  DatumGetCString(DirectFunctionCall1((f), (d)))
#define text2string(d) datum2string((d), textout)

/* string2text is simpler, so we implement it here with allocation in upper
 * memory context */
static Datum string2text (const char *str) {
  int l = strlen(str);
  text *dat = (text *) SPI_palloc(l + VARHDRSZ); /* in upper context */
  SET_VARSIZE(dat, l + VARHDRSZ);
  memcpy(VARDATA(dat), str, l);
  return PointerGetDatum(dat);
}

/* copy dat to upper memory context */
static Datum datumcopy (Datum dat, luaP_Typeinfo *ti) {
  if (!ti->byval) { /* by reference? */
    Size l = datumGetSize(dat, false, ti->len);
    void *copy = SPI_palloc(l);
    memcpy(copy, DatumGetPointer(dat), l);
    return PointerGetDatum(copy);
  }
  return dat;
}

/* get MemoryContext for state L */
static MemoryContext luaP_getmemctxt (lua_State *L) {
  MemoryContext mcxt;
  lua_pushlightuserdata(L, (void *) L);
  lua_rawget(L, LUA_REGISTRYINDEX);
  mcxt = (MemoryContext) lua_touserdata(L, -1);
  lua_pop(L, 1);
  return mcxt;
}


/* ======= Type ======= */

static int luaP_typeinfogc (lua_State *L) {
  luaP_Typeinfo *ti = lua_touserdata(L, 1);
  if (ti->tupdesc) FreeTupleDesc(ti->tupdesc);
  return 0;
}

static luaP_Typeinfo *luaP_gettypeinfo (lua_State *L, int oid) {
  luaP_Typeinfo *ti;
  lua_pushinteger(L, oid);
  lua_rawget(L, LUA_REGISTRYINDEX);
  if (lua_isnil(L, -1)) { /* not cached? */
    HeapTuple type;
    Form_pg_type typeinfo;
    MemoryContext mcxt = luaP_getmemctxt(L);
    /* query system */
    type = SearchSysCache(TYPEOID, ObjectIdGetDatum(oid), 0, 0, 0);
    if (!HeapTupleIsValid(type))
      elog(ERROR, "[pllua]: cache lookup failed for type %u", oid);
    typeinfo = (Form_pg_type) GETSTRUCT(type);
    /* cache */
    ti = lua_newuserdata(L, sizeof(luaP_Typeinfo));
    ti->len = typeinfo->typlen;
    ti->type = typeinfo->typtype;
    ti->align = typeinfo->typalign;
    ti->byval = typeinfo->typbyval;
    ti->elem = typeinfo->typelem;
    fmgr_info_cxt(typeinfo->typinput, &ti->input, mcxt);
    fmgr_info_cxt(typeinfo->typoutput, &ti->output, mcxt);
    ti->tupdesc = NULL;
    if (ti->type == 'c') { /* complex? */
      TupleDesc td = lookup_rowtype_tupdesc(oid, typeinfo->typtypmod);
      MemoryContext m = MemoryContextSwitchTo(mcxt);
      ti->tupdesc = CreateTupleDescCopyConstr(td);
      MemoryContextSwitchTo(m);
      BlessTupleDesc(ti->tupdesc);
      ReleaseTupleDesc(td);
    }
    ReleaseSysCache(type);
    lua_pushlightuserdata(L, (void *) PLLUA_TYPEINFO);
    lua_rawget(L, LUA_REGISTRYINDEX); /* Typeinfo_MT */
    lua_setmetatable(L, -2);
    lua_pushinteger(L, oid);
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX); /* REG[oid] = typeinfo */
    lua_pop(L, 2); /* nil and typeinfo */
  }
  else {
    ti = lua_touserdata(L, -1);
    lua_pop(L, 1);
  }
  return ti;
}

/* ======= Datum ======= */

static int luaP_datumtostring (lua_State *L) {
  luaP_Datum *d = lua_touserdata(L, 1);
  lua_pushstring(L, OutputFunctionCall(&d->ti->output, d->datum));
  return 1;
}

static int luaP_datumgc (lua_State *L) {
  luaP_Datum *d = lua_touserdata(L, 1);
  if (d->issaved) pfree(DatumGetPointer(d->datum));
  return 0;
}

static int luaP_datumsave (lua_State *L) {
  luaP_Datum *d = luaP_toudata(L, 1, PLLUA_DATUM);
  if (d == NULL) {
    const char *msg = lua_pushfstring(L, "%s expected, got %s",
        PLLUA_DATUM, luaL_typename(L, 1));
    luaL_argerror(L, 1, msg);
  }
  if (!d->ti->byval) { /* by reference? */
    Size l = datumGetSize(d->datum, false, d->ti->len);
    MemoryContext mcxt = luaP_getmemctxt(L);
    MemoryContext m = MemoryContextSwitchTo(mcxt);
    Pointer copy = palloc(l);
    Pointer dp = DatumGetPointer(d->datum);
    memcpy(copy, dp, l);
    MemoryContextSwitchTo(m);
    pfree(dp);
    d->issaved = 1;
    d->datum = PointerGetDatum(copy);
  }
  return 1;
}

static luaP_Datum *luaP_pushrawdatum (lua_State *L, Datum dat,
    luaP_Typeinfo *ti) {
  luaP_Datum *d = lua_newuserdata(L, sizeof(luaP_Datum));
  d->issaved = 0;
  d->datum = dat;
  d->ti = ti;
  lua_pushlightuserdata(L, (void *) PLLUA_DATUM);
  lua_rawget(L, LUA_REGISTRYINDEX); /* Datum_MT */
  lua_setmetatable(L, -2);
  return d;
}

/* ======= Trigger ======= */

static void luaP_preptrigger (lua_State *L, TriggerData *tdata) {
  const char *relname;
  lua_pushglobaltable(L);
  lua_pushstring(L, PLLUA_TRIGGERVAR);
  lua_newtable(L);
  /* when */
  if (TRIGGER_FIRED_BEFORE(tdata->tg_event))
    lua_pushstring(L, "before");
  else if (TRIGGER_FIRED_AFTER(tdata->tg_event))
    lua_pushstring(L, "after");
  else
    elog(ERROR, "[pllua]: unknown trigger 'when' event");
  lua_setfield(L, -2, "when");
  /* level */
  if (TRIGGER_FIRED_FOR_ROW(tdata->tg_event))
    lua_pushstring(L, "row");
  else if (TRIGGER_FIRED_FOR_STATEMENT(tdata->tg_event))
    lua_pushstring(L, "statement");
  else
    elog(ERROR, "[pllua]: unknown trigger 'level' event");
  lua_setfield(L, -2, "level");
  /* operation */
  if (TRIGGER_FIRED_BY_INSERT(tdata->tg_event))
    lua_pushstring(L, "insert");
  else if (TRIGGER_FIRED_BY_UPDATE(tdata->tg_event))
    lua_pushstring(L, "update");
  else if (TRIGGER_FIRED_BY_DELETE(tdata->tg_event))
    lua_pushstring(L, "delete");
#if PG_VERSION_NUM >= 80400
  else if (TRIGGER_FIRED_BY_TRUNCATE(tdata->tg_event))
    lua_pushstring(L, "truncate");
#endif
  else
    elog(ERROR, "[pllua]: unknown trigger 'operation' event");
  lua_setfield(L, -2, "operation");
  /* relation (name) */
  relname = NameStr(tdata->tg_relation->rd_rel->relname);
  lua_getfield(L, LUA_REGISTRYINDEX, relname);
  if (lua_isnil(L, -1)) { /* not cached? */
    lua_pop(L, 1);
    lua_createtable(L, 0, 2);
    lua_pushstring(L, relname);
    lua_setfield(L, -2, "name");
    luaP_pushdesctable(L, tdata->tg_relation->rd_att);
    lua_pushinteger(L, (int) tdata->tg_relation->rd_id);
    lua_pushvalue(L, -2); /* attribute table */
    lua_rawset(L, LUA_REGISTRYINDEX); /* cache desc */
    lua_setfield(L, -2, "attributes");
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, relname); /* cache relation */
  }
  lua_setfield(L, -2, "relation");
  /* row */
  if (TRIGGER_FIRED_FOR_ROW(tdata->tg_event)) {
    if (TRIGGER_FIRED_BY_UPDATE(tdata->tg_event)) {
      luaP_pushtuple(L, tdata->tg_relation->rd_att, tdata->tg_newtuple,
          tdata->tg_relation->rd_id, 0);
      lua_setfield(L, -2, "row"); /* new row */
      luaP_pushtuple(L, tdata->tg_relation->rd_att, tdata->tg_trigtuple,
          tdata->tg_relation->rd_id, 1);
      lua_setfield(L, -2, "old"); /* old row */
    }
    else { /* insert or delete */
      luaP_pushtuple(L, tdata->tg_relation->rd_att, tdata->tg_trigtuple,
          tdata->tg_relation->rd_id, 0);
      lua_setfield(L, -2, "row"); /* old row */
    }
  }
  /* trigger name */
  lua_pushstring(L, tdata->tg_trigger->tgname);
  lua_setfield(L, -2, "name");
  /* done setting up trigger; now set global */
  lua_rawset(L, -3); /* _G[PLLUA_TRIGGERVAR] = table */
  lua_pop(L, 1); /* _G */
}

static Datum luaP_gettriggerresult (lua_State *L) {
  HeapTuple tuple;
  lua_getglobal(L, PLLUA_TRIGGERVAR);
  lua_getfield(L, -1, "row");
  tuple = luaP_totuple(L);
  lua_pop(L, 2);
  return PointerGetDatum(tuple);
}

static void luaP_cleantrigger (lua_State *L) {
  lua_pushglobaltable(L);
  lua_pushstring(L, PLLUA_TRIGGERVAR);
  lua_pushnil(L);
  lua_rawset(L, -3);
  lua_pop(L, 1); /* _G */
}

/* ======= luaP_newstate: create a new Lua VM ======= */

static int luaP_modinit (lua_State *L) {
  int status;
  if (SPI_connect() != SPI_OK_CONNECT)
    elog(ERROR, "[pllua]: could not connect to SPI manager");
  status = SPI_execute(PLLUA_INIT_EXIST, 1, 0);
  if (status < 0)
    lua_pushfstring(L, "[pllua]: error reading pllua.init: %d", status);
  if (SPI_processed == 0) /* pllua.init does not exist? */
    status = 0;
  else {
    status = SPI_execute(PLLUA_INIT_LIST, 1, 0);
    if (status < 0)
      lua_pushfstring(L, "[pllua]: error loading modules from pllua.init: %d",
          status);
    else {
      status = 0;
      if (SPI_processed > 0) { /* any rows? */
        int i;
        for (i = 0; i < SPI_processed; i++) {
          bool isnull;
          /* push module name */
          lua_pushstring(L, text2string(heap_getattr(SPI_tuptable->vals[i],
              1, SPI_tuptable->tupdesc, &isnull))); /* first column */
          lua_getglobal(L, "require");
          lua_pushvalue(L, -2); /* module name */
          status = lua_pcall(L, 1, 1, 0);
          if (status) break; /* error? */
          if (!lua_isnil(L, -1)) { /* make sure global table is set? */
            lua_pushglobaltable(L);
            lua_pushvalue(L, -3); /* key */
            lua_pushvalue(L, -3); /* value */
            lua_rawset(L, -3); /* _G[key] = value */
            lua_pop(L, 1); /* _G */
          }
          else
            lua_pop(L, 1); /* module name */
        }
      }
    }
  }
  if (SPI_finish() != SPI_OK_FINISH)
    elog(ERROR, "[pllua]: could not disconnect from SPI manager");
  return status;
}

static int luaP_globalnewindex (lua_State *L) {
  return luaL_error(L, "attempt to set global var '%s'",
      lua_tostring(L, -2));
}

static int luaP_setshared (lua_State *L) {
  luaL_checkstring(L, 1);
  if (lua_gettop(L) == 1) lua_pushboolean(L, 1);
  lua_settop(L, 2); /* key, value */
  lua_pushvalue(L, -1);
  lua_insert(L, -3);
  lua_pushglobaltable(L);
  lua_insert(L, -3);
  lua_rawset(L, -3);
  lua_pop(L, 1); /* global table */
  return 1;
}

static int luaP_print (lua_State *L) {
  int i, n = lua_gettop(L); /* nargs */
  const char *s;
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  lua_getglobal(L, "tostring");
  for (i = 1; i <= n; i++) {
    lua_pushvalue(L, -1); /* tostring */
    lua_pushvalue(L, i); /* arg */
    lua_call(L, 1, 1);
    s = lua_tostring(L, -1);
    if (s == NULL)
      return luaL_error(L, "cannot convert to string");
    if (i > 1) luaL_addchar(&b, '\t');
    luaL_addlstring(&b, s, strlen(s));
    lua_pop(L, 1);
  }
  luaL_pushresult(&b);
  s = lua_tostring(L, -1);
  info(s);
  lua_pop(L, 1);
  return 0;
}

static int luaP_info (lua_State *L) {
  luaL_checkstring(L, 1);
  info(lua_tostring(L, 1));
  return 0;
}

static int luaP_log (lua_State *L) {
  luaL_checkstring(L, 1);
  ereport(LOG, (errmsg("%s", lua_tostring(L, 1))));
  return 0;
}

static int luaP_notice (lua_State *L) {
  luaL_checkstring(L, 1);
  ereport(NOTICE, (errmsg("%s", lua_tostring(L, 1))));
  return 0;
}

static int luaP_warning (lua_State *L) {
  luaL_checkstring(L, 1);
  ereport(WARNING, (errmsg("%s", lua_tostring(L, 1))));
  return 0;
}

static int luaP_fromstring (lua_State *L) {
  int oid = luaP_gettypeoid(luaL_checkstring(L, 1));
  const char *s = luaL_checkstring(L, 2);
  luaP_Typeinfo *ti = luaP_gettypeinfo(L, oid);
  int inoid = oid;
  Datum v;
  /* from getTypeIOParam in lsyscache.c */
  if (ti->type == 'b' && OidIsValid(ti->elem)) inoid = ti->elem;
  v = InputFunctionCall(&ti->input, (char *) s, inoid, 0); /* typmod = 0 */
  luaP_pushdatum(L, v, oid);
  return 1;
}

static const luaL_Reg luaP_funcs[] = {
  {"setshared", luaP_setshared},
  {"log", luaP_log},
  {"print", luaP_print},
  {"info", luaP_info},
  {"notice", luaP_notice},
  {"warning", luaP_warning},
  {"fromstring", luaP_fromstring},
  {NULL, NULL}
};

void luaP_close (lua_State *L) {
  MemoryContext mcxt = luaP_getmemctxt(L);
  MemoryContextDelete(mcxt);
  lua_close(L);
}

lua_State *luaP_newstate (int trusted) {
  int status;
  MemoryContext mcxt = AllocSetContextCreate(TopMemoryContext,
      "PL/Lua context", ALLOCSET_DEFAULT_MINSIZE, ALLOCSET_DEFAULT_INITSIZE,
      ALLOCSET_DEFAULT_MAXSIZE);
  lua_State *L = luaL_newstate();
  /* version */
  lua_pushliteral(L, PLLUA_VERSION);
  lua_setglobal(L, "_PLVERSION");
  /* memory context */
  lua_pushlightuserdata(L, (void *) L);
  lua_pushlightuserdata(L, (void *) mcxt);
  lua_rawset(L, LUA_REGISTRYINDEX);
  /* core libs */
  if (trusted) {
    const luaL_Reg luaP_trusted_libs[] = {
#if LUA_VERSION_NUM <= 501
      {"", luaopen_base},
#else
      {"_G", luaopen_base},
      {LUA_COLIBNAME, luaopen_coroutine},
      {LUA_BITLIBNAME, luaopen_bit32},
#endif
      {LUA_TABLIBNAME, luaopen_table},
      {LUA_STRLIBNAME, luaopen_string},
      {LUA_MATHLIBNAME, luaopen_math},
      {LUA_OSLIBNAME, luaopen_os}, /* restricted */
      {LUA_LOADLIBNAME, luaopen_package}, /* just for pllua.init modules */
      {NULL, NULL}
    };
    const char *os_funcs[] = {"date", "clock", "time", "difftime", NULL};
    const luaL_Reg *reg = luaP_trusted_libs;
    const char **s = os_funcs;
    for (; reg->func; reg++) {
#if LUA_VERSION_NUM <= 501
      lua_pushcfunction(L, reg->func);
      lua_pushstring(L, reg->name);
      lua_call(L, 1, 0);
#else
      luaL_requiref(L, reg->name, reg->func, 1); /* set in global table */
      lua_pop(L, 1); /* remove lib */
#endif
    }
    /* restricted os lib */
    lua_getglobal(L, LUA_OSLIBNAME);
    lua_newtable(L); /* new os */
    for (; *s; s++) {
      lua_getfield(L, -2, *s);
      lua_setfield(L, -2, *s);
    }
    lua_setglobal(L, LUA_OSLIBNAME);
    lua_pop(L, 1);
  }
  else
    luaL_openlibs(L);
  /* setup typeinfo and raw datum MTs */
  lua_pushlightuserdata(L, (void *) PLLUA_TYPEINFO);
  lua_newtable(L); /* luaP_Typeinfo MT */
  lua_pushcfunction(L, luaP_typeinfogc);
  lua_setfield(L, -2, "__gc");
  lua_rawset(L, LUA_REGISTRYINDEX);
  lua_pushlightuserdata(L, (void *) PLLUA_DATUM);
  lua_newtable(L); /* luaP_Datum MT */
  lua_pushcfunction(L, luaP_datumtostring);
  lua_setfield(L, -2, "__tostring");
  lua_pushcfunction(L, luaP_datumgc);
  lua_setfield(L, -2, "__gc");
  lua_createtable(L, 0, 1);
  lua_pushcfunction(L, luaP_datumsave);
  lua_setfield(L, -2, "save");
  lua_setfield(L, -2, "__index");
  lua_rawset(L, LUA_REGISTRYINDEX);
  /* load pllua.init modules */
  status = luaP_modinit(L);
  if (status != 0) /* SPI or module loading error? */
    elog(ERROR, "%s", lua_tostring(L, -1));
  /* set alias for _G */
  lua_pushglobaltable(L);
  lua_setglobal(L, PLLUA_SHAREDVAR); /* _G.shared = _G */
  /* globals */
  lua_pushglobaltable(L);
  luaP_register(L, luaP_funcs);
  lua_pop(L, 1);
  /* SPI */
  luaP_registerspi(L);
  lua_setglobal(L, PLLUA_SPIVAR);
  if (trusted) {
    const char *package_keys[] = { /* to be removed */
      "preload", "loadlib", "loaders", "seeall", NULL};
    const char *global_keys[] = { /* to be removed */
      "require", "module", "dofile", "loadfile", NULL};
    const char **s;
    /* clean package module */
    lua_getglobal(L, "package");
    for (s = package_keys; *s; s++) {
      lua_pushnil(L);
      lua_setfield(L, -2, *s);
    }
    lua_pop(L, 1); /* package table */
    /* clean global table */
    for (s = global_keys; *s; s++) {
      lua_pushnil(L);
      lua_setglobal(L, *s);
    }
    /* set _G as read-only */
    lua_pushglobaltable(L);
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, luaP_globalnewindex);
    lua_setfield(L, -2, "__newindex");
    lua_pushvalue(L, -1); /* metatable */
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, -2);
    lua_pop(L, 1); /* _G */
  }
  return L;
}


/* ======= luaP_pushfunction ======= */

static luaP_Info *luaP_newinfo (lua_State *L, int nargs, int oid,
    Form_pg_proc procst) {
  Oid *argtype = procst->proargtypes.values;
  Oid rettype = procst->prorettype;
  bool isset = procst->proretset;
  luaP_Info *fi;
  int i;
  luaP_Typeinfo *ti;
  fi = lua_newuserdata(L, sizeof(luaP_Info) + nargs * sizeof(Oid));
  fi->oid = oid;
  /* read arg types */
  for (i = 0; i < nargs; i++) {
    ti = luaP_gettypeinfo(L, argtype[i]);
    if (ti->type == 'p') /* pseudo-type? */
      ereport(ERROR,
          (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
           errmsg("[pllua]: functions cannot take type '%s'",
          format_type_be(argtype[i]))));
    fi->arg[i] = argtype[i];
  }
  /* read result type */
  ti = luaP_gettypeinfo(L, rettype);
  if (ti->type == 'p' && rettype != VOIDOID && rettype != TRIGGEROID) 
    ereport(ERROR,
        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
         errmsg("[pllua]: functions cannot return type '%s'",
           format_type_be(rettype))));
  fi->vararg = rettype == TRIGGEROID; /* triggers are vararg */
  fi->result = rettype;
  fi->result_isset = isset;
  fi->L = NULL;
  return fi;
}

/* test argument and return types, compile function, store it at registry,
 * and return info at the top of stack  */
static void luaP_newfunction (lua_State *L, int oid, HeapTuple proc,
    luaP_Info **fi) {
  int nargs; /* fcinfo->nargs */
  Form_pg_proc procst;
  bool isnull;
  Datum prosrc, *argname;
  const char *s, *fname;
  text *t;
  luaL_Buffer b;
  int init = (*fi == NULL); /* not initialized? */
  /* read proc info */
  procst = (Form_pg_proc) GETSTRUCT(proc);
  prosrc = SysCacheGetAttr(PROCOID, proc, Anum_pg_proc_prosrc, &isnull);
  if (isnull) elog(ERROR, "[pllua]: null prosrc");
  nargs = procst->pronargs;
  /* get info userdata */
  if (init) {
    lua_pushinteger(L, oid);
    *fi = luaP_newinfo(L, nargs, oid, procst);
  }
  lua_pushlightuserdata(L, (void *) *fi);
  /* check #argnames */
  if (nargs > 0) {
    int nnames;
    Datum argnames = SysCacheGetAttr(PROCOID, proc,
        Anum_pg_proc_proargnames, &isnull);
    if (!isnull)
      deconstruct_array(DatumGetArrayTypeP(argnames), TEXTOID, -1, false,
          'i', &argname, NULL, &nnames);
    if (nnames != nargs)
      (*fi)->vararg = 1;
    else { /* check empty names */
      int i;
      for (i = 0; i < nnames && !(*fi)->vararg; i++) {
        if (VARSIZE(DatumGetTextP(argname[i])) == VARHDRSZ) /* empty? */
          (*fi)->vararg = 1;
      }
    }
  }
  /* prepare buffer */
  luaL_buffinit(L, &b);
  /* read func name */
  fname = NameStr(procst->proname);
  /* prepare header: "local upvalue,f f=function(" */
  luaL_addlstring(&b, "local " PLLUA_LOCALVAR ",",
      6 + sizeof(PLLUA_LOCALVAR));
  luaL_addlstring(&b, fname, strlen(fname));
  luaL_addchar(&b, ' ');
  luaL_addlstring(&b, fname, strlen(fname));
  luaL_addlstring(&b, "=function(", 10);
  /* read arg names */
  if ((*fi)->vararg) luaL_addlstring(&b, "...", 3);
  else {
    int i;
    for (i = 0; i < nargs; i++) {
      if (i > 0) luaL_addchar(&b, ',');
      t = DatumGetTextP(argname[i]);
      luaL_addlstring(&b, VARDATA(t), VARSIZE(t) - VARHDRSZ);
    }
  }
  luaL_addlstring(&b, ") ", 2);
  /* read source */
  t = DatumGetTextP(prosrc);
  luaL_addlstring(&b, VARDATA(t), VARSIZE(t) - VARHDRSZ);
  /* prepare footer: " end return f" */
  luaL_addlstring(&b, " end return ", 12);
  luaL_addlstring(&b, fname, strlen(fname));
  /* create function */
  luaL_pushresult(&b);
  s = lua_tostring(L, -1);
  if (luaL_loadbuffer(L, s, strlen(s), PLLUA_CHUNKNAME))
    luaP_error(L, "compile");
  lua_remove(L, -2); /* source */
  if (lua_pcall(L, 0, 1, 0)) luaP_error(L, "call");
  rowstamp_set(&(*fi)->stamp, proc); /* row-stamp info */
  lua_pushvalue(L, -1); /* func */
  if (init) {
    lua_insert(L, -5);
    lua_rawset(L, LUA_REGISTRYINDEX); /* REG[light_info] = func */
    lua_rawset(L, LUA_REGISTRYINDEX); /* REG[oid] = info */
  }
  else {
    lua_insert(L, -3);
    lua_rawset(L, LUA_REGISTRYINDEX); /* REG[light_info] = func */
  }
}

/* leaves function info (fi) for oid in stack */
static luaP_Info *luaP_pushfunction (lua_State *L, int oid) {
  luaP_Info *fi = NULL;
  HeapTuple proc;
  proc = SearchSysCache(PROCOID, ObjectIdGetDatum((Oid) oid), 0, 0, 0);
  if (!HeapTupleIsValid(proc))
    elog(ERROR, "[pllua]: cache lookup failed for function %u", (Oid) oid);
  lua_pushinteger(L, oid);
  lua_rawget(L, LUA_REGISTRYINDEX);
  if (lua_isnil(L, -1)) { /* not interned? */
    lua_pop(L, 1); /* nil */
    luaP_newfunction(L, oid, proc, &fi);
  }
  else {
    fi = lua_touserdata(L, -1);
    lua_pop(L, 1); /* info udata */
    lua_pushlightuserdata(L, (void *) fi);
    if (rowstamp_check(&fi->stamp, proc)) /* not replaced? */
      lua_rawget(L, LUA_REGISTRYINDEX);
    else {
      lua_pushnil(L);
      lua_rawset(L, LUA_REGISTRYINDEX); /* REG[old_light_info] = nil */
      luaP_newfunction(L, oid, proc, &fi);
    }
  }
  ReleaseSysCache(proc);
  return fi;
}


/* ======= luaP_pushargs ======= */

static void luaP_pusharray (lua_State *L, char **p, int ndims,
    int *dims, int *lb, bits8 **bitmap, int *bitmask,
    luaP_Typeinfo *ti, Oid typeelem) {
  int i;
  lua_newtable(L);
  if (ndims == 1) { /* vector? */
    for (i = 0; i < (*dims); i++) {
      if (*bitmap == NULL || ((**bitmap) & (*bitmask)) != 0) { /* not NULL? */
        luaP_pushdatum(L, fetch_att(*p, ti->byval, ti->len), typeelem);
        lua_rawseti(L, -2, (*lb) + i);
        *p = att_addlength_pointer(*p, ti->len, *p);
        *p = (char *) att_align_nominal(*p, ti->align);
      }
      if (*bitmap) { /* advance bitmap pointer? */
        *bitmask <<= 1;
        if (*bitmask == 0x100) {
          (*bitmap)++;
          *bitmask = 1;
        }
      }
    }
  }
  else { /* multidimensional array */
    for (i = 0; i < (*dims); i++) {
      luaP_pusharray(L, p, ndims - 1, dims + 1, lb + 1, bitmap, bitmask,
          ti, typeelem);
      lua_rawseti(L, -2, (*lb) + i);
    }
  }
}

void luaP_pushdatum (lua_State *L, Datum dat, Oid type) {
  switch (type) {
    /* base and domain types */
    case BOOLOID:
      lua_pushboolean(L, (int) (dat != 0));
      break;
    case FLOAT4OID:
      lua_pushnumber(L, (lua_Number) DatumGetFloat4(dat));
      break;
    case FLOAT8OID:
      lua_pushnumber(L, (lua_Number) DatumGetFloat8(dat));
      break;
    case INT2OID:
      lua_pushinteger(L, (lua_Integer) DatumGetInt16(dat));
      break;
    case INT4OID:
      lua_pushinteger(L, (lua_Integer) DatumGetInt32(dat));
      break;
    case TEXTOID:
      lua_pushstring(L, text2string(dat));
      break;
    case BPCHAROID:
      lua_pushstring(L, datum2string(dat, bpcharout));
      break;
    case VARCHAROID:
      lua_pushstring(L, datum2string(dat, varcharout));
      break;
    case REFCURSOROID: {
      Portal cursor = SPI_cursor_find(text2string(dat));
      if (cursor != NULL) luaP_pushcursor(L, cursor);
      else lua_pushnil(L);
      break;
    }
    default: {
      luaP_Typeinfo *ti = luaP_gettypeinfo(L, type);
      switch (ti->type) {
        case 'c': { /* complex? */
          HeapTupleHeader tup = DatumGetHeapTupleHeader(dat);
          int i;
          const char *key;
          bool isnull;
          Datum value;
          lua_createtable(L, 0, ti->tupdesc->natts);
          for (i = 0; i < ti->tupdesc->natts; i++) {
            Form_pg_attribute att = ti->tupdesc->attrs[i];
            key = NameStr(att->attname);
            value = GetAttributeByNum(tup, att->attnum, &isnull);
            if (!isnull) {
              luaP_pushdatum(L, value, att->atttypid);
              lua_setfield(L, -2, key);
            }
          }
          break;
        }
        case 'p': /* pseudo? */
          if (type != VOIDOID) argerror(type);
          break;
        case 'b': /* base? */
        case 'd': /* domain? */
          if (ti->elem != 0 && ti->len == -1) { /* array? */
            ArrayType *arr = DatumGetArrayTypeP(dat);
            char *p = ARR_DATA_PTR(arr);
            bits8 *bitmap = ARR_NULLBITMAP(arr);
            int bitmask = 1;
            luaP_Typeinfo *te = luaP_gettypeinfo(L, ti->elem);
            luaP_pusharray(L, &p, ARR_NDIM(arr), ARR_DIMS(arr), ARR_LBOUND(arr),
                &bitmap, &bitmask, te, ti->elem);
          }
          else
            luaP_pushrawdatum(L, dat, ti);
          break;
#if PG_VERSION_NUM >= 80300
        case 'e': /* enum? */
          lua_pushinteger(L, (lua_Integer) DatumGetInt32(dat)); /* 4-byte long */
          break;
#endif
        default:
          argerror(type);
      }
    }
  }
}

static void luaP_pushargs (lua_State *L, FunctionCallInfo fcinfo,
    luaP_Info *fi) {
  int i;
  for (i = 0; i < fcinfo->nargs; i++) {
    if (fcinfo->argnull[i]) lua_pushnil(L);
    else luaP_pushdatum(L, fcinfo->arg[i], fi->arg[i]);
  }
}


/* ======= luaP_getresult ======= */

/* assume table is in top, returns size */
static int luaP_getarraydims (lua_State *L, int *ndims, int *dims,
    int *lb, luaP_Typeinfo *ti, Oid typeelem, int typmod, bool *hasnulls) {
  int size = 0;
  int nitems = 0;
  *ndims = -1;
  *hasnulls = false;
  lua_pushnil(L);
  while (lua_next(L, -2)) {
    if (lua_isnumber(L, -2)) {
      int n;
      int k = lua_tointeger(L, -2);
      /* set dims and lb */
      if (*ndims < 0) { /* first visit? */
        *lb = k;
        *dims = 1;
      }
      else {
        if (*lb > k) {
          *dims += *lb - k;
          *lb = k;
        }
        if (*dims - 1 + *lb < k) /* max < k? */
          *dims = k - *lb + 1;
      }
      /* set ndims */
      if (lua_type(L, -1) == LUA_TTABLE) {
        int d = -1, l = -1;
        if (*ndims == MAXDIM)
          elog(ERROR, "[pllua]: table exceeds max number of dimensions");
        if (*ndims > 1) {
          d = dims[1]; l = lb[1];
        }
        size += luaP_getarraydims(L, &n, dims + 1, lb + 1, ti, typeelem,
            typmod, hasnulls);
        if (*ndims > 1) { /* update dims and bounds? */
          if (l < lb[1]) {
            lb[1] = l;
            *hasnulls = true;
          }
          if (d + l > dims[1] + lb[1]) {
            dims[1] = d + l - lb[1];
            *hasnulls = true;
          }
        }
      }
      else {
        bool isnull;
        Datum d = luaP_todatum(L, typeelem, typmod, &isnull);
        Pointer v = DatumGetPointer(d);
        n = 0;
        if (ti->len == -1) /* varlena? */
          v = (Pointer) PG_DETOAST_DATUM(d);
        size = att_addlength_pointer(size, ti->len, v);
        size = att_align_nominal(size, ti->align);
        if (size > MaxAllocSize)
          elog(ERROR, "[pllua]: array size exceeds the maximum allowed");
      }
      n++;
      if (*ndims < 0) *ndims = n;
      else if (*ndims != n) elog(ERROR, "[pllua]: table is asymetric");
    }
    nitems++;
    lua_pop(L, 1);
  }
  if (!(*hasnulls)) *hasnulls = (nitems > 0 && nitems != *dims);
  return size;
}

static void luaP_toarray (lua_State *L, char **p, int ndims,
    int *dims, int *lb, bits8 **bitmap, int *bitmask, int *bitval,
    luaP_Typeinfo *ti, Oid typeelem, int typmod) {
  int i;
  bool isnull;
  if (ndims == 1) { /* vector? */
    for (i = 0; i < (*dims); i++) {
      Pointer v;
      lua_rawgeti(L, -1, (*lb) + i);
      v = DatumGetPointer(luaP_todatum(L, typeelem, typmod, &isnull));
      if (!isnull) {
        *bitval |= *bitmask;
        if (ti->len > 0) {
          if (ti->byval)
            store_att_byval(*p, PointerGetDatum(v), ti->len);
          else
            memmove(*p, v, ti->len);
          *p += att_align_nominal(ti->len, ti->align);
        }
        else {
          int inc;
          Assert(!ti->byval);
          inc = att_addlength_pointer(0, ti->len, v);
          memmove(*p, v, inc);
          *p += att_align_nominal(inc, ti->align);
        }
        if (!ti->byval) pfree(v);
      }
      else
        if (!(*bitmap))
          elog(ERROR, "[pllua]: no support for null elements");
      if (*bitmap) {
        *bitmask <<= 1;
        if (*bitmask == 0x100) {
          *(*bitmap)++ = *bitval;
          *bitval = 0;
          *bitmask = 1;
        }
      }
      lua_pop(L, 1);
    }
    if (*bitmap && *bitmask != 1) **bitmap = *bitval;
  }
  else { /* multidimensional array */
    for (i = 0; i < (*dims); i++) {
      lua_rawgeti(L, -1, (*lb) + i);
      luaP_toarray(L, p, ndims - 1, dims + 1, lb + 1, bitmap,
          bitmask, bitval, ti, typeelem, typmod);
      lua_pop(L, 1);
    }
  }
}

Datum luaP_todatum (lua_State *L, Oid type, int typmod, bool *isnull) {
  Datum dat = 0; /* NULL */
  *isnull = lua_isnil(L, -1);
  if (!(*isnull || type == VOIDOID)) {
    switch (type) {
      /* base and domain types */
      case BOOLOID:
        dat = BoolGetDatum(lua_toboolean(L, -1));
        break;
      case FLOAT4OID:
        dat = Float4GetDatum((float4) lua_tonumber(L, -1));
        break;
      case FLOAT8OID:
        dat = Float8GetDatum((float8) lua_tonumber(L, -1));
        break;
      case INT2OID:
        dat = Int16GetDatum(lua_tointeger(L, -1));
        break;
      case INT4OID:
        dat = Int32GetDatum(lua_tointeger(L, -1));
        break;
      case TEXTOID: {
        const char *s = lua_tostring(L, -1);
        if (s == NULL) elog(ERROR,
            "[pllua]: string expected for datum conversion, got %s",
            lua_typename(L, lua_type(L, -1)));
        dat = string2text(s);
        break;
      }
      case REFCURSOROID: {
        Portal cursor = luaP_tocursor(L, -1);
        dat = string2text(cursor->name);
        break;
      }
      default: {
        luaP_Typeinfo *ti = luaP_gettypeinfo(L, type);
        switch (ti->type) {
          case 'c': /* complex? */
            if (lua_type(L, -1) == LUA_TTABLE) {
              int i;
              luaP_Buffer *b;
              if (lua_type(L, -1) != LUA_TTABLE)
                elog(ERROR, "[pllua]: table expected for record result, got %s",
                    lua_typename(L, lua_type(L, -1)));
              /* create tuple */
              b = luaP_getbuffer(L, ti->tupdesc->natts);
              for (i = 0; i < ti->tupdesc->natts; i++) {
                lua_getfield(L, -1, NameStr(ti->tupdesc->attrs[i]->attname));
                /* only simple types allowed in record */
                b->value[i] = luaP_todatum(L, ti->tupdesc->attrs[i]->atttypid,
                    ti->tupdesc->attrs[i]->atttypmod, b->null + i);
                lua_pop(L, 1);
              }
              /* make copy in upper executor memory context */
              dat = PointerGetDatum(SPI_returntuple(heap_form_tuple(ti->tupdesc,
                      b->value, b->null), ti->tupdesc));
            }
            else { /* tuple */
              HeapTuple tuple = luaP_casttuple(L, ti->tupdesc);
              if (tuple == NULL)
                elog(ERROR,
                    "[pllua]: table or tuple expected for record result, got %s",
                    lua_typename(L, lua_type(L, -1)));
              dat = PointerGetDatum(SPI_returntuple(tuple, ti->tupdesc));
            }
            break;
          case 'b': /* base? */
          case 'd': /* domain? */
            if (ti->elem != 0 && ti->len == -1) { /* array? */
              luaP_Typeinfo *te;
              int ndims, dims[MAXDIM], lb[MAXDIM];
              int i, size;
              bool hasnulls;
              ArrayType *a;
              if (lua_type(L, -1) != LUA_TTABLE)
                elog(ERROR,
                    "[pllua]: table expected for array conversion, got %s",
                    lua_typename(L, lua_type(L, -1)));
              te = luaP_gettypeinfo(L, ti->elem);
              for (i = 0; i < MAXDIM; i++) dims[i] = lb[i] = -1;
              size = luaP_getarraydims(L, &ndims, dims, lb, te, ti->elem,
                  typmod, &hasnulls);
              if (size == 0) { /* empty array? */
                a = (ArrayType *) SPI_palloc(sizeof(ArrayType));
                SET_VARSIZE(a, sizeof(ArrayType));
                a->ndim = 0;
                a->dataoffset = 0;
                a->elemtype = ti->elem;
              }
              else {
                int nitems = 1;
                int offset;
                char *p;
                bits8 *bitmap;
                int bitmask = 1;
                int bitval = 0;
                for (i = 0; i < ndims; i++) {
                  nitems *= dims[i];
                  if (nitems > MaxArraySize)
                    elog(ERROR,
                        "[pllua]: array size exceeds maximum allowed");
                }
                if (hasnulls) {
                  offset = ARR_OVERHEAD_WITHNULLS(ndims, nitems);
                  size += offset;
                }
                else {
                  offset = 0;
                  size += ARR_OVERHEAD_NONULLS(ndims);
                }
                a = (ArrayType *) SPI_palloc(size);
                SET_VARSIZE(a, size);
                a->ndim = ndims;
                a->dataoffset = offset;
                a->elemtype = ti->elem;
                memcpy(ARR_DIMS(a), dims, ndims * sizeof(int));
                memcpy(ARR_LBOUND(a), lb, ndims * sizeof(int));
                p = ARR_DATA_PTR(a);
                bitmap = ARR_NULLBITMAP(a);
                luaP_toarray(L, &p, ndims, dims, lb, &bitmap, &bitmask,
                    &bitval, te, ti->elem, typmod);
              }
              dat = PointerGetDatum(a);
            }
            else {
              luaP_Datum *d = luaP_toudata(L, -1, PLLUA_DATUM);
              if (d == NULL) elog(ERROR,
                  "[pllua]: raw datum expected for datum conversion, got %s",
                  lua_typename(L, lua_type(L, -1)));
              dat = datumcopy(d->datum, ti);
            }
            break;
#if PG_VERSION_NUM >= 80300
          case 'e': /* enum? */
            dat = Int32GetDatum(lua_tointeger(L, -1));
            break;
#endif
          case 'p': /* pseudo? */
          default:
            resulterror(type);
        }
      }
    }
  }
  return dat;
}

static Datum luaP_getresult (lua_State *L, FunctionCallInfo fcinfo,
    Oid type) {
  Datum dat = luaP_todatum(L, type, 0, &fcinfo->isnull);
  lua_settop(L, 0);
  return dat;
}


/* ======= luaP_callhandler ======= */

static void luaP_cleanthread (lua_State *L, lua_State **thread) {
  lua_pushlightuserdata(L, (void *) *thread);
  lua_pushnil(L);
  lua_rawset(L, LUA_REGISTRYINDEX);
  *thread = NULL;
}

Datum luaP_validator (lua_State *L, Oid oid) {
  if (SPI_connect() != SPI_OK_CONNECT)
    elog(ERROR, "[pllua]: could not connect to SPI manager");
  PG_TRY();
  {
    luaP_pushfunction(L, (int) oid);
    lua_pop(L, 1);
  }
  PG_CATCH();
  {
    if (L != NULL) {
      lua_settop(L, 0); /* clear Lua stack */
      luaP_cleantrigger(L);
    }
    PG_RE_THROW();
  }
  PG_END_TRY();
  if (SPI_finish() != SPI_OK_FINISH)
    elog(ERROR, "[pllua]: could not disconnect from SPI manager");
  return 0; /* VOID */
}

Datum luaP_callhandler (lua_State *L, FunctionCallInfo fcinfo) {
  Datum retval = 0;
  luaP_Info *fi;
  bool istrigger;
  if (SPI_connect() != SPI_OK_CONNECT)
    elog(ERROR, "[pllua]: could not connect to SPI manager");
  istrigger = CALLED_AS_TRIGGER(fcinfo);
  fi = luaP_pushfunction(L, (int) fcinfo->flinfo->fn_oid);
  PG_TRY();
  {
    if ((fi->result == TRIGGEROID && !istrigger)
        || (fi->result != TRIGGEROID && istrigger))
      ereport(ERROR,
          (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
           errmsg("[pllua]: trigger function can only be called as trigger")));
    if (istrigger) {
      TriggerData *trigdata = (TriggerData *) fcinfo->context;
      int i, nargs;
      luaP_preptrigger(L, trigdata); /* set global trigger table */
      nargs = trigdata->tg_trigger->tgnargs;
      for (i = 0; i < nargs; i++) /* push args */
        lua_pushstring(L, trigdata->tg_trigger->tgargs[i]);
      if (lua_pcall(L, nargs, 0, 0)) luaP_error(L, "runtime");
      if (TRIGGER_FIRED_FOR_ROW(trigdata->tg_event)
          && TRIGGER_FIRED_BEFORE(trigdata->tg_event)) /* return? */
        retval = luaP_gettriggerresult(L);
      luaP_cleantrigger(L);
    }
    else { /* called as function */
      if (fi->result_isset) { /* SETOF? */
        int status, hasresult;
        ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
        if (fi->L == NULL) { /* first call? */
          if (!rsi || !IsA(rsi, ReturnSetInfo)
              || (rsi->allowedModes & SFRM_ValuePerCall) == 0)
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("[pllua]: set-valued function called in context"
                   "that cannot accept a set")));
          rsi->returnMode = SFRM_ValuePerCall;
          fi->L = lua_newthread(L);
          lua_pushlightuserdata(L, (void *) fi->L);
          lua_pushvalue(L, -2); /* thread */
          lua_rawset(L, LUA_REGISTRYINDEX);
          lua_pop(L, 1); /* new thread */
        }
        lua_xmove(L, fi->L, 1); /* function */
        luaP_pushargs(fi->L, fcinfo, fi);
#if LUA_VERSION_NUM <= 501
        status = lua_resume(fi->L, fcinfo->nargs);
#else
        status = lua_resume(fi->L, fi->L, fcinfo->nargs);
#endif
        hasresult = !lua_isnoneornil(fi->L, 1);
        if (status == LUA_YIELD && hasresult) {
          rsi->isDone = ExprMultipleResult; /* SRF: next */
          retval = luaP_getresult(fi->L, fcinfo, fi->result);
        }
        else if (status == 0 || !hasresult) { /* last call? */
          rsi->isDone = ExprEndResult; /* SRF: done */
          fcinfo->isnull = true;
          retval = (Datum) 0;
          luaP_cleanthread(L, &fi->L);
        }
        else luaP_error(fi->L, "runtime");
      }
      else {
        luaP_pushargs(L, fcinfo, fi);
        if (lua_pcall(L, fcinfo->nargs, 1, 0))
          luaP_error(L, "runtime");
        retval = luaP_getresult(L, fcinfo, fi->result);
      }
    }
    /* stack should be clean here: lua_gettop(L) == 0 */
  }
  PG_CATCH();
  {
    if (L != NULL) {
      luaP_cleantrigger(L);
      if (fi->result_isset && fi->L != NULL) /* clean thread? */
        luaP_cleanthread(L, &fi->L);
      lua_settop(L, 0); /* clear Lua stack */
    }
    fcinfo->isnull = true;
    retval = (Datum) 0;
    PG_RE_THROW();
  }
  PG_END_TRY();
  if (SPI_finish() != SPI_OK_FINISH)
    elog(ERROR, "[pllua]: could not disconnect from SPI manager");
  return retval;
}

#if PG_VERSION_NUM >= 90000
/* ======= luaP_inlinehandler ======= */

Datum luaP_inlinehandler (lua_State *L, const char *source) {
  if (SPI_connect() != SPI_OK_CONNECT)
    elog(ERROR, "[pllua]: could not connect to SPI manager");
  PG_TRY();
  {
    if (luaL_loadbuffer(L, source, strlen(source), PLLUA_CHUNKNAME))
      luaP_error(L, "compile");
    if (lua_pcall(L, 0, 0, 0)) luaP_error(L, "runtime");
  }
  PG_CATCH();
  {
    if (L != NULL) lua_settop(L, 0); /* clear Lua stack */
    PG_RE_THROW();
  }
  PG_END_TRY();
  if (SPI_finish() != SPI_OK_FINISH)
    elog(ERROR, "[pllua]: could not disconnect from SPI manager");
  PG_RETURN_VOID();
}
#endif

