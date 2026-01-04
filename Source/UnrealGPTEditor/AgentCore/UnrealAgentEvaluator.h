// Copyright 2024-2026 UnrealGPT. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UnrealAgentTypes.h"
#include "UnrealAgentGoal.h"
#include "UnrealAgentPlan.h"
#include "UnrealAgentWorldModel.h"

/**
 * Evaluates goal success and step outcomes using YOUR CODE, not LLM judgment.
 *
 * This is a critical component that makes this a true agent:
 * - Success criteria are evaluated programmatically
 * - Recovery decisions are made by code logic
 * - No hallucination risk - either the condition is met or it isn't
 */
class UNREALGPTEDITOR_API FAgentEvaluator
{
public:
	FAgentEvaluator() = default;

	// ==================== GOAL EVALUATION ====================

	/**
	 * Evaluate if a goal is complete based on its success criteria.
	 * This is YOUR CODE checking actual conditions, not LLM "thinking" it's done.
	 */
	FGoalEvaluation EvaluateGoal(FAgentGoal& Goal, const FAgentWorldModel& WorldModel);

	/**
	 * Evaluate a single success criterion.
	 */
	bool EvaluateCriterion(FSuccessCriterion& Criterion, const FAgentWorldModel& WorldModel);

	// ==================== STEP OUTCOME EVALUATION ====================

	/**
	 * Verify expected outcomes after a step executes.
	 */
	FOutcomeResult VerifyOutcomes(const FPlanStep& Step, const FToolResult& ToolResult,
		const FAgentWorldModel& WorldModel);

	/**
	 * Evaluate a single expected outcome.
	 */
	bool EvaluateOutcome(const FExpectedOutcome& Outcome, const FToolResult& ToolResult,
		const FAgentWorldModel& WorldModel);

	// ==================== RECOVERY DECISIONS ====================

	/**
	 * Determine what to do when a step fails.
	 * This is YOUR CODE making the decision based on failure patterns.
	 */
	ERecoveryAction DetermineRecoveryAction(const FAgentGoal& Goal, const FStepResult& FailedStep);

	/**
	 * Generate suggested fixes for a failed step.
	 */
	TArray<FSuggestedFix> GenerateSuggestedFixes(const FPlanStep& Step, const FStepResult& Result,
		const FAgentWorldModel& WorldModel);

	// ==================== CRITERION PARSING ====================

	/**
	 * Parse and evaluate a scene query criterion.
	 * Format: "class=X, label contains 'Y', count >= N"
	 */
	bool EvaluateSceneQueryCriterion(const FString& Query, const FAgentWorldModel& WorldModel);

	/**
	 * Parse and evaluate a property check criterion.
	 * Format: "actor.Location.Z > 0" or "WorldModel.CountActors(...) >= 10"
	 */
	bool EvaluatePropertyCheckCriterion(const FString& Check, const FAgentWorldModel& WorldModel);

	/**
	 * Evaluate an asset exists criterion.
	 * Format: "/Game/MyAssets/NewTexture"
	 */
	bool EvaluateAssetExistsCriterion(const FString& AssetPath);

	// ==================== TOOL RESULT ANALYSIS ====================

	/**
	 * Analyze a tool result for success indicators.
	 */
	bool AnalyzeToolResultSuccess(const FString& ToolName, const FToolResult& Result);

	/**
	 * Extract affected actor IDs from a tool result.
	 */
	TArray<FString> ExtractAffectedActors(const FString& ToolName, const FToolResult& Result);

private:
	// ==================== CRITERION PARSING HELPERS ====================

	/** Parse a scene query into filter components */
	struct FSceneQueryFilter
	{
		FString ClassFilter;
		FString LabelFilter;
		FString NameFilter;
		FString TagFilter;
		int32 ExpectedCount = -1;  // -1 means any
		FString CountOperator;     // ">=", "==", "<=", ">", "<"
	};

	FSceneQueryFilter ParseSceneQueryCriterion(const FString& Query);

	/** Check if count matches expected */
	bool CheckCountCondition(int32 ActualCount, const FString& Operator, int32 ExpectedCount);

	// ==================== FAILURE PATTERN RECOGNITION ====================

	/** Recognize common failure patterns for recovery decisions */
	enum class EFailurePattern
	{
		Unknown,
		ActorNotFound,
		InvalidArguments,
		PreconditionFailed,
		Timeout,
		PythonError,
		PermissionDenied,
		ResourceBusy
	};

	EFailurePattern RecognizeFailurePattern(const FStepResult& Result);

	/** Get recovery action for a failure pattern */
	ERecoveryAction GetRecoveryForPattern(EFailurePattern Pattern, int32 RetryCount, int32 MaxRetries);
};
