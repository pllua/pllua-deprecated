/*
 * PL/Lua
 * Author: Luis Carvalho <lexcarvalho at gmail.com>
 * Please check copyright notice at the bottom of this file
 * $Id: pllua.h,v 1.5 2007/09/21 03:20:52 carvalho Exp $
 */

/* PostgreSQL */
#include <postgres.h>
#include <fmgr.h>
#include <funcapi.h>
#include <access/heapam.h>
#include <catalog/namespace.h>
#include <catalog/pg_proc.h>
#include <catalog/pg_type.h>
#include <commands/trigger.h>
#include <executor/spi.h>
#include <nodes/makefuncs.h>
#include <parser/parse_type.h>
#include <utils/memutils.h>
#include <utils/syscache.h>
#include <utils/typcache.h>
/* Lua */
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

/* extended function info */
typedef struct luaP_Info {
  int oid;
  int nargs;
  Oid *arg;
  Oid result;
  bool result_isset;
  lua_State *L; /* thread for SETOF iterator */
} luaP_Info;


lua_State *luaP_newstate (int trusted);
luaP_Info *luaP_pushfunction (lua_State *L, FunctionCallInfo fcinfo,
    bool istrigger);
void luaP_pushargs (lua_State *L, FunctionCallInfo fcinfo, luaP_Info *fi);
Datum luaP_getresult (lua_State *L, FunctionCallInfo fcinfo, Oid type);

void luaP_preptrigger (lua_State *L, TriggerData *tdata);
Datum luaP_gettriggerresult (lua_State *L);
void luaP_cleantrigger (lua_State *L);

void luaP_pushdatum (lua_State *L, Datum dat, Oid type);
Datum luaP_todatum (lua_State *L, Oid type, int len, bool *isnull);
void luaP_pushtuple (lua_State *L, TupleDesc desc, HeapTuple tuple,
    Oid relid, int readonly);
HeapTuple luaP_totuple (lua_State *L);

void luaP_pushdesctable(lua_State *L, TupleDesc desc);
void luaP_registerspi(lua_State *L);

/* =========================================================================

Copyright (c) 2007 Luis Carvalho

Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation files
(the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify,
merge, publish, distribute, sublicense, and/or sell copies of the
Software, and to permit persons to whom the Software is furnished
to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

========================================================================= */

