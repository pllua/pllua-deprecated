/*
 * plluau.c: PL/Lua call handler, untrusted
 * Author: Luis Carvalho <lexcarvalho at gmail.com>
 * Please check copyright notice at the bottom of pllua.h
 * $Id: plluau.c,v 1.4 2008/01/09 17:56:19 carvalho Exp $
 */

#include "pllua.h"

PG_MODULE_MAGIC;

static lua_State *L = NULL; /* Lua VM */

Datum plluau_call_handler(PG_FUNCTION_ARGS);
Datum plluau_validator(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(plluau_call_handler);
Datum plluau_call_handler(PG_FUNCTION_ARGS) {
  if (L == NULL) L = luaP_newstate(0, TopMemoryContext); /* untrusted */
  return luaP_callhandler(L, fcinfo);
}

PG_FUNCTION_INFO_V1(plluau_validator);
Datum plluau_validator(PG_FUNCTION_ARGS) {
  if (L == NULL) L = luaP_newstate(0, TopMemoryContext); /* trusted */
  return luaP_validator(L, PG_GETARG_OID(0));
}

