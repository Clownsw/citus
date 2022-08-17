/*
 * causal_clock.c
 *
 * Core funtion defintions to implement hybrid logical clock.
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"

#include <sys/time.h>

#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/numeric.h"
#include "nodes/pg_list.h"
#include "executor/spi.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "storage/s_lock.h"

#include "distributed/causal_clock.h"
#include "distributed/function_utils.h"
#include "distributed/listutils.h"
#include "distributed/lock_graph.h"
#include "distributed/metadata_cache.h"
#include "distributed/pg_dist_commit_transaction.h"
#include "distributed/remote_commands.h"

PG_FUNCTION_INFO_V1(citus_get_cluster_clock);
PG_FUNCTION_INFO_V1(citus_internal_adjust_local_clock_to_remote);
PG_FUNCTION_INFO_V1(citus_is_clock_after);


/*
 * Holds the cluster clock related variables in shared memory.
 */
typedef struct LogicalClockShmemData
{
	slock_t clockMutex;

	/* Logical clock value of this node */
	uint64 clusterClockValue;

	/* Tracks initialization at boot */
	bool clockInitialized;
} LogicalClockShmemData;


static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static LogicalClockShmemData *logicalClockShmem = NULL;
static uint64 GetEpochTimeMs(void);
static void AdjustLocalClockToRemote(uint64 remLogicalClockValue,
									 uint32 remCounterClockValue);
static uint64 GetClusterClock(void);
static uint64 CurrentClusterClock(List *nodeConnectionList);
static void AdjustClocks(List *nodeConnectionList, uint64 transactionClockValue);
static void LogCommitTransactionRecord(char *transactionId, uint64 transactionClockValue);
static uint64 * ExecuteQueryResults(char *query, int resultSize, int spiok_type);
static bool IsClockAfter(uint64 logicalClock1, uint32 counterClock1,
						 uint64 logicalClock2, uint32 counterClock2);

bool EnableClusterClock = false;


/*
 * GetEpochTimeMs returns the epoch value in milliseconds.
 */
static uint64
GetEpochTimeMs(void)
{
	struct timeval tp;

	gettimeofday(&tp, NULL);

	uint64 result = (uint64) (tp.tv_sec) * 1000;
	result = result + (uint64) (tp.tv_usec) / 1000;
	return result;
}


/*
 * LogicalClockShmemSize returns the size that should be allocated
 * on the shared memory for logical clock management.
 */
size_t
LogicalClockShmemSize(void)
{
	Size size = 0;

	size = add_size(size, sizeof(LogicalClockShmemData));

	return size;
}


/*
 * InitializeClusterClockMem reserves shared-memory space needed to
 * store LogicalClockShmemData, and sets the hook for initialization
 * of the same.
 */
void
InitializeClusterClockMem(void)
{
	/* On PG 15 and above, we use shmem_request_hook_type */
	#if PG_VERSION_NUM < PG_VERSION_15

	/* allocate shared memory for < 15 PG versions */
	if (!IsUnderPostmaster)
	{
		RequestAddinShmemSpace(LogicalClockShmemSize());
	}

	#endif

	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = LogicalClockShmemInit;
}


/*
 * LogicalClockShmemInit Allocates and initializes shared memory for
 * cluster clock related variables.
 */
void
LogicalClockShmemInit(void)
{
	bool alreadyInitialized = false;

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	logicalClockShmem = (LogicalClockShmemData *)
						ShmemInitStruct("Logical Clock Shmem",
										LogicalClockShmemSize(),
										&alreadyInitialized);

	if (!alreadyInitialized)
	{
		/* A zero value indicates that the clock is not adjusted yet */
		logicalClockShmem->clusterClockValue = 0;
		SpinLockInit(&logicalClockShmem->clockMutex);
	}

	LWLockRelease(AddinShmemInitLock);

	if (prev_shmem_startup_hook != NULL)
	{
		prev_shmem_startup_hook();
	}
}


/*
 * GetNextClusterClock implements the internal guts of the UDF citus_get_cluster_clock()
 */
uint64
GetClusterClock(void)
{
	uint64 epochValue = GetEpochTimeMs();

	SpinLockAcquire(&logicalClockShmem->clockMutex);

	uint64 curLogicalClockValue = GET_LOGICAL(logicalClockShmem->clusterClockValue);
	uint64 curCounterClockValue = GET_COUNTER(logicalClockShmem->clusterClockValue);

	uint64 nextClusterClockValue = 0;

	/* Check if the clock is adjusted after the boot */
	if (curLogicalClockValue == 0)
	{
		SpinLockRelease(&logicalClockShmem->clockMutex);
		ereport(ERROR, (errmsg(
							"backend never adjusted the clock, "
							"please retry")));
	}


	if (curLogicalClockValue < epochValue)
	{
		ereport(DEBUG1, (errmsg(
							 "Wall clock moved, set logical clock to "
							 "the wallclock value(%lu)", epochValue)));

		/*
		 * Wall clock moved, set logical clock to the wall
		 * clock and reset the counter
		 */
		nextClusterClockValue = SET_CLOCK_LOGICAL(epochValue);
	}

	/* Check for overflow of the counter */
	else if (curCounterClockValue == MAX_COUNTER)
	{
		ereport(DEBUG1, (errmsg("Overflow of the counter")));

		/* Increment the logical clock and reset the counter */
		curLogicalClockValue = curLogicalClockValue + 1;
		nextClusterClockValue = SET_CLOCK_LOGICAL(curLogicalClockValue);
	}
	else
	{
		ereport(DEBUG1, (errmsg(
							 "Wall clock(%lu) is ahead or equal to the logical"
							 "clock value(%lu), incrementing the counter(%lu)",
							 epochValue, curLogicalClockValue, curCounterClockValue)));

		/*
		 * Wall clock is ahead or equal to the logical clock value,
		 * increment the counter.
		 */
		nextClusterClockValue = logicalClockShmem->clusterClockValue + 1;
	}

	logicalClockShmem->clusterClockValue = nextClusterClockValue;

	SpinLockRelease(&logicalClockShmem->clockMutex);

	return nextClusterClockValue;
}


/*
 * IsClockAfter immplements the internal guts of the UDF citus_is_clock_after()
 */
static bool
IsClockAfter(uint64 logicalClock1, uint32 counterClock1,
			 uint64 logicalClock2, uint32 counterClock2)
{
	ereport(NOTICE, (errmsg(
						 "Clock1 @ LC:%lu, C:%u, "
						 "Clock2 @ LC:%lu, C:%u",
						 logicalClock1, counterClock1,
						 logicalClock2, counterClock2)));

	if (logicalClock1 != logicalClock2)
	{
		return (logicalClock1 > logicalClock2);
	}
	else
	{
		return (counterClock1 > counterClock2);
	}
}


/*
 * AdjustLocalClockToGlobal Adjusts the local shared memory clock to the
 * received value from the remote node.
 */
void
AdjustLocalClockToRemote(uint64 remLogicalClockValue, uint32 remCounterClockValue)
{
	uint64 curLogicalClockValue = GET_LOGICAL(logicalClockShmem->clusterClockValue);
	uint64 curCounterClockValue = GET_COUNTER(logicalClockShmem->clusterClockValue);
	uint64 remClusterClockValue = SET_CLOCK(remLogicalClockValue, remCounterClockValue);

	SpinLockAcquire(&logicalClockShmem->clockMutex);

	if (remLogicalClockValue < curLogicalClockValue)
	{
		/* local clock is ahead, do nothing */
		SpinLockRelease(&logicalClockShmem->clockMutex);
		return;
	}

	if (remLogicalClockValue > curLogicalClockValue)
	{
		ereport(DEBUG1, (errmsg("Adjusting to remote clock "
					"logical(%lu) counter(%u)",
						remLogicalClockValue,
						remCounterClockValue)));

		/* Pick the remote value */
		logicalClockShmem->clusterClockValue = remClusterClockValue;
		SpinLockRelease(&logicalClockShmem->clockMutex);
		return;
	}

	/*
	 * Both the logical clock values are equal, pick the larger counter.
	 */
	if (remCounterClockValue > curCounterClockValue)
	{
		ereport(DEBUG1, (errmsg("Both the logical clock values are "
					"equal, pick the larger counter (%u) %lu",
							 remCounterClockValue, remLogicalClockValue)));
		logicalClockShmem->clusterClockValue = remClusterClockValue;
	}

	SpinLockRelease(&logicalClockShmem->clockMutex);
}


/*
 * SetTransactionClusterClock() takes the connection list of participating nodes in
 * the current transaction, and polls the logical clock value of all the nodes. It
 * sets the maximum logical clock value of all the nodes in the distributed transaction
 * id, which may be used as commit order for individual objects.
 */
static uint64
CurrentClusterClock(List *nodeConnectionList)
{
	/* get clock value of the local node */
	uint64 globalClockValue = GetClusterClock();

	ereport(DEBUG1, (errmsg("Coordinator transaction clock %lu:%u",
					GET_LOGICAL(globalClockValue),
					(uint32) GET_COUNTER(globalClockValue))));

	/* get clock value from each node */
	MultiConnection *connection = NULL;
	StringInfo queryToSend = makeStringInfo();
	appendStringInfo(queryToSend,
					 "SELECT logical, counter FROM citus_get_cluster_clock();");

	foreach_ptr(connection, nodeConnectionList)
	{
		int querySent = SendRemoteCommand(connection, queryToSend->data);
		if (querySent == 0)
		{
			ReportConnectionError(connection, ERROR);
		}
	}

	/* fetch the results and pick the maximum clock value of all the nodes */
	foreach_ptr(connection, nodeConnectionList)
	{
		bool raiseInterrupts = true;

		if (PQstatus(connection->pgConn) != CONNECTION_OK)
		{
			continue;
		}

		PGresult *result = GetRemoteCommandResult(connection, raiseInterrupts);
		if (!IsResponseOK(result))
		{
			ereport(ERROR,
					(errmsg("Internal error, connection failure")));
		}

		int32 rowCount = PQntuples(result);
		int32 colCount = PQnfields(result);

		/* Although it is not expected */
		if (colCount != 2 || rowCount != 1)
		{
			ereport(ERROR,
					(errmsg("unexpected result from citus_get_cluster_clock()")));
		}

		uint64 logical = ParseIntField(result, 0, 0);
		uint32 counter = ParseIntField(result, 0, 1);
		uint64 nodeClockValue = SET_CLOCK(logical, counter);

		ereport(DEBUG1, (errmsg(
							 "Node(%lu:%u) transaction clock %lu:%u",
							 connection->connectionId, connection->port,
							 logical, counter)));

		if (nodeClockValue > globalClockValue)
		{
			globalClockValue = nodeClockValue;
		}

		PQclear(result);
		ForgetResults(connection);
	}

	ereport(DEBUG1,
			(errmsg("Final global transaction clock %lu:%u",
					GET_LOGICAL(globalClockValue),
					(uint32) GET_COUNTER(globalClockValue))));

	return globalClockValue;
}


/*
 * AdjustClocks Sets the clock value of all the nodes, participated in the
 * PREPARE of the transaction, to the maximum clock value of all the nodes.
 */
static void
AdjustClocks(List *nodeConnectionList, uint64 transactionClockValue)
{
	StringInfo queryToSend = makeStringInfo();
	uint64 transactionLogicalClock = GET_LOGICAL(transactionClockValue);
	uint32 transactionCounterClock = GET_COUNTER(transactionClockValue);

	/* Set the adjusted value locally */
	AdjustLocalClockToRemote(transactionLogicalClock, transactionCounterClock);

	/* Set the clock value on participating worker nodes */
	MultiConnection *connection = NULL;
	resetStringInfo(queryToSend);
	appendStringInfo(queryToSend,
					 "SELECT pg_catalog.citus_internal_adjust_local_clock_to_remote(%lu, %u);",
					 transactionLogicalClock, transactionCounterClock);

	foreach_ptr(connection, nodeConnectionList)
	{
		int querySent = SendRemoteCommand(connection, queryToSend->data);

		if (querySent == 0)
		{
			ReportConnectionError(connection, ERROR);
		}
	}

	/* Process the result */
	foreach_ptr(connection, nodeConnectionList)
	{
		bool raiseInterrupts = true;

		PGresult *result = GetRemoteCommandResult(connection, raiseInterrupts);

		if (!IsResponseOK(result))
		{
			ereport(ERROR,
					(errmsg("Internal error, connection failure")));
		}

		PQclear(result);
		ForgetResults(connection);
	}
}


/*
 * During prepare, once all the nodes acknowledge commit, persist the current
 * transaction id along with the clock value in the catalog.
 */
void
PrepareAndSetTransactionClock(List *transactionNodeList)
{
	if (!EnableClusterClock)
	{
		/* citus.enable_cluster_clock is false */
		return;
	}

	/*
	 * Call get_current_transaction_id UDF to persist the current
	 * distributed transaction id with the commit clock
	 */
	Oid transactionFuncOid = FunctionOid("pg_catalog", "get_current_transaction_id", 0);
	Datum transactionIdHeapDatum = OidFunctionCall0(transactionFuncOid);

	FmgrInfo *outputFunction = (FmgrInfo *) palloc0(sizeof(FmgrInfo));
	Oid outputFunctionId = FunctionOid("pg_catalog", "record_out", 1);
	fmgr_info(outputFunctionId, outputFunction);
	char *transactionId = OutputFunctionCall(outputFunction, transactionIdHeapDatum);

	/* Pick the maximum logical clock value among all transaction-nodes */
	uint64 transactionClockValue = CurrentClusterClock(transactionNodeList);

	/* Persist the transactionId along with the logical commit-clock timestamp */
	LogCommitTransactionRecord(transactionId, transactionClockValue);

	/* Adjust all the nodes with the new clock value */
	AdjustClocks(transactionNodeList, transactionClockValue);
}


/*
 * LogCommitTransactionRecord registers the committed transaction along
 * with the commit clock.
 */
static void
LogCommitTransactionRecord(char *transactionId, uint64 transactionClockValue)
{
	Datum values[Natts_pg_dist_commit_transaction];
	bool isNulls[Natts_pg_dist_commit_transaction];
	uint64 clockLogical = GET_LOGICAL(transactionClockValue);
	uint32 clockCounter = GET_COUNTER(transactionClockValue);

	ereport(DEBUG1, (errmsg("Persiting transaction %s with "
				"clock logical (%lu) and counter(%u)",
					transactionId, clockLogical, clockCounter)));

	/* form new transaction tuple */
	memset(values, 0, sizeof(values));
	memset(isNulls, false, sizeof(isNulls));

	uint64 transactionTimestamp = GetEpochTimeMs();

	values[Anum_pg_dist_commit_transaction_transactionid - 1] =
		CStringGetTextDatum(transactionId);
	values[Anum_pg_dist_commit_transaction_clock_logical - 1] =
		Int64GetDatum(clockLogical);
	values[Anum_pg_dist_commit_transaction_clock_counter - 1] =
		Int64GetDatum(clockCounter);
	values[Anum_pg_dist_commit_transaction_timestamp - 1] =
		Int64GetDatum(transactionTimestamp);

	/* open transaction relation and insert new tuple */
	Relation pgDistCommitTransaction =
		table_open(DistCommitTransactionRelationId(), RowExclusiveLock);
	TupleDesc tupleDescriptor = RelationGetDescr(pgDistCommitTransaction);
	HeapTuple heapTuple = heap_form_tuple(tupleDescriptor, values, isNulls);

	CatalogTupleInsert(pgDistCommitTransaction, heapTuple);

	CommandCounterIncrement();

	/* close relation and invalidate previous cache entry */
	table_close(pgDistCommitTransaction, NoLock);
}


/*
 * Initialize the shared memory clock value to the highest
 * clock persisted. This will protect from any clock drifts.
 */
void
InitClockAtBoot(void)
{
	uint64 epochValue = GetEpochTimeMs();

	SpinLockAcquire(&logicalClockShmem->clockMutex);

	/* Avoid repeated initialization */
	if (logicalClockShmem->clockInitialized == true)
	{
		SpinLockRelease(&logicalClockShmem->clockMutex);
		return;
	}


	/* Start with the wall clock value */
	logicalClockShmem->clusterClockValue = SET_CLOCK_LOGICAL(epochValue);

	/*
	 * Set the flag before executing a distributed query
	 * (else it might trigger this routine recursively.)
	 */
	logicalClockShmem->clockInitialized = true;

	SpinLockRelease(&logicalClockShmem->clockMutex);

	/* select the maximum clock persisted */
	int numCols = 2;
	uint64 *results = ExecuteQueryResults(
		"SELECT clocklogical, clockcounter FROM "
		"pg_dist_commit_transaction ORDER BY 1 DESC, "
		"2 DESC LIMIT 1;", numCols, SPI_OK_SELECT);

	if (results != NULL)
	{
		uint64 logicalMaxClock = results[0];
		uint32 counterMaxClock = results[1];

		ereport(LOG,
				(errmsg("Adjusted the clock with value persisted"
						"logical(%lu) and counter(%u)",
						logicalMaxClock, counterMaxClock)));

		/*
		 * Adjust the local clock according to the most recent
		 * clock stamp value persisted in the catalog.
		 */
		AdjustLocalClockToRemote(logicalMaxClock, counterMaxClock);
	}
	else
	{
		/*
		 * No prior commit timestamps on this node, start from
		 * the wall clock.
		 */
		return;
	}
}


/*
 * ExecuteQueryResults connects to SPI, executes the query and checks if
 * the SPI returned the correct type. Returns an array of int64 results
 * in the caller's memory context.
 * vanishes.
 */
static uint64 *
ExecuteQueryResults(char *query, int resultSize, int spiok_type)
{
	/* Allocate in caller's context */
	uint64 *results = (uint64 *) palloc(resultSize * sizeof(uint64));

	int spiResult = SPI_connect();
	if (spiResult != SPI_OK_CONNECT)
	{
		ereport(ERROR, (errmsg("could not connect to SPI manager")));
	}

	spiResult = SPI_execute(query, false, 0);
	if (spiResult != spiok_type)
	{
		ereport(ERROR, (errmsg("could not run SPI query")));
	}

	if (SPI_processed != 1)
	{
		SPI_finish();

		/* Either no row or more than one(unexpected) */
		return NULL;
	}

	for (int i = 0; i < resultSize; i++)
	{
		bool isnull = false;
		results[i] = DatumGetInt64(
			SPI_getbinval(SPI_tuptable->vals[0],         /* First row */
						  SPI_tuptable->tupdesc,
						  i + 1, /* 'i+1' column */
						  &isnull));
	}

	spiResult = SPI_finish();
	if (spiResult != SPI_OK_FINISH)
	{
		ereport(ERROR, (errmsg("could not finish SPI connection")));
	}

	/* Result could be NULL datum, caller's responsibility to check */
	return results;
}


/*
 * citus_get_cluster_clock() is an UDF that returns a monotonically increasing
 * logical clock. Clock guarantees to never go back in value after restarts, and
 * makes best attempt to keep the value close to unix epoch time in milliseconds.
 */
Datum
citus_get_cluster_clock(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	TupleDesc tupleDescriptor = NULL;
	Datum values[5];
	bool isNulls[5];

	/* build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupleDescriptor) != TYPEFUNC_COMPOSITE)
	{
		elog(ERROR, "return type must be a row type");
	}

	uint64 clusterClockValue = GetClusterClock();

	memset(values, 0, sizeof(values));
	memset(isNulls, false, sizeof(isNulls));

	values[0] = Int64GetDatum(GET_LOGICAL(clusterClockValue)); /* LC */
	values[1] = Int32GetDatum(GET_COUNTER(clusterClockValue)); /* C */

	HeapTuple heapTuple = heap_form_tuple(tupleDescriptor, values, isNulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(heapTuple));
}


/*
 * citus_internal_adjust_local_clock_to_remote is an internal UDF to set the transaction
 * clock value of the remote nodes' distributed transaction id.
 */
Datum
citus_internal_adjust_local_clock_to_remote(PG_FUNCTION_ARGS)
{
	uint64 logicalClockValue = PG_GETARG_INT64(0);
	uint64 counterClockValue = PG_GETARG_UINT32(1);

	AdjustLocalClockToRemote(logicalClockValue, counterClockValue);

	PG_RETURN_VOID();
}


/*
 * citus_is_clock_after is an UDF that accepts logical clock timestamps of
 * two causally related events and returns true if the argument1 happened
 * before argument2.
 */
Datum
citus_is_clock_after(PG_FUNCTION_ARGS)
{
	bool isnull;

	/* Fetch both the arguments */
	HeapTupleHeader record1 = PG_GETARG_HEAPTUPLEHEADER(0);
	HeapTupleHeader record2 = PG_GETARG_HEAPTUPLEHEADER(1);

	/* Argument is of complex type (logical, counter) */
	uint64 logical1 = DatumGetInt64(GetAttributeByName(record1, "logical", &isnull));
	uint32 counter1 = DatumGetUInt32(GetAttributeByName(record1, "counter", &isnull));
	uint64 logical2 = DatumGetInt64(GetAttributeByName(record2, "logical", &isnull));
	uint32 counter2 = DatumGetUInt32(GetAttributeByName(record2, "counter", &isnull));

	bool result = IsClockAfter(logical1, counter1, logical2, counter2);

	PG_RETURN_BOOL(result);
}
