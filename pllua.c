/*
 * pllua.c: PL/Lua call handler, trusted
 * Author: Luis Carvalho <lexcarvalho at gmail.com>
 * Please check copyright notice at the bottom of pllua.h
 * $Id: pllua.c,v 1.10 2007/11/08 14:56:29 carvalho Exp $
 */

#include "pllua.h"

PG_MODULE_MAGIC;

static lua_State *L = NULL; /* Lua VM */

Datum pllua_call_handler(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(pllua_call_handler);
Datum pllua_call_handler(PG_FUNCTION_ARGS) {
  if (L == NULL) L = luaP_newstate(1, TopMemoryContext); /* trusted */
  return luaP_callhandler(L, fcinfo);
}

