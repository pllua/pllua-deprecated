/*
 * plluaapi.c: PL/Lua API
 * Author: Luis Carvalho <lexcarvalho at gmail.com>
 * Please check copyright notice at the bottom of pllua.h
 * $Id: plluaapi.c,v 1.13 2008/02/08 03:06:42 carvalho Exp $
 */

#include "pllua.h"

/* extended function info */
typedef struct luaP_Info {
  int oid;
  int vararg;
  Oid result;
  bool result_isset;
  lua_State *L; /* thread for SETOF iterator */
  Oid arg[1];
} luaP_Info;

#define PLLUA_LOCALVAR "upvalue"
#define PLLUA_LOCALVARSZ 7
#define PLLUA_SHAREDVAR "shared"
#define PLLUA_SPIVAR "server"
#define PLLUA_TRIGGERVAR "trigger"
#define PLLUA_CHUNKNAME "pllua chunk"
#define PLLUA_INIT_EXIST \
  "select 1 from pg_catalog.pg_tables where schemaname='pllua'" \
  "and tablename='init'"
#define PLLUA_INIT_LIST "select module from pllua.init"

#define MaxArraySize ((Size) (MaxAllocSize / sizeof(Datum)))

/* input */
#define text2string(d) DatumGetCString(DirectFunctionCall1(textout, (d)))
#define varchar2string(d) DatumGetCString(DirectFunctionCall1(varcharout, (d)))
#define char2string(d) DatumGetCString(DirectFunctionCall1(bpcharout, (d)))
#define numeric2string(n) DatumGetCString(DirectFunctionCall1(numeric_out, (n)))
/* output: later we need to copy the result to upper context */
#define string2varchar(s, l) \
  DirectFunctionCall3(varcharin, CStringGetDatum((s)), \
      ObjectIdGetDatum(VARCHAROID), Int32GetDatum(VARHDRSZ + (l)))
#define string2char(s, l) \
  DirectFunctionCall3(bpcharin, CStringGetDatum((s)), \
      ObjectIdGetDatum(BPCHAROID), Int32GetDatum(VARHDRSZ + (l)))
#define string2numeric(s, l) \
  DirectFunctionCall3(numeric_in, CStringGetDatum((s)), \
      ObjectIdGetDatum(NUMERICOID), Int32GetDatum(VARHDRSZ + (l)))

/* back compatibility to 8.2 */
#if PG_VERSION_NUM < 80300
#define SET_VARSIZE(ptr, len) VARATT_SIZEP(ptr) = len
#define att_addlength_pointer(len, typ, ptr) \
  att_addlength(len, typ, PointerGetDatum(ptr))
#define att_align_nominal(len, typ) att_align(len, typ)
#endif

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
static Datum varlenacopy (Datum dat, int len) {
  Size l = datumGetSize(dat, false, len);
  void *copy = SPI_palloc(l);
  memcpy(copy, DatumGetPointer(dat), l);
  return PointerGetDatum(copy);
}

#define info(msg) ereport(INFO, \
    (errcode(ERRCODE_SUCCESSFUL_COMPLETION), errmsg msg))
#define luaP_error(L, tag) \
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), \
             errmsg("[pllua]: " tag " error"), \
             errdetail("%s", lua_tostring((L), -1))))


/* ======= Trigger ======= */

static void luaP_preptrigger (lua_State *L, TriggerData *tdata) {
  const char *relname;
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
  lua_rawset(L, LUA_GLOBALSINDEX);
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
  lua_pushstring(L, PLLUA_TRIGGERVAR);
  lua_pushnil(L);
  lua_rawset(L, LUA_GLOBALSINDEX);
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
          lua_getfield(L, LUA_GLOBALSINDEX, "require");
          lua_pushvalue(L, -2); /* module name */
          status = lua_pcall(L, 1, 1, 0);
          if (status) break; /* error? */
          if (!lua_isnil(L, -1)) /* make sure global table is set? */
            lua_rawset(L, LUA_GLOBALSINDEX);
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
  lua_pushvalue(L, LUA_GLOBALSINDEX);
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
  info((s));
  lua_pop(L, 1);
  return 0;
}

static int luaP_info (lua_State *L) {
  luaL_checkstring(L, 1);
  info((lua_tostring(L, 1)));
  return 0;
}

static int luaP_log (lua_State *L) {
  luaL_checkstring(L, 1);
  ereport(LOG, (errcode(ERRCODE_SUCCESSFUL_COMPLETION),
        errmsg(lua_tostring(L, 1))));
  return 0;
}

static int luaP_notice (lua_State *L) {
  luaL_checkstring(L, 1);
  ereport(NOTICE, (errcode(ERRCODE_SUCCESSFUL_COMPLETION),
        errmsg(lua_tostring(L, 1))));
  return 0;
}

static int luaP_warning (lua_State *L) {
  luaL_checkstring(L, 1);
  ereport(WARNING, (errcode(ERRCODE_WARNING),
        errmsg(lua_tostring(L, 1))));
  return 0;
}

static const luaL_Reg luaP_funcs[] = {
  {"setshared", luaP_setshared},
  {"log", luaP_log},
  {"print", luaP_print},
  {"info", luaP_info},
  {"notice", luaP_notice},
  {"warning", luaP_warning},
  {NULL, NULL}
};

static void *luaP_alloc (void *ud, void *ptr, size_t osize, size_t nsize) {
  (void) osize; /* not used */
  if (nsize == 0) {
    if (ptr != NULL) pfree(ptr);
    return NULL;
  }
  else {
    if (ptr != NULL)
      return repalloc(ptr, nsize);
    else {
      void *a;
      MemoryContext m = MemoryContextSwitchTo((MemoryContext) ud);
      a = palloc(nsize);
      MemoryContextSwitchTo(m);
      return a;
    }
  }
}

lua_State *luaP_newstate (int trusted, MemoryContext memctxt) {
  int status;
  lua_State *L = lua_newstate(luaP_alloc, (void *) memctxt);
  if (trusted) {
    const luaL_Reg luaP_trusted_libs[] = {
      {"", luaopen_base},
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
      lua_pushcfunction(L, reg->func);
      lua_pushstring(L, reg->name);
      lua_call(L, 1, 0);
    }
    /* restricted os lib */
    lua_getglobal(L, LUA_OSLIBNAME);
    lua_newtable(L); /* new os */
    for (; *s; s++) {
      lua_getfield(L, -2, *s);
      lua_setfield(L, -2, *s);
    }
    lua_setglobal(L, LUA_OSLIBNAME);
    lua_pop(L, 2);
  }
  else
    luaL_openlibs(L);
  /* load pllua.init modules */
  status = luaP_modinit(L);
  if (status != 0) /* SPI or module loading error? */
    elog(ERROR, lua_tostring(L, -1));
  /* set alias for _G */
  lua_pushvalue(L, LUA_GLOBALSINDEX);
  lua_setglobal(L, PLLUA_SHAREDVAR); /* _G.shared = _G */
  /* globals */
  lua_pushvalue(L, LUA_GLOBALSINDEX);
  luaL_register(L, NULL, luaP_funcs);
  lua_pop(L, 1);
  /* SPI */
  luaP_registerspi(L);
  lua_setglobal(L, PLLUA_SPIVAR);
  /* _G metatable */
  if (trusted) {
    const char *package_keys[] = {
      "preload", "loadlib", "loaders", "seeall", NULL};
    const char **s = package_keys;
    /* clean package module */
    lua_getfield(L, LUA_GLOBALSINDEX, "package");
    for (; *s; s++) {
      lua_pushnil(L);
      lua_setfield(L, -2, *s);
    }
    lua_pop(L, 1); /* package table */
    lua_pushnil(L);
    lua_setfield(L, LUA_GLOBALSINDEX, "require");
    lua_pushnil(L);
    lua_setfield(L, LUA_GLOBALSINDEX, "module");
    /* set _G as read-only */
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, luaP_globalnewindex);
    lua_setfield(L, -2, "__newindex");
    lua_pushvalue(L, -1); /* metatable */
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, LUA_GLOBALSINDEX);
  }
  return L;
}


/* ======= luaP_pushfunction ======= */

static FormData_pg_type luaP_gettypeinfo (Oid typeoid) {
  HeapTuple type;
  FormData_pg_type typeinfo;
  type = SearchSysCache(TYPEOID, ObjectIdGetDatum(typeoid), 0, 0, 0);
  if (!HeapTupleIsValid(type))
    elog(ERROR, "[pllua]: cache lookup failed for type %u", typeoid);
  typeinfo = *((Form_pg_type) GETSTRUCT(type));
  ReleaseSysCache(type);
  return typeinfo;
}

static TupleDesc luaP_gettupledesc (Oid typeoid) {
  FormData_pg_type info = luaP_gettypeinfo(typeoid);
  return (info.typtype != 'c') ? NULL : /* tuple? */
    lookup_rowtype_tupdesc(typeoid, info.typtypmod);
}

static luaP_Info *luaP_newinfo (lua_State *L, int nargs, int oid,
    Form_pg_proc procst) {
  Oid *argtype = procst->proargtypes.values;
  Oid rettype = procst->prorettype;
  bool isset = procst->proretset;
  luaP_Info *fi;
  int i;
  char type;
  fi = lua_newuserdata(L, sizeof(luaP_Info) + nargs * sizeof(Oid));
  fi->oid = oid;
  /* read arg types */
  for (i = 0; i < nargs; i++) {
    type = luaP_gettypeinfo(argtype[i]).typtype;
    if (type == 'p') /* pseudo-type? */
      ereport(ERROR,
          (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
           errmsg("[pllua]: functions cannot take type '%s'",
          format_type_be(argtype[i]))));
    fi->arg[i] = argtype[i];
  }
  /* read result type */
  type = luaP_gettypeinfo(rettype).typtype;
  if (type == 'p' && rettype != VOIDOID && rettype != TRIGGEROID) 
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
static luaP_Info *luaP_newfunction (lua_State *L, int oid) {
  luaP_Info *fi;
  int nargs; /* fcinfo->nargs */
  HeapTuple proc;
  Form_pg_proc procst;
  bool isnull;
  Datum prosrc, *argname;
  const char *s, *fname;
  text *t;
  luaL_Buffer b;
  /* read proc info */
  proc = SearchSysCache(PROCOID, ObjectIdGetDatum((Oid) oid), 0, 0, 0);
  if (!HeapTupleIsValid(proc))
    elog(ERROR, "[pllua]: cache lookup failed for function %u", (Oid) oid);
  procst = (Form_pg_proc) GETSTRUCT(proc);
  prosrc = SysCacheGetAttr(PROCOID, proc, Anum_pg_proc_prosrc, &isnull);
  if (isnull) elog(ERROR, "[pllua]: null prosrc");
  nargs = procst->pronargs;
  /* get info userdata */
  lua_pushinteger(L, oid);
  fi = luaP_newinfo(L, nargs, oid, procst);
  lua_pushlightuserdata(L, (void *) fi);
  /* check #argnames */
  if (nargs > 0) {
    int nnames;
    Datum argnames = SysCacheGetAttr(PROCOID, proc,
        Anum_pg_proc_proargnames, &isnull);
    if (!isnull)
      deconstruct_array(DatumGetArrayTypeP(argnames), TEXTOID, -1, false,
          'i', &argname, NULL, &nnames);
    if (nnames != nargs)
      fi->vararg = 1;
    else { /* check empty names */
      int i;
      for (i = 0; i < nnames && !fi->vararg; i++) {
        if (VARSIZE(DatumGetTextP(argname[i])) == VARHDRSZ) /* empty? */
          fi->vararg = 1;
      }
    }
  }
  /* prepare buffer */
  luaL_buffinit(L, &b);
  /* read func name */
  fname = NameStr(procst->proname);
  /* prepare header: "local upvalue,f f=function(" */
  luaL_addlstring(&b, "local " PLLUA_LOCALVAR ",", 7 + PLLUA_LOCALVARSZ);
  luaL_addlstring(&b, fname, strlen(fname));
  luaL_addchar(&b, ' ');
  luaL_addlstring(&b, fname, strlen(fname));
  luaL_addlstring(&b, "=function(", 10);
  /* read arg names */
  if (fi->vararg) luaL_addlstring(&b, "...", 3);
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
  ReleaseSysCache(proc);
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
  lua_pushvalue(L, -1); /* func */
  lua_insert(L, -5);
  lua_rawset(L, LUA_REGISTRYINDEX); /* REG[light_info] = func */
  lua_rawset(L, LUA_REGISTRYINDEX); /* REG[oid] = info */
  return fi;
}

static luaP_Info *luaP_pushfunction (lua_State *L, int oid) {
  luaP_Info *fi;
  lua_pushinteger(L, oid);
  lua_rawget(L, LUA_REGISTRYINDEX);
  if (lua_isnil(L, -1)) { /* not interned? */
    lua_pop(L, 1);
    fi = luaP_newfunction(L, oid);
  }
  else {
    fi = lua_touserdata(L, -1);
    lua_pop(L, 1); /* info udata */
    lua_pushlightuserdata(L, (void *) fi);
    lua_rawget(L, LUA_REGISTRYINDEX);
  }
  return fi;
}


/* ======= luaP_pushargs ======= */

static void luaP_pusharray (lua_State *L, char **p, int ndims,
    int *dims, int *lb, bits8 **bitmap, int *bitmask,
    Form_pg_type typeinfo, Oid typeelem) {
  int i;
  lua_newtable(L);
  if (ndims == 1) { /* vector? */
    for (i = 0; i < (*dims); i++) {
      if (*bitmap == NULL || ((**bitmap) & (*bitmask)) != 0) { /* not NULL? */
        luaP_pushdatum(L, fetch_att(*p, typeinfo->typbyval, typeinfo->typlen),
            typeelem);
        lua_rawseti(L, -2, (*lb) + i);
        *p = att_addlength_pointer(*p, typeinfo->typlen, *p);
        *p = (char *) att_align_nominal(*p, typeinfo->typalign);
      }
      if (*bitmap) {
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
          typeinfo, typeelem);
      lua_rawseti(L, -2, (*lb) + i);
    }
  }
}

void luaP_pushdatum (lua_State *L, Datum dat, Oid type) {
  FormData_pg_type typeinfo = luaP_gettypeinfo(type);
#if PG_VERSION_NUM >= 80300
  if (typeinfo.typtype == 'e') { /* enum? */
    lua_pushinteger(L, (lua_Integer) DatumGetInt32(dat)); /* 4-byte long */
    return;
  }
#endif
  if (typeinfo.typtype == 'd') /* domain? */
    type = typeinfo.typbasetype;
  switch(type) {
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
    case INT8OID:
      lua_pushinteger(L, (lua_Integer) DatumGetInt64(dat));
      break;
    case NUMERICOID:
      lua_pushstring(L, numeric2string(dat));
      lua_pushnumber(L, lua_tonumber(L, -1));
      lua_replace(L, -2);
      break;
    case TEXTOID:
      lua_pushstring(L, text2string(dat));
      break;
    case BPCHAROID:
      lua_pushstring(L, char2string(dat));
      break;
    case VARCHAROID:
      lua_pushstring(L, varchar2string(dat));
      break;
    case REFCURSOROID: {
      Portal cursor = SPI_cursor_find(text2string(dat));
      if (cursor != NULL) luaP_pushcursor(L, cursor);
      else lua_pushnil(L);
      break;
    }
    /* non-base types */
    default: {
      Oid typeelem = typeinfo.typelem;
      if (typeelem != 0 && typeinfo.typlen == -1) { /* array? */
        ArrayType *arr = DatumGetArrayTypeP(dat);
        char *p = ARR_DATA_PTR(arr);
        bits8 *bitmap = ARR_NULLBITMAP(arr);
        int bitmask = 1;
        FormData_pg_type typeeleminfo = luaP_gettypeinfo(typeelem);
        luaP_pusharray(L, &p, ARR_NDIM(arr), ARR_DIMS(arr), ARR_LBOUND(arr),
            &bitmap, &bitmask, &typeeleminfo, typeelem);
      }
      else {
        TupleDesc tupdesc = luaP_gettupledesc(type);
        if (tupdesc == NULL) /* not a tuple? */
          elog(ERROR, "[pllua]: type '%s' (%d) not supported as argument",
              format_type_be(type), type);
        else {
          HeapTupleHeader tup = DatumGetHeapTupleHeader(dat);
          int i;
          const char *key;
          bool isnull;
          Datum value;
          lua_createtable(L, 0, tupdesc->natts);
          for (i = 0; i < tupdesc->natts; i++) {
            Form_pg_attribute att = tupdesc->attrs[i];
            key = NameStr(att->attname);
            value = GetAttributeByNum(tup, att->attnum, &isnull);
            if (!isnull) {
              luaP_pushdatum(L, value, att->atttypid);
              lua_setfield(L, -2, key);
            }
          }
          ReleaseTupleDesc(tupdesc);
        }
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
    int *lb, Form_pg_type typeinfo, Oid typeelem, int len, bool *hasnulls) {
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
        size += luaP_getarraydims(L, &n, dims + 1, lb + 1, typeinfo, typeelem,
            len, hasnulls);
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
        Datum d = luaP_todatum(L, typeelem, len, &isnull);
        Pointer v = DatumGetPointer(d);
        n = 0;
        if (typeinfo->typlen == -1) /* varlena? */
          v = (Pointer) PG_DETOAST_DATUM(d);
        size = att_addlength_pointer(size, typeinfo->typlen, v);
        size = att_align_nominal(size, typeinfo->typalign);
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
    Form_pg_type typeinfo, Oid typeelem, int len) {
  int i;
  bool isnull;
  if (ndims == 1) { /* vector? */
    for (i = 0; i < (*dims); i++) {
      Pointer v;
      lua_rawgeti(L, -1, (*lb) + i);
      v = DatumGetPointer(luaP_todatum(L, typeelem, len, &isnull));
      if (!isnull) {
        *bitval |= *bitmask;
        if (typeinfo->typlen > 0) {
          if (typeinfo->typbyval)
            store_att_byval(*p, PointerGetDatum(v), typeinfo->typlen);
          else
            memmove(*p, v, typeinfo->typlen);
          *p += att_align_nominal(typeinfo->typlen, typeinfo->typalign);
        }
        else {
          int inc;
          Assert(!typeinfo->typbyval);
          inc = att_addlength_pointer(0, typeinfo->typlen, v);
          memmove(*p, v, inc);
          *p += att_align_nominal(inc, typeinfo->typalign);
        }
        if (!typeinfo->typbyval) pfree(v);
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
          bitmask, bitval, typeinfo, typeelem, len);
      lua_pop(L, 1);
    }
  }
}

Datum luaP_todatum (lua_State *L, Oid type, int len, bool *isnull) {
  Datum dat = 0; /* NULL */
  *isnull = lua_isnil(L, -1);
  if (!(*isnull || type == VOIDOID)) {
    FormData_pg_type typeinfo = luaP_gettypeinfo(type);
#if PG_VERSION_NUM >= 80300
    if (typeinfo.typtype == 'e') /* enum? */
      return Int32GetDatum(lua_tointeger(L, -1));
#endif
    if (typeinfo.typtype == 'd') /* domain? */
      type = typeinfo.typbasetype;
    switch(type) {
      /* base and domain types */
      case BOOLOID:
        dat = BoolGetDatum(lua_toboolean(L, -1));
        break;
      case FLOAT4OID: { /* not by value */
        float4 *x = (float4 *) SPI_palloc(sizeof(float4));
        *x = lua_tonumber(L, -1);
        dat = PointerGetDatum(x);
        break;
      }
      case FLOAT8OID: { /* not by value */
        float8 *x = (float8 *) SPI_palloc(sizeof(float8));
        *x = lua_tonumber(L, -1);
        dat = PointerGetDatum(x);
        break;
      }
      case INT2OID:
        dat = Int16GetDatum(lua_tointeger(L, -1));
        break;
      case INT4OID:
        dat = Int32GetDatum(lua_tointeger(L, -1));
        break;
      case INT8OID: { /* not by value */
        int64 *x = (int64 *) SPI_palloc(8); /* sizeof(int64)<=8 */
        *x = lua_tointeger(L, -1);
        dat = PointerGetDatum(x);
        break;
      }
      case NUMERICOID:
        if (len <= 0)
          elog(ERROR, "[pllua]: type '%s' not supported in this context",
              format_type_be(type));
        dat = string2numeric(lua_tostring(L, -1), len);
        dat = varlenacopy(dat, typeinfo.typlen);
        break;
      case TEXTOID:
        dat = string2text(lua_tostring(L, -1));
        break;
      case BPCHAROID:
        if (len <= 0)
          elog(ERROR, "[pllua]: type '%s' not supported in this context",
              format_type_be(type));
        dat = string2char(lua_tostring(L, -1), len);
        dat = varlenacopy(dat, typeinfo.typlen);
        break;
      case VARCHAROID:
        if (len <= 0)
          elog(ERROR, "[pllua]: type '%s' not supported in this context",
              format_type_be(type));
        dat = string2varchar(lua_tostring(L, -1), len);
        dat = varlenacopy(dat, typeinfo.typlen);
        break;
      case REFCURSOROID: {
        Portal cursor = luaP_tocursor(L, -1);
        dat = string2text(cursor->name);
        break;
      }
      default: {
        Oid typeelem = typeinfo.typelem;
        if (typeelem != 0 && typeinfo.typlen == -1) { /* array? */
          FormData_pg_type typeeleminfo;
          int ndims, dims[MAXDIM], lb[MAXDIM];
          int i, size;
          bool hasnulls;
          ArrayType *a;
          if (lua_type(L, -1) != LUA_TTABLE)
            elog(ERROR, "[pllua]: table expected for array conversion, got %s",
                lua_typename(L, lua_type(L, -1)));
          typeeleminfo = luaP_gettypeinfo(typeelem);
          for (i = 0; i < MAXDIM; i++) dims[i] = lb[i] = -1;
          size = luaP_getarraydims(L, &ndims, dims, lb, &typeeleminfo,
              typeelem, len, &hasnulls);
          if (size == 0) { /* empty array? */
            a = (ArrayType *) SPI_palloc(sizeof(ArrayType));
            SET_VARSIZE(a, sizeof(ArrayType));
            a->ndim = 0;
            a->dataoffset = 0;
            a->elemtype = typeelem;
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
                elog(ERROR, "[pllua]: array size exceeds maximum allowed");
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
            a->elemtype = typeelem;
            memcpy(ARR_DIMS(a), dims, ndims * sizeof(int));
            memcpy(ARR_LBOUND(a), lb, ndims * sizeof(int));
            p = ARR_DATA_PTR(a);
            bitmap = ARR_NULLBITMAP(a);
            luaP_toarray(L, &p, ndims, dims, lb, &bitmap, &bitmask, &bitval,
                &typeeleminfo, typeelem, len);
          }
          dat = PointerGetDatum(a);
        }
        else {
          TupleDesc typedesc = luaP_gettupledesc(type);
          if (typedesc == NULL)
            elog(ERROR, "[pllua]: type '%s' not supported as result",
                format_type_be(type));
          else {
            int i;
            Datum *values;
            bool *nulls;
            if (lua_type(L, -1) != LUA_TTABLE)
              elog(ERROR, "[pllua]: table expected for record result, got %s",
                  lua_typename(L, lua_type(L, -1)));
            /* create tuple */
            values = palloc(typedesc->natts * sizeof(Datum));
            nulls = palloc(typedesc->natts * sizeof(bool));
            for (i = 0; i < typedesc->natts; i++) {
              lua_getfield(L, -1, NameStr(typedesc->attrs[i]->attname));
              /* only simple types allowed in record */
              values[i] = luaP_todatum(L, typedesc->attrs[i]->atttypid,
                  typedesc->attrs[i]->atttypmod, nulls + i);
              lua_pop(L, 1);
            }
            /* make copy in upper executor memory context */
            typedesc = BlessTupleDesc(typedesc);
            dat = PointerGetDatum(SPI_returntuple(heap_form_tuple(typedesc,
                    values, nulls), typedesc));
            ReleaseTupleDesc(typedesc);
            pfree(nulls);
            pfree(values);
          }
        }
      }
    }
  }
  return dat;
}

static Datum luaP_getresult (lua_State *L, FunctionCallInfo fcinfo,
    Oid type) {
  Datum dat = luaP_todatum(L, type, 0, &fcinfo->isnull);
  lua_pop(L, 1);
  return dat;
}


/* ======= luaP_callhandler ======= */

Datum luaP_validator (lua_State *L, Oid oid) {
  if (SPI_connect() != SPI_OK_CONNECT)
    elog(ERROR, "[pllua]: could not connect to SPI manager");
  PG_TRY();
  {
    luaP_newfunction(L, (int) oid);
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
  luaP_Info *fi = NULL;
  bool istrigger;
  if (SPI_connect() != SPI_OK_CONNECT)
    elog(ERROR, "[pllua]: could not connect to SPI manager");
  PG_TRY();
  {
    istrigger = CALLED_AS_TRIGGER(fcinfo);
    fi = luaP_pushfunction(L, (int) fcinfo->flinfo->fn_oid);
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
      if (lua_pcall(L, nargs, 0, 0))
        luaP_error(L, "runtime");
      if (TRIGGER_FIRED_FOR_ROW(trigdata->tg_event)
          && TRIGGER_FIRED_BEFORE(trigdata->tg_event)) /* return? */
        retval = luaP_gettriggerresult(L);
      luaP_cleantrigger(L);
    }
    else { /* called as function */
      if (fi->result_isset) { /* SETOF? */
        int status;
        ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
        if (fi->L == NULL) { /* first call? */
          if (!rsi || !IsA(rsi, ReturnSetInfo)
              || (rsi->allowedModes & SFRM_ValuePerCall) == 0)
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("set-valued function called in context"
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
        status = lua_resume(fi->L, fcinfo->nargs);
        if (status == LUA_YIELD && !lua_isnil(fi->L, -1)) {
          rsi->isDone = ExprMultipleResult; /* SRF: next */
          retval = luaP_getresult(fi->L, fcinfo, fi->result);
        }
        else if (status == 0 || lua_isnil(fi->L, -1)) { /* last call? */
          rsi->isDone = ExprEndResult; /* SRF: done */
          lua_pushlightuserdata(L, (void *) fi->L);
          lua_pushnil(L);
          lua_rawset(L, LUA_REGISTRYINDEX);
          fi->L = NULL;
          fcinfo->isnull = true;
          retval = (Datum) 0;
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
  }
  PG_CATCH();
  {
    if (L != NULL) {
      lua_settop(L, 0); /* clear Lua stack */
      if (fi != NULL && fi->result_isset
          && fi->L != NULL) { /* clean thread? */
        lua_pushlightuserdata(L, (void *) fi->L);
        lua_pushnil(L);
        lua_rawset(L, LUA_REGISTRYINDEX);
        fi->L = NULL;
      }
      luaP_cleantrigger(L);
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

