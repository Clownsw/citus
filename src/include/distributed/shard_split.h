/*-------------------------------------------------------------------------
 *
 * shard_split.h
 *
 * API for shard splits.
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#ifndef SHARDSPLIT_H_
#define SHARDSPLIT_H_

/* Split Modes supported by Shard Split API */
typedef enum SplitMode
{
	BLOCKING_SPLIT = 0,
	NON_BLOCKING_SPLIT = 1
} SplitMode;

/*
 * User Scenario calling Split Shard API.
 * The 'SplitOperation' type is used to customize info/error messages based on user scenario.
 */
typedef enum SplitOperation
{
	SHARD_SPLIT_API = 0,
	ISOLATE_TENANT_TO_NEW_SHARD,
	CREATE_DISTRIBUTED_TABLE
} SplitOperation;

/*
 * In-memory mapping of a split child shard.
 */
typedef struct ShardSplitInfo
{
	Oid distributedTableOid;     /* citus distributed table Oid */
	int partitionColumnIndex;    /* partition column index */
	Oid sourceShardOid;          /* parent shard Oid */
	Oid splitChildShardOid;      /* child shard Oid */
	int32 shardMinValue;         /* min hash value */
	int32 shardMaxValue;         /* max hash value */
	uint32_t nodeId;             /* node where child shard is to be placed */
	uint64 sourceShardId;        /* parent shardId */
	uint64 splitChildShardId;        /* child shardId*/
	char slotName[NAMEDATALEN];  /* replication slot name belonging to this node */
} ShardSplitInfo;

/*
 * SplitShard API to split a given shard (or shard group) using split mode and
 * specified split points to a set of destination nodes.
 */
extern void SplitShard(SplitMode splitMode,
					   SplitOperation splitOperation,
					   uint64 shardIdToSplit,
					   List *shardSplitPointsList,
					   List *nodeIdsForPlacementList);
extern void NonBlockingShardSplit(SplitOperation splitOperation,
								  ShardInterval *shardIntervalToSplit,
								  List *shardSplitPointsList,
								  List *workersForPlacementList,
								  char *snapshotName);
extern char * CreateTemplateReplicationSlotAndReturnSnapshot(ShardInterval *shardInterval,
															 WorkerNode *sourceWorkerNode);

/* TODO(niupre): Make all these APIs private when all consumers (Example : ISOLATE_TENANT_TO_NEW_SHARD) directly call 'SplitShard' API. */
extern void ErrorIfCannotSplitShard(SplitOperation splitOperation,
									ShardInterval *sourceShard);
extern void DropShardList(List *shardIntervalList);

#endif /* SHARDSPLIT_H_ */
