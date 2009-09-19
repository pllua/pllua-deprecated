
#ifndef _ROWSTAMP_H_
#define _ROWSTAMP_H_

/*
 * Row version check changed in 8.3
 */

#if PG_VERSION_NUM < 80300
#define ROWSTAMP_PRE83
#endif

/*
 * Row version info
 */
typedef struct RowStamp {
	TransactionId	xmin;
#ifdef ROWSTAMP_PRE83
	CommandId		cmin;
#else
	ItemPointerData		tid;
#endif
} RowStamp;

static void rowstamp_set(RowStamp *stamp, HeapTuple tup) {
	stamp->xmin = HeapTupleHeaderGetXmin(tup->t_data);
#ifdef ROWSTAMP_PRE83
	stamp->cmin = HeapTupleHeaderGetCmin(tup->t_data);
#else
	stamp->tid = tup->t_self;
#endif
}

static bool rowstamp_check(RowStamp *stamp, HeapTuple tup) {
	return stamp->xmin == HeapTupleHeaderGetXmin(tup->t_data)
#ifdef ROWSTAMP_PRE83
		&& stamp->cmin == HeapTupleHeaderGetCmin(tup->t_data);
#else
		&& ItemPointerEquals(&stamp->tid, &tup->t_self);
#endif
}

#endif

