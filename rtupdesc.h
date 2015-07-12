#ifndef RTUPDESC_H
#define RTUPDESC_H

#include "plluacommon.h"
#include "rtupdescstk.h"

typedef struct _RTupDesc{
    int ref_count;
    RTDNodePtr weakNodeStk;
    TupleDesc tupdesc;
} RTupDesc;

RTupDesc* rtupdesc_ctor(lua_State * state, TupleDesc tupdesc);

RTupDesc* rtupdesc_ref(RTupDesc* rtupdesc);

RTupDesc* rtupdesc_unref(RTupDesc* rtupdesc);


TupleDesc rtupdesc_gettup(RTupDesc* rtupdesc);

void rtupdesc_freedesc(RTupDesc* rtupdesc);

void rtupdesc_dtor(RTupDesc* rtupdesc);

int rtupdesc_obj_count(void);


#endif // RTUPDESC_H
