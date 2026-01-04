// Copyright 2024-2026 UnrealGPT. All Rights Reserved.

#include "UnrealAgentGoal.h"

// ==================== GOAL LIFECYCLE ====================

FAgentGoal& FAgentGoalManager::CreateGoal(const FString& Description)
{
	FAgentGoal NewGoal;
	NewGoal.Description = Description;
	NewGoal.OriginalRequest = Description;
	Goals.Add(NewGoal);
	return Goals.Last();
}

void FAgentGoalManager::AddGoal(const FAgentGoal& Goal)
{
	Goals.Add(Goal);
}

FAgentGoal* FAgentGoalManager::GetActiveGoal()
{
	// First check the stack
	if (GoalStack.Num() > 0)
	{
		return &GoalStack.Last();
	}

	// Otherwise find first in-progress goal
	for (FAgentGoal& Goal : Goals)
	{
		if (Goal.Status == EAgentGoalStatus::InProgress)
		{
			return &Goal;
		}
	}

	// Or first pending goal
	for (FAgentGoal& Goal : Goals)
	{
		if (Goal.Status == EAgentGoalStatus::Pending)
		{
			return &Goal;
		}
	}

	return nullptr;
}

FAgentGoal* FAgentGoalManager::GetGoal(const FString& GoalId)
{
	// Check all goals (sub-goals are stored in the same array with ParentGoalId set)
	for (FAgentGoal& Goal : Goals)
	{
		if (Goal.GoalId == GoalId)
		{
			return &Goal;
		}
	}

	// Also check the stack
	for (FAgentGoal& Goal : GoalStack)
	{
		if (Goal.GoalId == GoalId)
		{
			return &Goal;
		}
	}

	return nullptr;
}

bool FAgentGoalManager::RemoveGoal(const FString& GoalId)
{
	int32 Index = FindGoalIndex(GoalId);
	if (Index != INDEX_NONE)
	{
		Goals.RemoveAt(Index);
		return true;
	}
	return false;
}

// ==================== GOAL STACK ====================

void FAgentGoalManager::PushGoal(const FAgentGoal& Goal)
{
	GoalStack.Add(Goal);
}

FAgentGoal FAgentGoalManager::PopGoal()
{
	if (GoalStack.Num() > 0)
	{
		return GoalStack.Pop();
	}
	return FAgentGoal();
}

FAgentGoal* FAgentGoalManager::PeekGoal()
{
	if (GoalStack.Num() > 0)
	{
		return &GoalStack.Last();
	}
	return nullptr;
}

bool FAgentGoalManager::HasActiveGoals() const
{
	if (GoalStack.Num() > 0)
	{
		return true;
	}

	for (const FAgentGoal& Goal : Goals)
	{
		if (Goal.IsActive())
		{
			return true;
		}
	}

	return false;
}

// ==================== QUERIES ====================

TArray<FAgentGoal*> FAgentGoalManager::GetGoalsByStatus(EAgentGoalStatus Status)
{
	TArray<FAgentGoal*> Result;
	for (FAgentGoal& Goal : Goals)
	{
		if (Goal.Status == Status)
		{
			Result.Add(&Goal);
		}
	}
	return Result;
}

TArray<FAgentGoal*> FAgentGoalManager::GetPendingGoals()
{
	return GetGoalsByStatus(EAgentGoalStatus::Pending);
}

TArray<FAgentGoal*> FAgentGoalManager::GetSubGoals(const FString& ParentGoalId)
{
	TArray<FAgentGoal*> Result;

	// Find all goals whose ParentGoalId matches
	for (FAgentGoal& Goal : Goals)
	{
		if (Goal.ParentGoalId == ParentGoalId)
		{
			Result.Add(&Goal);
		}
	}

	return Result;
}

FAgentGoal& FAgentGoalManager::CreateSubGoal(const FString& ParentGoalId, const FString& Description)
{
	FAgentGoal SubGoal;
	SubGoal.Description = Description;
	SubGoal.ParentGoalId = ParentGoalId;
	Goals.Add(SubGoal);

	// Link to parent
	FAgentGoal* Parent = GetGoal(ParentGoalId);
	if (Parent)
	{
		Parent->AddSubGoalId(SubGoal.GoalId);
	}

	return Goals.Last();
}

float FAgentGoalManager::CalculateGoalProgress(const FString& GoalId)
{
	FAgentGoal* Goal = GetGoal(GoalId);
	if (!Goal)
	{
		return 0.0f;
	}

	if (Goal->Status == EAgentGoalStatus::Completed)
	{
		return 100.0f;
	}

	// If goal has sub-goals, calculate from them
	if (Goal->HasSubGoals())
	{
		TArray<FAgentGoal*> SubGoals = GetSubGoals(GoalId);
		if (SubGoals.Num() > 0)
		{
			int32 CompletedCount = 0;
			for (FAgentGoal* SubGoal : SubGoals)
			{
				if (SubGoal && SubGoal->Status == EAgentGoalStatus::Completed)
				{
					CompletedCount++;
				}
			}
			return (static_cast<float>(CompletedCount) / SubGoals.Num()) * 100.0f;
		}
	}

	// Otherwise use the goal's own calculation (based on criteria)
	return Goal->CalculateProgress();
}

// ==================== STATE ====================

void FAgentGoalManager::Clear()
{
	Goals.Empty();
	GoalStack.Empty();
}

FString FAgentGoalManager::GetDebugSummary() const
{
	FString Summary;

	Summary += FString::Printf(TEXT("Goals: %d total, Stack: %d\n"), Goals.Num(), GoalStack.Num());

	for (const FAgentGoal& Goal : Goals)
	{
		// Skip sub-goals in top-level listing (they'll be shown under their parent)
		if (Goal.IsSubGoal())
		{
			continue;
		}

		FString StatusStr;
		switch (Goal.Status)
		{
		case EAgentGoalStatus::Pending: StatusStr = TEXT("PENDING"); break;
		case EAgentGoalStatus::InProgress: StatusStr = TEXT("IN_PROGRESS"); break;
		case EAgentGoalStatus::Completed: StatusStr = TEXT("COMPLETED"); break;
		case EAgentGoalStatus::Failed: StatusStr = TEXT("FAILED"); break;
		case EAgentGoalStatus::Blocked: StatusStr = TEXT("BLOCKED"); break;
		case EAgentGoalStatus::Cancelled: StatusStr = TEXT("CANCELLED"); break;
		}

		Summary += FString::Printf(TEXT("  [%s] %s (%.0f%%)\n"),
			*StatusStr,
			*Goal.Description.Left(50),
			Goal.CalculateProgress());

		// Show sub-goals by ID
		for (const FString& SubGoalId : Goal.SubGoalIds)
		{
			// Find sub-goal in Goals array
			for (const FAgentGoal& SubGoal : Goals)
			{
				if (SubGoal.GoalId == SubGoalId)
				{
					Summary += FString::Printf(TEXT("    -> %s\n"), *SubGoal.Description.Left(40));
					break;
				}
			}
		}
	}

	return Summary;
}

int32 FAgentGoalManager::FindGoalIndex(const FString& GoalId) const
{
	for (int32 i = 0; i < Goals.Num(); i++)
	{
		if (Goals[i].GoalId == GoalId)
		{
			return i;
		}
	}
	return INDEX_NONE;
}
