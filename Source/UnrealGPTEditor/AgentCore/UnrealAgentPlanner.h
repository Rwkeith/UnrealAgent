// Copyright 2024-2026 UnrealGPT. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UnrealAgentTypes.h"
#include "UnrealAgentGoal.h"
#include "UnrealAgentPlan.h"
#include "UnrealAgentWorldModel.h"

// Forward declarations
class FAgentLLMInterface;

/**
 * Creates and validates plans for achieving goals.
 *
 * Key difference from LLM-controlled:
 * - LLM SUGGESTS plans, but YOUR CODE validates them
 * - Plans are validated before execution (tool names, arguments, branching)
 * - Invalid plans are rejected or corrected
 * - YOU can also generate plans programmatically for common tasks
 */
class UNREALGPTEDITOR_API FAgentPlanner
{
public:
	FAgentPlanner();

	/** Set the LLM interface for plan suggestions */
	void SetLLMInterface(FAgentLLMInterface* InLLM) { LLMInterface = InLLM; }

	// ==================== PLAN CREATION ====================

	/**
	 * Create a plan for a goal.
	 * Uses LLM to suggest plan, then validates it.
	 */
	FAgentPlan CreatePlan(const FAgentGoal& Goal, const FAgentWorldModel& WorldModel);

	/**
	 * Create a plan from LLM suggestion (with validation).
	 */
	FAgentPlan CreatePlanFromLLMSuggestion(const FAgentGoal& Goal, const FAgentWorldModel& WorldModel);

	/**
	 * Create a plan programmatically (for common patterns).
	 */
	FAgentPlan CreatePlanProgrammatically(const FAgentGoal& Goal, const FAgentWorldModel& WorldModel);

	// ==================== PLAN VALIDATION ====================

	/**
	 * Validate a plan before execution.
	 */
	FPlanValidation ValidatePlan(const FAgentPlan& Plan);

	/**
	 * Validate and fix a plan (correct minor issues automatically).
	 */
	FAgentPlan ValidateAndFixPlan(const FAgentPlan& Plan, const FAgentWorldModel& WorldModel);

	// ==================== REPLANNING ====================

	/**
	 * Generate a new plan after a step fails.
	 */
	FAgentPlan ReplanFromFailure(const FAgentPlan& FailedPlan, int32 FailedStepIndex,
		const FStepResult& FailedResult, const FAgentWorldModel& WorldModel);

	/**
	 * Adapt plan based on new world state.
	 */
	FAgentPlan AdaptPlan(const FAgentPlan& OriginalPlan, const FAgentWorldModel& WorldModel);

	// ==================== PLAN TEMPLATES ====================

	/**
	 * Get a plan template for a common task type.
	 */
	FAgentPlan GetPlanTemplate(const FString& TaskType);

	/**
	 * Check if a goal matches a known pattern.
	 */
	FString RecognizeGoalPattern(const FAgentGoal& Goal);

	// ==================== STEP GENERATION ====================

	/**
	 * Generate an observation step (scene_query, get_actor, etc.).
	 */
	FPlanStep CreateObservationStep(const FString& Description, const FActorQuery& Query);

	/**
	 * Generate a verification step.
	 */
	FPlanStep CreateVerificationStep(const FString& Description, const FSuccessCriterion& Criterion);

	/**
	 * Generate a tool call step with validation.
	 */
	FPlanStep CreateToolCallStep(const FString& Description, const FString& ToolName,
		const TMap<FString, FString>& Args);

	// ==================== CONFIGURATION ====================

	/** Enable/disable LLM for planning (fallback to programmatic) */
	void SetUseLLM(bool bUse) { bUseLLMForPlanning = bUse; }

	/** Set maximum steps per plan */
	void SetMaxSteps(int32 Max) { MaxStepsPerPlan = Max; }

	/** Set whether to auto-add verification steps */
	void SetAutoVerification(bool bAuto) { bAutoAddVerification = bAuto; }

private:
	// ==================== LLM PLAN PARSING ====================

	/**
	 * Parse plan steps from LLM response.
	 * Validates each step as it parses.
	 */
	TArray<FPlanStep> ParseLLMPlanResponse(const FString& Response);

	/**
	 * Sanitize a step parsed from LLM (fix common issues).
	 */
	FPlanStep SanitizeStep(const FPlanStep& Step);

	/**
	 * Build prompt for LLM plan generation.
	 */
	FString BuildPlanPrompt(const FAgentGoal& Goal, const FAgentWorldModel& WorldModel);

	// ==================== PROGRAMMATIC PLAN GENERATION ====================

	/**
	 * Generate plan for "spawn objects" type goals.
	 */
	FAgentPlan GenerateSpawnPlan(const FAgentGoal& Goal, const FAgentWorldModel& WorldModel);

	/**
	 * Generate plan for "move/transform" type goals.
	 */
	FAgentPlan GenerateTransformPlan(const FAgentGoal& Goal, const FAgentWorldModel& WorldModel);

	/**
	 * Generate plan for "query/inspect" type goals.
	 */
	FAgentPlan GenerateInspectionPlan(const FAgentGoal& Goal, const FAgentWorldModel& WorldModel);

	/**
	 * Generate plan for "arrange/layout" type goals.
	 */
	FAgentPlan GenerateArrangementPlan(const FAgentGoal& Goal, const FAgentWorldModel& WorldModel);

	// ==================== STEP INJECTION ====================

	/**
	 * Add observation steps at plan start if needed.
	 */
	void InjectObservationSteps(FAgentPlan& Plan, const FAgentWorldModel& WorldModel);

	/**
	 * Add verification steps after modifying steps.
	 */
	void InjectVerificationSteps(FAgentPlan& Plan, const FAgentGoal& Goal);

	/**
	 * Add preconditions based on world model.
	 */
	void AddPreconditionsFromWorldModel(FPlanStep& Step, const FAgentWorldModel& WorldModel);

	// ==================== PATTERN RECOGNITION ====================

	/** Known goal patterns for programmatic planning */
	enum class EGoalPattern
	{
		Unknown,
		SpawnObjects,       // "place", "spawn", "create", "add" actors
		TransformObjects,   // "move", "rotate", "scale", "transform"
		QueryScene,         // "find", "list", "show", "count"
		ArrangeObjects,     // "arrange", "layout", "organize", "grid", "circle"
		DeleteObjects,      // "delete", "remove", "clear"
		ModifyProperties    // "change", "set", "update" properties
	};

	EGoalPattern DetectGoalPattern(const FAgentGoal& Goal);
	FString GoalPatternToString(EGoalPattern Pattern);

	// ==================== STATE ====================

	FAgentLLMInterface* LLMInterface = nullptr;

	bool bUseLLMForPlanning = true;
	int32 MaxStepsPerPlan = 20;
	bool bAutoAddVerification = true;
};
