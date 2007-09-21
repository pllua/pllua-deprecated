/*
 * plluau.c: PL/Lua call handler, untrusted
 * Author: Luis Carvalho <lexcarvalho at gmail.com>
 * Please check copyright notice at the bottom of pllua.h
 * $Id: plluau.c,v 1.2 2007/09/21 03:38:01 carvalho Exp $
 */

#include "pllua.h"

static lua_State *L = NULL; /* Lua VM */

PG_MODULE_MAGIC;

Datum plluau_call_handler(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(plluau_call_handler);
Datum plluau_call_handler(PG_FUNCTION_ARGS) {
  if (L == NULL) L = luaP_newstate(0); /* untrusted */
  return luaP_callhandler(L, fcinfo);
}

