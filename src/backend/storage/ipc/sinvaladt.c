/*-------------------------------------------------------------------------
 *
 * sinvaladt.c
 *	  POSTGRES shared cache invalidation data manager.
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>

#include "miscadmin.h"
#include "storage/backendid.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "storage/sinvaladt.h"
#include "storage/spin.h"


/*
 * Conceptually, the shared cache invalidation messages are stored in an
 * infinite array, where maxMsgNum is the next array subscript to store a
 * submitted message in, minMsgNum is the smallest array subscript containing
 * a message not yet read by all backends, and we always have maxMsgNum >=
 * minMsgNum.  (They are equal when there are no messages pending.)  For each
 * active backend, there is a nextMsgNum pointer indicating the next message it
 * needs to read; we have maxMsgNum >= nextMsgNum >= minMsgNum for every
 * backend.
 *
 * (In the current implementation, minMsgNum is a lower bound for the
 * per-process nextMsgNum values, but it isn't rigorously kept equal to the
 * smallest nextMsgNum --- it may lag behind.  We only update it when
 * SICleanupQueue is called, and we try not to do that often.)
 *
 * In reality, the messages are stored in a circular buffer of MAXNUMMESSAGES
 * entries.  We translate MsgNum values into circular-buffer indexes by
 * computing MsgNum % MAXNUMMESSAGES (this should be fast as long as
 * MAXNUMMESSAGES is a constant and a power of 2).	As long as maxMsgNum
 * doesn't exceed minMsgNum by more than MAXNUMMESSAGES, we have enough space
 * in the buffer.  If the buffer does overflow, we recover by setting the
 * "reset" flag for each backend that has fallen too far behind.  A backend
 * that is in "reset" state is ignored while determining minMsgNum.  When
 * it does finally attempt to receive inval messages, it must discard all
 * its invalidatable state, since it won't know what it missed.
 *
 * To reduce the probability of needing resets, we send a "catchup" interrupt
 * to any backend that seems to be falling unreasonably far behind.  The
 * normal behavior is that at most one such interrupt is in flight at a time;
 * when a backend completes processing a catchup interrupt, it executes
 * SICleanupQueue, which will signal the next-furthest-behind backend if
 * needed.  This avoids undue contention from multiple backends all trying
 * to catch up at once.  However, the furthest-back backend might be stuck
 * in a state where it can't catch up.  Eventually it will get reset, so it
 * won't cause any more problems for anyone but itself.  But we don't want
 * to find that a bunch of other backends are now too close to the reset
 * threshold to be saved.  So SICleanupQueue is designed to occasionally
 * send extra catchup interrupts as the queue gets fuller, to backends that
 * are far behind and haven't gotten one yet.  As long as there aren't a lot
 * of "stuck" backends, we won't need a lot of extra interrupts, since ones
 * that aren't stuck will propagate their interrupts to the next guy.
 *
 * We would have problems if the MsgNum values overflow an integer, so
 * whenever minMsgNum exceeds MSGNUMWRAPAROUND, we subtract MSGNUMWRAPAROUND
 * from all the MsgNum variables simultaneously.  MSGNUMWRAPAROUND can be
 * large so that we don't need to do this often.  It must be a multiple of
 * MAXNUMMESSAGES so that the existing circular-buffer entries don't need
 * to be moved when we do it.
 *
 * Access to the shared sinval array is protected by two locks, SInvalReadLock
 * and SInvalWriteLock.  Readers take SInvalReadLock in shared mode; this
 * authorizes them to modify their own ProcState but not to modify or even
 * look at anyone else's.  When we need to perform array-wide updates,
 * such as in SICleanupQueue, we take SInvalReadLock in exclusive mode to
 * lock out all readers.  Writers take SInvalWriteLock (always in exclusive
 * mode) to serialize adding messages to the queue.  Note that a writer
 * can operate in parallel with one or more readers, because the writer
 * has no need to touch anyone's ProcState, except in the infrequent cases
 * when SICleanupQueue is needed.  The only point of overlap is that
 * the writer wants to change maxMsgNum while readers need to read it.
 * We deal with that by having a spinlock that readers must take for just
 * long enough to read maxMsgNum, while writers take it for just long enough
 * to write maxMsgNum.  (The exact rule is that you need the spinlock to
 * read maxMsgNum if you are not holding SInvalWriteLock, and you need the
 * spinlock to write maxMsgNum unless you are holding both locks.)
 *
 * Note: since maxMsgNum is an int and hence presumably atomically readable/
 * writable, the spinlock might seem unnecessary.  The reason it is needed
 * is to provide a memory barrier: we need to be sure that messages written
 * to the array are actually there before maxMsgNum is increased, and that
 * readers will see that data after fetching maxMsgNum.  Multiprocessors
 * that have weak memory-ordering guarantees can fail without the memory
 * barrier instructions that are included in the spinlock sequences.
 */


/*
 * Configurable parameters.
 *
 * MAXNUMMESSAGES: max number of shared-inval messages we can buffer.
 * Must be a power of 2 for speed.
 *
 * MSGNUMWRAPAROUND: how often to reduce MsgNum variables to avoid overflow.
 * Must be a multiple of MAXNUMMESSAGES.  Should be large.
 *
 * CLEANUP_MIN: the minimum number of messages that must be in the buffer
 * before we bother to call SICleanupQueue.
 *
 * CLEANUP_QUANTUM: how often (in messages) to call SICleanupQueue once
 * we exceed CLEANUP_MIN.  Should be a power of 2 for speed.
 *
 * SIG_THRESHOLD: the minimum number of messages a backend must have fallen
 * behind before we'll send it SIGUSR1.
 *
 * WRITE_QUANTUM: the max number of messages to push into the buffer per
 * iteration of SIInsertDataEntries.  Noncritical but should be less than
 * CLEANUP_QUANTUM, because we only consider calling SICleanupQueue once
 * per iteration.
 */

#define MAXNUMMESSAGES 4096
#define MSGNUMWRAPAROUND (MAXNUMMESSAGES * 262144)
#define CLEANUP_MIN (MAXNUMMESSAGES / 2)
#define CLEANUP_QUANTUM (MAXNUMMESSAGES / 16)
#define SIG_THRESHOLD (MAXNUMMESSAGES / 2)
#define WRITE_QUANTUM 64

/* Per-backend state in shared invalidation structure */
typedef struct ProcState
{
	/* procPid is zero in an inactive ProcState array entry. */
	pid_t		procPid;		/* PID of backend, for signaling */
	/* nextMsgNum is meaningless if procPid == 0 or resetState is true. */
	int			nextMsgNum;		/* next message number to read */
	bool		resetState;		/* backend needs to reset its state */
	bool		signaled;		/* backend has been sent catchup signal */

	/*
	 * Next LocalTransactionId to use for each idle backend slot.  We keep
	 * this here because it is indexed by BackendId and it is convenient to
	 * copy the value to and from local memory when MyBackendId is set.
	 * It's meaningless in an active ProcState entry.
	 */
	LocalTransactionId nextLXID;
} ProcState;

/* Shared cache invalidation memory segment */
typedef struct SISeg
{
	/*
	 * General state information
	 */
	int			minMsgNum;		/* oldest message still needed */
	int			maxMsgNum;		/* next message number to be assigned */
	int			nextThreshold;	/* # of messages to call SICleanupQueue */
	int			lastBackend;	/* index of last active procState entry, +1 */
	int			maxBackends;	/* size of procState array */

	slock_t		msgnumLock;		/* spinlock protecting maxMsgNum */

	/*
	 * Circular buffer holding shared-inval messages
	 */
	SharedInvalidationMessage buffer[MAXNUMMESSAGES];

	/*
	 * Per-backend state info.
	 *
	 * We declare procState as 1 entry because C wants a fixed-size array, but
	 * actually it is maxBackends entries long.
	 */
	ProcState	procState[1];	/* reflects the invalidation state */
} SISeg;

static SISeg *shmInvalBuffer;	/* pointer to the shared inval buffer */


static LocalTransactionId nextLocalTransactionId;

static void CleanupInvalidationState(int status, Datum arg);


/*
 * SInvalShmemSize --- return shared-memory space needed
 */
Size
SInvalShmemSize(void)
{
	Size		size;

	size = offsetof(SISeg, procState);
	size = add_size(size, mul_size(sizeof(ProcState), MaxBackends));

	return size;
}

/*
 * SharedInvalBufferInit
 *		Create and initialize the SI message buffer
 */
void
CreateSharedInvalidationState(void)
{
	Size		size;
	int			i;
	bool		found;

	/* Allocate space in shared memory */
	size = offsetof(SISeg, procState);
	size = add_size(size, mul_size(sizeof(ProcState), MaxBackends));

	shmInvalBuffer = (SISeg *)
		ShmemInitStruct("shmInvalBuffer", size, &found);
	if (found)
		return;

	/* Clear message counters, save size of procState array, init spinlock */
	shmInvalBuffer->minMsgNum = 0;
	shmInvalBuffer->maxMsgNum = 0;
	shmInvalBuffer->nextThreshold = CLEANUP_MIN;
	shmInvalBuffer->lastBackend = 0;
	shmInvalBuffer->maxBackends = MaxBackends;
	SpinLockInit(&shmInvalBuffer->msgnumLock);

	/* The buffer[] array is initially all unused, so we need not fill it */

	/* Mark all backends inactive, and initialize nextLXID */
	for (i = 0; i < shmInvalBuffer->maxBackends; i++)
	{
		shmInvalBuffer->procState[i].procPid = 0;			/* inactive */
		shmInvalBuffer->procState[i].nextMsgNum = 0;		/* meaningless */
		shmInvalBuffer->procState[i].resetState = false;
		shmInvalBuffer->procState[i].signaled = false;
		shmInvalBuffer->procState[i].nextLXID = InvalidLocalTransactionId;
	}
}

/*
 * SharedInvalBackendInit
 *		Initialize a new backend to operate on the sinval buffer
 */
void
SharedInvalBackendInit(void)
{
	int			index;
	ProcState  *stateP = NULL;
	SISeg	   *segP = shmInvalBuffer;

	/*
	 * This can run in parallel with read operations, and for that matter
	 * with write operations; but not in parallel with additions and removals
	 * of backends, nor in parallel with SICleanupQueue.  It doesn't seem
	 * worth having a third lock, so we choose to use SInvalWriteLock to
	 * serialize additions/removals.
	 */
	LWLockAcquire(SInvalWriteLock, LW_EXCLUSIVE);

	/* Look for a free entry in the procState array */
	for (index = 0; index < segP->lastBackend; index++)
	{
		if (segP->procState[index].procPid == 0)		/* inactive slot? */
		{
			stateP = &segP->procState[index];
			break;
		}
	}

	if (stateP == NULL)
	{
		if (segP->lastBackend < segP->maxBackends)
		{
			stateP = &segP->procState[segP->lastBackend];
			Assert(stateP->procPid == 0);
			segP->lastBackend++;
		}
		else
		{
			/*
			 * out of procState slots: MaxBackends exceeded -- report normally
			 */
			MyBackendId = InvalidBackendId;
			LWLockRelease(SInvalWriteLock);
			ereport(FATAL,
					(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
					 errmsg("sorry, too many clients already")));
		}
	}

	MyBackendId = (stateP - &segP->procState[0]) + 1;

	/* Advertise assigned backend ID in MyProc */
	MyProc->backendId = MyBackendId;

	/* Fetch next local transaction ID into local memory */
	nextLocalTransactionId = stateP->nextLXID;

	/* mark myself active, with all extant messages already read */
	stateP->procPid = MyProcPid;
	stateP->nextMsgNum = segP->maxMsgNum;
	stateP->resetState = false;
	stateP->signaled = false;

	LWLockRelease(SInvalWriteLock);

	/* register exit routine to mark my entry inactive at exit */
	on_shmem_exit(CleanupInvalidationState, PointerGetDatum(segP));

	elog(DEBUG4, "my backend id is %d", MyBackendId);
}

/*
 * CleanupInvalidationState
 *		Mark the current backend as no longer active.
 *
 * This function is called via on_shmem_exit() during backend shutdown.
 *
 * arg is really of type "SISeg*".
 */
static void
CleanupInvalidationState(int status, Datum arg)
{
	SISeg	   *segP = (SISeg *) DatumGetPointer(arg);
	ProcState  *stateP;
	int			i;

	Assert(PointerIsValid(segP));

	LWLockAcquire(SInvalWriteLock, LW_EXCLUSIVE);

	stateP = &segP->procState[MyBackendId - 1];

	/* Update next local transaction ID for next holder of this backendID */
	stateP->nextLXID = nextLocalTransactionId;

	/* Mark myself inactive */
	stateP->procPid = 0;
	stateP->nextMsgNum = 0;
	stateP->resetState = false;
	stateP->signaled = false;

	/* Recompute index of last active backend */
	for (i = segP->lastBackend; i > 0; i--)
	{
		if (segP->procState[i - 1].procPid != 0)
			break;
	}
	segP->lastBackend = i;

	LWLockRelease(SInvalWriteLock);
}

/*
 * BackendIdIsActive
 *		Test if the given backend ID is currently assigned to a process.
 */
bool
BackendIdIsActive(int backendID)
{
	bool		result;
	SISeg	   *segP = shmInvalBuffer;

	/* Need to lock out additions/removals of backends */
	LWLockAcquire(SInvalWriteLock, LW_SHARED);

	if (backendID > 0 && backendID <= segP->lastBackend)
	{
		ProcState  *stateP = &segP->procState[backendID - 1];

		result = (stateP->procPid != 0);
	}
	else
		result = false;

	LWLockRelease(SInvalWriteLock);

	return result;
}

/*
 * SIInsertDataEntries
 *		Add new invalidation message(s) to the buffer.
 */
void
SIInsertDataEntries(const SharedInvalidationMessage *data, int n)
{
	SISeg	   *segP = shmInvalBuffer;

	/*
	 * N can be arbitrarily large.  We divide the work into groups of no more
	 * than WRITE_QUANTUM messages, to be sure that we don't hold the lock for
	 * an unreasonably long time.  (This is not so much because we care about
	 * letting in other writers, as that some just-caught-up backend might be
	 * trying to do SICleanupQueue to pass on its signal, and we don't want it
	 * to have to wait a long time.)  Also, we need to consider calling
	 * SICleanupQueue every so often.
	 */
	while (n > 0)
	{
		int		nthistime = Min(n, WRITE_QUANTUM);
		int		numMsgs;
		int		max;

		n -= nthistime;

		LWLockAcquire(SInvalWriteLock, LW_EXCLUSIVE);

		/*
		 * If the buffer is full, we *must* acquire some space.  Clean the
		 * queue and reset anyone who is preventing space from being freed.
		 * Otherwise, clean the queue only when it's exceeded the next
		 * fullness threshold.  We have to loop and recheck the buffer state
		 * after any call of SICleanupQueue.
		 */
		for (;;)
		{
			numMsgs = segP->maxMsgNum - segP->minMsgNum;
			if (numMsgs + nthistime > MAXNUMMESSAGES ||
				numMsgs >= segP->nextThreshold)
				SICleanupQueue(true, nthistime);
			else
				break;
		}

		/*
		 * Insert new message(s) into proper slot of circular buffer
		 */
		max = segP->maxMsgNum;
		while (nthistime-- > 0)
		{
			segP->buffer[max % MAXNUMMESSAGES] = *data++;
			max++;
		}

		/* Update current value of maxMsgNum using spinlock */
		{
			/* use volatile pointer to prevent code rearrangement */
			volatile SISeg *vsegP = segP;

			SpinLockAcquire(&vsegP->msgnumLock);
			vsegP->maxMsgNum = max;
			SpinLockRelease(&vsegP->msgnumLock);
		}

		LWLockRelease(SInvalWriteLock);
	}
}

/*
 * SIGetDataEntries
 *		get next SI message(s) for current backend, if there are any
 *
 * Possible return values:
 *	0:   no SI message available
 *	n>0: next n SI messages have been extracted into data[]
 * -1:   SI reset message extracted
 *
 * If the return value is less than the array size "datasize", the caller
 * can assume that there are no more SI messages after the one(s) returned.
 * Otherwise, another call is needed to collect more messages.
 *
 * NB: this can run in parallel with other instances of SIGetDataEntries
 * executing on behalf of other backends, since each instance will modify only
 * fields of its own backend's ProcState, and no instance will look at fields
 * of other backends' ProcStates.  We express this by grabbing SInvalReadLock
 * in shared mode.  Note that this is not exactly the normal (read-only)
 * interpretation of a shared lock! Look closely at the interactions before
 * allowing SInvalReadLock to be grabbed in shared mode for any other reason!
 *
 * NB: this can also run in parallel with SIInsertDataEntries.  It is not
 * guaranteed that we will return any messages added after the routine is
 * entered.
 *
 * Note: we assume that "datasize" is not so large that it might be important
 * to break our hold on SInvalReadLock into segments.
 */
int
SIGetDataEntries(SharedInvalidationMessage *data, int datasize)
{
	SISeg	   *segP;
	ProcState  *stateP;
	int			max;
	int			n;
	
	LWLockAcquire(SInvalReadLock, LW_SHARED);

	segP = shmInvalBuffer;
	stateP = &segP->procState[MyBackendId - 1];

	/* Fetch current value of maxMsgNum using spinlock */
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile SISeg *vsegP = segP;

		SpinLockAcquire(&vsegP->msgnumLock);
		max = vsegP->maxMsgNum;
		SpinLockRelease(&vsegP->msgnumLock);
	}

	if (stateP->resetState)
	{
		/*
		 * Force reset.  We can say we have dealt with any messages added
		 * since the reset, as well; and that means we should clear the
		 * signaled flag, too.
		 */
		stateP->nextMsgNum = max;
		stateP->resetState = false;
		stateP->signaled = false;
		LWLockRelease(SInvalReadLock);
		return -1;
	}

	/*
	 * Retrieve messages and advance backend's counter, until data array is
	 * full or there are no more messages.
	 *
	 * There may be other backends that haven't read the message(s), so we
	 * cannot delete them here.  SICleanupQueue() will eventually remove them
	 * from the queue.
	 */
	n = 0;
	while (n < datasize && stateP->nextMsgNum < max)
	{
		data[n++] = segP->buffer[stateP->nextMsgNum % MAXNUMMESSAGES];
		stateP->nextMsgNum++;
	}

	/*
	 * Reset our "signaled" flag whenever we have caught up completely.
	 */
	if (stateP->nextMsgNum >= max)
		stateP->signaled = false;

	LWLockRelease(SInvalReadLock);
	return n;
}

/*
 * SICleanupQueue
 *		Remove messages that have been consumed by all active backends
 *
 * callerHasWriteLock is TRUE if caller is holding SInvalWriteLock.
 * minFree is the minimum number of message slots to make free.
 *
 * Possible side effects of this routine include marking one or more
 * backends as "reset" in the array, and sending a catchup interrupt (SIGUSR1)
 * to some backend that seems to be getting too far behind.  We signal at
 * most one backend at a time, for reasons explained at the top of the file.
 *
 * Caution: because we transiently release write lock when we have to signal
 * some other backend, it is NOT guaranteed that there are still minFree
 * free message slots at exit.  Caller must recheck and perhaps retry.
 */
void
SICleanupQueue(bool callerHasWriteLock, int minFree)
{
	SISeg	   *segP = shmInvalBuffer;
	int			min,
				minsig,
				lowbound,
				numMsgs,
				i;
	ProcState  *needSig = NULL;

	/* Lock out all writers and readers */
	if (!callerHasWriteLock)
		LWLockAcquire(SInvalWriteLock, LW_EXCLUSIVE);
	LWLockAcquire(SInvalReadLock, LW_EXCLUSIVE);

	/*
	 * Recompute minMsgNum = minimum of all backends' nextMsgNum, identify
	 * the furthest-back backend that needs signaling (if any), and reset
	 * any backends that are too far back.
	 */
	min = segP->maxMsgNum;
	minsig = min - SIG_THRESHOLD;
	lowbound = min - MAXNUMMESSAGES + minFree;

	for (i = 0; i < segP->lastBackend; i++)
	{
		ProcState  *stateP = &segP->procState[i];
		int		n = stateP->nextMsgNum;

		/* Ignore if inactive or already in reset state */
		if (stateP->procPid == 0 || stateP->resetState)
			continue;

		/*
		 * If we must free some space and this backend is preventing it,
		 * force him into reset state and then ignore until he catches up.
		 */
		if (n < lowbound)
		{
			stateP->resetState = true;
			/* no point in signaling him ... */
			continue;
		}

		/* Track the global minimum nextMsgNum */
		if (n < min)
			min = n;

		/* Also see who's furthest back of the unsignaled backends */
		if (n < minsig && !stateP->signaled)
		{
			minsig = n;
			needSig = stateP;
		}
	}
	segP->minMsgNum = min;

	/*
	 * When minMsgNum gets really large, decrement all message counters so as
	 * to forestall overflow of the counters.  This happens seldom enough
	 * that folding it into the previous loop would be a loser.
	 */
	if (min >= MSGNUMWRAPAROUND)
	{
		segP->minMsgNum -= MSGNUMWRAPAROUND;
		segP->maxMsgNum -= MSGNUMWRAPAROUND;
		for (i = 0; i < segP->lastBackend; i++)
		{
			/* we don't bother skipping inactive entries here */
			segP->procState[i].nextMsgNum -= MSGNUMWRAPAROUND;
		}
	}

	/*
	 * Determine how many messages are still in the queue, and set the
	 * threshold at which we should repeat SICleanupQueue().
	 */
	numMsgs = segP->maxMsgNum - segP->minMsgNum;
	if (numMsgs < CLEANUP_MIN)
		segP->nextThreshold = CLEANUP_MIN;
	else
		segP->nextThreshold = (numMsgs / CLEANUP_QUANTUM + 1) * CLEANUP_QUANTUM;

	/*
	 * Lastly, signal anyone who needs a catchup interrupt.  Since kill()
	 * might not be fast, we don't want to hold locks while executing it.
	 */
	if (needSig)
	{
		pid_t	his_pid = needSig->procPid;

		needSig->signaled = true;
		LWLockRelease(SInvalReadLock);
		LWLockRelease(SInvalWriteLock);
		elog(DEBUG4, "sending sinval catchup signal to PID %d", (int) his_pid);
		kill(his_pid, SIGUSR1);
		if (callerHasWriteLock)
			LWLockAcquire(SInvalWriteLock, LW_EXCLUSIVE);
	}
	else
	{
		LWLockRelease(SInvalReadLock);
		if (!callerHasWriteLock)
			LWLockRelease(SInvalWriteLock);
	}
}


/*
 * GetNextLocalTransactionId --- allocate a new LocalTransactionId
 *
 * We split VirtualTransactionIds into two parts so that it is possible
 * to allocate a new one without any contention for shared memory, except
 * for a bit of additional overhead during backend startup/shutdown.
 * The high-order part of a VirtualTransactionId is a BackendId, and the
 * low-order part is a LocalTransactionId, which we assign from a local
 * counter.  To avoid the risk of a VirtualTransactionId being reused
 * within a short interval, successive procs occupying the same backend ID
 * slot should use a consecutive sequence of local IDs, which is implemented
 * by copying nextLocalTransactionId as seen above.
 */
LocalTransactionId
GetNextLocalTransactionId(void)
{
	LocalTransactionId result;

	/* loop to avoid returning InvalidLocalTransactionId at wraparound */
	do
	{
		result = nextLocalTransactionId++;
	} while (!LocalTransactionIdIsValid(result));

	return result;
}
