/*
 * debug helpers
 * Author: Eugene Sergeev <eugeney.sergeev at gmail.com>
 * Please check copyright notice at the bottom of pllua.h
 */

#ifndef PLLUA_DEBUG_H
#define PLLUA_DEBUG_H

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>


void setLINE(char *location);
const char * getLINE(void);


#include <stdio.h>
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define AT __FILE__ ":" TOSTRING(__LINE__)

void stackDump (lua_State *L);

#define lua_settable(...) setLINE(AT);lua_settable(__VA_ARGS__)
#define lua_rawseti(...) setLINE(AT);lua_rawseti(__VA_ARGS__)
#define lua_rawset(...) setLINE(AT);lua_rawset(__VA_ARGS__)
#define lua_setmetatable(...) setLINE(AT);lua_setmetatable(__VA_ARGS__)


#define out(...) ereport(INFO, (errmsg(__VA_ARGS__)))
#define dolog(...) ereport(LOG, (errmsg(__VA_ARGS__)))

#endif // PLLUA_DEBUG_H
