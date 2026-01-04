// Copyright 2024-2026 UnrealGPT. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UnrealAgentTypes.h"
#include "UnrealAgentPlan.h"
#include "UnrealAgentWorldModel.h"
#include "UnrealAgentEvaluator.h"

// Forward declarations
class UUnrealGPTAgentClient;

/**
 * Executes plan steps with precondition checking and outcome verification.
 *
 * This is YOUR CODE controlling execution:
 * - Checks preconditions BEFORE running a tool
 * - Updates world model AFTER running a tool
 * - Verifies expected outcomes
 * - Does NOT blindly execute whatever the LLM says
 */
class UNREALGPTEDITOR_API FAgentExecutor
{
public:
	FAgentExecutor();

	/** Set the tool dispatcher (connects to your existing tool execution) */
	void SetAgentClient(UUnrealGPTAgentClient* InClient) { AgentClient = InClient; }

	/** Set the evaluator for outcome verification */
	void SetEvaluator(FAgentEvaluator* InEvaluator) { Evaluator = InEvaluator; }

	// ==================== STEP EXECUTION ====================

	/**
	 * Execute a single plan step with full lifecycle:
	 * 1. Check preconditions
	 * 2. Execute tool
	 * 3. Update world model
	 * 4. Verify outcomes
	 */
	FStepResult ExecuteStep(FPlanStep& Step, FAgentWorldModel& WorldModel,
		const FString& GoalId = TEXT(""));

	/**
	 * Check preconditions before executing a step.
	 */
	FPreconditionResult CheckPreconditions(const FPlanStep& Step, const FAgentWorldModel& WorldModel);

	/**
	 * Execute the actual tool call.
	 * This bridges to your existing tool dispatcher.
	 */
	FToolResult ExecuteTool(const FString& ToolName, const TMap<FString, FString>& Args);

	/**
	 * Update world model based on tool execution.
	 */
	void UpdateWorldModel(const FPlanStep& Step, const FToolResult& Result,
		FAgentWorldModel& WorldModel, const FString& GoalId);

	// ==================== BATCH EXECUTION ====================

	/**
	 * Execute multiple steps in sequence.
	 * Stops on first failure unless ContinueOnError is true.
	 */
	TArray<FStepResult> ExecuteSteps(TArray<FPlanStep>& Steps, FAgentWorldModel& WorldModel,
		const FString& GoalId = TEXT(""), bool bContinueOnError = false);

	// ==================== ASYNC EXECUTION ====================

	/** Delegate for step completion */
	DECLARE_DELEGATE_TwoParams(FOnStepComplete, const FPlanStep&, const FStepResult&);
	FOnStepComplete OnStepComplete;

	/** Delegate for tool result received */
	DECLARE_DELEGATE_TwoParams(FOnToolResult, const FString&, const FToolResult&);
	FOnToolResult OnToolResult;

	/**
	 * Execute a step asynchronously.
	 * Completion is signaled via OnStepComplete delegate.
	 */
	void ExecuteStepAsync(FPlanStep& Step, FAgentWorldModel& WorldModel,
		const FString& GoalId = TEXT(""));

	// ==================== STATE ====================

	/** Check if executor is currently running a step */
	bool IsExecuting() const { return bIsExecuting; }

	/** Cancel current execution */
	void Cancel();

	/** Get the last step result */
	const FStepResult& GetLastResult() const { return LastResult; }

private:
	// ==================== TOOL EXECUTION BRIDGE ====================

	/**
	 * Convert tool arguments map to JSON string for existing dispatcher.
	 */
	FString ArgsToJson(const TMap<FString, FString>& Args);

	/**
	 * Parse tool result from JSON string returned by existing dispatcher.
	 */
	FToolResult ParseToolResult(const FString& ToolName, const FString& ResultJson);

	/**
	 * Execute tool synchronously (blocking).
	 * In production, this would interface with your existing UnrealGPTToolDispatcher.
	 */
	FString ExecuteToolInternal(const FString& ToolName, const FString& ArgsJson);

	// ==================== PRECONDITION EVALUATION ====================

	/**
	 * Evaluate a single precondition expression against world model.
	 */
	bool EvaluatePrecondition(const FStepPrecondition& Precondition, const FAgentWorldModel& WorldModel);

	/**
	 * Parse and evaluate a precondition check expression.
	 * Examples:
	 * - "WorldModel.FindActor('ActorId') != null"
	 * - "WorldModel.IsAreaClear(Bounds)"
	 * - "WorldModel.CanModifyActor('ActorId')"
	 */
	bool EvaluateCheckExpression(const FString& Expression, const FAgentWorldModel& WorldModel);

	// ==================== WORLD MODEL UPDATES ====================

	/**
	 * Update world model based on specific tool results.
	 */
	void UpdateWorldModelFromSceneQuery(const FToolResult& Result, FAgentWorldModel& WorldModel);
	void UpdateWorldModelFromGetActor(const FToolResult& Result, FAgentWorldModel& WorldModel);
	void UpdateWorldModelFromSetTransform(const TMap<FString, FString>& Args, const FToolResult& Result,
		FAgentWorldModel& WorldModel, const FString& GoalId, const FString& StepId);
	void UpdateWorldModelFromDuplicate(const FToolResult& Result, FAgentWorldModel& WorldModel,
		const FString& GoalId, const FString& StepId);
	void UpdateWorldModelFromPythonExecute(const FToolResult& Result, FAgentWorldModel& WorldModel,
		const FString& GoalId, const FString& StepId);

	// ==================== STATE ====================

	UUnrealGPTAgentClient* AgentClient = nullptr;
	FAgentEvaluator* Evaluator = nullptr;

	bool bIsExecuting = false;
	bool bCancelRequested = false;
	FStepResult LastResult;

	/** Currently executing step (for async) */
	FPlanStep* CurrentStep = nullptr;
	FAgentWorldModel* CurrentWorldModel = nullptr;
	FString CurrentGoalId;
};
