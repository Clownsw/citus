//
// Created by Nils Dijk on 17/01/2020.
//

#ifndef CITUS_PATH_BASED_PLANNER_H
#define CITUS_PATH_BASED_PLANNER_H

#include "nodes/parsenodes.h"
#include "nodes/pathnodes.h"

extern bool EnableBroadcastJoin;

extern void PathBasedPlannerRelationHook(PlannerInfo *root,
										 RelOptInfo *relOptInfo,
										 Index restrictionIndex,
										 RangeTblEntry *rte);
extern void PathBasedPlannerJoinHook(PlannerInfo *root,
									 RelOptInfo *joinrel,
									 RelOptInfo *outerrel,
									 RelOptInfo *innerrel,
									 JoinType jointype,
									 JoinPathExtraData *extra);
extern void PathBasedPlannedUpperPathHook(PlannerInfo *root,
										  UpperRelationKind stage,
										  RelOptInfo *input_rel,
										  RelOptInfo *output_rel,
										  void *extra);

#endif //CITUS_PATH_BASED_PLANNER_H
