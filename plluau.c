/*
 * plluau.c: PL/Lua call handler, untrusted
 * Author: Luis Carvalho <lexcarvalho at gmail.com>
 * Please check copyright notice at the bottom of pllua.h
 * $Id: plluau.c,v 1.1 2007/09/21 03:20:52 carvalho Exp $
 */

#include "pllua.h"

static lua_State *L = NULL; /* Lua VM */

PG_MODULE_MAGIC;

Datum plluau_call_handler(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(plluau_call_handler);
Datum plluau_call_handler(PG_FUNCTION_ARGS) {
  Datum retval = 0;
  luaP_Info *fi = NULL;
  bool istrigger;
  if (L == NULL) L = luaP_newstate(0); /* untrusted */
  if (SPI_connect() != SPI_OK_CONNECT)
    elog(ERROR, "[pllua]: could not connect to SPI manager");
  PG_TRY();
  {
    istrigger = CALLED_AS_TRIGGER(fcinfo);
    fi = luaP_pushfunction(L, fcinfo, istrigger);
    if (istrigger) {
      TriggerData *trigdata = (TriggerData *) fcinfo->context;
      int i, nargs;
      luaP_preptrigger(L, trigdata); /* set global trigger table */
      nargs = trigdata->tg_trigger->tgnargs;
      for (i = 0; i < nargs; i++) /* push args */
        lua_pushstring(L, trigdata->tg_trigger->tgargs[i]);
      if (lua_pcall(L, nargs, 0, 0))
        elog(ERROR, "[runtime]: %s", lua_tostring(L, -1));
      if (TRIGGER_FIRED_FOR_ROW(trigdata->tg_event)
          && !TRIGGER_FIRED_BY_DELETE(trigdata->tg_event)
          && TRIGGER_FIRED_BEFORE(trigdata->tg_event))
        retval = luaP_gettriggerresult(L);
      luaP_cleantrigger(L);
    }
    else { /* called as function */
      if (fi->result_isset) { /* SETOF? */
        int status;
        ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
        if (fi->L == NULL) { /* first call? */
          if (!rsi || !IsA(rsi, ReturnSetInfo)
              || (rsi->allowedModes & SFRM_ValuePerCall) == 0)
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("set-valued function called in context"
                   "that cannot accept a set")));
          rsi->returnMode = SFRM_ValuePerCall;
          fi->L = lua_newthread(L);
          lua_pushlightuserdata(L, (void *) fi->L);
          lua_pushvalue(L, -2); /* thread */
          lua_rawset(L, LUA_REGISTRYINDEX);
          lua_pop(L, 1); /* new thread */
        }
        lua_xmove(L, fi->L, 1); /* function */
        luaP_pushargs(fi->L, fcinfo, fi);
        status = lua_resume(fi->L, fi->nargs);
        if (status == LUA_YIELD && !lua_isnil(fi->L, -1)) {
          rsi->isDone = ExprMultipleResult;
          retval = luaP_getresult(fi->L, fcinfo, fi->result);
        }
        else if (status == 0 || lua_isnil(fi->L, -1)) { /* last call? */
          rsi->isDone = ExprEndResult;
          lua_pushlightuserdata(L, (void *) fi->L);
          lua_pushnil(L);
          lua_rawset(L, LUA_REGISTRYINDEX);
          fcinfo->isnull = true;
          retval = (Datum) 0;
        }
        else
          elog(ERROR, "[runtime]: %s", lua_tostring(fi->L, -1));
      }
      else {
        luaP_pushargs(L, fcinfo, fi);
        if (lua_pcall(L, fcinfo->nargs, 1, 0))
          elog(ERROR, "[runtime]: %s", lua_tostring(L, -1));
        retval = luaP_getresult(L, fcinfo, fi->result);
      }
    }
  }
  PG_CATCH();
  {
    if (L != NULL) {
      lua_settop(L, 0); /* clear Lua stack */
      if (fi != NULL && fi->result_isset
          && fi->L != NULL) { /* clean thread? */
        lua_pushlightuserdata(L, (void *) fi->L);
        lua_pushnil(L);
        lua_rawset(L, LUA_REGISTRYINDEX);
      }
      luaP_cleantrigger(L);
    }
    fcinfo->isnull = true;
    retval = (Datum) 0;
    PG_RE_THROW();
  }
  PG_END_TRY();
  if (SPI_finish() != SPI_OK_FINISH)
    elog(ERROR, "[pllua]: could not disconnect from SPI manager");
  return retval;
}

