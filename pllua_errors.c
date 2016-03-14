#include "pllua_errors.h"
#include "plluacommon.h"

extern LVMInfo lvm_info[2];

static const char error_type_name[] = "pllua_error";

int luaB_assert (lua_State *L) {
  luaL_checkany(L, 1);
  if (!lua_toboolean(L, 1))
    return luaL_error(L, "%s", luaL_optstring(L, 2, "assertion failed!"));
  return lua_gettop(L);
}

int luaB_error (lua_State *L) {
    int level = luaL_optinteger(L, 2, 1);
    lua_settop(L, 1);
    if (lua_isnoneornil(L, 1)){
        if (lua_isnil(L, 1)){
            lua_pop(L, 1);
        }
        if (level >0){
            luaL_where(L, level);
            lua_pushstring(L, "no exception data");
            lua_concat(L, 2);
        }else{
            lua_pushstring(L, "no exception data");
        }
    }else
//    if (lua_isstring(L, 1) && level > 0) {  /* add extra information? */
//        luaL_where(L, level);
//        lua_pushvalue(L, 1);
//        lua_concat(L, 2);
//    } else
    if (lua_istable(L,1)){
        set_error_mt(L);
    }
    return lua_error(L);
}


int luaL_error_skip_where (lua_State *L, const char *fmt, ...) {
  va_list argp;
  va_start(argp, fmt);
  /*luaL_where(L, 1);*/
  lua_pushvfstring(L, fmt, argp);
  va_end(argp);
  /*lua_concat(L, 2);*/
  return lua_error(L);
}

static int error_tostring(lua_State *L)
{
    lua_pushstring(L, "message");
    lua_rawget(L, -2);
    return 1;
}

static luaL_Reg regs[] =
{
    {"__tostring", error_tostring},
    { NULL, NULL }
};

void register_error_mt(lua_State *L)
{
    lua_newtable(L);
    lua_pushlightuserdata(L, (void *) error_type_name);
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);

    luaP_register(L, regs);
    lua_pop(L, 1);
}

void set_error_mt(lua_State *L)
{
    luaP_getfield(L, error_type_name);
    lua_setmetatable(L, -2);
}

void push_spi_error(lua_State *L, MemoryContext oldcontext)
{
    ErrorData  *edata;

    /* Save error info */
    MemoryContextSwitchTo(oldcontext);
    edata = CopyErrorData();
    FlushErrorState();
    lua_newtable(L);
    if (edata->message){
        lua_pushstring(L, edata->message); //"no exception data"
        lua_setfield(L, -2,  "message");
    } else {
        lua_pushstring(L, "no exception data");
        lua_setfield(L, -2,  "message");
    }

    if (edata->detail){
        lua_pushstring(L, edata->detail);
        lua_setfield(L, -2,  "detail");
    }
    if (edata->context){
        lua_pushstring(L, edata->context);
        lua_setfield(L, -2,  "context");
    }
    if (edata->hint){
        lua_pushstring(L, edata->hint);
        lua_setfield(L, -2,  "hint");
    }
    if (edata->sqlerrcode){
        lua_pushinteger(L, edata->sqlerrcode);
        lua_setfield(L, -2,  "sqlerrcode");
    }
    set_error_mt(L);

    FreeErrorData(edata);
}


static void pllua_parse_error(lua_State *L, ErrorData *edata){
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING){
            const char *key = lua_tostring(L, -2);
            if (lua_type(L, -1) == LUA_TSTRING){
                if (strcmp(key, "message") == 0){
                    edata->message = pstrdup( lua_tostring(L, -1) );
                } else if (strcmp(key, "detail") == 0){
                    edata->detail = pstrdup( lua_tostring(L, -1) );
                }  else if (strcmp(key, "hint") == 0){
                    edata->hint = pstrdup( lua_tostring(L, -1) );
                } else if (strcmp(key, "context") == 0){
                    edata->context = pstrdup( lua_tostring(L, -1) );
                }

            }else if (lua_type(L, -1) == LUA_TNUMBER){
                if (strcmp(key, "sqlerrcode") == 0){
                    edata->sqlerrcode = (int)( lua_tonumber(L, -1) );
                }
            }
        }
        lua_pop(L, 1);
    }
}

void luatable_topgerror(lua_State *L)
{
    luatable_report(L, ERROR);
}

void luatable_report(lua_State *L, int elevel)
{
    ErrorData	edata;

    char	   *query = NULL;
    int			position = 0;

    edata.message = NULL;
    edata.sqlerrcode = 0;
    edata.detail = NULL;
    edata.hint = NULL;
    edata.context = NULL;

    pllua_parse_error(L, &edata);
    lua_pop(L, lua_gettop(L));

    elevel = Min(elevel, ERROR);

    ereport(elevel,
            (errcode(edata.sqlerrcode ? edata.sqlerrcode : ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
             errmsg_internal("%s", edata.message ? edata.message : "no exception data"),
             (edata.detail) ? errdetail_internal("%s", edata.detail) : 0,
             (edata.context) ? errcontext("%s", edata.context) : 0,
             (edata.hint) ? errhint("%s", edata.hint) : 0,
             (query) ? internalerrquery(query) : 0,
             (position) ? internalerrposition(position) : 0));
}



/* this is a copy from lua source to call debug.traceback without access it from metatables */
#define LEVELS1	12	/* size of the first part of the stack */
#define LEVELS2	10	/* size of the second part of the stack */

static lua_State *getthread (lua_State *L, int *arg) {
    if (lua_isthread(L, 1)) {
        *arg = 1;
        return lua_tothread(L, 1);
    }
    else {
        *arg = 0;
        return L;
    }
}

static int db_errorfb (lua_State *L) {
    int level;
    int firstpart = 1;  /* still before eventual `...' */
    int arg;
    lua_State *L1 = getthread(L, &arg);
    lua_Debug ar;
    luaL_Buffer b;
    if (lua_isnumber(L, arg+2)) {
        level = (int)lua_tointeger(L, arg+2);
        lua_pop(L, 1);
    }
    else
        level = (L == L1) ? 1 : 0;  /* level 0 may be this own function */
    if (lua_gettop(L) == arg)
        lua_pushliteral(L, "");
    else if (!lua_isstring(L, arg+1)) return 1;  /* message is not a string */
    else lua_pushliteral(L, "\n");

    luaL_buffinit(L, &b);
    luaL_addstring(&b, "stack traceback(");
    luaL_addstring(&b, lvm_info[pllua_getmaster_index(L)].name);
    luaL_addstring(&b, "):");
    luaL_pushresult(&b);
    //lua_pushliteral(L, "stack traceback:");
    while (lua_getstack(L1, level++, &ar)) {
        if (level > LEVELS1 && firstpart) {
            /* no more than `LEVELS2' more levels? */
            if (!lua_getstack(L1, level+LEVELS2, &ar))
                level--;  /* keep going */
            else {
                lua_pushliteral(L, "\n\t...");  /* too many levels */
                while (lua_getstack(L1, level+LEVELS2, &ar))  /* find last levels */
                    level++;
            }
            firstpart = 0;
            continue;
        }
        lua_pushliteral(L, "\n\t");
        lua_getinfo(L1, "Snl", &ar);
        lua_pushfstring(L, "%s:", ar.short_src);
        if (ar.currentline > 0)
            lua_pushfstring(L, "%d:", ar.currentline);
        if (*ar.namewhat != '\0')  /* is there a name? */
            lua_pushfstring(L, " in function " LUA_QS, ar.name);
        else {
            if (*ar.what == 'm')  /* main? */
                lua_pushfstring(L, " in main chunk");
            else if (*ar.what == 'C' || *ar.what == 't')
                lua_pushliteral(L, " ?");  /* C function or tail call */
            else
                lua_pushfstring(L, " in function <%s:%d>",
                                ar.short_src, ar.linedefined);
        }
        lua_concat(L, lua_gettop(L) - arg);
    }
    lua_concat(L, lua_gettop(L) - arg);
    return 1;
}

int traceback (lua_State *L) {
    int stateIndex = pllua_getmaster_index(L);
    if (lvm_info[stateIndex].hasTraceback)
        return 1;

    if (lua_isstring(L, 1)){ /* 'message' not a string? */
        lua_newtable(L);

        lua_pushcfunction(L, db_errorfb);
        lua_pushstring(L,""); /* empty concat string for context */
        lua_pushinteger(L, 2);  /* skip this function and traceback */
        lua_call(L, 2, 1);  /* call debug.traceback */
        lvm_info[stateIndex].hasTraceback = true;
        lua_setfield(L, -2,  "context");
        lua_swap(L); /*text <-> table*/
        lua_setfield(L, -2,  "message");
        set_error_mt(L);
        return 1;

    }else if (lua_istable(L,1)){

        lua_pushstring(L,"context");
        lua_rawget(L, -2);
        if (!lua_isstring(L, -1)){
            lua_pop(L,1);
            lua_pushstring(L,""); /* empty concat string for context */
        }

        lua_pushcfunction(L, db_errorfb);
        lua_swap(L);

        lua_pushinteger(L, 2);  /* skip this function and traceback */
        lua_call(L, 2, 1);  /* call debug.traceback */
        lvm_info[stateIndex].hasTraceback = true;

        lua_setfield(L, -2,  "context");

        return 1;
    }

    return 1;  /* keep it intact */
}

