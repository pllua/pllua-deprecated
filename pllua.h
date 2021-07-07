/*
 * PL/Lua
 * Author: Luis Carvalho <lexcarvalho at gmail.com>
 * Version: 1.0
 * Please check copyright notice at the bottom of this file
 * $Id: pllua.h,v 1.17 2009/09/19 16:20:45 carvalho Exp $
 */


#include "plluacommon.h"

/*used as a key for saving lua context lua_pushlightuserdata(p_lua_mem_cxt)
  instead of lua_pushlightuserdata((void*)L) (in SRF the L is a lua_newthread)  */
int p_lua_mem_cxt(void);
int p_lua_master_state(void);

typedef struct luaP_Buffer {
  int size;
  Datum *value;
  char *null;
} luaP_Buffer;

/* utils */
void *luaP_toudata (lua_State *L, int ud, const char *tname);
luaP_Buffer *luaP_getbuffer (lua_State *L, int n);
void init_procedure_caches(void);
/* call handler API */
lua_State *luaP_newstate (int trusted);
void luaP_close (lua_State *L);
Datum luaP_validator (lua_State *L, Oid oid);
Datum luaP_callhandler (lua_State *L, FunctionCallInfo fcinfo);
#if PG_VERSION_NUM >= 90000
Datum luaP_inlinehandler (lua_State *L, const char *source);
#endif
/* general API */
void luaP_pushdatum (lua_State *L, Datum dat, Oid type);
Datum luaP_todatum (lua_State *L, Oid type, int len, bool *isnull, int idx);

void luaP_pushtuple_trg (lua_State *L, TupleDesc desc, HeapTuple tuple,
    Oid relid, int readonly);
HeapTuple luaP_totuple (lua_State *L);
HeapTuple luaP_casttuple (lua_State *L, TupleDesc tupdesc);
/* SPI */
void luaP_pushdesctable(lua_State *L, TupleDesc desc);
void luaP_registerspi(lua_State *L);
void luaP_pushcursor (lua_State *L, Portal cursor);
void luaP_pushrecord(lua_State *L, Datum record);
Portal luaP_tocursor (lua_State *L, int pos);

/* =========================================================================

Copyright (c) 2008 Luis Carvalho

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

