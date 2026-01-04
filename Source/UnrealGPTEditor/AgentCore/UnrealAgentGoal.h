// Copyright 2024-2026 UnrealGPT. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UnrealAgentTypes.h"
#include "UnrealAgentGoal.generated.h"

/**
 * Represents an explicit goal that the agent is trying to achieve.
 *
 * Key difference from LLM-controlled:
 * - Goals have explicit success criteria that YOUR CODE evaluates
 * - Goals can be decomposed into sub-goals
 * - Progress is tracked programmatically, not by LLM judgment
 */
USTRUCT(BlueprintType)
struct UNREALGPTEDITOR_API FAgentGoal
{
	GENERATED_BODY()

	// ==================== IDENTITY ====================

	/** Unique identifier for this goal */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Meta = (IgnoreForMemberInitializationTest))
	FString GoalId;

	/** Human-readable description of what we're trying to achieve */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString Description;

	/** Original user request that spawned this goal */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString OriginalRequest;

	// ==================== STATUS ====================

	/** Current status */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	EAgentGoalStatus Status = EAgentGoalStatus::Pending;

	/** Priority (higher = more important) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 Priority = 0;

	// ==================== SUCCESS CRITERIA ====================

	/**
	 * Explicit success criteria - YOUR CODE evaluates these.
	 * The goal is complete when all required criteria pass.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FSuccessCriterion> SuccessCriteria;

	// ==================== DECOMPOSITION ====================

	/** Sub-goal IDs (if this goal was decomposed).
	 *  NOTE: We store IDs instead of nested FAgentGoal to avoid UHT recursion errors.
	 *  Sub-goals are stored in FAgentGoalManager and referenced by ID.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FString> SubGoalIds;

	/** Parent goal ID (if this is a sub-goal) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString ParentGoalId;

	// ==================== CONTEXT ====================

	/** Parameters extracted from user request */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TMap<FString, FString> Parameters;

	/** Constraints on how to achieve this goal */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FString> Constraints;

	// ==================== PROGRESS TRACKING ====================

	/** How many times we've attempted this goal */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 AttemptCount = 0;

	/** Maximum attempts before escalating */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 MaxAttempts = 3;

	/** Reasons for previous failures */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FString> FailureReasons;

	/** When this goal was created */
	FDateTime CreatedAt;

	/** When this goal was last updated */
	FDateTime UpdatedAt;

	/** When this goal was completed (if applicable) */
	FDateTime CompletedAt;

	// ==================== METHODS ====================

	FAgentGoal()
		: CreatedAt(FDateTime::Now())
		, UpdatedAt(FDateTime::Now())
	{
		GoalId = FGuid::NewGuid().ToString();
	}

	/** Check if goal has sub-goals */
	bool HasSubGoals() const { return SubGoalIds.Num() > 0; }

	/** Check if this is a sub-goal */
	bool IsSubGoal() const { return !ParentGoalId.IsEmpty(); }

	/** Check if we've exceeded max attempts */
	bool ExceededMaxAttempts() const { return AttemptCount >= MaxAttempts; }

	/** Check if goal is in a terminal state */
	bool IsTerminal() const
	{
		return Status == EAgentGoalStatus::Completed ||
			   Status == EAgentGoalStatus::Failed ||
			   Status == EAgentGoalStatus::Cancelled;
	}

	/** Check if goal is active (can be worked on) */
	bool IsActive() const
	{
		return Status == EAgentGoalStatus::Pending ||
			   Status == EAgentGoalStatus::InProgress;
	}

	/** Mark goal as in progress */
	void MarkInProgress()
	{
		Status = EAgentGoalStatus::InProgress;
		UpdatedAt = FDateTime::Now();
	}

	/** Mark goal as completed */
	void MarkCompleted()
	{
		Status = EAgentGoalStatus::Completed;
		CompletedAt = FDateTime::Now();
		UpdatedAt = FDateTime::Now();
	}

	/** Mark goal as failed */
	void MarkFailed(const FString& Reason)
	{
		Status = EAgentGoalStatus::Failed;
		FailureReasons.Add(Reason);
		UpdatedAt = FDateTime::Now();
	}

	/** Mark goal as blocked */
	void MarkBlocked(const FString& Reason)
	{
		Status = EAgentGoalStatus::Blocked;
		FailureReasons.Add(FString::Printf(TEXT("Blocked: %s"), *Reason));
		UpdatedAt = FDateTime::Now();
	}

	/** Record an attempt */
	void RecordAttempt()
	{
		AttemptCount++;
		UpdatedAt = FDateTime::Now();
	}

	/** Get a parameter value with default */
	FString GetParameter(const FString& Key, const FString& Default = TEXT("")) const
	{
		const FString* Value = Parameters.Find(Key);
		return Value ? *Value : Default;
	}

	/** Set a parameter value */
	void SetParameter(const FString& Key, const FString& Value)
	{
		Parameters.Add(Key, Value);
		UpdatedAt = FDateTime::Now();
	}

	/** Add a success criterion */
	void AddCriterion(const FString& Desc, ESuccessCriterionType Type, const FString& Query, bool bRequired = true)
	{
		FSuccessCriterion Criterion;
		Criterion.Description = Desc;
		Criterion.Type = Type;
		Criterion.ValidationQuery = Query;
		Criterion.bRequired = bRequired;
		SuccessCriteria.Add(Criterion);
	}

	/** Add a sub-goal ID (the sub-goal itself should be added to FAgentGoalManager) */
	void AddSubGoalId(const FString& SubGoalId)
	{
		SubGoalIds.Add(SubGoalId);
	}

	/** Calculate progress based on criteria.
	 *  NOTE: For sub-goal progress, use FAgentGoalManager::CalculateGoalProgress() which has access to all goals.
	 */
	float CalculateProgress() const
	{
		if (Status == EAgentGoalStatus::Completed)
		{
			return 100.0f;
		}

		// Calculate from criteria
		if (SuccessCriteria.Num() > 0)
		{
			int32 PassedCount = 0;
			for (const FSuccessCriterion& Criterion : SuccessCriteria)
			{
				if (Criterion.bLastEvaluationPassed)
				{
					PassedCount++;
				}
			}
			return (static_cast<float>(PassedCount) / SuccessCriteria.Num()) * 100.0f;
		}

		// If we have sub-goals but no criteria, return 0 (use GoalManager for accurate calculation)
		if (HasSubGoals())
		{
			return 0.0f; // Caller should use FAgentGoalManager::CalculateGoalProgress()
		}

		return 0.0f;
	}
};

/**
 * Manages the lifecycle of goals.
 */
class UNREALGPTEDITOR_API FAgentGoalManager
{
public:
	FAgentGoalManager() = default;

	// ==================== GOAL LIFECYCLE ====================

	/** Create a new goal from user request (may use LLM to parse) */
	FAgentGoal& CreateGoal(const FString& Description);

	/** Add an existing goal */
	void AddGoal(const FAgentGoal& Goal);

	/** Get the current active goal */
	FAgentGoal* GetActiveGoal();

	/** Get a goal by ID */
	FAgentGoal* GetGoal(const FString& GoalId);

	/** Get all goals */
	TArray<FAgentGoal>& GetAllGoals() { return Goals; }

	/** Remove a goal */
	bool RemoveGoal(const FString& GoalId);

	// ==================== GOAL STACK ====================

	/** Push a goal onto the stack (becomes active) */
	void PushGoal(const FAgentGoal& Goal);

	/** Pop the current goal from the stack */
	FAgentGoal PopGoal();

	/** Peek at the current goal without removing */
	FAgentGoal* PeekGoal();

	/** Check if there are active goals */
	bool HasActiveGoals() const;

	/** Get goal stack depth */
	int32 GetStackDepth() const { return GoalStack.Num(); }

	// ==================== QUERIES ====================

	/** Get all goals with a specific status */
	TArray<FAgentGoal*> GetGoalsByStatus(EAgentGoalStatus Status);

	/** Get all pending goals */
	TArray<FAgentGoal*> GetPendingGoals();

	/** Get all sub-goals of a parent */
	TArray<FAgentGoal*> GetSubGoals(const FString& ParentGoalId);

	/** Create a sub-goal and link it to parent */
	FAgentGoal& CreateSubGoal(const FString& ParentGoalId, const FString& Description);

	/** Calculate progress for a goal including sub-goals */
	float CalculateGoalProgress(const FString& GoalId);

	// ==================== STATE ====================

	/** Clear all goals */
	void Clear();

	/** Get summary for debugging */
	FString GetDebugSummary() const;

private:
	/** All goals */
	TArray<FAgentGoal> Goals;

	/** Goal stack (for nested goals) */
	TArray<FAgentGoal> GoalStack;

	/** Find goal index by ID */
	int32 FindGoalIndex(const FString& GoalId) const;
};
