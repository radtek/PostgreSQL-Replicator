/*-------------------------------------------------------------------------
 *
 * nodeSetOp.h
 *
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODESETOP_H
#define NODESETOP_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsSetOp(SetOp *node);
extern SetOpState *ExecInitSetOp(SetOp *node, EState *estate);
extern TupleTableSlot *ExecSetOp(SetOpState *node);
extern void ExecEndSetOp(SetOpState *node);
extern void ExecReScanSetOp(SetOpState *node, ExprContext *exprCtxt);

#endif   /* NODESETOP_H */
