#ifndef PLLUA_ERRORS_H
#define PLLUA_ERRORS_H

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <postgres.h>

#define PLLUA_PG_CATCH_RETHROW(source_code)  do\
{\
    MemoryContext ____oldContext = CurrentMemoryContext;\
    PG_TRY();\
    {\
        source_code\
    }\
    PG_CATCH();\
    {\
        lua_pop(L, lua_gettop(L));\
        push_spi_error(L, ____oldContext);\
        return lua_error(L);\
    }PG_END_TRY();\
}while(0)

#if defined(PLLUA_DEBUG)
#define luapg_error(L, tag) do{\
    if (lua_type(L, -1) == LUA_TSTRING){ \
        const char *err = pstrdup( lua_tostring((L), -1)); \
        lua_pop(L, lua_gettop(L));\
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), \
             errmsg("[pllua]: error: %s", tag), \
             errdetail("%s", err)));\
    }else {\
        luatable_topgerror(L);\
    }\
    }while(0)
#else
#define luapg_error(L, tag)do{\
  if (lua_type(L, -1) == LUA_TSTRING){ \
    const char *err = pstrdup( lua_tostring((L), -1)); \
    lua_pop(L, lua_gettop(L));\
    ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), \
      errmsg("[pllua]: " tag " error"),\
      errdetail("%s", err)));\
  }else {\
        luatable_topgerror(L);\
  }\
}while(0)
#endif

/*shows error as "error text" instead of  "[string "anonymous"]:2: error text*/
#define luaL_error luaL_error_skip_where

int luaB_assert (lua_State *L);
int luaB_error (lua_State *L);


int luaL_error_skip_where (lua_State *L, const char *fmt, ...);

void register_error_mt(lua_State * L);
void set_error_mt(lua_State * L);

void push_spi_error(lua_State *L, MemoryContext oldcontext);
void luatable_topgerror(lua_State *L);
void luatable_report(lua_State *L, int elevel);


int traceback (lua_State *L) ;

#endif // PLLUA_ERRORS_H
