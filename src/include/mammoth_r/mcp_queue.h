/*-----------------------
 * mcp_queue.h
 * 		MCP Queue header definitions
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group,
 * Copyright (c) 2006, Command Prompt, Inc.
 *
 * $Id: mcp_queue.h 2109 2009-04-23 16:46:20Z alvherre $
 * ------------------------
 */
#ifndef MCP_QUEUE_H
#define MCP_QUEUE_H

#include "mammoth_r/mcp_file.h"
#include "storage/lwlock.h"


#define InvalidRecno	(UINT64CONST(0))

typedef enum
{
	MCPQUnsynced,
	MCPQSynced
} MCPQSync;

#define	MCPQSyncAsString(sync) \
	((sync == MCPQUnsynced) ? "Unsynced" : \
	(sync == MCPQSynced) ? "Synced" : \
	"Unknown Sync Value")


typedef struct
{
	uint16		dh_id;			/* Must be equal to DATA_HEADER_ID */
	uint32		dh_flags;		/* per-transaction flags, like dump/data etc. */
	off_t		dh_listoffset;	/* starting offset of the tablelist data */
	off_t		dh_len;			/* length of transaction data, incl. header */
} TxDataHeader;

#define DATA_HEADER_ID 0x5458
 
/* struct definition appears in mcp_queue.c */
typedef struct MCPQueue MCPQueue;

/* queue initialization, open and close functions */
extern void		BootStrapMCPQueue(void);
extern Size		MCPQueueShmemSize(void);
extern void		MCPQueueShmemInit(void);
extern MCPQueue *MCPQueueInit(bool forwarder);
extern void		MCPQueueDestroy(MCPQueue *q);

/* queue read-write functions */
extern void		MCPQueueReadDataHeader(MCPQueue *q, TxDataHeader *hdr);
extern bool		MCPQueueReadData(MCPQueue *q, void  *buf, ssize_t len,
								 bool eof_allowed);
extern void		MCPQueuePutData(MCPQueue *q, void *data, ssize_t len);

/* queue optimization functions */
extern ssize_t	MCPQueueCalculateSize(MCPQueue *q, ullong recno);
extern void		MCPQueuePrune(MCPQueue *q);
extern void		MCPQueueCleanup(MCPQueue *q, ullong recno);

/* queue status, get and set */
extern MCPFile *MCPQueueGetDatafile(MCPQueue *q);
extern void		MCPQueueSetFirstRecno(MCPQueue *q, ullong recno);
extern ullong	MCPQueueGetFirstRecno(MCPQueue *q);
extern void 	MCPQueueSetLastRecno(MCPQueue *q, ullong lrecno);
extern ullong	MCPQueueGetLastRecno(MCPQueue *q);
extern ullong	MCPQueueGetAckRecno(MCPQueue *q);
extern void 	MCPQueueSetAckRecno(MCPQueue *q, ullong vrecno);
extern ullong	MCPQueueGetInitialRecno(MCPQueue *q);
extern void 	MCPQueueSetInitialRecno(MCPQueue *q, ullong brecno);
extern bool		MCPQueueIsEmpty(MCPQueue *q);
extern MCPQSync	MCPQueueGetSync(MCPQueue *q);
extern void		MCPQueueSetSync(MCPQueue *q, MCPQSync sync);
extern time_t 	MCPQueueGetEnqueueTimestamp(MCPQueue *q);
extern void 	MCPQueueSetEnqueueTimestamp(MCPQueue *q);
extern time_t 	MCPQueueGetDequeueTimestamp(MCPQueue *q);
extern void 	MCPQueueSetDequeueTimestamp(MCPQueue *q);

/* misc functions */
extern void		MCPQueueLogHdrStatus(int elevel, MCPQueue *queue, char *prefix);
extern char	   *MCPQueueGetLocalFilename(MCPQueue *q, char *base);

/* functions dealing with invidual transactions */
extern void		MCPQueueNextTx(MCPQueue *q);
extern void		MCPQueueTxOpen(MCPQueue *q, ullong recno);
extern void		MCPQueueTxClose(MCPQueue *q);
extern void		MCPQueueTxCommit(MCPQueue *q, ullong recno);
extern ullong	MCPQueueCommit(MCPQueue *q, MCPFile *txdata, ullong recno);

/* functions to lock/unlock the queue */
extern void 	LockReplicationQueue(MCPQueue *q, LWLockMode mode);
extern void 	UnlockReplicationQueue(MCPQueue *q);
extern bool 	QueueLockHeldByMe(MCPQueue *q);

#endif   /* MCP_QUEUE_H */