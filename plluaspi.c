/*
 * plluaspi.c: PL/Lua SPI
 * Author: Luis Carvalho <lexcarvalho at gmail.com>
 * Please check copyright notice at the bottom of pllua.h
 * $Id: plluaspi.c,v 1.19 2008/03/31 22:57:45 carvalho Exp $
 */

#include "pllua.h"

#ifndef SPI_prepare_cursor
#define SPI_prepare_cursor(cmd, nargs, argtypes, copts) \
  SPI_prepare(cmd, nargs, argtypes)
#endif

#define SPI_plan void
static const char PLLUA_BUFFER[] = "luaP_Buffer";
static const char PLLUA_TUPTABLE[] = "luaP_Tuptable";
static const char PLLUA_TUPLEMT[] = "tuple";
static const char PLLUA_PLANMT[] = "plan";
static const char PLLUA_CURSORMT[] = "cursor";
static const char PLLUA_TUPTABLEMT[] = "tupletable";

#include <limits.h>
static unsigned int p_seq_luaP_Cursor = 0;
static unsigned int get_cursor_seq(){
    if (p_seq_luaP_Cursor == UINT_MAX)
        p_seq_luaP_Cursor = 0;
    ++p_seq_luaP_Cursor;
    return p_seq_luaP_Cursor;
}

typedef struct luaP_Tuple {
  int changed;
  Oid relid;
  HeapTuple tuple;
  TupleDesc tupdesc;
  Datum *value;
  bool *null;
  unsigned int tupleIdxSeq;
  int deldesc;
} luaP_Tuple;

typedef struct luaP_Tuptable {
  int size;
  Portal cursor;
  SPITupleTable *tuptable;
  TupleDesc tupdesc;
  unsigned int tupTableIdxSeq;
} luaP_Tuptable;

typedef struct luaP_Cursor {
  Portal cursor;
  unsigned int sequence;
} luaP_Cursor;

typedef struct luaP_Plan {
  int nargs;
  int issaved;
  SPI_plan *plan;
  Oid type[1];
} luaP_Plan;


/* ======= Utils ======= */

static int luaP_typeerror (lua_State *L, int narg, const char *tname) {
  const char *msg = lua_pushfstring(L, "%s expected, got %s",
      tname, luaL_typename(L, narg));
  return luaL_argerror(L, narg, msg);
}

#define luaP_getfield(L, s) \
  lua_pushlightuserdata((L), (void *)(s)); \
  lua_rawget((L), LUA_REGISTRYINDEX)

static void luaP_newmetatable (lua_State *L, const char *tname) {
  lua_newtable(L);
  lua_pushlightuserdata(L, (void *) tname);
  lua_pushvalue(L, -2);
  lua_rawset(L, LUA_REGISTRYINDEX);
}

void *luaP_toudata (lua_State *L, int ud, const char *tname) {
  void *p = lua_touserdata(L, ud);
  if (p != NULL) { /* value is userdata? */
    if (lua_getmetatable(L, ud)) { /* does it have a metatable? */
      luaP_getfield(L, tname); /* get metatable */
      if (lua_rawequal(L, -1, -2)) { /* MTs match? */
        lua_pop(L, 2); /* MTs */
        return p;
      }
    }
  }
  return NULL;
}

static void *luaP_checkudata (lua_State *L, int ud, const char *tname) {
  void *p = luaP_toudata(L, ud, tname);
  if (p == NULL) luaP_typeerror(L, ud, tname);
  return p;
}

void luaP_pushdesctable (lua_State *L, TupleDesc desc) {
  int i;
  lua_newtable(L);
  for (i = 0; i < desc->natts; i++) {
    lua_pushstring(L, NameStr(desc->attrs[i]->attname));
    lua_pushinteger(L, i);
    lua_rawset(L, -3); /* t[att] = i */
  }
}

static int changeCounter(lua_State *L, unsigned int tupleIdxSeq, int value){
    BEGINLUA;
    char str[16];
    int counter;
    lua_pushlightuserdata(L, p_tuple_info);  /* push address */
    lua_gettable(L, LUA_REGISTRYINDEX);  /* retrieve value */

    sprintf(str, "c%u", tupleIdxSeq);
    lua_pushstring(L,str);
    lua_gettable(L, -2);  /* get background[key] */
    counter = (int)lua_tonumber(L, -1);
    counter += value;

    lua_pop(L,1);
    lua_pushstring(L,str);
    lua_pushinteger(L, counter);
    lua_settable(L, -3);

    lua_pop(L,1);
    ENDLUA;
    return counter;
}

static void pushCustomTuple (lua_State *L, TupleDesc desc, HeapTuple tuple,
                      int readonly, unsigned int tupleIdxSeq) {
    BEGINLUA;
    luaP_Tuple *t;
    int i, n = desc->natts;

    t = lua_newuserdata(L, sizeof(luaP_Tuple)
                        + n * (sizeof(Datum) + sizeof(bool)));

    t->value = (Datum *) (t + 1);
    t->null = (bool *) (t->value + n);
    t->tupleIdxSeq = tupleIdxSeq;
    t->deldesc = 1;
    for (i = 0; i < n; i++) {

        bool isnull;
        t->value[i] = heap_getattr(tuple, desc->attrs[i]->attnum, desc,
                                   &isnull);
        t->null[i] = isnull;
    }

    if (readonly) {
        t->changed = -1;
    }
    else {
        t->changed = 0;
    }

    MTOLUA(L);
    t->tupdesc = CreateTupleDescCopy(desc);
    MTOPG(L);
    t->relid = 0;
    t->tuple = tuple;
    luaP_getfield(L, PLLUA_TUPLEMT);
    lua_setmetatable(L, -2);
    ENDLUAV(1);
}



static int luaP_rowsaux (lua_State *L) {
    BEGINLUA;
    luaP_Cursor *c = (luaP_Cursor *) lua_touserdata(L, lua_upvalueindex(1));
    int init = lua_toboolean(L, lua_upvalueindex(2));

    SPI_cursor_fetch(c->cursor, 1, 1);
    if (SPI_processed > 0) { /* any row? */
        if (!init) { /* register tupdesc */
            char str[16];
            lua_pushlightuserdata(L, p_tuple_info);
            lua_gettable(L, LUA_REGISTRYINDEX);  /* retrieve table for tupdesc*/

            sprintf(str, "p%u", c->sequence); /*p_tuple_info[]*/

            lua_pushstring(L,str);
            luaP_pushdesctable(L, SPI_tuptable->tupdesc);
            lua_settable(L, -3);/*p_tuple_info[pAddress] = desc */

            sprintf(str, "c%u", c->sequence);

            lua_pushstring(L,str);
            lua_pushinteger(L, 0);
            lua_settable(L, -3);

            lua_pop(L,1);

            lua_pushboolean(L, 1);
            lua_replace(L, lua_upvalueindex(2));
        }

        changeCounter(L, c->sequence, +1);
        pushCustomTuple(L, SPI_tuptable->tupdesc, SPI_tuptable->vals[0],
                1, c->sequence);
        SPI_freetuptable(SPI_tuptable);
    }
    else {

        SPI_cursor_close(c->cursor);
        lua_pushnil(L);
    }

    ENDLUAV(1);
    return 1;
}

/* ======= Buffer ======= */

luaP_Buffer *luaP_getbuffer (lua_State *L, int n) {
  int i;
  luaP_Buffer *b;
  luaP_getfield(L, PLLUA_BUFFER);
  b = (luaP_Buffer *) lua_touserdata(L, -1);
  lua_pop(L, 1);
  if (b == NULL || n > b->size) { /* resize? */
    lua_pushlightuserdata(L, (void *) PLLUA_BUFFER);
    b = (luaP_Buffer *) lua_newuserdata(L, sizeof(luaP_Buffer)
        + n * (sizeof(Datum) + sizeof(char)));
    b->size = n;
    b->value = (Datum *) (b + 1);
    b->null = (char *) (b->value + n);
    lua_rawset(L, LUA_REGISTRYINDEX);
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
static int luaP_tuplegc (lua_State *L) {
    luaP_Tuple *t = (luaP_Tuple *) lua_touserdata(L, 1);
    if (t->tupdesc && t->deldesc)
    {
        FreeTupleDesc(t->tupdesc);
        t->tupdesc = NULL;
    }

    if (t->tupleIdxSeq != 0){
        char str[16];
        sprintf(str, "c%u", t->tupleIdxSeq);
        lua_pushlightuserdata(L, p_tuple_info);  /* push address */
        lua_gettable(L, LUA_REGISTRYINDEX);  /* retrieve value */

        lua_pushstring(L, str);
        lua_gettable(L, -2);
        int counter = luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);
        --counter;

        if (counter==0){

            lua_pushstring(L, str);
            lua_pushnil(L);
            lua_settable(L,-3);


            sprintf(str, "p%u", t->tupleIdxSeq);
            lua_pushstring(L, str);
            lua_pushnil(L);
            lua_settable(L,-3);

        }else if(counter>0){

            lua_pushstring(L, str);
            lua_pushinteger(L,counter);
            lua_settable(L,-3);

        }
        lua_pop(L,1);

    }
    return 0;
}

static int luaP_tupleindex (lua_State *L) {
  luaP_Tuple *t = (luaP_Tuple *) lua_touserdata(L, 1);
  const char *name = luaL_checkstring(L, 2);
  int i =-1;
  if (t->tupleIdxSeq != 0){
      char str[16];
      sprintf(str, "p%u", t->tupleIdxSeq);

      lua_pushlightuserdata(L, p_tuple_info);  /* push address */
      lua_gettable(L, LUA_REGISTRYINDEX);  /* retrieve value */

      lua_pushstring(L, str);  /* push address */
      lua_gettable(L, -2);  /* retrieve value */

      lua_getfield(L, -1, name);
      i = luaL_optinteger(L, -1, -1);
      lua_pop(L, 3);


  }else{
      lua_pushinteger(L, (int) t->relid);
      lua_rawget(L, LUA_REGISTRYINDEX);
      lua_getfield(L, -1, name);
      i = luaL_optinteger(L, -1, -1);
  }

  if (i >= 0) {
      if (!t->null[i])
        luaP_pushdatum(L, t->value[i], t->tupdesc->attrs[i]->atttypid);
      else lua_pushnil(L);
  }
  else lua_pushnil(L);
  return 1;
}

static int luaP_tuplenewindex (lua_State *L) {
  luaP_Tuple *t = (luaP_Tuple *) lua_touserdata(L, 1);
  const char *name = luaL_checkstring(L, 2);
  int i;
  if (t->changed == -1) /* read-only? */
    return luaL_error(L, "tuple is read-only");
  lua_pushinteger(L, (int) t->relid);
  lua_rawget(L, LUA_REGISTRYINDEX);
  lua_getfield(L, -1, name);
  i = luaL_optinteger(L, -1, -1);
  lua_settop(L, 3);
  if (i >= 0) { /* found? */
    bool isnull;
    t->value[i] = luaP_todatum(L, t->tupdesc->attrs[i]->atttypid,
        t->tupdesc->attrs[i]->atttypmod, &isnull);
    t->null[i] = isnull;
    t->changed = 1;
  }
  else
    return luaL_error(L, "column not found in relation: '%s'", name);
  return 0;
}

static int luaP_tupletostring (lua_State *L) {
  lua_pushfstring(L, "%s: %p", PLLUA_TUPLEMT, lua_touserdata(L, 1));
  return 1;
}

 void luaP_pushtuple (lua_State *L, TupleDesc desc, HeapTuple tuple,
    Oid relid, int readonly, unsigned int tupleIdxSeq) {
    BEGINLUA;
  luaP_Tuple *t;
  int i, n = desc->natts;

  t = lua_newuserdata(L, sizeof(luaP_Tuple)
                      + n * (sizeof(Datum) + sizeof(bool)));
  if (readonly) {
      t->changed = -1;
  }
  else {
      t->changed = 0;
  }
    t->deldesc = 0;
    t->changed = 0;
    t->value = (Datum *) (t + 1);
    t->null = (bool *) (t->value + n);
    t->tupleIdxSeq = tupleIdxSeq;
    for (i = 0; i < n; i++) {
      bool isnull;
      t->value[i] = heap_getattr(tuple, desc->attrs[i]->attnum, desc,
          &isnull);
      t->null[i] = isnull;
    }

  t->tupdesc = desc;
  t->relid = relid;
  t->tuple = tuple;
  luaP_getfield(L, PLLUA_TUPLEMT);
  lua_setmetatable(L, -2);
  ENDLUAV(1);
}


/* adapted from SPI_modifytuple */
static HeapTuple luaP_copytuple (luaP_Tuple *t) {
  HeapTuple tuple = heap_form_tuple(t->tupdesc, t->value, t->null);
  /* copy identification info */
  tuple->t_data->t_ctid = t->tuple->t_data->t_ctid;
  tuple->t_self = t->tuple->t_self;
  tuple->t_tableOid = t->tuple->t_tableOid;
  if (t->tupdesc->tdhasoid)
    HeapTupleSetOid(tuple, HeapTupleGetOid(t->tuple));
  return SPI_copytuple(tuple); /* in upper mem context */
}

static luaP_Tuple *luaP_checktuple (lua_State *L, int pos) {
  luaP_Tuple *t = (luaP_Tuple *) lua_touserdata(L, pos);
  if (t != NULL) {
    if (lua_getmetatable(L, pos)) {
      luaP_getfield(L, PLLUA_TUPLEMT);
      if (!lua_rawequal(L, -1, -2)) /* not tuple? */
        t = NULL;
      lua_pop(L, 2); /* metatables */
    }
  }
  return t;
}

/* tuple on top of stack */
HeapTuple luaP_totuple (lua_State *L) {
  luaP_Tuple *t = luaP_checktuple(L, -1);
  if (t == NULL) return NULL; /* not a tuple */
  return (t->changed == 1) ? luaP_copytuple(t) : t->tuple;
}

/* tuple on top of stack */
HeapTuple luaP_casttuple (lua_State *L, TupleDesc tupdesc) {
  luaP_Tuple *t = luaP_checktuple(L, -1);
  int i;
  luaP_Buffer *b;
  if (t == NULL) return NULL; /* not a tuple */
  lua_pushinteger(L, (int) t->relid);
  lua_rawget(L, LUA_REGISTRYINDEX); /* tuple desc table */
  b = luaP_getbuffer(L, tupdesc->natts);
  for (i = 0; i < tupdesc->natts; i++) {
    int j;
    lua_getfield(L, -1, NameStr(tupdesc->attrs[i]->attname));
    j = luaL_optinteger(L, -1, -1);
    if (j >= 0) {
      if (t->changed == -1) /* read-only? */
        b->value[i] = heap_getattr(t->tuple,
            t->tupdesc->attrs[j]->attnum, t->tupdesc, b->null + i);
      else {
        b->value[i] = t->value[j];
        b->null[i] = t->null[j];
      }
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 1); /* desc table */
  return heap_form_tuple(tupdesc, b->value, b->null);
}

/* ======= TupleTable ======= */

static void luaP_pushtuptable (lua_State *L, Portal cursor) {
    BEGINLUA;
  luaP_Tuptable *t;
  luaP_getfield(L, PLLUA_TUPTABLE);
  t = (luaP_Tuptable *) lua_touserdata(L, -1);
  if (t == NULL) { /* not initialized? */
    lua_pop(L, 1);
    t = (luaP_Tuptable *) lua_newuserdata(L, sizeof(luaP_Tuptable));
    t->tupTableIdxSeq = get_cursor_seq();

    luaP_getfield(L, PLLUA_TUPTABLEMT);
    lua_setmetatable(L, -2);
    lua_pushlightuserdata(L, (void *) PLLUA_TUPTABLE);
    lua_pushvalue(L, -2); /* tuptable */
    lua_rawset(L, LUA_REGISTRYINDEX);
  }
  t->size = SPI_processed;
  t->tuptable = SPI_tuptable;
  //MTOLUA(L); why crash?
  //t->tupdesc = CreateTupleDescCopy(SPI_tuptable->tupdesc);
  //MTOPG;
  if (cursor == NULL || (cursor != NULL && t->cursor != cursor)) {
    //----------------------------------------
    char str[16];
    lua_pushlightuserdata(L, p_tuple_info);
    lua_gettable(L, LUA_REGISTRYINDEX);  /* retrieve table for tupdesc*/

    sprintf(str, "p%u", t->tupTableIdxSeq); /*p_tuple_info[]*/

    lua_pushstring(L,str);
    luaP_pushdesctable(L, t->tuptable->tupdesc);
    lua_settable(L, -3);/*p_tuple_info[pAddress]*/

    sprintf(str, "c%u", t->tupTableIdxSeq);


    lua_pushstring(L,str);
    lua_pushinteger(L, 0);
    lua_settable(L, -3);

    lua_pop(L,1);

    //-----------------------------------------------
    t->cursor = cursor;
  }

  changeCounter(L, t->tupTableIdxSeq, +1);
  /* reset tuptable env */
  lua_newtable(L); /* env */
  lua_setuservalue(L, -2);
  ENDLUAV(1);
}

static int luaP_tuptableindex (lua_State *L) {
    luaP_Tuptable *t = (luaP_Tuptable *) lua_touserdata(L, 1);
    int k = lua_tointeger(L, 2);

    if (k == 0) { /* attributes? */

        if (t->tupTableIdxSeq != 0){
            char str[16];
            sprintf(str, "p%u", t->tupTableIdxSeq);

            lua_pushlightuserdata(L, p_tuple_info);  /* push address */
            lua_gettable(L, LUA_REGISTRYINDEX);  /* retrieve value */

            lua_pushstring(L, str);  /* push address */
            lua_gettable(L, -2);  /* retrieve value */
        }else{
            lua_pushnil(L);
        }
    }
    else if (k > 0 && k <= t->size) {
        lua_getuservalue(L, 1);
        lua_rawgeti(L, -1, k);
        if (lua_isnil(L, -1)) { /* not interned? */
            lua_pop(L, 1); /* nil */
            luaP_pushtuple(L, t->tuptable->tupdesc, t->tuptable->vals[k - 1],
                    InvalidOid, 1, t->tupTableIdxSeq);

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

static int luaP_tuptabletostring (lua_State *L) {
  lua_pushfstring(L, "%s: %p", PLLUA_TUPTABLEMT, lua_touserdata(L, 1));
  return 1;
}


/* ======= Cursor ======= */


void luaP_pushcursor (lua_State *L, Portal cursor) {

  luaP_Cursor *c = (luaP_Cursor *) lua_newuserdata(L, sizeof(luaP_Cursor));
  c->cursor = cursor;
  c->sequence = get_cursor_seq();
  luaP_getfield(L, PLLUA_CURSORMT);
  lua_setmetatable(L, -2);
}

Portal luaP_tocursor (lua_State *L, int pos) {
  luaP_Cursor *c = (luaP_Cursor *) luaP_checkudata(L, pos, PLLUA_CURSORMT);
  return c->cursor;
}

static int luaP_cursortostring (lua_State *L) {
  luaP_Cursor *c = (luaP_Cursor *) lua_touserdata(L, 1);
  lua_pushfstring(L, "%s: %p [%s]", PLLUA_CURSORMT, c, c->cursor->name);
  return 1;
}

static int luaP_cursorfetch (lua_State *L) {
  luaP_Cursor *c = (luaP_Cursor *) luaP_checkudata(L, 1, PLLUA_CURSORMT);
  SPI_cursor_fetch(c->cursor, 1, luaL_optlong(L, 2, FETCH_ALL));
  if (SPI_processed > 0) /* any rows? */
    luaP_pushtuptable(L, c->cursor);
  else
    lua_pushnil(L);
  return 1;
}

static int luaP_cursormove (lua_State *L) {
  luaP_Cursor *c = (luaP_Cursor *) luaP_checkudata(L, 1, PLLUA_CURSORMT);
  SPI_cursor_move(c->cursor, 1, luaL_optlong(L, 2, 0));
  return 0;
}

#if PG_VERSION_NUM >= 80300
static int luaP_cursorposfetch (lua_State *L) {
  luaP_Cursor *c = (luaP_Cursor *) luaP_checkudata(L, 1, PLLUA_CURSORMT);
  FetchDirection fd = (lua_toboolean(L, 3)) ? FETCH_RELATIVE : FETCH_ABSOLUTE;
  SPI_scroll_cursor_fetch(c->cursor, fd, luaL_optlong(L, 2, FETCH_ALL));
  if (SPI_processed > 0) /* any rows? */
    luaP_pushtuptable(L, c->cursor);
  else
    lua_pushnil(L);
  return 1;
}

static int luaP_cursorposmove (lua_State *L) {
  luaP_Cursor *c = (luaP_Cursor *) luaP_checkudata(L, 1, PLLUA_CURSORMT);
  FetchDirection fd = (lua_toboolean(L, 3)) ? FETCH_RELATIVE : FETCH_ABSOLUTE;
  SPI_scroll_cursor_move(c->cursor, fd, luaL_optlong(L, 2, 0));
  return 0;
}
#endif


static int luaP_cursorclose (lua_State *L) {
  luaP_Cursor *c = (luaP_Cursor *) luaP_checkudata(L, 1, PLLUA_CURSORMT);
  SPI_cursor_close(c->cursor);
  return 0;
}


/* ======= Plan ======= */

static int luaP_plangc (lua_State *L) {
  luaP_Plan *p = (luaP_Plan *) lua_touserdata(L, 1);
  if (p->issaved) SPI_freeplan(p->plan);
  return 0;
}

static int luaP_plantostring (lua_State *L) {
  lua_pushfstring(L, "plan: %p", lua_touserdata(L, 1));
  return 1;
}

static int luaP_executeplan (lua_State *L) {
  luaP_Plan *p = (luaP_Plan *) luaP_checkudata(L, 1, PLLUA_PLANMT);
  bool ro = (bool) lua_toboolean(L, 3);
  long c = luaL_optlong(L, 4, 0);
  int result;
  if (p->nargs > 0) {
    luaP_Buffer *b;
    if (lua_type(L, 2) != LUA_TTABLE) luaP_typeerror(L, 2, "table");
    b = luaP_getbuffer(L, p->nargs);
    luaP_fillbuffer(L, 2, p->type, b);
    result = SPI_execute_plan(p->plan, b->value, b->null, ro, c); 
  }
  else
    result = SPI_execute_plan(p->plan, NULL, NULL, ro, c); 
  if (result < 0)
    return luaL_error(L, "SPI_execute_plan error: %d", result);
  if (result == SPI_OK_SELECT && SPI_processed > 0) /* any rows? */
    luaP_pushtuptable(L, NULL);
  else
    lua_pushnil(L);
  return 1;
}

static int luaP_saveplan (lua_State *L) {
  luaP_Plan *p = (luaP_Plan *) luaP_checkudata(L, 1, PLLUA_PLANMT);
  p->plan = SPI_saveplan(p->plan);
  switch (SPI_result) {
    case SPI_ERROR_ARGUMENT:
      return luaL_error(L, "null plan to be saved");
    case SPI_ERROR_UNCONNECTED:
      return luaL_error(L, "unconnected procedure");
  }
  p->issaved = 1;
  return 1;
}

static int luaP_issavedplan (lua_State *L) {
  luaP_Plan *p = (luaP_Plan *) luaP_checkudata(L, 1, PLLUA_PLANMT);
  lua_pushboolean(L, p->issaved);
  return 1;
}

static int luaP_getcursorplan (lua_State *L) {
  luaP_Plan *p = (luaP_Plan *) luaP_checkudata(L, 1, PLLUA_PLANMT);
  bool ro = (bool) lua_toboolean(L, 3);
  const char *name = lua_tostring(L, 4);
  Portal cursor;
  if (SPI_is_cursor_plan(p->plan)) {
    if (p->nargs > 0) {
      luaP_Buffer *b;
      if (lua_type(L, 2) != LUA_TTABLE) luaP_typeerror(L, 2, "table");
      b = luaP_getbuffer(L, p->nargs);
      luaP_fillbuffer(L, 2, p->type, b);
      cursor = SPI_cursor_open(name, p->plan, b->value, b->null, ro);
    }
    else
      cursor = SPI_cursor_open(name, p->plan, NULL, NULL, ro);
    if (cursor == NULL)
      return luaL_error(L, "error opening cursor");
    luaP_pushcursor(L, cursor);
  }
  else lua_pushnil(L);
  return 1;
}

static int luaP_rowsplan (lua_State *L) {
  luaP_Plan *p = (luaP_Plan *) luaP_checkudata(L, 1, PLLUA_PLANMT);
  Portal cursor;
  if (!SPI_is_cursor_plan(p->plan))
    return luaL_error(L, "Plan is not iterable");
  if (p->nargs > 0) {
    luaP_Buffer *b;
    if (lua_type(L, 2) != LUA_TTABLE) luaP_typeerror(L, 2, "table");
    b = luaP_getbuffer(L, p->nargs);
    luaP_fillbuffer(L, 2, p->type, b);
    cursor = SPI_cursor_open(NULL, p->plan, b->value, b->null, 1);
  }
  else
    cursor = SPI_cursor_open(NULL, p->plan, NULL, NULL, 1);
  if (cursor == NULL)
    return luaL_error(L, "error opening cursor");
  luaP_pushcursor(L, cursor);
  lua_pushboolean(L, 0); /* not inited */
  lua_pushcclosure(L, luaP_rowsaux, 2);
  return 1;
}


/* ======= SPI ======= */

Oid luaP_gettypeoid (const char *typename) {
#if PG_VERSION_NUM < 80300
  List *namelist = stringToQualifiedNameList(typename, NULL);
  HeapTuple typetup = typenameType(NULL, makeTypeNameFromNameList(namelist));
#else
  List *namelist = stringToQualifiedNameList(typename);
  HeapTuple typetup = typenameType(NULL, makeTypeNameFromNameList(namelist), NULL);
#endif
  Oid typeoid = HeapTupleGetOid(typetup);
  ReleaseSysCache(typetup);
  list_free(namelist);
  return typeoid;
}

static int luaP_prepare (lua_State *L) {
    int nargs, cursoropt;
    const char *q = luaL_checkstring(L, 1);

    luaP_Plan *p;
    if (lua_isnoneornil(L, 2)) nargs = 0;
    else {
        if (lua_type(L, 2) != LUA_TTABLE) luaP_typeerror(L, 2, "table");
        nargs = lua_rawlen(L, 2);
    }
    cursoropt = luaL_optinteger(L, 3, 0);
    (void)cursoropt;
    p = (luaP_Plan *) lua_newuserdata(L,
                                      sizeof(luaP_Plan) + nargs * sizeof(Oid));
    p->issaved = 0;
    p->nargs = nargs;
    if (nargs > 0) { /* read types? */
        lua_pushnil(L);
        while (lua_next(L, 2)) {
            int k = lua_tointeger(L, -2);
            if (k > 0) {
                const char *s = luaL_checkstring(L, -1);
                Oid type = luaP_gettypeoid(s);
                if (type == InvalidOid)
                    return luaL_error(L, "invalid type to plan: %s", s);
                p->type[k - 1] = type;
            }
            lua_pop(L, 1);
        }
    }
    p->plan = SPI_prepare_cursor(q, nargs, p->type, cursoropt);
    if (SPI_result < 0)
        return luaL_error(L, "SPI_prepare error: %d", SPI_result);
    luaP_getfield(L, PLLUA_PLANMT);
    lua_setmetatable(L, -2);
    return 1;
}

static int luaP_execute (lua_State *L) {
  int result = SPI_execute(luaL_checkstring(L, 1),
      (bool) lua_toboolean(L, 2), luaL_optlong(L, 3, 0));
  if (result < 0)
    return luaL_error(L, "SPI_execute_plan error: %d", result);
  if (result == SPI_OK_SELECT && SPI_processed > 0) /* any rows? */
    luaP_pushtuptable(L, NULL);
  else
    lua_pushnil(L);
  return 1;
}

/* returns cursor */
static int luaP_find (lua_State *L) {
  Portal cursor = SPI_cursor_find(luaL_checkstring(L, 1));
  if (cursor != NULL) luaP_pushcursor(L, cursor);
  else lua_pushnil(L);
  return 1;
}

static int luaP_rows (lua_State *L) {
  Portal cursor;
  SPI_plan *p = SPI_prepare_cursor(luaL_checkstring(L, 1), 0, NULL, 0);
  if (SPI_result < 0)
    return luaL_error(L, "SPI_prepare error: %d", SPI_result);
  if (!SPI_is_cursor_plan(p))
    return luaL_error(L, "Statement is not iterable");
  cursor = SPI_cursor_open(NULL, p, NULL, NULL, 1);
  if (cursor == NULL)
    return luaL_error(L, "error opening cursor");
  luaP_pushcursor(L, cursor);
  lua_pushboolean(L, 0); /* not inited */
  lua_pushcclosure(L, luaP_rowsaux, 2);
  return 1;
}


/* ======= luaP_registerspi ======= */

static const luaL_Reg luaP_Plan_funcs[] = {
  {"execute", luaP_executeplan},
  {"save", luaP_saveplan},
  {"issaved", luaP_issavedplan},
  {"getcursor", luaP_getcursorplan},
  {"rows", luaP_rowsplan},
  {NULL, NULL}
};

static const luaL_Reg luaP_Cursor_funcs[] = {
  {"fetch", luaP_cursorfetch},
  {"move", luaP_cursormove},
#if PG_VERSION_NUM >= 80300
  {"posfetch", luaP_cursorposfetch},
  {"posmove", luaP_cursorposmove},
#endif
  {"close", luaP_cursorclose},
  {NULL, NULL}
};

static const luaL_Reg luaP_SPI_funcs[] = {
  {"prepare", luaP_prepare},
  {"execute", luaP_execute},
  {"find", luaP_find},
  {"rows", luaP_rows},
  {NULL, NULL}
};

static const luaL_Reg luaP_Tuple_mt[] = {
  {"__index", luaP_tupleindex},
  {"__newindex", luaP_tuplenewindex},
  {"__tostring", luaP_tupletostring},
  {"__gc", luaP_tuplegc},
  {NULL, NULL}
};

static const luaL_Reg luaP_Tuptable_mt[] = {
  {"__index", luaP_tuptableindex},
  {"__len", luaP_tuptablelen},
  {"__gc", luaP_tuptablegc},
  {"__tostring", luaP_tuptabletostring},
  {NULL, NULL}
};

static const luaL_Reg luaP_Cursor_mt[] = {
  {"__tostring", luaP_cursortostring},
  {NULL, NULL}
};

static const luaL_Reg luaP_Plan_mt[] = {
  {"__gc", luaP_plangc},
  {"__tostring", luaP_plantostring},
  {NULL, NULL}
};

void luaP_registerspi (lua_State *L) {
  /* tuple */
  luaP_newmetatable(L, PLLUA_TUPLEMT);
  luaP_register(L, luaP_Tuple_mt);
  lua_pop(L, 1);
  /* tuptable */
  luaP_newmetatable(L, PLLUA_TUPTABLEMT);
  luaP_register(L, luaP_Tuptable_mt);
  lua_pop(L, 1);
  /* cursor */
  luaP_newmetatable(L, PLLUA_CURSORMT);
  lua_newtable(L);
  luaP_register(L, luaP_Cursor_funcs);
  lua_setfield(L, -2, "__index");
  luaP_register(L, luaP_Cursor_mt);
  lua_pop(L, 1);
  /* plan */
  luaP_newmetatable(L, PLLUA_PLANMT);
  lua_newtable(L);
  luaP_register(L, luaP_Plan_funcs);
  lua_setfield(L, -2, "__index");
  luaP_register(L, luaP_Plan_mt);
  lua_pop(L, 1);
  /* SPI */
  lua_newtable(L);
#if PG_VERSION_NUM >= 80300
  lua_newtable(L); /* cursor options */
  lua_pushinteger(L, CURSOR_OPT_BINARY);
  lua_setfield(L, -2, "binary");
  lua_pushinteger(L, CURSOR_OPT_SCROLL);
  lua_setfield(L, -2, "scroll");
  lua_pushinteger(L, CURSOR_OPT_NO_SCROLL);
  lua_setfield(L, -2, "noscroll");
  lua_pushinteger(L, CURSOR_OPT_INSENSITIVE);
  lua_setfield(L, -2, "insensitive");
  lua_pushinteger(L, CURSOR_OPT_HOLD); /* ignored */
  lua_setfield(L, -2, "hold");
  lua_pushinteger(L, CURSOR_OPT_FAST_PLAN);
  lua_setfield(L, -2, "fastplan");
  lua_setfield(L, -2, "option");
#endif
  luaP_register(L, luaP_SPI_funcs);
}

