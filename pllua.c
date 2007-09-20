/*
 * pllua.c: PL/Lua call handler
 * Author: Luis Carvalho <lexcarvalho at gmail.com>
 * Please check copyright notice at the bottom of pllua.h
 * $Id: pllua.c,v 1.5 2007/09/20 19:50:00 carvalho Exp $
 */

#include "pllua.h"

typedef struct luaP_Info {
  int oid;
  int nargs;
  Oid *arg;
  Oid result;
  bool result_isset;
  lua_State *L; /* thread for SETOF iterator */
} luaP_Info;

#define PLLUA_LOCALVAR "upvalue"
#define PLLUA_LOCALVARSZ 7
#define PLLUA_SHAREDVAR "shared"
#define PLLUA_SPIVAR "server"
#define PLLUA_TRIGGERVAR "trigger"
#define PLLUA_CHUNKNAME "pllua chunk"
#define PLLUA_VARARG (-1)

/* from catalog/pg_type.h */
#define BOOLARRAYOID 1000
#define INT2ARRAYOID 1005
#define TEXTARRAYOID 1009
#define INT8ARRAYOID 1016
#define FLOAT8ARRAYOID 1022

#define MaxArraySize ((Size) (MaxAllocSize / sizeof(Datum)))

#define info(msg) ereport(INFO, (errcode(ERRCODE_WARNING), errmsg msg))
#define warning(msg) ereport(WARNING, (errcode(ERRCODE_WARNING), errmsg msg))
#define text2string(d) DatumGetCString(DirectFunctionCall1(textout, (d)))
#define string2text(s) DirectFunctionCall1(textin, CStringGetDatum((s)))
#define varchar2string(d) DatumGetCString(DirectFunctionCall1(varcharout, (d)))
#define string2varchar(s, l) \
  DirectFunctionCall3(varcharin, CStringGetDatum((s)), \
      ObjectIdGetDatum(VARCHAROID), Int32GetDatum(VARHDRSZ + (l)))

#define luaP_gettypechar(o) ((luaP_gettypeinfo((o))).typtype)
#define luaP_gettypeelem(o) ((luaP_gettypeinfo((o))).typelem)
#define luaP_cleantrigger(L) \
  lua_pushstring(L, PLLUA_TRIGGERVAR); lua_pushnil(L); \
  lua_rawset(L, LUA_GLOBALSINDEX);


static lua_State *L = NULL; /* Lua VM */

PG_MODULE_MAGIC;
Datum pllua_call_handler(PG_FUNCTION_ARGS);

/* Trigger */
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
    elog(ERROR, "unknown trigger 'when' event");
  lua_setfield(L, -2, "when");
  /* level */
  if (TRIGGER_FIRED_FOR_ROW(tdata->tg_event))
    lua_pushstring(L, "row");
  else if (TRIGGER_FIRED_FOR_STATEMENT(tdata->tg_event))
    lua_pushstring(L, "statement");
  else
    elog(ERROR, "unknown trigger 'level' event");
  lua_setfield(L, -2, "level");
  /* operation */
  if (TRIGGER_FIRED_BY_INSERT(tdata->tg_event))
    lua_pushstring(L, "insert");
  else if (TRIGGER_FIRED_BY_UPDATE(tdata->tg_event))
    lua_pushstring(L, "update");
  else if (TRIGGER_FIRED_BY_DELETE(tdata->tg_event))
    lua_pushstring(L, "delete");
  else
    elog(ERROR, "unknown trigger 'operation' event");
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
  luaP_pushtuple(L, tdata->tg_relation->rd_att,
      (TRIGGER_FIRED_BY_UPDATE(tdata->tg_event)) ? tdata->tg_newtuple
      : tdata->tg_trigtuple, tdata->tg_relation->rd_id, 0);
  lua_setfield(L, -2, "row");
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


/* luaP_newstate: create a new Lua VM */
static int luaP_globalnewindex (lua_State *L) {
  elog(ERROR, "[pllua]: attempt to set global var '%s'", lua_tostring(L, -2));
  return 0;
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
    if (s == NULL) return luaL_error(L, "cannot convert to string");
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

static int luaP_warning (lua_State *L) {
  luaL_checkstring(L, 1);
  warning((lua_tostring(L, 1)));
  return 0;
}

static const luaL_Reg luaP_funcs[] = {
  {"setshared", luaP_setshared},
  {"print", luaP_print},
  {"info", luaP_info},
  {"warning", luaP_warning},
  {NULL, NULL}
};

static const luaL_Reg luaP_libs[] = {
  {"", luaopen_base},
  {LUA_TABLIBNAME, luaopen_table},
  {LUA_STRLIBNAME, luaopen_string},
  {LUA_MATHLIBNAME, luaopen_math},
  {LUA_OSLIBNAME, luaopen_os}, /* restricted */
  {NULL, NULL}
};

static const char *os_funcs[] = {"date", "time", "difftime", NULL};

static lua_State *luaP_newstate (void) {
  lua_State *L;
  const luaL_Reg *reg = luaP_libs;
  const char **s;
  L = lua_open();
  if (L == NULL) elog(ERROR, "cannot allocate Lua VM");
  for (; reg->func; reg++) {
    lua_pushcfunction(L, reg->func);
    lua_pushstring(L, reg->name);
    lua_call(L, 1, 0);
  }
  /* restricted os lib */
  lua_getglobal(L, LUA_OSLIBNAME);
  lua_newtable(L); /* new os */
  s = os_funcs;
  for (; *s; s++) {
    lua_getfield(L, -2, *s);
    lua_setfield(L, -2, *s);
  }
  lua_setglobal(L, LUA_OSLIBNAME);
  lua_pop(L, 2);
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
  /* metatable */
  lua_createtable(L, 0, 1);
  lua_pushcfunction(L, luaP_globalnewindex);
  lua_setfield(L, -2, "__newindex");
  lua_pushvalue(L, -1); /* metatable */
  lua_setfield(L, -2, "__metatable");
  lua_setmetatable(L, LUA_GLOBALSINDEX);
  return L;
}

/* luaP_pushfunction */
static FormData_pg_type luaP_gettypeinfo (Oid typeoid) {
  HeapTuple type;
  FormData_pg_type typeinfo;
  type = SearchSysCache(TYPEOID, ObjectIdGetDatum(typeoid), 0, 0, 0);
  if (!HeapTupleIsValid(type))
    elog(ERROR, "cache lookup failed for type %u", typeoid);
  typeinfo = *((Form_pg_type) GETSTRUCT(type));
  ReleaseSysCache(type);
  return typeinfo;
}

static TupleDesc luaP_gettupledesc (Oid typeoid) {
  FormData_pg_type info = luaP_gettypeinfo(typeoid);
  return (info.typtype != 'c') ? NULL : /* tuple? */
    lookup_rowtype_tupdesc(typeoid, info.typtypmod);
}

static luaP_Info *luaP_newinfo (lua_State *L, FunctionCallInfo fcinfo,
    Form_pg_proc procst, bool istrigger) {
  Oid *argtype = procst->proargtypes.values;
  Oid rettype = procst->prorettype;
  bool isset = procst->proretset;
  luaP_Info *fi;
  int i;
  char type;
  fi = lua_newuserdata(L, sizeof(luaP_Info) + fcinfo->nargs * sizeof(Oid));
  fi->oid = (int) fcinfo->flinfo->fn_oid;
  fi->nargs = (istrigger) ? PLLUA_VARARG : fcinfo->nargs;
  fi->arg = (fcinfo->nargs > 0) ? ((Oid *) (fi + 1)) : NULL;
  /* read arg types */
  for (i = 0; i < fcinfo->nargs; i++) {
    type = luaP_gettypechar(argtype[i]);
    if (type == 'p') /* pseudo-type? */
      ereport(ERROR,
          (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
           errmsg("pllua functions cannot take type '%s'",
          format_type_be(argtype[i]))));
    fi->arg[i] = argtype[i];
  }
  /* read result type */
  if ((rettype == TRIGGEROID && !istrigger)
      || (rettype != TRIGGEROID && istrigger))
    ereport(ERROR,
        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
         errmsg("trigger function can only be called as trigger")));
  type = luaP_gettypechar(rettype);
  if (type == 'p' && rettype != VOIDOID && rettype != TRIGGEROID) 
    ereport(ERROR,
        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
         errmsg("pllua functions cannot return type '%s'",
           format_type_be(rettype))));
  fi->result = rettype;
  fi->result_isset = isset;
  fi->L = NULL;
  return fi;
}

static luaP_Info *luaP_pushfunction (lua_State *L, FunctionCallInfo fcinfo,
    bool istrigger) {
  luaP_Info *fi;
  int oid = (int) fcinfo->flinfo->fn_oid;
  lua_pushinteger(L, oid);
  lua_rawget(L, LUA_REGISTRYINDEX);
  if (lua_isnil(L, -1)) { /* not interned? */
    HeapTuple proc;
    Form_pg_proc procst;
    bool isnull;
    int status;
    Datum prosrc, *argname;
    const char *s, *fname;
    luaL_Buffer b;
    lua_pop(L, 1); /* nil */
    /* read proc info */
    proc = SearchSysCache(PROCOID, ObjectIdGetDatum(fcinfo->flinfo->fn_oid),
        0, 0, 0);
    if (!HeapTupleIsValid(proc))
      elog(ERROR, "cache lookup failed for function %u",
          fcinfo->flinfo->fn_oid);
    procst = (Form_pg_proc) GETSTRUCT(proc);
    prosrc = SysCacheGetAttr(PROCOID, proc, Anum_pg_proc_prosrc, &isnull);
    if (isnull) elog(ERROR, "null prosrc");
    /* get info userdata */
    lua_pushinteger(L, oid);
    fi = luaP_newinfo(L, fcinfo, procst, istrigger);
    lua_pushlightuserdata(L, (void *) fi);
    /* check #argnames */
    if (fcinfo->nargs > 0) {
      int nnames;
      Datum argnames = SysCacheGetAttr(PROCOID, proc,
          Anum_pg_proc_proargnames, &isnull);
      if (isnull) elog(ERROR, "null argnames");
      deconstruct_array(DatumGetArrayTypeP(argnames), TEXTOID, -1, false,
          'i', &argname, NULL, &nnames);
      if (nnames != fcinfo->nargs) fi->nargs = PLLUA_VARARG;
    }
    /* prepare buffer */
    luaL_buffinit(L, &b);
    /* read func name */
    fname = NameStr(procst->proname);
    /* prepare header: "local upvalue, f f=function(" */
    luaL_addlstring(&b, "local " PLLUA_LOCALVAR ",", 7 + PLLUA_LOCALVARSZ);
    luaL_addlstring(&b, fname, strlen(fname));
    luaL_addchar(&b, ' ');
    luaL_addlstring(&b, fname, strlen(fname));
    luaL_addlstring(&b, "=function(", 10);
    /* read arg names */
    if (fi->nargs == PLLUA_VARARG) luaL_addlstring(&b, "...", 3);
    else {
      int i;
      for (i = 0; i < fi->nargs; i++) {
        s = text2string(argname[i]);
        if (i > 0) luaL_addchar(&b, ',');
        luaL_addlstring(&b, s, strlen(s));
      }
    }
    luaL_addlstring(&b, ") ", 2);
    /* read source */
    s = text2string(prosrc);
    luaL_addlstring(&b, s, strlen(s));
    ReleaseSysCache(proc);
    /* prepare footer: " end return f" */
    luaL_addlstring(&b, " end return ", 12);
    luaL_addlstring(&b, fname, strlen(fname));
    /* create function */
    luaL_pushresult(&b);
    s = lua_tostring(L, -1);
    status = luaL_loadbuffer(L, s, strlen(s), PLLUA_CHUNKNAME);
    if (status) elog(ERROR, "[compile]: %s", lua_tostring(L, -1));
    lua_remove(L, -2); /* source */
    lua_call(L, 0, 1);
    lua_pushvalue(L, -1); /* func */
    lua_insert(L, -5);
    lua_rawset(L, LUA_REGISTRYINDEX); /* REG[light_info] = func */
    lua_rawset(L, LUA_REGISTRYINDEX); /* REG[oid] = info */
  }
  else {
    fi = lua_touserdata(L, -1);
    lua_pop(L, 1); /* info udata */
    lua_pushlightuserdata(L, (void *) fi);
    lua_rawget(L, LUA_REGISTRYINDEX);
  }
  return fi;
}


/* luaP_pushargs */
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
        *p = att_addlength(*p, typeinfo->typlen, PointerGetDatum(*p));
        *p = (char *) att_align(*p, typeinfo->typalign);
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
  switch(type) {
    case BOOLOID:
      lua_pushboolean(L, (int) (dat != 0));
      break;
    case FLOAT4OID:
      lua_pushnumber(L, (lua_Number) DatumGetFloat4(dat));
      break;
    case FLOAT8OID:
    case NUMERICOID:
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
    case TEXTOID:
      lua_pushstring(L, text2string(dat));
      break;
    case VARCHAROID:
      lua_pushstring(L, varchar2string(dat));
      break;
    case BOOLARRAYOID:
    case FLOAT4ARRAYOID:
    case FLOAT8ARRAYOID:
    case INT2ARRAYOID:
    case INT4ARRAYOID:
    case INT8ARRAYOID:
    case TEXTARRAYOID: {
      ArrayType *arr = DatumGetArrayTypeP(dat);
      char *p = ARR_DATA_PTR(arr);
      bits8 *bitmap = ARR_NULLBITMAP(arr);
      int bitmask = 1;
      Oid typeelem = luaP_gettypeelem(type);
      FormData_pg_type typeinfo = luaP_gettypeinfo(typeelem);
      luaP_pusharray(L, &p, ARR_NDIM(arr), ARR_DIMS(arr), ARR_LBOUND(arr),
          &bitmap, &bitmask, &typeinfo, typeelem);
      break;
    }
    default: {
      TupleDesc tupdesc = luaP_gettupledesc(type);
      if (tupdesc == NULL) /* not a tuple? */
        elog(ERROR, "type '%s' (%d) not supported as argument",
            format_type_be(type), type);
      else {
        HeapTupleHeader tup = DatumGetHeapTupleHeader(dat);
        int i;
        const char *key;
        bool isnull;
        Datum value;
        lua_createtable(L, 0, tupdesc->natts);
        for (i = 0; i < tupdesc->natts; i++) {
          key = NameStr(tupdesc->attrs[i]->attname);
          value = GetAttributeByNum(tup, tupdesc->attrs[i]->attnum, &isnull);
          if (!isnull) {
            luaP_pushdatum(L, value, tupdesc->attrs[i]->atttypid);
            lua_setfield(L, -2, key);
          }
        }
        ReleaseTupleDesc(tupdesc);
      }
      break;
    }
  }
}

static void luaP_pushargs (lua_State *L, FunctionCallInfo fcinfo,
    luaP_Info *fi) {
  int i;
  for (i = 0; i < fi->nargs; i++) {
    if (fcinfo->argnull[i]) lua_pushnil(L);
    else luaP_pushdatum(L, fcinfo->arg[i], fi->arg[i]);
  }
}


/* luaP_getresult */

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
          elog(ERROR, "table exceeds max number of dimensions");
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
        Datum v = luaP_todatum(L, typeelem, len, &isnull);
        n = 0;
        if (typeinfo->typlen == -1) /* varlena? */
          v = PointerGetDatum(PG_DETOAST_DATUM(v));
        size = att_addlength(size, typeinfo->typlen, v);
        size = att_align(size, typeinfo->typalign);
        if (size > MaxAllocSize)
          elog(ERROR, "array size exceeds the maximum allowed");
      }
      n++;
      if (*ndims < 0) *ndims = n;
      else if (*ndims != n) elog(ERROR, "table is asymetric");
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
      Datum v;
      lua_rawgeti(L, -1, (*lb) + i);
      v = luaP_todatum(L, typeelem, len, &isnull);
      if (!isnull) {
        *bitval |= *bitmask;
        if (typeinfo->typlen > 0) {
          if (typeinfo->typbyval) store_att_byval(*p, v, typeinfo->typlen);
          else memmove(*p, DatumGetPointer(v), typeinfo->typlen);
          *p += att_align(typeinfo->typlen, typeinfo->typalign);
        }
        else {
          int inc;
          Assert(!typeinfo->typbyval);
          inc = att_addlength(0, typeinfo->typlen, v);
          memmove(*p, DatumGetPointer(v), inc);
          *p += att_align(inc, typeinfo->typalign);
        }
        if (!typeinfo->typbyval) pfree(DatumGetPointer(v));
      }
      else
        if (!(*bitmap)) elog(ERROR, "no support for null elements");
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
    switch(type) {
      case BOOLOID:
        dat = BoolGetDatum(lua_toboolean(L, -1));
        break;
      case FLOAT4OID:
        dat = Float4GetDatum(lua_tonumber(L, -1));
        break;
      case FLOAT8OID:
        dat = Float8GetDatum(lua_tonumber(L, -1));
        break;
      case INT2OID:
        dat = Int16GetDatum(lua_tointeger(L, -1));
        break;
      case INT4OID:
        dat = Int32GetDatum(lua_tointeger(L, -1));
        break;
      case INT8OID:
        dat = Int64GetDatum(lua_tointeger(L, -1));
        break;
      case TEXTOID:
        dat = string2text(lua_tostring(L, -1));
        break;
      case VARCHAROID: {
        if (len <= 0)
          elog(ERROR, "type '%s' not supported in this context",
              format_type_be(type));
        dat = string2varchar(lua_tostring(L, -1), len);
        break;
      }
      case BOOLARRAYOID:
      case FLOAT4ARRAYOID:
      case FLOAT8ARRAYOID:
      case INT2ARRAYOID:
      case INT4ARRAYOID:
      case INT8ARRAYOID:
      case TEXTARRAYOID: {
        FormData_pg_type typeinfo;
        Oid typeelem;
        int ndims, dims[MAXDIM], lb[MAXDIM];
        int i, size;
        bool hasnulls;
        if (lua_type(L, -1) != LUA_TTABLE)
          elog(ERROR, "table expected");
        typeelem = luaP_gettypeelem(type);
        typeinfo = luaP_gettypeinfo(typeelem);
        for (i = 0; i < MAXDIM; i++) dims[i] = lb[i] = -1;
        size = luaP_getarraydims(L, &ndims, dims, lb, &typeinfo, typeelem, len,
            &hasnulls);
        if (size == 0) /* empty array? */
          dat = PointerGetDatum(construct_empty_array(typeelem));
        else {
          int nitems = 1;
          ArrayType *a;
          int offset;
          char *p;
          bits8 *bitmap;
          int bitmask = 1;
          int bitval = 0;
          for (i = 0; i < ndims; i++) {
            nitems *= dims[i];
            if (nitems > MaxArraySize)
              elog(ERROR, "array size exceeds maximum allowed");
          }
          if (hasnulls) {
            offset = ARR_OVERHEAD_WITHNULLS(ndims, nitems);
            size += offset;
          }
          else {
            offset = 0;
            size += ARR_OVERHEAD_NONULLS(ndims);
          }
          a = (ArrayType *) palloc(size);
          a->size = size;
          a->ndim = ndims;
          a->dataoffset = offset;
          a->elemtype = typeelem;
          memcpy(ARR_DIMS(a), dims, ndims * sizeof(int));
          memcpy(ARR_LBOUND(a), lb, ndims * sizeof(int));
          p = ARR_DATA_PTR(a);
          bitmap = ARR_NULLBITMAP(a);
          luaP_toarray(L, &p, ndims, dims, lb, &bitmap, &bitmask, &bitval,
              &typeinfo, typeelem, len);
          dat = PointerGetDatum(a);
        }
        break;
      }
      default: {
        TupleDesc typedesc = luaP_gettupledesc(type);
        if (typedesc == NULL)
          elog(ERROR, "type '%s' not supported as result",
              format_type_be(type));
        else {
          int i;
          Datum *values;
          bool *nulls;
          if (lua_type(L, -1) != LUA_TTABLE)
            elog(ERROR, "table expected for record result, got %s",
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
          dat = HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(typedesc),
                values, nulls));
          ReleaseTupleDesc(typedesc);
          pfree(values);
          pfree(nulls);
        }
        break;
      }
    }
  }
  return dat;
}

static Datum luaP_getresult (lua_State *L, FunctionCallInfo fcinfo,
    Oid type) {
  bool isnull;
  Datum dat = luaP_todatum(L, type, 0, &isnull);
  fcinfo->isnull = isnull;
  lua_pop(L, 1);
  return dat;
}


/* TODO:
 *  o error msgs: notice, [runtime] tags
 *  o bpchar, numeric (variable)
 *  o tuple as luaP_Tuple
 */


/* call handler */
PG_FUNCTION_INFO_V1(pllua_call_handler);
Datum pllua_call_handler(PG_FUNCTION_ARGS) {
  Datum retval = 0;
  luaP_Info *fi = NULL;
  int status;
  bool istrigger;
  if (L == NULL) L = luaP_newstate();
  if (L == NULL) elog(ERROR, "cannot initialize Lua state");
  if (SPI_connect() != SPI_OK_CONNECT)
    elog(ERROR, "could not connect to SPI manager");
  PG_TRY();
  {
    istrigger = CALLED_AS_TRIGGER(fcinfo);
    fi = luaP_pushfunction(L, fcinfo, istrigger);
    if (istrigger) {
      TriggerData *trigdata = (TriggerData *) fcinfo->context;
      int i, nargs;
      luaP_preptrigger(L, trigdata); /* set global trigger table */
      nargs = trigdata->tg_trigger->tgnargs;
      for (i = 0; i < nargs; i++) /* push args */
        lua_pushstring(L, trigdata->tg_trigger->tgargs[i]);
      status = lua_pcall(L, nargs, 0, 0);
      if (status) elog(ERROR, "[runtime]: %s", lua_tostring(L, -1));
      if (TRIGGER_FIRED_FOR_ROW(trigdata->tg_event)
          && !TRIGGER_FIRED_BY_DELETE(trigdata->tg_event)
          && TRIGGER_FIRED_BEFORE(trigdata->tg_event))
        retval = luaP_gettriggerresult(L);
      luaP_cleantrigger(L);
    }
    else { /* called as function */
      if (fi->result_isset) { /* SETOF? */
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
        status = lua_resume(fi->L, fi->nargs);
        if (status == LUA_YIELD && !lua_isnil(fi->L, -1)) {
          rsi->isDone = ExprMultipleResult;
          retval = luaP_getresult(fi->L, fcinfo, fi->result);
        }
        else if (status == 0 || lua_isnil(fi->L, -1)) { /* last call? */
          rsi->isDone = ExprEndResult;
          lua_pushlightuserdata(L, (void *) fi->L);
          lua_pushnil(L);
          lua_rawset(L, LUA_REGISTRYINDEX);
          fcinfo->isnull = true;
          retval = (Datum) 0;
        }
        else
          elog(ERROR, "[runtime]: %s", lua_tostring(fi->L, -1));
      }
      else {
        luaP_pushargs(L, fcinfo, fi);
        status = lua_pcall(L, fcinfo->nargs, 1, 0);
        if (status) elog(ERROR, "[runtime]: %s", lua_tostring(L, -1));
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
      }
      luaP_cleantrigger(L);
    }
    fcinfo->isnull = true;
    retval = (Datum) 0;
    PG_RE_THROW();
  }
  PG_END_TRY();
  if (SPI_finish() != SPI_OK_FINISH)
    elog(ERROR, "could not disconnect from SPI manager");
  return retval;
}

