// Copyright 2024-2026 UnrealGPT. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UnrealAgentTypes.h"
#include "UnrealAgentPlan.generated.h"

/**
 * A single step in a plan.
 *
 * Key difference from LLM-controlled:
 * - Steps have explicit preconditions checked BY YOUR CODE
 * - Steps have expected outcomes verified BY YOUR CODE
 * - Steps can branch based on results
 */
USTRUCT(BlueprintType)
struct UNREALGPTEDITOR_API FPlanStep
{
	GENERATED_BODY()

	// ==================== IDENTITY ====================

	/** Unique step identifier */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Meta = (IgnoreForMemberInitializationTest))
	FString StepId;

	/** Human-readable description of what this step does */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString Description;

	/** Step type */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	EPlanStepType Type = EPlanStepType::ToolCall;

	// ==================== TOOL CALL ====================

	/** Tool name to invoke (for ToolCall type) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString ToolName;

	/** Tool arguments as key-value pairs */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TMap<FString, FString> ToolArguments;

	// ==================== CONDITIONS ====================

	/** Preconditions that must be true before this step runs */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FStepPrecondition> Preconditions;

	/** Expected outcomes to verify after this step runs */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FExpectedOutcome> ExpectedOutcomes;

	// ==================== EXECUTION STATE ====================

	/** Has this step been executed */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bExecuted = false;

	/** Result of execution (if executed) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FStepResult Result;

	/** Number of retry attempts */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 RetryCount = 0;

	/** Maximum retries allowed */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 MaxRetries = 2;

	// ==================== BRANCHING ====================

	/**
	 * Map of result status to next step index.
	 * If empty, proceeds to next step sequentially.
	 * Use -1 to indicate plan completion.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TMap<FString, int32> NextStepOnResult;  // "Success" -> 5, "Failed" -> 10

	/** Condition for conditional steps (Decision type) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString Condition;

	// ==================== SUB-PLAN ====================

	/** Nested plan for SubPlan type */
	FString SubPlanId;

	// ==================== METHODS ====================

	FPlanStep()
	{
		StepId = FGuid::NewGuid().ToString();
	}

	/** Create a tool call step */
	static FPlanStep CreateToolCall(const FString& Desc, const FString& Tool, const TMap<FString, FString>& Args)
	{
		FPlanStep Step;
		Step.Description = Desc;
		Step.Type = EPlanStepType::ToolCall;
		Step.ToolName = Tool;
		Step.ToolArguments = Args;
		return Step;
	}

	/** Create an observation step */
	static FPlanStep CreateObservation(const FString& Desc, const FString& Tool, const TMap<FString, FString>& Args)
	{
		FPlanStep Step;
		Step.Description = Desc;
		Step.Type = EPlanStepType::Observation;
		Step.ToolName = Tool;
		Step.ToolArguments = Args;
		return Step;
	}

	/** Create a verification step */
	static FPlanStep CreateVerification(const FString& Desc, const FString& Query)
	{
		FPlanStep Step;
		Step.Description = Desc;
		Step.Type = EPlanStepType::Verification;
		Step.ToolName = TEXT("scene_query");

		// Parse simple query into arguments
		Step.ToolArguments.Add(TEXT("query"), Query);
		return Step;
	}

	/** Add a precondition */
	void AddPrecondition(const FString& Desc, const FString& Check, bool bBlocks = true)
	{
		FStepPrecondition Precond;
		Precond.Description = Desc;
		Precond.CheckExpression = Check;
		Precond.bBlocksExecution = bBlocks;
		Preconditions.Add(Precond);
	}

	/** Add an expected outcome */
	void AddExpectedOutcome(const FString& Desc, const FString& Verification, bool bRequired = true)
	{
		FExpectedOutcome Outcome;
		Outcome.Description = Desc;
		Outcome.VerificationExpression = Verification;
		Outcome.bRequired = bRequired;
		ExpectedOutcomes.Add(Outcome);
	}

	/** Check if step can be retried */
	bool CanRetry() const
	{
		return RetryCount < MaxRetries;
	}

	/** Get tool arguments as JSON string */
	FString GetArgumentsJson() const;

	/** Check if this step has branching logic */
	bool HasBranching() const
	{
		return NextStepOnResult.Num() > 0;
	}

	/** Get next step index based on result */
	int32 GetNextStepIndex(EStepResultStatus ResultStatus) const;
};

/**
 * A complete plan for achieving a goal.
 *
 * Key difference from LLM-controlled:
 * - Plan is validated BY YOUR CODE before execution
 * - Steps have explicit ordering and branching
 * - Plan tracks execution state
 */
USTRUCT(BlueprintType)
struct UNREALGPTEDITOR_API FAgentPlan
{
	GENERATED_BODY()

	// ==================== IDENTITY ====================

	/** Unique plan identifier */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Meta = (IgnoreForMemberInitializationTest))
	FString PlanId;

	/** Goal this plan is for */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString GoalId;

	/** Human-readable plan summary */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString Summary;

	// ==================== STEPS ====================

	/** Ordered list of steps */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FPlanStep> Steps;

	/** Current step index */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 CurrentStepIndex = 0;

	// ==================== STATUS ====================

	/** Plan status */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	EPlanStatus Status = EPlanStatus::Draft;

	// ==================== METADATA ====================

	/** Why this approach was chosen */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString Rationale;

	/** Assumptions the plan makes */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FString> Assumptions;

	/** Known risks */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FString> Risks;

	/** When this plan was created */
	FDateTime CreatedAt;

	/** When this plan was last updated */
	FDateTime UpdatedAt;

	// ==================== METHODS ====================

	FAgentPlan()
		: CreatedAt(FDateTime::Now())
		, UpdatedAt(FDateTime::Now())
	{
		PlanId = FGuid::NewGuid().ToString();
	}

	/** Check if plan is valid for execution */
	bool IsValid() const
	{
		return Status == EPlanStatus::Validated || Status == EPlanStatus::Executing;
	}

	/** Check if plan needs replanning */
	bool NeedsReplan() const
	{
		return Status == EPlanStatus::Replanning || Status == EPlanStatus::Failed;
	}

	/** Check if plan is complete */
	bool IsComplete() const
	{
		return Status == EPlanStatus::Completed;
	}

	/** Check if there are more steps */
	bool HasMoreSteps() const
	{
		return CurrentStepIndex < Steps.Num();
	}

	/** Get current step */
	FPlanStep* GetCurrentStep()
	{
		if (CurrentStepIndex < Steps.Num())
		{
			return &Steps[CurrentStepIndex];
		}
		return nullptr;
	}

	/** Get next step (without advancing) */
	FPlanStep* GetNextStep()
	{
		return GetCurrentStep();
	}

	/** Advance to next step */
	void AdvanceToNextStep()
	{
		CurrentStepIndex++;
		UpdatedAt = FDateTime::Now();
	}

	/** Advance to specific step (for branching) */
	void AdvanceToStep(int32 StepIndex)
	{
		CurrentStepIndex = StepIndex;
		UpdatedAt = FDateTime::Now();
	}

	/** Add a step to the plan */
	FPlanStep& AddStep(const FPlanStep& Step)
	{
		Steps.Add(Step);
		UpdatedAt = FDateTime::Now();
		return Steps.Last();
	}

	/** Add a tool call step */
	FPlanStep& AddToolCall(const FString& Desc, const FString& Tool, const TMap<FString, FString>& Args)
	{
		return AddStep(FPlanStep::CreateToolCall(Desc, Tool, Args));
	}

	/** Add an observation step */
	FPlanStep& AddObservation(const FString& Desc, const FString& Tool, const TMap<FString, FString>& Args)
	{
		return AddStep(FPlanStep::CreateObservation(Desc, Tool, Args));
	}

	/** Add a verification step */
	FPlanStep& AddVerification(const FString& Desc, const FString& Query)
	{
		return AddStep(FPlanStep::CreateVerification(Desc, Query));
	}

	/** Mark plan as validated */
	void MarkValidated()
	{
		Status = EPlanStatus::Validated;
		UpdatedAt = FDateTime::Now();
	}

	/** Mark plan as executing */
	void MarkExecuting()
	{
		Status = EPlanStatus::Executing;
		UpdatedAt = FDateTime::Now();
	}

	/** Mark plan as completed */
	void MarkCompleted()
	{
		Status = EPlanStatus::Completed;
		UpdatedAt = FDateTime::Now();
	}

	/** Mark plan as failed */
	void MarkFailed()
	{
		Status = EPlanStatus::Failed;
		UpdatedAt = FDateTime::Now();
	}

	/** Mark plan as needing replan */
	void MarkNeedsReplan()
	{
		Status = EPlanStatus::Replanning;
		UpdatedAt = FDateTime::Now();
	}

	/** Calculate progress percentage */
	float CalculateProgress() const
	{
		if (Steps.Num() == 0) return 0.0f;

		int32 CompletedSteps = 0;
		for (const FPlanStep& Step : Steps)
		{
			if (Step.bExecuted && Step.Result.IsSuccess())
			{
				CompletedSteps++;
			}
		}

		return (static_cast<float>(CompletedSteps) / Steps.Num()) * 100.0f;
	}

	/** Get step by ID */
	FPlanStep* GetStep(const FString& StepId)
	{
		for (FPlanStep& Step : Steps)
		{
			if (Step.StepId == StepId)
			{
				return &Step;
			}
		}
		return nullptr;
	}

	/** Get step index by ID */
	int32 GetStepIndex(const FString& StepId) const
	{
		for (int32 i = 0; i < Steps.Num(); i++)
		{
			if (Steps[i].StepId == StepId)
			{
				return i;
			}
		}
		return INDEX_NONE;
	}

	/** Get debug summary */
	FString GetDebugSummary() const;
};

/**
 * Validates plans before execution.
 */
class UNREALGPTEDITOR_API FAgentPlanValidator
{
public:
	/** Validate a complete plan */
	static FPlanValidation ValidatePlan(const FAgentPlan& Plan);

	/** Validate a single step */
	static FPlanValidation ValidateStep(const FPlanStep& Step);

	/** Check if a tool name is valid */
	static bool IsValidTool(const FString& ToolName);

	/** Check if tool arguments are valid */
	static bool ValidateToolArguments(const FString& ToolName, const TMap<FString, FString>& Args, TArray<FString>& OutErrors);

private:
	/** Get list of valid tool names */
	static TArray<FString> GetValidToolNames();
};
