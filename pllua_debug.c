#include "pllua_debug.h"

/* PostgreSQL */
#include <postgres.h>
#include <fmgr.h>
#include <funcapi.h>
#include <access/heapam.h>
#if PG_VERSION_NUM >= 90300
#include <access/htup_details.h>
#endif
#include <catalog/namespace.h>
#include <catalog/pg_proc.h>
#include <catalog/pg_type.h>
#include <commands/trigger.h>
#include <executor/spi.h>
#include <nodes/makefuncs.h>
#include <parser/parse_type.h>
#include <utils/array.h>
#include <utils/builtins.h>
#include <utils/datum.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/rel.h>
#include <utils/syscache.h>
#include <utils/typcache.h>



static  char *_location;
void setLINE(char *location)
{
    _location = location;
}


const char *getLINE(void)
{
    return _location;
}


#define DMP 2

void stackDump(lua_State *L) {
    int i=lua_gettop(L);
    ereport(INFO, (errmsg("%s", "----------------  Stack Dump ----------------")));
    while(  i   ) {
        int t = lua_type(L, i);
        switch (t) {
        case LUA_TSTRING:
            ereport(INFO, (errmsg("%d:`%s'", i, lua_tostring(L, i))));
            break;
        case LUA_TBOOLEAN:
            ereport(INFO, (errmsg("%d: %s",i,lua_toboolean(L, i) ? "true" : "false")));
            break;
        case LUA_TNUMBER:
            ereport(INFO, (errmsg("%d: %g",  i, lua_tonumber(L, i))));
            break;
        case LUA_TTABLE:
            ereport(INFO, (errmsg("%d: table",  i)));
            if (DMP==1){
                /* table is in the stack at index 't' */
                lua_pushnil(L);  /* first key */

                while (lua_next(L, i) != 0) {
                    /* uses 'key' (at index -2) and 'value' (at index -1) */
                    ereport(INFO, (errmsg("===%s - %s\n",
                                          lua_tostring(L, -2),
                                          lua_typename(L, lua_type(L, -1)))));
                    /* removes 'value'; keeps 'key' for next iteration */
                    lua_pop(L, 1);
                }
            }else if (DMP == 2){
                int cnt = 0;
                lua_pushnil(L);  /* first key */

                while (lua_next(L, i) != 0) {
                    ++cnt;
                    /* removes 'value'; keeps 'key' for next iteration */
                    lua_pop(L, 1);
                }
                ereport(INFO, (errmsg("===length %i: table",  cnt)));
            }
            break;
        default:
            ereport(INFO, (errmsg("%d: %s", i, lua_typename(L, t))));
            break;
        }
        i--;
    }
    ereport(INFO, (errmsg("%s","--------------- Stack Dump Finished ---------------" )));
}
