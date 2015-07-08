#ifndef LUA_INT64_H
#define LUA_INT64_H

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <postgres.h>


void register_int64(lua_State * L);

int64 get64lua(lua_State * L,int index);
void setInt64lua(lua_State * L,int64 value);



#endif // LUA_INT64_H
