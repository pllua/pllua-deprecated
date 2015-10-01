#include "pllua_xact_cleanup.h"

#define MTOCOMMON {MemoryContext ___m;\
    ___m  = MemoryContextSwitchTo(cmn_ctx)
#define MTOPREV MemoryContextSwitchTo(___m);}

struct RSStack;
static MemoryContext cmn_ctx = NULL;

typedef struct {
    void *data;
    RSDtorCallback dtor;
} Resource;


typedef struct RSNode {
    void *data;
    RSDtorCallback dtor;
    struct RSNode *next;
    struct RSNode *prev;
    struct RSStack *tail;
} RSNode, *RSNodePtr;


typedef struct RSStack {
    RSNodePtr top;
} RSStack, *RSStaskPtr;

static RSStaskPtr resource_stk = NULL;

static RSNodePtr rsp_push(RSStaskPtr S, void *d, RSDtorCallback dtor) {
    RSNodePtr np;
    if (S == NULL) return NULL;
    MTOCOMMON;
    np = (RSNodePtr) palloc(sizeof(RSNode));
    MTOPREV;
    np->tail = S;
    np -> data = d;
    np -> dtor = dtor;
    np -> next = S -> top;
    np -> prev = NULL;
    if (S->top != NULL)
        S -> top->prev = np;
    S -> top = np;
    return np;
}

void *unregister_resource(void* d)
{
    RSStaskPtr S;
    RSNodePtr np;
    if (d == NULL) return NULL;
    np = (RSNodePtr)d;
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
    return NULL;
}

void *register_resource(void *d, RSDtorCallback dtor)
{
    return (void*)rsp_push(resource_stk, d, dtor);
}

static int rsp_isempty(RSStaskPtr S) {
    return (S -> top == NULL);
}

static Resource rsp_pop(RSStaskPtr S) {
    Resource hold;
    RSNodePtr temp;

    hold.data = NULL;
    hold.dtor = NULL;

    if (rsp_isempty(S)) {
        return hold;
    }
    hold.data = S -> top -> data;
    hold.dtor = S -> top -> dtor;
    temp = S -> top;
    S -> top = S -> top -> next;
    if (S->top != NULL)
        S -> top->prev = NULL;
    pfree(temp);
    return hold;
}



static RSStaskPtr rsp_initStack() {
    RSStaskPtr sp;
    MTOCOMMON;
    sp = (RSStaskPtr) palloc(sizeof(RSStack));
    MTOPREV;
    sp->top = NULL;
    return sp;
}

static void clean(RSStaskPtr S){
    Resource res = rsp_pop(S);
    while (res.data || res.dtor) {
        if (res.dtor){
            (res.dtor)(res.data);
        }else{
            pfree(res.data);
        }
        res = rsp_pop(S);
    }
}


void pllua_xact_cb(XactEvent event, void *arg)
{
    //TODO: check events
    (void)event;
    (void)arg;
    clean(resource_stk);
}


void pllua_init_common_ctx()
{
    cmn_ctx = AllocSetContextCreate(TopMemoryContext,
        "PL/Lua common context", ALLOCSET_DEFAULT_MINSIZE, ALLOCSET_DEFAULT_INITSIZE,
        ALLOCSET_DEFAULT_MAXSIZE);

    resource_stk = rsp_initStack();
}


void pllua_delete_common_ctx()
{
    pfree(resource_stk);
    MemoryContextDelete(cmn_ctx);
}


MemoryContext get_common_ctx()
{
    return cmn_ctx;
}
