/* -----------------------
 * mcp_helper.c
 *
 * $Id: mcp_helper.c 2186 2009-06-25 12:14:51Z alexk $
 * -----------------------
 */
#include "postgres.h"

#include <sys/types.h>
#include <unistd.h>

#include "libpq/pqsignal.h"
#include "mammoth_r/forwarder.h"
#include "mammoth_r/mcp_processes.h"
#include "mammoth_r/txlog.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "utils/ps_status.h"

static void OptimizeQueue(MCPQueue *q, MCPHosts *h, ullong confirmed_recno);
static void sigquit_handler(SIGNAL_ARGS);
static void sigterm_handler(SIGNAL_ARGS);

static bool terminate = false;

void
ForwarderHelperMain(int argc, char *argv)
{
	MCPQueue   *q;
	MCPHosts   *h;
	int			rounds_to_optimize = ForwarderOptimizerRounds;

	/* we are a postmaster subprocess now */
	IsUnderPostmaster = true;
	/* reset MyProcPid */
	MyProcPid = getpid();
	
	/* Lose the postmaster's on-exit routines */
	on_exit_reset();
	
	/* identify myself via ps */
	init_ps_display("forwarder helper process", "", "", "");
	set_ps_display("", true);

	pqsignal(SIGQUIT, sigquit_handler);
	pqsignal(SIGTERM, sigterm_handler);
	pqsignal(SIGINT, SIG_IGN);

	/*
     * Create a dummy PGPROC struct in shared memory, except in the
     * EXEC_BACKEND case where this was done in SubPostmasterMain. We must do
     * this before we can use LWLocks (and in the EXEC_BACKEND case we already
     * had to do some stuff with LWLocks).
     */
#ifndef EXEC_BACKEND
    InitAuxiliaryProcess();
#endif

	/* now we can get signalled */
	PG_SETMASK(&UnBlockSig);

	/* initialize the queue */
	LWLockAcquire(MCPServerLock, LW_EXCLUSIVE);
	q = MCPQueueInit(true);
	h = MCPHostsInit();
	LWLockRelease(MCPServerLock);

	/* switch to the forwarder txlog and start it up */
	SelectActiveTxlog(true);
	LockReplicationQueue(q, LW_SHARED);
	TXLOGStartup(MCPQueueGetFirstRecno(q));
	UnlockReplicationQueue(q);

	for (;;)
	{
		/*
		 * Reduce the number of loop rounds until the queue optimization 
		 * can be checked and probably fired
		 */
		rounds_to_optimize--;

		if (terminate)
		{
			elog(LOG, "ought to quit now");
			break;
		}

		if (rounds_to_optimize <= 0)
		{
			ullong 	confirmed_recno;
			
			elog(DEBUG3, "running optimization checks");

			/* XXX: too many locks here, can we get rid of some of them ? */
			LWLockAcquire(MCPServerLock, LW_SHARED);
						
			/* prevent another process from truncating the queue */
			LWLockAcquire(ReplicationQueueTruncateLock, LW_SHARED);
			LockReplicationQueue(q, LW_EXCLUSIVE);
			MCPHostsLockAll(h, LW_EXCLUSIVE);

			/* Get the minimum among confirmed recnos of connected slaves */
			confirmed_recno = 
				MCPHostsGetMinAckedRecno(h, ServerCtl->node_pid);

			/* 
			 * Run optimization. Note that no data will be remove until later
			 * in MCPQueuePrune call.
			 */

			if (confirmed_recno > MCPQueueGetInitialRecno(q))
				OptimizeQueue(q, h, confirmed_recno);
				
			MCPHostsUnlockAll(h);

			/* Remove records up to the point calculated above */
			MCPQueuePrune(q);

			UnlockReplicationQueue(q);
			LWLockRelease(ReplicationQueueTruncateLock);
			LWLockRelease(MCPServerLock);

			/* Restore optimization counter */
			rounds_to_optimize = ForwarderOptimizerRounds;
		}

		if (terminate)
		{
			elog(LOG, "ought to quit now");
			break;
		}

		/* sleep for a bit */
		pg_usleep(ForwarderOptimizerNaptime * 1000000L);
	}
	
	WriteForwarderStateFile();	
	proc_exit(0);
}

/*
 * Caller must hold the global MCPHosts lock
 */
static void
OptimizeQueue(MCPQueue *q, MCPHosts *h, ullong confirmed_recno)
{
	ullong 	new_vrecno;
	ullong	dump_end_recno;	
	
	Assert(LWLockHeldByMe(MCPHostsLock));

	set_ps_display("performing queue optimization", true);
	elog(LOG, "performing queue optimization");

	MCPQueueLogHdrStatus(DEBUG4, q, "PRE OPTIMIZE");
	MCPHostsLogTabStatus(DEBUG4, h, -1, "PRE OPTIMIZE", ServerCtl->node_pid);
	elog(DEBUG4, "OPTIMIZE: host dump_recno "UNI_LLU, FullDumpGetStartRecno());

	dump_end_recno = FullDumpGetEndRecno();
	new_vrecno = MCPHostsGetPruningRecno(h, q, confirmed_recno, 
										 FullDumpGetStartRecno(), 
										 dump_end_recno,
										 ForwarderDumpCacheMaxSize, 
										 ServerCtl->node_pid);
	if (new_vrecno != InvalidRecno)
	{
		/* 
		 * XXX: since MCPQueuePrune check min of vrecno, frecno we should set
		 * both here. Note that we can't change that check without changing
 		 * master/slave code, since vrecno <= frecno on master, but 
 		 * vrecno >= frecno on slave.
		 */
		MCPQueueSetAckRecno(q, new_vrecno);
		MCPQueueSetFirstRecno(q, new_vrecno + 1);

		/* Check whether we should invalidate dump record numbers */
		if (dump_end_recno != InvalidRecno && new_vrecno >= dump_end_recno)
		{
			FullDumpSetStartRecno(InvalidRecno);
			FullDumpSetEndRecno(InvalidRecno);
			FullDumpSetSize(0);
			elog(DEBUG4, "full dump is removed from the queue");
		}
	}
	
	MCPQueueLogHdrStatus(DEBUG4, q, "POST OPTIMIZE");
	MCPHostsLogTabStatus(DEBUG4, h, -1, "POST OPTIMIZE", ServerCtl->node_pid);

	set_ps_display("", true);
}

static void
sigquit_handler(SIGNAL_ARGS)
{
	exit(1);
}

static void
sigterm_handler(SIGNAL_ARGS)
{
	elog(LOG, "got TERM signal");
	terminate = true;
}