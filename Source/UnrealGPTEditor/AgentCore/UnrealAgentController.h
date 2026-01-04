// Copyright 2024-2026 UnrealGPT. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UnrealAgentTypes.h"
#include "UnrealAgentGoal.h"
#include "UnrealAgentPlan.h"
#include "UnrealAgentWorldModel.h"
#include "UnrealAgentEvaluator.h"
#include "UnrealAgentExecutor.h"
#include "UnrealAgentPlanner.h"
#include "UnrealAgentLLMInterface.h"

// Forward declarations
class UUnrealGPTAgentClient;

/**
 * Agent state machine states.
 */
enum class EAgentState : uint8
{
	Idle,               // Waiting for user input
	ParsingGoal,        // Parsing user request into goal
	Planning,           // Generating plan for goal
	Executing,          // Executing plan steps
	Evaluating,         // Evaluating goal completion
	Recovering,         // Handling failure, replanning
	WaitingForUser,     // Needs user input to continue
	Completed,          // Goal completed successfully
	Failed              // Goal failed, cannot recover
};

/**
 * The main agent controller - YOUR CODE runs the show.
 *
 * This is the brain of the true agent:
 * - Orchestrates goal -> plan -> execute -> evaluate cycle
 * - Makes decisions based on code logic, not LLM
 * - Uses LLM as advisor for suggestions
 * - Handles recovery from failures
 *
 * Key principle: Control flow is explicit and deterministic.
 * The LLM cannot make the agent do arbitrary things.
 */
class UNREALGPTEDITOR_API FAgentController
{
public:
	FAgentController();
	~FAgentController();

	/** Initialize with existing agent client (for tool execution) */
	void Initialize(UUnrealGPTAgentClient* InAgentClient);

	// ==================== USER INTERFACE ====================

	/**
	 * Handle a user request - main entry point.
	 * Parses into goal and starts agent loop.
	 */
	void HandleUserRequest(const FString& UserInput);

	/**
	 * Handle user response to a question.
	 */
	void HandleUserResponse(const FString& Response);

	/**
	 * Cancel current operation.
	 */
	void Cancel();

	/**
	 * Reset agent to idle state.
	 */
	void Reset();

	// ==================== AGENT LOOP ====================

	/**
	 * Run one iteration of the agent loop.
	 * Call this from tick or manually advance.
	 */
	void Tick();

	/**
	 * Run agent loop until completion or user input needed.
	 * Blocking call - use for testing or synchronous execution.
	 */
	void RunToCompletion();

	/**
	 * Check if agent loop should continue.
	 */
	bool ShouldContinue() const;

	// ==================== STATE ACCESS ====================

	/** Get current agent state */
	EAgentState GetState() const { return CurrentState; }

	/** Get current goal (if any) */
	FAgentGoal* GetCurrentGoal();

	/** Get current plan (if any) */
	FAgentPlan* GetCurrentPlan() { return CurrentPlan.IsValid() ? &CurrentPlan : nullptr; }

	/** Get world model */
	FAgentWorldModel& GetWorldModel() { return WorldModelManager.GetWorldModel(); }

	/** Get goal manager */
	FAgentGoalManager& GetGoalManager() { return GoalManager; }

	/** Get LLM interface */
	FAgentLLMInterface& GetLLMInterface() { return LLMInterface; }

	// ==================== DELEGATES ====================

	/** Called when agent state changes */
	DECLARE_DELEGATE_TwoParams(FOnStateChanged, EAgentState /*OldState*/, EAgentState /*NewState*/);
	FOnStateChanged OnStateChanged;

	/** Called when a goal is completed */
	DECLARE_DELEGATE_TwoParams(FOnGoalCompleted, const FAgentGoal&, const FGoalEvaluation&);
	FOnGoalCompleted OnGoalCompleted;

	/** Called when a goal fails */
	DECLARE_DELEGATE_TwoParams(FOnGoalFailed, const FAgentGoal&, const FString& /*Reason*/);
	FOnGoalFailed OnGoalFailed;

	/** Called when a step completes */
	DECLARE_DELEGATE_TwoParams(FOnStepCompleted, const FPlanStep&, const FStepResult&);
	FOnStepCompleted OnStepCompleted;

	/** Called when agent needs user input */
	DECLARE_DELEGATE_OneParam(FOnNeedUserInput, const FString& /*Question*/);
	FOnNeedUserInput OnNeedUserInput;

	/** Called to report progress */
	DECLARE_DELEGATE_TwoParams(FOnProgress, const FString& /*Message*/, float /*Percent*/);
	FOnProgress OnProgress;

	// ==================== CONFIGURATION ====================

	/** Set maximum iterations per goal */
	void SetMaxIterations(int32 Max) { MaxIterationsPerGoal = Max; }

	/** Set whether to use LLM for planning */
	void SetUseLLM(bool bUse) { Planner.SetUseLLM(bUse); }

	/** Set whether to auto-verify after steps */
	void SetAutoVerification(bool bAuto) { Planner.SetAutoVerification(bAuto); }

private:
	// ==================== STATE MACHINE ====================

	void SetState(EAgentState NewState);
	FString StateToString(EAgentState State);

	// State handlers
	void HandleIdleState();
	void HandleParsingGoalState();
	void HandlePlanningState();
	void HandleExecutingState();
	void HandleEvaluatingState();
	void HandleRecoveringState();
	void HandleWaitingForUserState();

	// ==================== GOAL MANAGEMENT ====================

	/** Parse user input into a goal */
	FAgentGoal ParseGoal(const FString& UserInput);

	/** Activate next pending goal */
	bool ActivateNextGoal();

	/** Complete the current goal */
	void CompleteGoal(const FAgentGoal& Goal, const FGoalEvaluation& Evaluation);

	/** Fail the current goal */
	void FailGoal(FAgentGoal& Goal, const FString& Reason);

	// ==================== PLAN EXECUTION ====================

	/** Create a plan for the current goal */
	bool CreatePlanForCurrentGoal();

	/** Execute the next step in current plan */
	void ExecuteNextStep();

	/** Handle a completed step */
	void HandleStepResult(const FStepResult& Result);

	/** Advance to next step based on result */
	void AdvancePlan(const FStepResult& Result);

	// ==================== RECOVERY ====================

	/** Determine recovery action for a failure */
	void HandleFailure(const FStepResult& FailedResult);

	/** Retry the current step */
	void RetryCurrentStep();

	/** Retry with suggested fix applied */
	void RetryWithFix(const FSuggestedFix& Fix);

	/** Generate new plan from current state */
	void Replan();

	/** Ask user for help */
	void EscalateToUser(const FString& Reason);

	// ==================== WORLD MODEL ====================

	/** Refresh world model if needed */
	void RefreshWorldModelIfNeeded();

	// ==================== COMPONENTS ====================

	UUnrealGPTAgentClient* AgentClient = nullptr;

	FAgentGoalManager GoalManager;
	FAgentWorldModelManager WorldModelManager;
	FAgentPlanner Planner;
	FAgentExecutor Executor;
	FAgentEvaluator Evaluator;
	FAgentLLMInterface LLMInterface;

	// ==================== STATE ====================

	EAgentState CurrentState = EAgentState::Idle;
	FAgentPlan CurrentPlan;
	FString PendingUserInput;
	FString PendingQuestion;

	// Iteration tracking
	int32 CurrentIteration = 0;
	int32 MaxIterationsPerGoal = 50;

	// Flags
	bool bCancelRequested = false;
	bool bInitialized = false;
};
