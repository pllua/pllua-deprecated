/*
 * pgfunc interface
 * Author: Eugene Sergeev <eugeney.sergeev at gmail.com>
 * Please check copyright notice at the bottom of pllua.h
 */

#ifndef PLLUA_PGFUNC_H
#define PLLUA_PGFUNC_H

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <postgres.h>

int get_pgfunc(lua_State * L);
void register_funcinfo_mt(lua_State * L);


#endif // PLLUA_PGFUNC_H
