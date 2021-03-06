/*-------------------------------------------------------------------------
 *
 * multi_router_planner.h
 *
 * Declarations for public functions and types related to router planning.
 *
 * Copyright (c) 2014-2016, Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#ifndef MULTI_ROUTER_PLANNER_H
#define MULTI_ROUTER_PLANNER_H

#include "c.h"

#include "distributed/errormessage.h"
#include "distributed/multi_logical_planner.h"
#include "distributed/multi_physical_planner.h"
#include "distributed/multi_planner.h"
#include "nodes/parsenodes.h"


/* reserved parameted id, we chose a negative number since it is not assigned by postgres */
#define UNINSTANTIATED_PARAMETER_ID INT_MIN

/* reserved alias name for UPSERTs */
#define CITUS_TABLE_ALIAS "citus_table_alias"

extern bool EnableRouterExecution;

extern MultiPlan * CreateRouterPlan(Query *originalQuery, Query *query,
									RelationRestrictionContext *restrictionContext);
extern MultiPlan * CreateModifyPlan(Query *originalQuery, Query *query,
									RelationRestrictionContext *restrictionContext);

extern void AddUninstantiatedPartitionRestriction(Query *originalQuery);
extern DeferredErrorMessage * ModifyQuerySupported(Query *queryTree);
extern Query * ReorderInsertSelectTargetLists(Query *originalQuery,
											  RangeTblEntry *insertRte,
											  RangeTblEntry *subqueryRte);
extern bool InsertSelectQuery(Query *query);
extern Oid ExtractFirstDistributedTableId(Query *query);
extern RangeTblEntry * ExtractSelectRangeTableEntry(Query *query);
extern RangeTblEntry * ExtractInsertRangeTableEntry(Query *query);
extern void AddShardIntervalRestrictionToSelect(Query *subqery,
												ShardInterval *shardInterval);
extern ShardInterval * FastShardPruning(Oid distributedTableId, Datum partitionValue);


#endif /* MULTI_ROUTER_PLANNER_H */
