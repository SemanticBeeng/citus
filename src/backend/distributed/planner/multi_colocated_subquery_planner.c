/*
 * multi_colocated_subquery_planner.c
 *
 * This file contains functions helper functions for planning
 * queries with colocated tables and subqueries.
 *
 * Copyright (c) 2017-2017, Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "c.h"

#include "distributed/multi_colocated_subquery_planner.h"
#include "distributed/multi_planner.h"
#include "distributed/multi_logical_planner.h"
#include "distributed/multi_logical_optimizer.h"
#include "distributed/pg_dist_partition.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "nodes/relation.h"
#include "parser/parsetree.h"
#include "optimizer/pathnode.h"

static uint32 attributeEquivalenceId = 1;


/*
 * AttributeEquivalenceClass
 *
 * Whenever we find an equality clause A = B, where both A and B originates from
 * relation attributes (i.e., not random expressions), we create an
 * AttributeEquivalenceClass to record this knowledge. If we later find another
 * equivalence B = C, we create another AttributeEquivalenceClass. Finally, we can
 * apply transitity rules and generate a new AttributeEquivalenceClass which includes
 * A, B and C.
 *
 * Note that equality among the members are identified by the varattno and rteIdentity.
 */
typedef struct AttributeEquivalenceClass
{
	uint32 equivalenceId;
	List *equivalentAttributes;
} AttributeEquivalenceClass;

/*
 *  AttributeEquivalenceClassMember - one member expression of an
 *  AttributeEquivalenceClassMember. The important thing to consider is that
 *  the class member contains "rteIndentity" field. Note that each RTE_RELATION
 *  is assigned a unique rteIdentity in AssignRTEIdentities() function.
 *
 *  "varno" and "varattrno" is directly used from a Var clause that is being added
 *  to the attribute equivalence. Since we only use this class for relations, the member
 *  also includes the relation id field.
 */
typedef struct AttributeEquivalenceClassMember
{
	Index varno;
	AttrNumber varattno;
	Oid relationId;
	int rteIdendity;
} AttributeEquivalenceClassMember;


static bool SafeToPushdownTopLevelUnion(RelationRestrictionContext *restrictionContext);
static Var * FindTranslatedVar(List *appendRelList, Oid relationOid,
							   Index relationRteIndex, Index *partitionKeyIndex);
static bool AllRelationsJoinedOnPartitionKey(RelationRestrictionContext *
											 restrictionContext,
											 JoinRestrictionContext *
											 joinRestrictionContext);
static bool EquivalenceListContainsRelationsEquality(List *attributeEquivalenceList,
													 RelationRestrictionContext *
													 restrictionContext);
static uint32 ReferenceRelationCount(RelationRestrictionContext *restrictionContext);
static List * GenerateAttributeEquivalencesForRelationRestrictions(
	RelationRestrictionContext *restrictionContext);
static AttributeEquivalenceClass * AttributeEquivalenceClassForEquivalenceClass(
	EquivalenceClass *plannerEqClass, RelationRestriction *relationRestriction);
static void AddToAttributeEquivalenceClass(PlannerInfo *root, Var *varToBeAdded,
										   AttributeEquivalenceClass **
										   attributeEquivalanceClass);
static void AddUnionSetOperationsToAttributeEquivalenceClass(Query *query,
															 SetOperationStmt *
															 setOperation,
															 Var *varToBeAdded,
															 AttributeEquivalenceClass **
															 attributeEquivalenceClass);
static void AddRangeTableReferenceToEquivalenceClass(Query *query,
													 Node *rteReferenceArgument,
													 AttributeEquivalenceClass **
													 attributeEquivalenceClass,
													 Var *varToBeAdded);
static void AddRteToAttributeEquivalenceClass(RangeTblEntry *rangeTableEntry,
											  AttributeEquivalenceClass **
											  attrEquivalenceClass,
											  Var *varToBeAdded);
static Var * GetVarFromAssignedParam(List *parentPlannerParamList,
									 Param *plannerParam);
static List * GenerateAttributeEquivalencesForJoinRestrictions(JoinRestrictionContext
															   *joinRestrictionContext);
static bool AttributeClassContainsAttributeClassMember(AttributeEquivalenceClassMember *
													   inputMember,
													   AttributeEquivalenceClass *
													   attributeEquivalenceClass);
static List * AddAttributeClassToAttributeClassList(AttributeEquivalenceClass *
													attributeEquivalance,
													List *attributeEquivalenceList);
static AttributeEquivalenceClass * GenerateCommonEquivalence(List *
															 attributeEquivalenceList);
static void ListConcatUniqueAttributeClassMemberLists(AttributeEquivalenceClass **
													  firstClass,
													  AttributeEquivalenceClass *
													  secondClass);
static bool TopLevelUnionQuery(Query *queryTree);
static Index RelationRestrictionPartitionKeyIndex(RelationRestriction *
												  relationRestriction);


/*
 * SafeToPushDownSubquery returns true if either
 *    (i)  there exists join in the query and all relations joined on their
 *         partition keys
 *    (ii) there exists only union set operations and all relations has
 *         partition keys in the same ordinal position in the query
 */
bool
SafeToPushDownSubquery(RelationRestrictionContext *restrictionContext,
					   JoinRestrictionContext *joinRestrictionContext)
{
	bool allRelationsJoinedOnPartitionKey =
		AllRelationsJoinedOnPartitionKey(restrictionContext, joinRestrictionContext);

	if (allRelationsJoinedOnPartitionKey)
	{
		return true;
	}

	if (TopLevelUnionQuery(restrictionContext->parseTree))
	{
		return SafeToPushdownTopLevelUnion(restrictionContext);
	}

	return false;
}


/*
 * SafeToPushdownTopLevelUnion returns true if all the relations are returns
 * partition keys in the same ordinal position.
 *
 * Note that the function expects (and asserts) the input query to be a top
 * level union query defined by TopLevelUnionQuery().
 */
static bool
SafeToPushdownTopLevelUnion(RelationRestrictionContext *restrictionContext)
{
	Index unionQueryPartitionKeyIndex = 0;
	AttributeEquivalenceClass *attributeEquivalance =
		palloc0(sizeof(AttributeEquivalenceClass));
	ListCell *relationRestrictionCell = NULL;

	Assert(TopLevelUnionQuery(restrictionContext->parseTree));

	attributeEquivalance->equivalenceId = attributeEquivalenceId++;

	foreach(relationRestrictionCell, restrictionContext->relationRestrictionList)
	{
		RelationRestriction *relationRestriction = lfirst(relationRestrictionCell);
		Index partitionKeyIndex = InvalidAttrNumber;
		PlannerInfo *relationPlannerRoot = relationRestriction->plannerInfo;
		List *targetList = relationPlannerRoot->parse->targetList;
		List *appendRelList = relationPlannerRoot->append_rel_list;
		Var *varToBeAdded = NULL;
		TargetEntry *targetEntryToAdd = NULL;

		/* we first check whether UNION ALLs are pulled up */
		if (appendRelList != NULL)
		{
			varToBeAdded = FindTranslatedVar(appendRelList,
											 relationRestriction->relationId,
											 relationRestriction->index,
											 &partitionKeyIndex);

			/* union does not have partition key in the target list */
			if (partitionKeyIndex == 0)
			{
				return false;
			}
		}
		else
		{
			partitionKeyIndex =
				RelationRestrictionPartitionKeyIndex(relationRestriction);

			/* union does not have partition key in the target list */
			if (partitionKeyIndex == 0)
			{
				return false;
			}

			targetEntryToAdd = list_nth(targetList, partitionKeyIndex - 1);
			if (!IsA(targetEntryToAdd->expr, Var))
			{
				return false;
			}

			varToBeAdded = (Var *) targetEntryToAdd->expr;
		}

		/*
		 * If the first relation doesn't have partition key on the target
		 * list of the query that the relation in, simply not allow to push down
		 * the query.
		 */
		if (partitionKeyIndex == InvalidAttrNumber)
		{
			return false;
		}

		/*
		 * We find the first relations partition key index in the target list. Later,
		 * we check whether all the relations have partition keys in the
		 * same position.
		 */
		if (unionQueryPartitionKeyIndex == InvalidAttrNumber)
		{
			unionQueryPartitionKeyIndex = partitionKeyIndex;
		}
		else if (unionQueryPartitionKeyIndex != partitionKeyIndex)
		{
			return false;
		}

		AddToAttributeEquivalenceClass(relationPlannerRoot, varToBeAdded,
									   &attributeEquivalance);
	}

	return EquivalenceListContainsRelationsEquality(list_make1(attributeEquivalance),
													restrictionContext);
}


/*
 * FindTranslatedVar iterates on the appendRelList and tries to find a translated
 * child var identified by the relation id and the relation rte index. The function
 * returns NULL if it cannot find a translated var.
 */
static Var *
FindTranslatedVar(List *appendRelList, Oid relationOid, Index relationRteIndex,
				  Index *partitionKeyIndex)
{
	ListCell *appendRelCell = NULL;

	*partitionKeyIndex = 0;

	/* iterate on the queries that are part of UNION ALL subselects */
	foreach(appendRelCell, appendRelList)
	{
		AppendRelInfo *appendRelInfo = (AppendRelInfo *) lfirst(appendRelCell);
		List *translaterVars = appendRelInfo->translated_vars;
		ListCell *translatedVarCell = NULL;
		AttrNumber childAttrNumber = 0;
		Var *relationPartitionKey = NULL;

		/*
		 * We're only interested in the child rel that is equal to the
		 * relation we're investigating.
		 */
		if (appendRelInfo->child_relid != relationRteIndex)
		{
			continue;
		}

		relationPartitionKey = PartitionKey(relationOid);

		foreach(translatedVarCell, translaterVars)
		{
			Var *targetVar = (Var *) lfirst(translatedVarCell);

			childAttrNumber++;

			if (targetVar->varno == relationRteIndex &&
				targetVar->varattno == relationPartitionKey->varattno)
			{
				*partitionKeyIndex = childAttrNumber;

				return targetVar;
			}
		}
	}

	return NULL;
}


/*
 * AllRelationsJoinedOnPartitionKey aims to deduce whether each of the RTE_RELATION
 * is joined with at least one another RTE_RELATION on their partition keys. If each
 * RTE_RELATION follows the above rule, we can conclude that all RTE_RELATIONs are
 * joined on their partition keys.
 *
 * In order to do that, we invented a new equivalence class namely:
 * AttributeEquivalenceClass. In very simple words, a AttributeEquivalenceClass is
 * identified by an unique id and consists of a list of AttributeEquivalenceMembers.
 *
 * Each AttributeEquivalenceMember is designed to identify attributes uniquely within the
 * whole query. The necessity of this arise since varno attributes are defined within
 * a single level of a query. Instead, here we want to identify each RTE_RELATION uniquely
 * and try to find equality among each RTE_RELATION's partition key.
 *
 * Each equality among RTE_RELATION is saved using an AttributeEquivalenceClass where
 * each member attribute is identified by a AttributeEquivalenceMember. In the final
 * step, we try generate a common attribute equivalence class that holds as much as
 * AttributeEquivalenceMembers whose attributes are a partition keys.
 *
 * AllRelationsJoinedOnPartitionKey uses both relation restrictions and join restrictions
 * to find as much as information that Postgres planner provides to extensions. For the
 * details of the usage, please see GenerateAttributeEquivalencesForRelationRestrictions()
 * and GenerateAttributeEquivalencesForJoinRestrictions()
 *
 * Finally, as the name of the function reveals, the function returns true if all relations
 * are joined on their partition keys. Otherwise, the function returns false.
 */
static bool
AllRelationsJoinedOnPartitionKey(RelationRestrictionContext *restrictionContext,
								 JoinRestrictionContext *joinRestrictionContext)
{
	List *relationRestrictionAttributeEquivalenceList = NIL;
	List *joinRestrictionAttributeEquivalenceList = NIL;
	List *allAttributeEquivalenceList = NIL;
	uint32 referenceRelationCount = ReferenceRelationCount(restrictionContext);
	uint32 totalRelationCount = list_length(restrictionContext->relationRestrictionList);
	uint32 nonReferenceRelationCount = totalRelationCount - referenceRelationCount;

	/*
	 * If the query includes a single relation which is not a reference table,
	 * we should not check the partition column equality.
	 * Consider two example cases:
	 *   (i)   The query includes only a single colocated relation
	 *   (ii)  A colocated relation is joined with a reference
	 *         table where colocated relation is not joined on the partition key
	 *
	 * For the above two cases, we should not execute the partition column equality
	 * algorithm.
	 */
	if (nonReferenceRelationCount <= 1)
	{
		return true;
	}

	/* reset the equivalence id counter per call to prevent overflows */
	attributeEquivalenceId = 1;

	relationRestrictionAttributeEquivalenceList =
		GenerateAttributeEquivalencesForRelationRestrictions(restrictionContext);
	joinRestrictionAttributeEquivalenceList =
		GenerateAttributeEquivalencesForJoinRestrictions(joinRestrictionContext);

	allAttributeEquivalenceList =
		list_concat(relationRestrictionAttributeEquivalenceList,
					joinRestrictionAttributeEquivalenceList);

	return EquivalenceListContainsRelationsEquality(allAttributeEquivalenceList,
													restrictionContext);
}


/*
 * EquivalenceListContainsRelationsEquality gets a list of attributed equivalence
 * list and a relation restriction context. The function first generates a common
 * equivalence class out of the attributeEquivalenceList. Later, the function checks
 * whether all the relations exists in the common equivalence class.
 *
 */
static bool
EquivalenceListContainsRelationsEquality(List *attributeEquivalenceList,
										 RelationRestrictionContext *restrictionContext)
{
	AttributeEquivalenceClass *commonEquivalenceClass = NULL;
	ListCell *commonEqClassCell = NULL;
	ListCell *relationRestrictionCell = NULL;
	Relids commonRteIdentities = NULL;

	/*
	 * In general we're trying to expand existing the equivalence classes to find a
	 * common equivalence class. The main goal is to test whether this main class
	 * contains all partition keys of the existing relations.
	 */
	commonEquivalenceClass = GenerateCommonEquivalence(attributeEquivalenceList);

	/* add the rte indexes of relations to a bitmap */
	foreach(commonEqClassCell, commonEquivalenceClass->equivalentAttributes)
	{
		AttributeEquivalenceClassMember *classMember = lfirst(commonEqClassCell);
		int rteIdentity = classMember->rteIdendity;

		commonRteIdentities = bms_add_member(commonRteIdentities, rteIdentity);
	}

	/* check whether all relations exists in the main restriction list */
	foreach(relationRestrictionCell, restrictionContext->relationRestrictionList)
	{
		RelationRestriction *relationRestriction = lfirst(relationRestrictionCell);
		int rteIdentity = GetRTEIdentity(relationRestriction->rte);

		if (PartitionKey(relationRestriction->relationId) &&
			!bms_is_member(rteIdentity, commonRteIdentities))
		{
			return false;
		}
	}

	return true;
}


/*
 * ReferenceRelationCount iterates over the relations and returns the reference table
 * relation count.
 */
static uint32
ReferenceRelationCount(RelationRestrictionContext *restrictionContext)
{
	ListCell *relationRestrictionCell = NULL;
	uint32 referenceRelationCount = 0;

	foreach(relationRestrictionCell, restrictionContext->relationRestrictionList)
	{
		RelationRestriction *relationRestriction = lfirst(relationRestrictionCell);

		if (PartitionMethod(relationRestriction->relationId) == DISTRIBUTE_BY_NONE)
		{
			referenceRelationCount++;
		}
	}

	return referenceRelationCount;
}


/*
 * GenerateAttributeEquivalencesForRelationRestrictions gets a relation restriction
 * context and returns a list of AttributeEquivalenceClass.
 *
 * The algorithm followed can be summarized as below:
 *
 * - Per relation restriction
 *     - Per plannerInfo's eq_class
 *         - Create an AttributeEquivalenceClass
 *         - Add all Vars that appear in the plannerInfo's
 *           eq_class to the AttributeEquivalenceClass
 *               - While doing that, consider LATERAL vars as well.
 *                 See GetVarFromAssignedParam() for the details. Note
 *                 that we're using parentPlannerInfo while adding the
 *                 LATERAL vars given that we rely on that plannerInfo.
 *
 */
static List *
GenerateAttributeEquivalencesForRelationRestrictions(RelationRestrictionContext
													 *restrictionContext)
{
	List *attributeEquivalenceList = NIL;
	ListCell *relationRestrictionCell = NULL;

	foreach(relationRestrictionCell, restrictionContext->relationRestrictionList)
	{
		RelationRestriction *relationRestriction = lfirst(relationRestrictionCell);
		List *equivalenceClasses = relationRestriction->plannerInfo->eq_classes;
		ListCell *equivilanceClassCell = NULL;

		foreach(equivilanceClassCell, equivalenceClasses)
		{
			EquivalenceClass *plannerEqClass = lfirst(equivilanceClassCell);

			AttributeEquivalenceClass *attributeEquivalance =
				AttributeEquivalenceClassForEquivalenceClass(plannerEqClass,
															 relationRestriction);

			attributeEquivalenceList =
				AddAttributeClassToAttributeClassList(attributeEquivalance,
													  attributeEquivalenceList);
		}
	}

	return attributeEquivalenceList;
}


/*
 * AttributeEquivalenceClassForEquivalenceClass is a helper function for
 * GenerateAttributeEquivalencesForRelationRestrictions. The function takes an
 * EquivalenceClass and the relation restriction that the equivalence class
 * belongs to. The function returns an AttributeEquivalenceClass that is composed
 * of ec_members that are simple Var references.
 *
 * The function also takes case of LATERAL joins by simply replacing the PARAM_EXEC
 * with the corresponding expression.
 */
static AttributeEquivalenceClass *
AttributeEquivalenceClassForEquivalenceClass(EquivalenceClass *plannerEqClass,
											 RelationRestriction *relationRestriction)
{
	AttributeEquivalenceClass *attributeEquivalance =
		palloc0(sizeof(AttributeEquivalenceClass));
	ListCell *equivilanceMemberCell = NULL;
	PlannerInfo *plannerInfo = relationRestriction->plannerInfo;

	attributeEquivalance->equivalenceId = attributeEquivalenceId++;

	foreach(equivilanceMemberCell, plannerEqClass->ec_members)
	{
		EquivalenceMember *equivalenceMember = lfirst(equivilanceMemberCell);
		Node *equivalenceNode = strip_implicit_coercions(
			(Node *) equivalenceMember->em_expr);
		Expr *strippedEquivalenceExpr = (Expr *) equivalenceNode;

		Var *expressionVar = NULL;

		if (IsA(strippedEquivalenceExpr, Param))
		{
			List *parentParamList = relationRestriction->parentPlannerParamList;
			Param *equivalenceParam = (Param *) strippedEquivalenceExpr;

			expressionVar = GetVarFromAssignedParam(parentParamList,
													equivalenceParam);
			if (expressionVar)
			{
				AddToAttributeEquivalenceClass(
					relationRestriction->parentPlannerInfo,
					expressionVar,
					&attributeEquivalance);
			}
		}
		else if (IsA(strippedEquivalenceExpr, Var))
		{
			expressionVar = (Var *) strippedEquivalenceExpr;
			AddToAttributeEquivalenceClass(plannerInfo, expressionVar,
										   &attributeEquivalance);
		}
	}

	return attributeEquivalance;
}


/*
 *
 * GetVarFromAssignedParam returns the Var that is assigned to the given
 * plannerParam if its kind is PARAM_EXEC.
 *
 * If the paramkind is not equal to PARAM_EXEC the function returns NULL. Similarly,
 * if there is no var that the given param is assigned to, the function returns NULL.
 *
 * Rationale behind this function:
 *
 *   While iterating through the equivalence classes of RTE_RELATIONs, we
 *   observe that there are PARAM type of equivalence member expressions for
 *   the RTE_RELATIONs which actually belong to lateral vars from the other query
 *   levels.
 *
 *   We're also keeping track of the RTE_RELATION's parent_root's
 *   plan_param list which is expected to hold the parameters that are required
 *   for its lower level queries as it is documented:
 *
 *        plan_params contains the expressions that this query level needs to
 *        make available to a lower query level that is currently being planned.
 *
 *   This function is a helper function to iterate through the parent query's
 *   plan_params and looks for the param that the equivalence member has. The
 *   comparison is done via the "paramid" field. Finally, if the found parameter's
 *   item is a Var, we conclude that Postgres standard_planner replaced the Var
 *   with the Param on assign_param_for_var() function
 *   @src/backend/optimizer//plan/subselect.c.
 *
 */
static Var *
GetVarFromAssignedParam(List *parentPlannerParamList, Param *plannerParam)
{
	Var *assignedVar = NULL;
	ListCell *plannerParameterCell = NULL;

	Assert(plannerParam != NULL);

	/* we're only interested in parameters that Postgres added for execution */
	if (plannerParam->paramkind != PARAM_EXEC)
	{
		return NULL;
	}

	foreach(plannerParameterCell, parentPlannerParamList)
	{
		PlannerParamItem *plannerParamItem = lfirst(plannerParameterCell);

		if (plannerParamItem->paramId != plannerParam->paramid)
		{
			continue;
		}

		/* TODO: Should we consider PlaceHolderVar?? */
		if (!IsA(plannerParamItem->item, Var))
		{
			continue;
		}

		assignedVar = (Var *) plannerParamItem->item;

		break;
	}

	return assignedVar;
}


/*
 * GenerateCommonEquivalence gets a list of unrelated AttributeEquiavalanceClass
 * whose all members are partition keys.
 *
 * With the equivalence classes, the function follows the algorithm
 * outlined below:
 *
 *     - Add the first equivalence class to the common equivalence class
 *     - Then, iterate on the remaining equivalence classes
 *          - If any of the members equal to the common equivalence class
 *            add all the members of the equivalence class to the common
 *            class
 *          - Start the iteration from the beginning. The reason is that
 *            in case any of the classes we've passed is equivalent to the
 *            newly added one. To optimize the algorithm, we utilze the
 *            equivalence class ids and skip the ones that are already added.
 *      - Finally, return the common equivalence class.
 */
static AttributeEquivalenceClass *
GenerateCommonEquivalence(List *attributeEquivalenceList)
{
	AttributeEquivalenceClass *commonEquivalenceClass = NULL;
	AttributeEquivalenceClass *firstEquivalenceClass = NULL;
	Bitmapset *addedEquivalenceIds = NULL;
	uint32 equivalenceListSize = list_length(attributeEquivalenceList);
	uint32 equivalenceClassIndex = 0;

	commonEquivalenceClass = palloc0(sizeof(AttributeEquivalenceClass));
	commonEquivalenceClass->equivalenceId = 0;

	/* think more on this. */
	if (equivalenceListSize < 1)
	{
		return commonEquivalenceClass;
	}

	/* setup the initial state of the main equivalence class */
	firstEquivalenceClass = linitial(attributeEquivalenceList);
	commonEquivalenceClass->equivalentAttributes =
		firstEquivalenceClass->equivalentAttributes;
	addedEquivalenceIds = bms_add_member(addedEquivalenceIds,
										 firstEquivalenceClass->equivalenceId);

	for (; equivalenceClassIndex < equivalenceListSize; ++equivalenceClassIndex)
	{
		AttributeEquivalenceClass *currentEquivalenceClass =
			list_nth(attributeEquivalenceList, equivalenceClassIndex);
		ListCell *equivalenceMemberCell = NULL;

		/*
		 * This is an optimization. If we already added the same equivalence class,
		 * we could skip it since we've already added all the relevant equivalence
		 * members.
		 */
		if (bms_is_member(currentEquivalenceClass->equivalenceId, addedEquivalenceIds))
		{
			continue;
		}

		foreach(equivalenceMemberCell, currentEquivalenceClass->equivalentAttributes)
		{
			AttributeEquivalenceClassMember *attributeEquialanceMember =
				(AttributeEquivalenceClassMember *) lfirst(equivalenceMemberCell);

			if (AttributeClassContainsAttributeClassMember(attributeEquialanceMember,
														   commonEquivalenceClass))
			{
				ListConcatUniqueAttributeClassMemberLists(&commonEquivalenceClass,
														  currentEquivalenceClass);

				addedEquivalenceIds = bms_add_member(addedEquivalenceIds,
													 currentEquivalenceClass->
													 equivalenceId);

				/*
				 * It seems inefficient to start from the beginning.
				 * But, we should somehow restart from the beginning to test that
				 * whether the already skipped ones are equal or not.
				 */
				equivalenceClassIndex = 0;

				break;
			}
		}
	}

	return commonEquivalenceClass;
}


/*
 * ListConcatUniqueAttributeClassMemberLists gets two attribute equivalence classes. It
 * basically concatenates attribute equivalence member lists uniquely and updates the
 * firstClass' member list with the list.
 *
 * Basically, the function iterates over the secondClass' member list and checks whether
 * it already exists in the firstClass' member list. If not, the member is added to the
 * firstClass.
 */
static void
ListConcatUniqueAttributeClassMemberLists(AttributeEquivalenceClass **firstClass,
										  AttributeEquivalenceClass *secondClass)
{
	ListCell *equivalenceClassMemberCell = NULL;
	List *equivalenceMemberList = secondClass->equivalentAttributes;

	foreach(equivalenceClassMemberCell, equivalenceMemberList)
	{
		AttributeEquivalenceClassMember *newEqMember = lfirst(equivalenceClassMemberCell);

		if (AttributeClassContainsAttributeClassMember(newEqMember, *firstClass))
		{
			continue;
		}

		(*firstClass)->equivalentAttributes = lappend((*firstClass)->equivalentAttributes,
													  newEqMember);
	}
}


/*
 * GenerateAttributeEquivalencesForJoinRestrictions gets a join restriction
 * context and returns a list of AttrributeEquivalenceClass.
 *
 * The algorithm followed can be summarized as below:
 *
 * - Per join restriction
 *     - Per RestrictInfo of the join restriction
 *     - Check whether the join restriction is in the form of (Var1 = Var2)
 *         - Create an AttributeEquivalenceClass
 *         - Add both Var1 and Var2 to the AttributeEquivalenceClass
 */
static List *
GenerateAttributeEquivalencesForJoinRestrictions(JoinRestrictionContext *
												 joinRestrictionContext)
{
	List *attributeEquivalenceList = NIL;
	ListCell *joinRestrictionCell = NULL;

	foreach(joinRestrictionCell, joinRestrictionContext->joinRestrictionList)
	{
		JoinRestriction *joinRestriction = lfirst(joinRestrictionCell);
		ListCell *restrictionInfoList = NULL;

		foreach(restrictionInfoList, joinRestriction->joinRestrictInfoList)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(restrictionInfoList);
			OpExpr *restrictionOpExpr = NULL;
			Node *leftNode = NULL;
			Node *rightNode = NULL;
			Expr *strippedLeftExpr = NULL;
			Expr *strippedRightExpr = NULL;
			Var *leftVar = NULL;
			Var *rightVar = NULL;
			Expr *restrictionClause = rinfo->clause;
			AttributeEquivalenceClass *attributeEquivalance = NULL;

			if (!IsA(restrictionClause, OpExpr))
			{
				continue;
			}

			restrictionOpExpr = (OpExpr *) restrictionClause;
			if (list_length(restrictionOpExpr->args) != 2)
			{
				continue;
			}
			if (!OperatorImplementsEquality(restrictionOpExpr->opno))
			{
				continue;
			}

			leftNode = linitial(restrictionOpExpr->args);
			rightNode = lsecond(restrictionOpExpr->args);

			/* we also don't want implicit coercions */
			strippedLeftExpr = (Expr *) strip_implicit_coercions((Node *) leftNode);
			strippedRightExpr = (Expr *) strip_implicit_coercions((Node *) rightNode);

			if (!(IsA(strippedLeftExpr, Var) && IsA(strippedRightExpr, Var)))
			{
				continue;
			}

			leftVar = (Var *) strippedLeftExpr;
			rightVar = (Var *) strippedRightExpr;

			attributeEquivalance = palloc0(sizeof(AttributeEquivalenceClass));
			attributeEquivalance->equivalenceId = attributeEquivalenceId++;

			AddToAttributeEquivalenceClass(joinRestriction->plannerInfo, leftVar,
										   &attributeEquivalance);
			AddToAttributeEquivalenceClass(joinRestriction->plannerInfo, rightVar,
										   &attributeEquivalance);

			attributeEquivalenceList =
				AddAttributeClassToAttributeClassList(attributeEquivalance,
													  attributeEquivalenceList);
		}
	}

	return attributeEquivalenceList;
}


/*
 * AddToAttributeEquivalenceClass is a key function for building the attribute
 * equivalences. The function gets a plannerInfo, var and attribute equivalence
 * class. It searches for the RTE_RELATION(s) that the input var belongs to and
 * adds the found Var(s) to the input attribute equivalence class.
 *
 * Note that the input var could come from a subquery (i.e., not directly from an
 * RTE_RELATION). That's the reason we recursively call the function until the
 * RTE_RELATION found.
 *
 * The algorithm could be summarized as follows:
 *
 *    - If the RTE that corresponds to a relation
 *        - Generate an AttributeEquivalenceMember and add to the input
 *          AttributeEquivalenceClass
 *    - If the RTE that corresponds to a subquery
 *        - If the RTE that corresponds to a UNION ALL subquery
 *            - Iterate on each of the appendRels (i.e., each of the UNION ALL query)
 *            - Recursively add all children of the set operation's
 *              corresponding target entries
 *        - If the corresponding subquery entry is a UNION set operation
 *             - Recursively add all children of the set operation's
 *               corresponding target entries
 *        - If the corresponding subquery is a regular subquery (i.e., No set operations)
 *             - Recursively try to add the corresponding target entry to the
 *               equivalence class
 */
static void
AddToAttributeEquivalenceClass(PlannerInfo *root, Var *varToBeAdded,
							   AttributeEquivalenceClass **attributeEquivalanceClass)
{
	RangeTblEntry *rangeTableEntry = root->simple_rte_array[varToBeAdded->varno];

	if (rangeTableEntry->rtekind == RTE_RELATION)
	{
		AddRteToAttributeEquivalenceClass(rangeTableEntry, attributeEquivalanceClass,
										  varToBeAdded);
	}
	else if (rangeTableEntry->rtekind == RTE_SUBQUERY)
	{
		Query *subquery = rangeTableEntry->subquery;
		RelOptInfo *baseRelOptInfo = NULL;
		TargetEntry *subqueryTargetEntry = NULL;

		/* punt if it's a whole-row var rather than a plain column reference */
		if (varToBeAdded->varattno == InvalidAttrNumber)
		{
			return;
		}

		/*
		 * For subqueries other than "UNION ALL", find the corresponding subquery. See
		 * the details of how we process subqueries in the below comments.
		 */
		if (!rangeTableEntry->inh)
		{
			baseRelOptInfo = find_base_rel(root, varToBeAdded->varno);

			/* If the subquery hasn't been planned yet, we have to punt */
			if (baseRelOptInfo->subroot == NULL)
			{
				return;
			}

			Assert(IsA(baseRelOptInfo->subroot, PlannerInfo));

			subquery = baseRelOptInfo->subroot->parse;
			Assert(IsA(subquery, Query));
		}

		/* get the subquery output expression referenced by the upper Var */
		subqueryTargetEntry = get_tle_by_resno(subquery->targetList,
											   varToBeAdded->varattno);
		if (subqueryTargetEntry == NULL || subqueryTargetEntry->resjunk)
		{
			ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							errmsg("subquery %s does not have attribute %d",
								   rangeTableEntry->eref->aliasname,
								   varToBeAdded->varattno)));
		}

		/* we're only interested in Vars */
		if (!IsA(subqueryTargetEntry->expr, Var))
		{
			return;
		}

		varToBeAdded = (Var *) subqueryTargetEntry->expr;

		/*
		 *  "inh" flag is set either when inheritance or "UNION ALL" exists in the
		 *  subquery. Here we're only interested in the "UNION ALL" case.
		 *
		 *  Else, we check one more thing: Does the subquery contain a "UNION" query.
		 *  If so, we recursively traverse all "UNION" tree and add the corresponding
		 *  target list elements to the attribute equivalence.
		 *
		 *  Finally, if it is a regular subquery (i.e., does not contain UNION or UNION ALL),
		 *  we simply recurse to find the corresponding RTE_RELATION to add to the equivalence
		 *  class.
		 *
		 *  Note that we're treating "UNION" and "UNION ALL" clauses differently given that postgres
		 *  planner process/plans them separately.
		 */
		if (rangeTableEntry->inh)
		{
			List *appendRelList = root->append_rel_list;
			ListCell *appendRelCell = NULL;

			/* iterate on the queries that are part of UNION ALL subselects */
			foreach(appendRelCell, appendRelList)
			{
				AppendRelInfo *appendRelInfo = (AppendRelInfo *) lfirst(appendRelCell);

				/*
				 * We're only interested in UNION ALL clauses and parent_reloid is invalid
				 * only for UNION ALL (i.e., equals to a legitimate Oid for inheritance)
				 */
				if (appendRelInfo->parent_reloid != InvalidOid)
				{
					continue;
				}

				/* set the varno accordingly for this specific child */
				varToBeAdded->varno = appendRelInfo->child_relid;

				AddToAttributeEquivalenceClass(root, varToBeAdded,
											   attributeEquivalanceClass);
			}
		}
		else if (subquery->setOperations)
		{
			AddUnionSetOperationsToAttributeEquivalenceClass(subquery,
															 (SetOperationStmt *)
															 subquery->setOperations,
															 varToBeAdded,
															 attributeEquivalanceClass);
		}
		else if (varToBeAdded && IsA(varToBeAdded, Var) && varToBeAdded->varlevelsup == 0)
		{
			AddToAttributeEquivalenceClass(baseRelOptInfo->subroot, varToBeAdded,
										   attributeEquivalanceClass);
		}
	}
}


/*
 * AddUnionSetOperationsToAttributeEquivalenceClass recursively iterates on all the
 * setOperations and adds each corresponding target entry to the given equivalence
 * class.
 */
static void
AddUnionSetOperationsToAttributeEquivalenceClass(Query *query,
												 SetOperationStmt *setOperation,
												 Var *varToBeAdded,
												 AttributeEquivalenceClass **
												 attributeEquivalenceClass)
{
	Node *leftArgument = setOperation->larg;
	Node *rightArgument = setOperation->rarg;

	/* we're only interested in UNIONs */
	if (setOperation->op != SETOP_UNION)
	{
		return;
	}

	if (IsA(leftArgument, RangeTblRef))
	{
		AddRangeTableReferenceToEquivalenceClass(query, leftArgument,
												 attributeEquivalenceClass,
												 varToBeAdded);
	}
	else if (IsA(leftArgument, SetOperationStmt))
	{
		SetOperationStmt *leftUnion = (SetOperationStmt *) leftArgument;

		AddUnionSetOperationsToAttributeEquivalenceClass(query, leftUnion, varToBeAdded,
														 attributeEquivalenceClass);
	}

	if (IsA(rightArgument, RangeTblRef))
	{
		AddRangeTableReferenceToEquivalenceClass(query, rightArgument,
												 attributeEquivalenceClass,
												 varToBeAdded);
	}
	else if (IsA(rightArgument, SetOperationStmt))
	{
		SetOperationStmt *rightUnion = (SetOperationStmt *) rightArgument;

		AddUnionSetOperationsToAttributeEquivalenceClass(query, rightUnion, varToBeAdded,
														 attributeEquivalenceClass);
	}
}


/*
 * AddRangeTableReferenceToEquivalenceClass adds range table entry (referenced
 * by rteReferenceArgument on the query) to the given attributeEquivalenceClass. It is
 * almost a wrapper around AddRteToAttributeEquivalenceClass() function.
 */
static void
AddRangeTableReferenceToEquivalenceClass(Query *query, Node *rteReferenceArgument,
										 AttributeEquivalenceClass **
										 attributeEquivalenceClass,
										 Var *varToBeAdded)
{
	RangeTblRef *rangeTableReference = (RangeTblRef *) rteReferenceArgument;
	RangeTblEntry *rangeTableEntry = rt_fetch(rangeTableReference->rtindex,
											  query->rtable);
	Query *subquery = NULL;
	TargetEntry *targetEntry = NULL;
	Var *targetVar = NULL;
	RangeTblEntry *targetRangeTableEntry = NULL;

	Assert(rangeTableEntry->rtekind == RTE_SUBQUERY);

	subquery = rangeTableEntry->subquery;
	targetEntry = get_tle_by_resno(subquery->targetList, varToBeAdded->varattno);
	if (!IsA(targetEntry->expr, Var))
	{
		return;
	}

	targetVar = (Var *) targetEntry->expr;
	targetRangeTableEntry = rt_fetch(targetVar->varno, subquery->rtable);

	/* we currently only support relations within UNIONs */
	if (targetRangeTableEntry->rtekind != RTE_RELATION)
	{
		return;
	}

	AddRteToAttributeEquivalenceClass(targetRangeTableEntry, attributeEquivalenceClass,
									  targetVar);
}


/*
 * AddRteToAttributeEquivalenceClass adds the given var to the given equivalence
 * class using the rteIdentity provided by the rangeTableEntry. Note that
 * rteIdentities are only assigned to RTE_RELATIONs and this function asserts
 * the input rte to be an RTE_RELATION.
 *
 * Note that this function only adds partition keys to the attributeEquivalanceClass.
 * This implies that there wouldn't be any columns for reference tables.
 */
static void
AddRteToAttributeEquivalenceClass(RangeTblEntry *rangeTableEntry,
								  AttributeEquivalenceClass **attrEquivalenceClass,
								  Var *varToBeAdded)
{
	AttributeEquivalenceClassMember *attributeEqMember = NULL;
	Oid relationId = InvalidOid;
	Var *relationPartitionKey = NULL;

	Assert(rangeTableEntry->rtekind == RTE_RELATION);

	relationId = rangeTableEntry->relid;
	if (PartitionMethod(relationId) == DISTRIBUTE_BY_NONE)
	{
		return;
	}

	relationPartitionKey = PartitionKey(relationId);
	if (relationPartitionKey->varattno != varToBeAdded->varattno)
	{
		return;
	}

	attributeEqMember = palloc0(sizeof(AttributeEquivalenceClassMember));

	attributeEqMember->varattno = varToBeAdded->varattno;
	attributeEqMember->varno = varToBeAdded->varno;
	attributeEqMember->rteIdendity = GetRTEIdentity(rangeTableEntry);
	attributeEqMember->relationId = rangeTableEntry->relid;

	(*attrEquivalenceClass)->equivalentAttributes =
		lappend((*attrEquivalenceClass)->equivalentAttributes,
				attributeEqMember);
}


/*
 * AttributeClassContainsAttributeClassMember returns true if it the input class member
 * is already exists in the attributeEquivalenceClass. An equality is identified by the
 * varattno and rteIdentity.
 */
static bool
AttributeClassContainsAttributeClassMember(AttributeEquivalenceClassMember *inputMember,
										   AttributeEquivalenceClass *
										   attributeEquivalenceClass)
{
	ListCell *classCell = NULL;
	foreach(classCell, attributeEquivalenceClass->equivalentAttributes)
	{
		AttributeEquivalenceClassMember *memberOfClass = lfirst(classCell);
		if (memberOfClass->rteIdendity == inputMember->rteIdendity &&
			memberOfClass->varattno == inputMember->varattno)
		{
			return true;
		}
	}

	return false;
}


/*
 * AddAttributeClassToAttributeClassList checks for certain properties of the
 * input attributeEquivalance before adding it to the attributeEquivalenceList.
 *
 * Firstly, the function skips adding NULL attributeEquivalance to the list.
 * Secondly, since an attribute equivalence class with a single member does
 * not contribute to our purposes, we skip such classed adding to the list.
 */
static List *
AddAttributeClassToAttributeClassList(AttributeEquivalenceClass *attributeEquivalance,
									  List *attributeEquivalenceList)
{
	List *equivalentAttributes = NULL;

	if (attributeEquivalance == NULL)
	{
		return attributeEquivalenceList;
	}

	equivalentAttributes = attributeEquivalance->equivalentAttributes;
	if (list_length(equivalentAttributes) < 2)
	{
		return attributeEquivalenceList;
	}

	attributeEquivalenceList = lappend(attributeEquivalenceList,
									   attributeEquivalance);

	return attributeEquivalenceList;
}


/*
 * TopLevelUnionQuery gets a queryTree and returns true if top level query
 * contains a UNION set operation. Note that the function allows upper level
 * queries with a single range table element. In other words, we allow top
 * level unions being wrapped into aggregations queries and/or simple projection
 * queries that only selects some fields from the lower level queries.
 *
 * If there exists joins before the set operations, the function returns false.
 * Similarly, if the query does not contain any union set operations, the
 * function returns false.
 */
static bool
TopLevelUnionQuery(Query *queryTree)
{
	List *rangeTableList = queryTree->rtable;
	Node *setOperations = queryTree->setOperations;
	RangeTblEntry *rangeTableEntry = NULL;
	Query *subqueryTree = NULL;

	/* don't allow joines on top of unions */
	if (list_length(rangeTableList) > 1)
	{
		return false;
	}

	rangeTableEntry = linitial(rangeTableList);
	if (rangeTableEntry->rtekind != RTE_SUBQUERY)
	{
		return false;
	}

	subqueryTree = rangeTableEntry->subquery;
	setOperations = subqueryTree->setOperations;
	if (setOperations != NULL)
	{
		SetOperationStmt *setOperationStatement = (SetOperationStmt *) setOperations;

		if (setOperationStatement->op != SETOP_UNION)
		{
			return false;
		}

		return true;
	}

	return TopLevelUnionQuery(subqueryTree);
}


/*
 * RelationRestrictionPartitionKeyIndex gets a relation restriction and finds the
 * index that the partition key of the relation exists in the query. The query is
 * found in the planner info of the relation restriction.
 */
static Index
RelationRestrictionPartitionKeyIndex(RelationRestriction *relationRestriction)
{
	PlannerInfo *relationPlannerRoot = NULL;
	Query *relationPlannerParseQuery = NULL;
	List *relationTargetList = NIL;
	ListCell *targetEntryCell = NULL;
	Index partitionKeyTargetAttrIndex = 0;

	relationPlannerRoot = relationRestriction->plannerInfo;
	relationPlannerParseQuery = relationPlannerRoot->parse;
	relationTargetList = relationPlannerParseQuery->targetList;

	foreach(targetEntryCell, relationTargetList)
	{
		TargetEntry *targetEntry = (TargetEntry *) lfirst(targetEntryCell);
		Expr *targetExpression = targetEntry->expr;

		partitionKeyTargetAttrIndex++;

		if (!targetEntry->resjunk &&
			IsPartitionColumn(targetExpression, relationPlannerParseQuery))
		{
			Var *targetColumn = (Var *) targetExpression;

			if (targetColumn->varno == relationRestriction->index)
			{
				return partitionKeyTargetAttrIndex;
			}
		}
	}

	return InvalidAttrNumber;
}
