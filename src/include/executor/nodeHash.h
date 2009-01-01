/*-------------------------------------------------------------------------
 *
 * nodeHash.h
 *	  prototypes for nodeHash.c
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEHASH_H
#define NODEHASH_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsHash(Hash *node);
extern HashState *ExecInitHash(Hash *node, EState *estate, int eflags);
extern TupleTableSlot *ExecHash(HashState *node);
extern Node *MultiExecHash(HashState *node);
extern void ExecEndHash(HashState *node);
extern void ExecReScanHash(HashState *node, ExprContext *exprCtxt);

extern HashJoinTable ExecHashTableCreate(Hash *node, List *hashOperators);
extern void ExecHashTableDestroy(HashJoinTable hashtable);
extern void ExecHashTableInsert(HashJoinTable hashtable,
					TupleTableSlot *slot,
					uint32 hashvalue);
extern bool ExecHashGetHashValue(HashJoinTable hashtable,
					 ExprContext *econtext,
					 List *hashkeys,
					 bool outer_tuple,
					 bool keep_nulls,
					 uint32 *hashvalue);
extern void ExecHashGetBucketAndBatch(HashJoinTable hashtable,
						  uint32 hashvalue,
						  int *bucketno,
						  int *batchno);
extern HashJoinTuple ExecScanHashBucket(HashJoinState *hjstate,
				   ExprContext *econtext);
extern void ExecHashTableReset(HashJoinTable hashtable);
extern void ExecChooseHashTableSize(double ntuples, int tupwidth,
						int *numbuckets,
						int *numbatches);

#endif   /* NODEHASH_H */
