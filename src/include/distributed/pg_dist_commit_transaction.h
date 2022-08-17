/*-------------------------------------------------------------------------
 *
 * pg_dist_commit_transaction.h
 *	  definition of the "commit-transaction" relation (pg_dist_commit_transaction).
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_DIST_COMMIT_TRANSACTION_H
#define PG_DIST_COMMIT_TRANSACTION_H

/* ----------------
 *		pg_dist_commit_transaction definition.
 * ----------------
 */
typedef struct FormData_pg_dist_commit_transaction
{
	text transactionId;      /* id of the current transaction committed */
	uint64 clockLogical;      /* cluster clock logical timestamp at the commit */
	uint64 clockCounter;      /* cluster clock counter value at the commit */
	uint64 timestamp;        /* epoch timestamp in milliseconds */
} FormData_pg_dist_commit_transaction;


/* ----------------
 *      Form_pg_dist_commit_transactions corresponds to a pointer to a tuple with
 *      the format of pg_dist_commit_transactions relation.
 * ----------------
 */
typedef FormData_pg_dist_commit_transaction *Form_pg_dist_commit_transaction;


/* ----------------
 *      compiler constants for pg_dist_commit_transaction
 * ----------------
 */
#define Natts_pg_dist_commit_transaction 4
#define Anum_pg_dist_commit_transaction_transactionid 1
#define Anum_pg_dist_commit_transaction_clock_logical 2
#define Anum_pg_dist_commit_transaction_clock_counter 3
#define Anum_pg_dist_commit_transaction_timestamp 4

#endif   /* PG_DIST_COMMIT_TRANSACTION_H */
