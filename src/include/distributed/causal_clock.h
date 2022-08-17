/*
 * causal_clock.h
 *
 * Data structure definitions for managing hybrid logical clock and
 * related function declarations.
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#ifndef CAUSAL_CLOCK_H
#define CAUSAL_CLOCK_H

extern size_t LogicalClockShmemSize(void);

/*
 * Clock components - Unsigned 64 bit <LC, C>
 * Epoch Milliseconds (LC): 42 bits
 * Logical counter (C): 22 bits
 *
 * 2^42 milliseconds - 4398046511104 milliseconds, which is ~139 years.
 * 2^22 ticks - maximum of four million operations per millisecond.
 *
 */

#define COUNTER_BITS 22
#define LOGICAL_MASK ((1U << COUNTER_BITS) - 1)
#define MAX_COUNTER LOGICAL_MASK

#define GET_LOGICAL(x) (x >> COUNTER_BITS)
#define GET_COUNTER(x) (x & LOGICAL_MASK)

/* Set the logical to msb and reset the counter to zero */
#define SET_CLOCK_LOGICAL(x) (x << COUNTER_BITS)

/* concatenate logical and counter to 64 bit clock value */
#define SET_CLOCK(lc, c) ((lc << COUNTER_BITS) | c)

extern bool EnableClusterClock;
extern void LogicalClockShmemInit(void);
extern void InitializeClusterClockMem(void);
extern void PrepareAndSetTransactionClock(List *txnNodeList);
extern void InitClockAtBoot(void);

#endif /* CAUSAL_CLOCK_H */
