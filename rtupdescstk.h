/*
 * functions for tupledesc resource management
 * Author: Eugene Sergeev <eugeney.sergeev at gmail.com>
 * Please check copyright notice at the bottom of pllua.h
 */

#ifndef RTUPDESCSTK_H
#define RTUPDESCSTK_H

#include "plluacommon.h"

#include <stdlib.h>
struct stackType;

typedef struct RTDnode {
    void *data;
    struct RTDnode *next;
    struct RTDnode *prev;
    struct stackType *tail;
} RTDNode, *RTDNodePtr;

typedef struct stackType {
    int ref_count;
    lua_State *L;
    RTDNodePtr top;
    void *resptr;
} RTupDescStackType, *RTupDescStack;

RTupDescStack rtds_set_current(void *s);
RTupDescStack rtds_get_current(void);

int rtds_get_length(RTupDescStack S);


RTupDescStack rtds_initStack(lua_State *L);

int rtds_isempty(RTupDescStack S);


void *rtds_pop(RTupDescStack S);

void rtds_tryclean(RTupDescStack S);

RTDNodePtr rtds_push_current(void *d);
void rtds_remove_node(RTDNodePtr np);

void rtds_inuse(RTupDescStack S);
void rtds_notinuse(RTupDescStack S);

RTupDescStack rtds_unref(RTupDescStack S);
RTupDescStack rtds_free_if_not_used(RTupDescStack S);




#endif // RTUPDESCSTK_H
