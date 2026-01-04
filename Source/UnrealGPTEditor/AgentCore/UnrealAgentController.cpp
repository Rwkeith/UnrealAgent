// Copyright 2024-2026 UnrealGPT. All Rights Reserved.

#include "UnrealAgentController.h"
#include "UnrealGPTAgentClient.h"

FAgentController::FAgentController()
{
}

FAgentController::~FAgentController()
{
}

void FAgentController::Initialize(UUnrealGPTAgentClient* InAgentClient)
{
	AgentClient = InAgentClient;

	// Wire up components
	Executor.SetAgentClient(AgentClient);
	Executor.SetEvaluator(&Evaluator);

	Planner.SetLLMInterface(&LLMInterface);

	LLMInterface.SetAgentClient(AgentClient);

	// Set up executor callbacks
	Executor.OnStepComplete.BindLambda([this](const FPlanStep& Step, const FStepResult& Result)
	{
		HandleStepResult(Result);
	});

	bInitialized = true;

	UE_LOG(LogTemp, Log, TEXT("FAgentController initialized"));
}

// ==================== USER INTERFACE ====================

void FAgentController::HandleUserRequest(const FString& UserInput)
{
	if (!bInitialized)
	{
		UE_LOG(LogTemp, Error, TEXT("Agent controller not initialized"));
		return;
	}

	if (CurrentState != EAgentState::Idle && CurrentState != EAgentState::WaitingForUser)
	{
		UE_LOG(LogTemp, Warning, TEXT("Agent is busy, cannot handle new request"));
		return;
	}

	// Store input and transition to parsing
	PendingUserInput = UserInput;
	SetState(EAgentState::ParsingGoal);
}

void FAgentController::HandleUserResponse(const FString& Response)
{
	if (CurrentState != EAgentState::WaitingForUser)
	{
		UE_LOG(LogTemp, Warning, TEXT("Agent is not waiting for user input"));
		return;
	}

	PendingUserInput = Response;

	// Return to previous state (typically Executing or Recovering)
	SetState(EAgentState::Executing);
}

void FAgentController::Cancel()
{
	bCancelRequested = true;
	Executor.Cancel();

	// Cancel any pending LLM requests
	if (LLMInterface.IsRequestInProgress())
	{
		LLMInterface.CancelAsyncRequest();
	}

	if (CurrentState != EAgentState::Idle)
	{
		FAgentGoal* Goal = GetCurrentGoal();
		if (Goal)
		{
			Goal->Status = EAgentGoalStatus::Cancelled;
		}
		SetState(EAgentState::Idle);
	}
}

void FAgentController::Reset()
{
	Cancel();
	GoalManager.Clear();
	WorldModelManager.GetWorldModel().Clear();
	CurrentPlan = FAgentPlan();
	CurrentIteration = 0;
	bCancelRequested = false;
	SetState(EAgentState::Idle);
}

// ==================== AGENT LOOP ====================

void FAgentController::Tick()
{
	if (bCancelRequested)
	{
		return;
	}

	switch (CurrentState)
	{
	case EAgentState::Idle:
		HandleIdleState();
		break;

	case EAgentState::ParsingGoal:
		HandleParsingGoalState();
		break;

	case EAgentState::Planning:
		HandlePlanningState();
		break;

	case EAgentState::Executing:
		HandleExecutingState();
		break;

	case EAgentState::Evaluating:
		HandleEvaluatingState();
		break;

	case EAgentState::Recovering:
		HandleRecoveringState();
		break;

	case EAgentState::WaitingForUser:
		HandleWaitingForUserState();
		break;

	case EAgentState::Completed:
	case EAgentState::Failed:
		// Terminal states - do nothing
		break;
	}
}

void FAgentController::RunToCompletion()
{
	while (ShouldContinue())
	{
		Tick();
	}
}

bool FAgentController::ShouldContinue() const
{
	if (bCancelRequested)
	{
		return false;
	}

	switch (CurrentState)
	{
	case EAgentState::Idle:
	case EAgentState::WaitingForUser:
	case EAgentState::Completed:
	case EAgentState::Failed:
		return false;

	default:
		return CurrentIteration < MaxIterationsPerGoal;
	}
}

// ==================== STATE ACCESS ====================

FAgentGoal* FAgentController::GetCurrentGoal()
{
	return GoalManager.GetActiveGoal();
}

// ==================== STATE MACHINE ====================

void FAgentController::SetState(EAgentState NewState)
{
	if (CurrentState == NewState)
	{
		return;
	}

	EAgentState OldState = CurrentState;
	CurrentState = NewState;

	UE_LOG(LogTemp, Log, TEXT("Agent state: %s -> %s"),
		*StateToString(OldState), *StateToString(NewState));

	if (OnStateChanged.IsBound())
	{
		OnStateChanged.Execute(OldState, NewState);
	}
}

FString FAgentController::StateToString(EAgentState State)
{
	switch (State)
	{
	case EAgentState::Idle: return TEXT("Idle");
	case EAgentState::ParsingGoal: return TEXT("ParsingGoal");
	case EAgentState::Planning: return TEXT("Planning");
	case EAgentState::Executing: return TEXT("Executing");
	case EAgentState::Evaluating: return TEXT("Evaluating");
	case EAgentState::Recovering: return TEXT("Recovering");
	case EAgentState::WaitingForUser: return TEXT("WaitingForUser");
	case EAgentState::Completed: return TEXT("Completed");
	case EAgentState::Failed: return TEXT("Failed");
	default: return TEXT("Unknown");
	}
}

void FAgentController::HandleIdleState()
{
	// Check if there are pending goals to work on
	if (GoalManager.HasActiveGoals())
	{
		if (ActivateNextGoal())
		{
			SetState(EAgentState::Planning);
		}
	}
}

void FAgentController::HandleParsingGoalState()
{
	CurrentIteration++;

	if (OnProgress.IsBound())
	{
		OnProgress.Execute(TEXT("Parsing goal..."), 0.0f);
	}

	// Parse user input into a goal
	FAgentGoal Goal = ParseGoal(PendingUserInput);
	PendingUserInput.Empty();

	// Push goal onto the stack (this is the active goal)
	// NOTE: We only use PushGoal, NOT AddGoal. AddGoal adds to a separate Goals array
	// which would create a duplicate that doesn't get its status updated, causing
	// HasActiveGoals() to return true after the stack goal is completed/popped.
	GoalManager.PushGoal(Goal);

	// Move to planning
	SetState(EAgentState::Planning);
}

void FAgentController::HandlePlanningState()
{
	CurrentIteration++;

	FAgentGoal* Goal = GetCurrentGoal();
	if (!Goal)
	{
		SetState(EAgentState::Idle);
		return;
	}

	if (OnProgress.IsBound())
	{
		OnProgress.Execute(TEXT("Creating plan..."), 10.0f);
	}

	// Refresh world model if needed
	RefreshWorldModelIfNeeded();

	// Create plan for goal
	if (!CreatePlanForCurrentGoal())
	{
		FailGoal(*Goal, TEXT("Failed to create valid plan"));
		return;
	}

	Goal->MarkInProgress();

	// Move to execution
	SetState(EAgentState::Executing);
}

void FAgentController::HandleExecutingState()
{
	CurrentIteration++;

	if (!CurrentPlan.IsValid())
	{
		SetState(EAgentState::Evaluating);
		return;
	}

	if (!CurrentPlan.HasMoreSteps())
	{
		// Plan complete, evaluate goal
		SetState(EAgentState::Evaluating);
		return;
	}

	// Safety check
	if (CurrentIteration > MaxIterationsPerGoal)
	{
		FAgentGoal* Goal = GetCurrentGoal();
		if (Goal)
		{
			FailGoal(*Goal, TEXT("Exceeded maximum iterations"));
		}
		return;
	}

	// Execute next step
	ExecuteNextStep();
}

void FAgentController::HandleEvaluatingState()
{
	CurrentIteration++;

	FAgentGoal* Goal = GetCurrentGoal();
	if (!Goal)
	{
		SetState(EAgentState::Idle);
		return;
	}

	if (OnProgress.IsBound())
	{
		OnProgress.Execute(TEXT("Evaluating goal completion..."), 90.0f);
	}

	// CRITICAL: Refresh world model before evaluation so newly spawned actors are visible
	// Without this, actors created by python_execute won't be found during success criteria evaluation
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT Agent: Refreshing world model before goal evaluation"));
	WorldModelManager.RefreshFullScene();

	// Evaluate goal completion
	FGoalEvaluation Evaluation = Evaluator.EvaluateGoal(*Goal, WorldModelManager.GetWorldModel());

	UE_LOG(LogTemp, Log, TEXT("UnrealGPT Agent: Evaluation result - Complete: %s, Progress: %.1f%%, Summary: %s"),
		Evaluation.IsComplete() ? TEXT("YES") : TEXT("NO"),
		Evaluation.ProgressPercent,
		*Evaluation.Summary);

	if (Evaluation.IsComplete())
	{
		CompleteGoal(*Goal, Evaluation);
	}
	else
	{
		// Goal not complete - check if we can retry
		if (Goal->AttemptCount < Goal->MaxAttempts)
		{
			Goal->RecordAttempt();
			SetState(EAgentState::Recovering);
		}
		else
		{
			FailGoal(*Goal, FString::Printf(TEXT("Goal incomplete after %d attempts: %s"),
				Goal->AttemptCount, *Evaluation.Summary));
		}
	}
}

void FAgentController::HandleRecoveringState()
{
	CurrentIteration++;

	FAgentGoal* Goal = GetCurrentGoal();
	if (!Goal)
	{
		SetState(EAgentState::Idle);
		return;
	}

	if (OnProgress.IsBound())
	{
		OnProgress.Execute(TEXT("Recovering from failure..."), 50.0f);
	}

	// Try to create a new plan
	Replan();
}

void FAgentController::HandleWaitingForUserState()
{
	// Just wait - HandleUserResponse will resume
}

// ==================== GOAL MANAGEMENT ====================

FAgentGoal FAgentController::ParseGoal(const FString& UserInput)
{
	// Use LLM to parse intent, or create simple goal
	FAgentGoal Goal;

	if (LLMInterface.GetAgentClient())
	{
		Goal = LLMInterface.ParseUserIntent(UserInput);
	}
	else
	{
		Goal.Description = UserInput;
		Goal.OriginalRequest = UserInput;
	}

	// Extract parameters if LLM didn't
	if (Goal.Parameters.Num() == 0)
	{
		Goal.Parameters = LLMInterface.ExtractParameters(UserInput);
	}

	// Suggest success criteria if none
	if (Goal.SuccessCriteria.Num() == 0)
	{
		TArray<FSuccessCriterion> SuggestedCriteria = LLMInterface.SuggestSuccessCriteria(Goal);
		for (const FSuccessCriterion& Criterion : SuggestedCriteria)
		{
			Goal.SuccessCriteria.Add(Criterion);
		}
	}

	return Goal;
}

bool FAgentController::ActivateNextGoal()
{
	FAgentGoal* Goal = GoalManager.GetActiveGoal();
	if (!Goal)
	{
		TArray<FAgentGoal*> Pending = GoalManager.GetPendingGoals();
		if (Pending.Num() > 0)
		{
			GoalManager.PushGoal(*Pending[0]);
			return true;
		}
		return false;
	}
	return true;
}

void FAgentController::CompleteGoal(const FAgentGoal& Goal, const FGoalEvaluation& Evaluation)
{
	FAgentGoal* ActiveGoal = GetCurrentGoal();
	if (ActiveGoal)
	{
		ActiveGoal->MarkCompleted();
	}

	GoalManager.PopGoal();

	if (OnProgress.IsBound())
	{
		OnProgress.Execute(TEXT("Goal completed!"), 100.0f);
	}

	if (OnGoalCompleted.IsBound())
	{
		OnGoalCompleted.Execute(Goal, Evaluation);
	}

	// Check if there are more goals
	bool bHasMoreGoals = GoalManager.HasActiveGoals();
	UE_LOG(LogTemp, Log, TEXT("UnrealGPT Agent: Goal completed. HasActiveGoals=%s, StackDepth=%d"),
		bHasMoreGoals ? TEXT("YES") : TEXT("NO"),
		GoalManager.GetStackDepth());

	if (bHasMoreGoals)
	{
		SetState(EAgentState::Planning);
	}
	else
	{
		SetState(EAgentState::Completed);
	}
}

void FAgentController::FailGoal(FAgentGoal& Goal, const FString& Reason)
{
	Goal.MarkFailed(Reason);
	GoalManager.PopGoal();

	if (OnGoalFailed.IsBound())
	{
		OnGoalFailed.Execute(Goal, Reason);
	}

	// Check if there are more goals
	if (GoalManager.HasActiveGoals())
	{
		SetState(EAgentState::Planning);
	}
	else
	{
		SetState(EAgentState::Failed);
	}
}

// ==================== PLAN EXECUTION ====================

bool FAgentController::CreatePlanForCurrentGoal()
{
	FAgentGoal* Goal = GetCurrentGoal();
	if (!Goal)
	{
		return false;
	}

	CurrentPlan = Planner.CreatePlan(*Goal, WorldModelManager.GetWorldModel());

	// Validate plan
	FPlanValidation Validation = Planner.ValidatePlan(CurrentPlan);

	if (!Validation.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("Plan validation failed:"));
		for (const FString& Error : Validation.Errors)
		{
			UE_LOG(LogTemp, Warning, TEXT("  - %s"), *Error);
		}

		// Try to fix the plan
		CurrentPlan = Planner.ValidateAndFixPlan(CurrentPlan, WorldModelManager.GetWorldModel());
		Validation = Planner.ValidatePlan(CurrentPlan);

		if (!Validation.IsValid())
		{
			return false;
		}
	}

	CurrentPlan.MarkValidated();
	return true;
}

void FAgentController::ExecuteNextStep()
{
	if (!CurrentPlan.HasMoreSteps())
	{
		return;
	}

	FPlanStep* Step = CurrentPlan.GetNextStep();
	if (!Step)
	{
		return;
	}

	FAgentGoal* Goal = GetCurrentGoal();
	FString GoalId = Goal ? Goal->GoalId : TEXT("");

	// Report progress
	if (OnProgress.IsBound())
	{
		float Progress = 10.0f + (CurrentPlan.CalculateProgress() * 0.8f);
		OnProgress.Execute(FString::Printf(TEXT("Executing: %s"), *Step->Description), Progress);
	}

	// Execute step synchronously for now
	// In production, could use async execution
	CurrentPlan.MarkExecuting();
	FStepResult Result = Executor.ExecuteStep(*Step, WorldModelManager.GetWorldModel(), GoalId);

	// Result is handled via callback or directly here
	if (!Executor.OnStepComplete.IsBound())
	{
		HandleStepResult(Result);
	}
}

void FAgentController::HandleStepResult(const FStepResult& Result)
{
	if (OnStepCompleted.IsBound())
	{
		FPlanStep* Step = CurrentPlan.GetCurrentStep();
		if (Step)
		{
			OnStepCompleted.Execute(*Step, Result);
		}
	}

	if (Result.IsSuccess())
	{
		AdvancePlan(Result);
	}
	else
	{
		HandleFailure(Result);
	}
}

void FAgentController::AdvancePlan(const FStepResult& Result)
{
	FPlanStep* Step = CurrentPlan.GetCurrentStep();

	// Check for branching
	if (Step && Step->HasBranching())
	{
		int32 NextIndex = Step->GetNextStepIndex(Result.Status);
		if (NextIndex == -1)
		{
			// -1 means proceed sequentially
			CurrentPlan.AdvanceToNextStep();
		}
		else if (NextIndex >= 0)
		{
			CurrentPlan.AdvanceToStep(NextIndex);
		}
		// Other values could indicate plan completion
	}
	else
	{
		CurrentPlan.AdvanceToNextStep();
	}

	// Continue execution or evaluate
	if (CurrentPlan.HasMoreSteps())
	{
		// Stay in executing state, Tick will execute next step
	}
	else
	{
		CurrentPlan.MarkCompleted();
		SetState(EAgentState::Evaluating);
	}
}

// ==================== RECOVERY ====================

void FAgentController::HandleFailure(const FStepResult& FailedResult)
{
	FAgentGoal* Goal = GetCurrentGoal();
	if (!Goal)
	{
		SetState(EAgentState::Failed);
		return;
	}

	// Determine recovery action
	ERecoveryAction Action = Evaluator.DetermineRecoveryAction(*Goal, FailedResult);

	switch (Action)
	{
	case ERecoveryAction::Retry:
		RetryCurrentStep();
		break;

	case ERecoveryAction::RetryWithFix:
		if (FailedResult.SuggestedFixes.Num() > 0)
		{
			RetryWithFix(FailedResult.SuggestedFixes[0]);
		}
		else
		{
			RetryCurrentStep();
		}
		break;

	case ERecoveryAction::Replan:
		Replan();
		break;

	case ERecoveryAction::SkipStep:
		CurrentPlan.AdvanceToNextStep();
		break;

	case ERecoveryAction::EscalateToUser:
		EscalateToUser(FailedResult.Summary);
		break;

	case ERecoveryAction::Abort:
		FailGoal(*Goal, FailedResult.Summary);
		break;
	}
}

void FAgentController::RetryCurrentStep()
{
	FPlanStep* Step = CurrentPlan.GetCurrentStep();
	if (Step && Step->CanRetry())
	{
		Step->RetryCount++;
		// Stay in executing state - Tick will re-execute
	}
	else
	{
		// Can't retry - try replanning
		Replan();
	}
}

void FAgentController::RetryWithFix(const FSuggestedFix& Fix)
{
	FPlanStep* Step = CurrentPlan.GetCurrentStep();
	if (Step)
	{
		// Apply the fix
		if (!Fix.ParameterName.IsEmpty() && !Fix.NewValue.IsEmpty())
		{
			Step->ToolArguments.Add(Fix.ParameterName, Fix.NewValue);
		}
		Step->RetryCount++;
	}
	// Stay in executing state
}

void FAgentController::Replan()
{
	FAgentGoal* Goal = GetCurrentGoal();
	if (!Goal)
	{
		SetState(EAgentState::Failed);
		return;
	}

	// Get the last failed result
	FStepResult LastResult;
	FPlanStep* CurrentStep = CurrentPlan.GetCurrentStep();
	if (CurrentStep)
	{
		LastResult = CurrentStep->Result;
	}

	// Generate new plan
	CurrentPlan = Planner.ReplanFromFailure(CurrentPlan, CurrentPlan.CurrentStepIndex,
		LastResult, WorldModelManager.GetWorldModel());

	FPlanValidation Validation = Planner.ValidatePlan(CurrentPlan);
	if (Validation.IsValid())
	{
		CurrentPlan.MarkValidated();
		SetState(EAgentState::Executing);
	}
	else
	{
		FailGoal(*Goal, TEXT("Failed to generate valid recovery plan"));
	}
}

void FAgentController::EscalateToUser(const FString& Reason)
{
	PendingQuestion = LLMInterface.GenerateClarifyingQuestion(
		*GetCurrentGoal(),
		TArray<FString>{Reason});

	if (OnNeedUserInput.IsBound())
	{
		OnNeedUserInput.Execute(PendingQuestion);
	}

	SetState(EAgentState::WaitingForUser);
}

// ==================== WORLD MODEL ====================

void FAgentController::RefreshWorldModelIfNeeded()
{
	if (WorldModelManager.GetWorldModel().NeedsRefresh())
	{
		WorldModelManager.RefreshFullScene();
	}
}
