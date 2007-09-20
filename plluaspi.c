/*
 * plluaspi.c: PL/Lua SPI
 * Author: Luis Carvalho <lexcarvalho at gmail.com>
 * Please check copyright notice at the bottom of pllua.h
 * $Id: plluaspi.c,v 1.4 2007/09/20 21:25:29 carvalho Exp $
 */

#include "pllua.h"

#define SPI_plan void
#define PLLUA_BUFFER "_luaP_Buffer"
#define PLLUA_TUPLEMT "luaP_Tuple"
#define PLLUA_TUPTABLE "_luaP_Tuptable"
#define PLLUA_PLANMT "luaP_Plan"
#define PLLUA_CURSORMT "luaP_Cursor"
#define PLLUA_TUPTABLEMT "luaP_Tuptable"

/* FIXME */
#define info(msg) ereport(INFO, (errcode(ERRCODE_WARNING), errmsg msg))

typedef struct luaP_reg {
  const char *name;
  Oid oid;
} luaP_reg;

typedef struct luaP_Buffer {
  int size;
  Datum *value;
  char *null;
} luaP_Buffer;

typedef struct luaP_Tuple {
  int changed;
  Oid relid;
  HeapTuple tuple;
  HeapTuple newtuple;
  TupleDesc desc;
  Datum *value;
  bool *null;
} luaP_Tuple;

typedef struct luaP_Tuptable {
  int size;
  Portal cursor;
  SPITupleTable *tuptable;
} luaP_Tuptable;

typedef struct luaP_Cursor {
  Portal cursor;
} luaP_Cursor;

typedef struct luaP_Plan {
  int nargs;
  int issaved;
  SPI_plan *plan;
  Oid *type;
} luaP_Plan;


/* ======= Utils ======= */

void luaP_pushdesctable(lua_State *L, TupleDesc desc) {
  int i;
  lua_newtable(L);
  for (i = 0; i < desc->natts; i++) {
    lua_pushstring(L, NameStr(desc->attrs[i]->attname));
    lua_pushinteger(L, i);
    lua_rawset(L, -3);
  }
}

/* ======= Buffer ======= */

static luaP_Buffer *luaP_getbuffer (lua_State *L, int n) {
  int i;
  luaP_Buffer *b;
  lua_getfield(L, LUA_REGISTRYINDEX, PLLUA_BUFFER);
  b = (luaP_Buffer *) lua_touserdata(L, -1);
  lua_pop(L, 1);
  if (b == NULL || n > b->size) { /* resize? */
    b = (luaP_Buffer *) lua_newuserdata(L, sizeof(luaP_Buffer)
        + n * (sizeof(Datum) + sizeof(char)));
    b->size = n;
    b->value = (Datum *) (b + 1);
    b->null = (char *) (b->value + n);
    lua_setfield(L, LUA_REGISTRYINDEX, PLLUA_BUFFER);
  }
  for (i = 0; i < n; i++) {
    b->value[i] = 0;
    b->null[i] = 'n';
  }
  return b;
}

static void luaP_fillbuffer (lua_State *L, int pos, Oid *type,
    luaP_Buffer *b) {
  lua_pushnil(L);
  while (lua_next(L, pos)) {
    int k = lua_tointeger(L, -2);
    if (k > 0) {
      bool isnull;
      k--; /* zero based */
      b->value[k] = luaP_todatum(L, type[k], 0, &isnull);
      b->null[k] = (isnull) ? 'n' : ' ';
    }
    lua_pop(L, 1);
  }
}


/* ======= Tuple ======= */

static int luaP_tupleindex (lua_State *L) {
  luaP_Tuple *t = (luaP_Tuple *) lua_touserdata(L, 1);
  const char *name = luaL_checkstring(L, 2);
  int i;
  lua_pushinteger(L, (int) t->relid);
  lua_rawget(L, LUA_REGISTRYINDEX);
  lua_getfield(L, -1, name);
  i = luaL_optinteger(L, -1, -1);
  if (i >= 0) {
    if (t->changed == -1) { /* read-only? */
      bool isnull;
      Datum v;
      v = heap_getattr(t->tuple, t->desc->attrs[i]->attnum, t->desc, &isnull);
      if (!isnull) luaP_pushdatum(L, v, t->desc->attrs[i]->atttypid);
    }
    else if (!t->null[i])
      luaP_pushdatum(L, t->value[i], t->desc->attrs[i]->atttypid);
  }
  return 1;
}

static int luaP_tuplenewindex (lua_State *L) {
  luaP_Tuple *t = (luaP_Tuple *) lua_touserdata(L, 1);
  const char *name = luaL_checkstring(L, 2);
  int i;
  if (t->changed == -1) /* read-only? */
    luaL_error(L, "tuple is read-only");
  lua_pushinteger(L, (int) t->relid);
  lua_rawget(L, LUA_REGISTRYINDEX);
  lua_getfield(L, -1, name);
  i = luaL_optinteger(L, -1, -1);
  lua_settop(L, 3);
  if (i >= 0) { /* found? */
    bool isnull;
    t->value[i] = luaP_todatum(L, t->desc->attrs[i]->atttypid,
        t->desc->attrs[i]->atttypmod, &isnull);
    t->null[i] = isnull;
    t->changed = 1;
  }
  else luaL_error(L, "column not found in relation: '%s'", name);
  return 0;
}

static int luaP_tuplegc (lua_State *L) {
  luaP_Tuple *t = (luaP_Tuple *) lua_touserdata(L, 1);
  if (t->newtuple) /* allocated in top context? */
    heap_freetuple(t->newtuple);
  return 0;
}

void luaP_pushtuple (lua_State *L, TupleDesc desc, HeapTuple tuple,
    Oid relid, int readonly) {
  luaP_Tuple *t;
  int i, n = desc->natts;
  if (readonly) {
    t = lua_newuserdata(L, sizeof(luaP_Tuple));
    t->changed = -1;
    t->value = NULL;
    t->null = NULL;
  }
  else {
    t = lua_newuserdata(L, sizeof(luaP_Tuple)
        + n * (sizeof(Datum) + sizeof(bool)));
    t->changed = 0;
    t->value = (Datum *) (t + 1);
    t->null = (bool *) (t->value + n);
    for (i = 0; i < n; i++) {
      bool isnull;
      t->value[i] = heap_getattr(tuple, desc->attrs[i]->attnum, desc,
          &isnull);
      t->null[i] = isnull;
    }
  }
  t->desc = desc;
  t->relid = relid;
  t->tuple = tuple;
  t->newtuple = NULL;
  luaL_getmetatable(L, PLLUA_TUPLEMT);
  lua_setmetatable(L, -2);
}

static HeapTuple luaP_copytuple (luaP_Tuple *t) {
  MemoryContext currcxt = MemoryContextSwitchTo(TopTransactionContext);
  HeapTuple tuple = heap_form_tuple(t->desc, t->value, t->null);
  /* copy identification info */
  tuple->t_data->t_ctid = t->tuple->t_data->t_ctid;
  tuple->t_self = t->tuple->t_self;
  tuple->t_tableOid = t->tuple->t_tableOid;
  if (t->desc->tdhasoid)
    HeapTupleSetOid(tuple, HeapTupleGetOid(t->tuple));
  t->newtuple = tuple;
  MemoryContextSwitchTo(currcxt);
  return tuple;
}

/* tuple in top of stack */
HeapTuple luaP_totuple (lua_State *L) {
  HeapTuple tuple = NULL;
  luaP_Tuple *t = (luaP_Tuple *) lua_touserdata(L, -1);
  if (t != NULL) {
    if (lua_getmetatable(L, -1)) {
      lua_getfield(L, LUA_REGISTRYINDEX, PLLUA_TUPLEMT);
      if (lua_rawequal(L, -1, -2)) /* tuple? */
        tuple = (t->changed == 1) ? luaP_copytuple(t) : t->tuple;
      lua_pop(L, 2); /* metatables */
    }
  }
  return tuple;
}


/* ======= TupleTable ======= */

static void luaP_pushtuptable (lua_State *L, Portal cursor) {
  luaP_Tuptable *t;
  lua_getfield(L, LUA_REGISTRYINDEX, PLLUA_TUPTABLE);
  t = (luaP_Tuptable *) lua_touserdata(L, -1);
  if (t == NULL) { /* not initialized? */
    lua_pop(L, 1);
    t = (luaP_Tuptable *) lua_newuserdata(L, sizeof(luaP_Tuptable));
    luaL_getmetatable(L, PLLUA_TUPTABLEMT);
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1); /* tuptable */
    lua_setfield(L, LUA_REGISTRYINDEX, PLLUA_TUPTABLE);
  }
  t->size = SPI_processed;
  t->tuptable = SPI_tuptable;
  if (cursor == NULL || (cursor != NULL && t->cursor != cursor)) {
    lua_pushinteger(L, (int) InvalidOid);
    luaP_pushdesctable(L, t->tuptable->tupdesc);
    lua_rawset(L, LUA_REGISTRYINDEX);
    t->cursor = cursor;
  }
  /* reset tuptable env */
  lua_newtable(L); /* env */
  lua_setfenv(L, -2);
}

static int luaP_tuptableindex (lua_State *L) {
  luaP_Tuptable *t = (luaP_Tuptable *) lua_touserdata(L, 1);
  int k = lua_tointeger(L, 2);
  if (k == 0) { /* attributes? */
    lua_pushinteger(L, (int) InvalidOid);
    lua_rawget(L, LUA_REGISTRYINDEX);
  }
  else if (k > 0 && k <= t->size) {
    lua_getfenv(L, 1);
    lua_rawgeti(L, -1, k);
    if (lua_isnil(L, -1)) { /* not interned? */
      lua_pop(L, 1); /* nil */
      luaP_pushtuple(L, t->tuptable->tupdesc, t->tuptable->vals[k - 1],
          InvalidOid, 1);
      lua_pushvalue(L, -1);
      lua_rawseti(L, -3, k);
    }
  }
  return 1;
}

static int luaP_tuptablelen (lua_State *L) {
  luaP_Tuptable *t = (luaP_Tuptable *) lua_touserdata(L, 1);
  lua_pushinteger(L, t->size);
  return 1;
}

static int luaP_tuptablegc (lua_State *L) {
  luaP_Tuptable *t = (luaP_Tuptable *) lua_touserdata(L, 1);
  SPI_freetuptable(t->tuptable);
  return 0;
}


/* ======= Cursor ======= */

static int luaP_newcursor (lua_State *L, Portal cursor) {
  luaP_Cursor *c = (luaP_Cursor *) lua_newuserdata(L, sizeof(luaP_Cursor));
  c->cursor = cursor;
  luaL_getmetatable(L, PLLUA_CURSORMT);
  lua_setmetatable(L, -2);
  return 1;
}

static int luaP_cursorgc (lua_State *L) {
  luaP_Cursor *c = (luaP_Cursor *) lua_touserdata(L, 1);
  SPI_cursor_close(c->cursor);
  return 0;
}

static int luaP_cursorfetch (lua_State *L) {
  luaP_Cursor *c = (luaP_Cursor *) luaL_checkudata(L, 1, PLLUA_CURSORMT);
  SPI_cursor_fetch(c->cursor, 1, luaL_optlong(L, 2, FETCH_ALL));
  luaP_pushtuptable(L, c->cursor);
  return 1;
}

static int luaP_cursormove (lua_State *L) {
  luaP_Cursor *c = (luaP_Cursor *) luaL_checkudata(L, 1, PLLUA_CURSORMT);
  SPI_cursor_move(c->cursor, 1, luaL_optlong(L, 2, 0));
  return 0;
}

static int luaP_cursorclose (lua_State *L) {
  luaP_Cursor *c = (luaP_Cursor *) luaL_checkudata(L, 1, PLLUA_CURSORMT);
  SPI_cursor_close(c->cursor);
  return 0;
}


/* ======= Plan ======= */

static int luaP_plangc (lua_State *L) {
  luaP_Plan *p = (luaP_Plan *) lua_touserdata(L, 1);
  if (p->issaved) SPI_freeplan(p->plan);
  return 0;
}

static int luaP_executeplan (lua_State *L) {
  luaP_Plan *p = (luaP_Plan *) luaL_checkudata(L, 1, PLLUA_PLANMT);
  bool ro = (bool) lua_toboolean(L, 3);
  long c = luaL_optlong(L, 4, 0);
  int result;
  if (p->nargs > 0) {
    luaP_Buffer *b;
    if (lua_type(L, 2) != LUA_TTABLE) luaL_typerror(L, 2, "table");
    b = luaP_getbuffer(L, p->nargs);
    luaP_fillbuffer(L, 2, p->type, b);
    result = SPI_execute_plan(p->plan, b->value, b->null, ro, c); 
  }
  else
    result = SPI_execute_plan(p->plan, NULL, NULL, ro, c); 
  if (result < 0)
    luaL_error(L, "SPI_execute_plan error: %d", result);
  luaP_pushtuptable(L, NULL);
  return 1;
}

static int luaP_saveplan (lua_State *L) {
  luaP_Plan *p = (luaP_Plan *) luaL_checkudata(L, 1, PLLUA_PLANMT);
  SPI_saveplan(p->plan);
  switch (SPI_result) {
    case SPI_ERROR_ARGUMENT: luaL_error(L, "null plan to be saved");
    case SPI_ERROR_UNCONNECTED: luaL_error(L, "unconnected procedure");
  }
  p->issaved = 1;
  return 0;
}

static int luaP_issavedplan (lua_State *L) {
  luaP_Plan *p = (luaP_Plan *) luaL_checkudata(L, 1, PLLUA_PLANMT);
  lua_pushboolean(L, p->issaved);
  return 1;
}

static int luaP_getcursorplan (lua_State *L) {
  luaP_Plan *p = (luaP_Plan *) luaL_checkudata(L, 1, PLLUA_PLANMT);
  bool ro = (bool) lua_toboolean(L, 3);
  const char *name = lua_tostring(L, 4);
  Portal cursor;
  if (SPI_is_cursor_plan(p->plan)) {
    if (p->nargs > 0) {
      luaP_Buffer *b;
      if (lua_type(L, 2) != LUA_TTABLE) luaL_typerror(L, 2, "table");
      b = luaP_getbuffer(L, p->nargs);
      luaP_fillbuffer(L, 2, p->type, b);
      cursor = SPI_cursor_open(name, p->plan, b->value, b->null, ro);
    }
    else
      cursor = SPI_cursor_open(name, p->plan, NULL, NULL, ro);
    if (cursor == NULL)
      luaL_error(L, "error opening cursor");
    luaP_newcursor(L, cursor);
  }
  return 1;
}


/* ======= SPI ======= */

static Oid luaP_gettypeoid (const char *typename) {
  List *namelist = stringToQualifiedNameList(typename, NULL);
  HeapTuple typetup = typenameType(NULL, makeTypeNameFromNameList(namelist));
  Oid typeoid = HeapTupleGetOid(typetup);
  ReleaseSysCache(typetup);
  list_free(namelist);
  return typeoid;
}

static int luaP_prepare (lua_State *L) {
  const char *q = luaL_checkstring(L, 1);
  int nargs;
  luaP_Plan *p;
  if (lua_isnoneornil(L, 2)) nargs = 0;
  else {
    if (lua_type(L, 2) != LUA_TTABLE) luaL_typerror(L, 2, "table");
    nargs = lua_objlen(L, 2);
  }
  p = (luaP_Plan *) lua_newuserdata(L,
      sizeof(luaP_Plan) + nargs * sizeof(Oid));
  p->issaved = 0;
  p->nargs = nargs;
  p->type = (Oid *) (p + 1);
  if (nargs > 0) { /* read types? */
    lua_pushnil(L);
    while (lua_next(L, 2)) {
      int k = lua_tointeger(L, -2);
      if (k > 0) {
        const char *s = luaL_checkstring(L, -1);
        Oid type = luaP_gettypeoid(s);
        if (type == InvalidOid)
          luaL_error(L, "invalid type to plan: %s", s);
        p->type[k - 1] = type;
      }
      lua_pop(L, 1);
    }
  }
  p->plan = SPI_prepare(q, nargs, p->type);
  if (SPI_result < 0)
    luaL_error(L, "SPI_prepare error: %d", SPI_result);
  luaL_getmetatable(L, PLLUA_PLANMT);
  lua_setmetatable(L, -2);
  return 1;
}

static int luaP_execute (lua_State *L) {
  int result = SPI_execute(luaL_checkstring(L, 1),
      (bool) lua_toboolean(L, 2), luaL_optlong(L, 3, 0));
  if (result < 0)
    luaL_error(L, "SPI_execute_plan error: %d", result);
  luaP_pushtuptable(L, NULL);
  return 1;
}

/* returns cursor */
static int luaP_find (lua_State *L) {
  Portal cursor = SPI_cursor_find(luaL_checkstring(L, 1));
  if (cursor != NULL) luaP_newcursor(L, cursor);
  return 1;
}


/* ======= luaP_registerspi ======= */

static const luaL_reg luaP_Plan_funcs[] = {
  {"execute", luaP_executeplan},
  {"save", luaP_saveplan},
  {"issaved", luaP_issavedplan},
  {"getcursor", luaP_getcursorplan},
  {NULL, NULL}
};

static const luaL_reg luaP_Cursor_funcs[] = {
  {"fetch", luaP_cursorfetch},
  {"move", luaP_cursormove},
  {"close", luaP_cursorclose},
  {NULL, NULL}
};

static const luaL_reg luaP_SPI_funcs[] = {
  {"prepare", luaP_prepare},
  {"execute", luaP_execute},
  {"find", luaP_find},
  {NULL, NULL}
};

static const luaL_reg luaP_Tuple_mt[] = {
  {"__index", luaP_tupleindex},
  {"__newindex", luaP_tuplenewindex},
  {"__gc", luaP_tuplegc},
  {NULL, NULL}
};

static const luaL_reg luaP_Tuptable_mt[] = {
  {"__index", luaP_tuptableindex},
  {"__len", luaP_tuptablelen},
  {"__gc", luaP_tuptablegc},
  {NULL, NULL}
};

void luaP_registerspi (lua_State *L) {
  /* tuple */
  luaL_newmetatable(L, PLLUA_TUPLEMT);
  luaL_register(L, NULL, luaP_Tuple_mt);
  lua_pop(L, 1);
  /* tuptable */
  luaL_newmetatable(L, PLLUA_TUPTABLEMT);
  luaL_register(L, NULL, luaP_Tuptable_mt);
  lua_pop(L, 1);
  /* cursor */
  luaL_newmetatable(L, PLLUA_CURSORMT);
  lua_newtable(L);
  luaL_register(L, NULL, luaP_Cursor_funcs);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, luaP_cursorgc);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);
  /* plan */
  luaL_newmetatable(L, PLLUA_PLANMT);
  lua_newtable(L);
  luaL_register(L, NULL, luaP_Plan_funcs);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, luaP_plangc);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);
  /* SPI */
  lua_newtable(L);
  luaL_register(L, NULL, luaP_SPI_funcs);
}

