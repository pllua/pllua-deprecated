#include "rtupdescstk.h"
#include "rtupdesc.h"

static void* current_func_cxt = NULL;
RTupDescStack rtds_set_current(void *s){
    RTupDescStack prev = current_func_cxt;
    current_func_cxt = s;
    return prev;
}

RTupDescStack rtds_get_current(void){
    return current_func_cxt;
}

static void clean(RTupDescStack S){
    void *top = rtds_pop(S);
    while (top) {
        RTupDesc* rtupdesc = (RTupDesc*)top;
        rtupdesc_freedesc(rtupdesc);
        rtupdesc->weakNodeStk = NULL;
        top = rtds_pop(S);
    }
}

void rtds_tryclean(RTupDescStack S){
    if (S == NULL) return;
    S->ref_count -= 1;
    if (S->ref_count != 0) return;
    clean(S);
}

void *rtds_pop(RTupDescStack S) {
    void *hold;
    RTDNodePtr temp;
    if (rtds_isempty(S)) {
        return NULL;
    }
    hold = S -> top -> data;
    temp = S -> top;
    S -> top = S -> top -> next;
    if (S->top != NULL)
        S -> top->prev = NULL;
    pfree(temp);
    return hold;
}


static RTDNodePtr rtds_push(RTupDescStack S, void *d) {
    RTDNodePtr np;
    if (S == NULL) return NULL;
    MTOLUA(S->L);
    np = (RTDNodePtr) palloc(sizeof(RTDNode));
    MTOPG;
    np->tail = S;
    np -> data = d;
    np -> next = S -> top;
    np -> prev = NULL;
    if (S->top != NULL)
        S -> top->prev = np;
    S -> top = np;
    return np;
}


int rtds_isempty(RTupDescStack S) {
    return (S -> top == NULL);
}


RTupDescStack rtds_initStack(lua_State *L) {
    RTupDescStack sp;
    L = pllua_getmaster(L);
    MTOLUA(L);
    sp = (RTupDescStack) palloc(sizeof(RTupDescStackType));
    MTOPG;
    sp->ref_count = 0;
    sp->L = L;
    sp->top = NULL;
    return sp;
}


RTDNodePtr rtds_push_current(void *d)
{
    return rtds_push(current_func_cxt,d);
}


RTupDescStack rtds_unref(RTupDescStack S)
{
    if (S == NULL) return NULL;
    rtds_notinuse(S);
    return rtds_free_if_not_used(S);
}


void rtds_remove_node(RTDNodePtr np)
{
    RTupDescStack S;
    if (np == NULL) return;
    S = np->tail;
    if (S->top == np){
        S->top = np->next;
        if (S->top){
            S->top->prev = NULL;
        }
    }else{
        if (np->prev)
            np->prev->next = np->next;
        if (np->next)
            np->next->prev = np->prev;
    }
    pfree(np);

}


int rtds_get_length(RTupDescStack S)
{
    int count = 0;
    RTDNodePtr node = S->top;
    while(node){
        count += 1;
        node = node->next;
    }
    return count;
}


void rtds_inuse(RTupDescStack S)
{
    S->ref_count += 1;
}


void rtds_notinuse(RTupDescStack S)
{
    S->ref_count -= 1;
}


RTupDescStack rtds_free_if_not_used(RTupDescStack S)
{
    if (S == NULL) return NULL;
    if (S->ref_count == 0){
        clean(S);
        pfree(S);
        return NULL;
    }
    return S;
}
