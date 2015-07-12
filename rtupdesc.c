#include "rtupdesc.h"

static int obj_count = 0;

RTupDesc *rtupdesc_ctor(lua_State *state, TupleDesc tupdesc)
{
    void* p;
    RTupDesc* rtupdesc = 0;

    MTOLUA(state);
    p = palloc(sizeof(RTupDesc));
    if (p){
        rtupdesc = (RTupDesc*)p;
        rtupdesc->ref_count = 1;
        rtupdesc->tupdesc = CreateTupleDescCopy(tupdesc);
        obj_count += 1;
        rtupdesc->weakNodeStk = rtds_push_current(p);
    }
    MTOPG;

    return rtupdesc;
}


RTupDesc *rtupdesc_ref(RTupDesc *rtupdesc)
{
    if(rtupdesc)
        rtupdesc->ref_count += 1;
    return rtupdesc;
}


RTupDesc *rtupdesc_unref(RTupDesc *rtupdesc)
{
    if(rtupdesc){
        rtupdesc->ref_count -= 1;
        if (rtupdesc->ref_count == 0){
            rtupdesc_dtor(rtupdesc);
            return NULL;
        }
    }
    return rtupdesc;
}


void rtupdesc_freedesc(RTupDesc *rtupdesc)
{
    if (rtupdesc && rtupdesc->tupdesc){
        FreeTupleDesc(rtupdesc->tupdesc);
        rtupdesc->tupdesc = NULL;
        obj_count -= 1;
    }
}

TupleDesc rtupdesc_gettup(RTupDesc *rtupdesc)
{
    return (rtupdesc ? rtupdesc->tupdesc : NULL);
}


void rtupdesc_dtor(RTupDesc *rtupdesc)
{

    if(rtupdesc){
        rtds_remove_node(rtupdesc->weakNodeStk);

        if (rtupdesc->tupdesc){
            FreeTupleDesc(rtupdesc->tupdesc);
            rtupdesc->tupdesc = NULL;
            obj_count -= 1;
        }

        pfree(rtupdesc);

    }

}

int rtupdesc_obj_count(void)
{
    return obj_count;
}
