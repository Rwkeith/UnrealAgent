// Copyright 2024-2026 UnrealGPT. All Rights Reserved.

#include "UnrealAgentEvaluator.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"

// ==================== GOAL EVALUATION ====================

FGoalEvaluation FAgentEvaluator::EvaluateGoal(FAgentGoal& Goal, const FAgentWorldModel& WorldModel)
{
	FGoalEvaluation Eval;
	Eval.bComplete = true;  // Assume complete until we find a failing criterion

	int32 TotalRequired = 0;
	int32 PassedRequired = 0;

	for (FSuccessCriterion& Criterion : Goal.SuccessCriteria)
	{
		bool bPassed = EvaluateCriterion(Criterion, WorldModel);
		Criterion.bLastEvaluationPassed = bPassed;
		Criterion.LastEvaluatedAt = FDateTime::Now();

		if (bPassed)
		{
			Eval.PassedCriteria.Add(Criterion.Description);
		}
		else
		{
			Eval.FailedCriteria.Add(Criterion.Description);

			if (Criterion.bRequired)
			{
				Eval.bComplete = false;
			}
		}

		if (Criterion.bRequired)
		{
			TotalRequired++;
			if (bPassed)
			{
				PassedRequired++;
			}
		}
	}

	// Calculate progress
	if (TotalRequired > 0)
	{
		Eval.ProgressPercent = (static_cast<float>(PassedRequired) / TotalRequired) * 100.0f;
	}
	else
	{
		Eval.ProgressPercent = Eval.bComplete ? 100.0f : 0.0f;
	}

	// Build summary
	if (Eval.bComplete)
	{
		Eval.Summary = FString::Printf(TEXT("Goal complete: %d/%d criteria passed"),
			Eval.PassedCriteria.Num(), Goal.SuccessCriteria.Num());
	}
	else
	{
		Eval.Summary = FString::Printf(TEXT("Goal incomplete: %d/%d required criteria passed"),
			PassedRequired, TotalRequired);
	}

	return Eval;
}

bool FAgentEvaluator::EvaluateCriterion(FSuccessCriterion& Criterion, const FAgentWorldModel& WorldModel)
{
	switch (Criterion.Type)
	{
	case ESuccessCriterionType::SceneQuery:
		return EvaluateSceneQueryCriterion(Criterion.ValidationQuery, WorldModel);

	case ESuccessCriterionType::PropertyCheck:
		return EvaluatePropertyCheckCriterion(Criterion.ValidationQuery, WorldModel);

	case ESuccessCriterionType::AssetExists:
		return EvaluateAssetExistsCriterion(Criterion.ValidationQuery);

	case ESuccessCriterionType::VisualVerification:
		// Visual verification requires screenshot + vision model
		// For now, return false (not implemented)
		return false;

	case ESuccessCriterionType::Custom:
		// Custom criteria need a callback mechanism
		// For now, return false (not implemented)
		return false;

	default:
		return false;
	}
}

// ==================== STEP OUTCOME EVALUATION ====================

FOutcomeResult FAgentEvaluator::VerifyOutcomes(const FPlanStep& Step, const FToolResult& ToolResult,
	const FAgentWorldModel& WorldModel)
{
	FOutcomeResult Result;
	Result.bAllPassed = true;

	for (const FExpectedOutcome& Outcome : Step.ExpectedOutcomes)
	{
		bool bPassed = EvaluateOutcome(Outcome, ToolResult, WorldModel);

		if (bPassed)
		{
			Result.PassedOutcomes.Add(Outcome.Description);
		}
		else
		{
			Result.FailedOutcomes.Add(Outcome.Description);

			if (Outcome.bRequired)
			{
				Result.bAllPassed = false;
			}
		}
	}

	return Result;
}

bool FAgentEvaluator::EvaluateOutcome(const FExpectedOutcome& Outcome, const FToolResult& ToolResult,
	const FAgentWorldModel& WorldModel)
{
	const FString& Expr = Outcome.VerificationExpression;

	// Check for WorldModel queries
	if (Expr.StartsWith(TEXT("WorldModel.")))
	{
		// Parse WorldModel method calls
		if (Expr.Contains(TEXT("FindActor")))
		{
			// Extract actor ID from expression like "WorldModel.FindActor(ActorId) != null"
			int32 StartParen = Expr.Find(TEXT("("));
			int32 EndParen = Expr.Find(TEXT(")"));
			if (StartParen != INDEX_NONE && EndParen != INDEX_NONE)
			{
				FString ActorId = Expr.Mid(StartParen + 1, EndParen - StartParen - 1);
				ActorId = ActorId.TrimQuotes();

				const FActorState* Actor = WorldModel.FindActor(ActorId);

				if (Expr.Contains(TEXT("!= null")) || Expr.Contains(TEXT("!= nullptr")))
				{
					return Actor != nullptr;
				}
				else if (Expr.Contains(TEXT("== null")) || Expr.Contains(TEXT("== nullptr")))
				{
					return Actor == nullptr;
				}
			}
		}
		else if (Expr.Contains(TEXT("CountActors")))
		{
			// Parse count comparison
			// "WorldModel.CountActors(ByLabel('Tree')) >= 10"
			return EvaluatePropertyCheckCriterion(Expr, WorldModel);
		}
		else if (Expr.Contains(TEXT("IsAreaClear")))
		{
			// For now, assume area clear checks pass if no actors in result
			return true;
		}
	}

	// Check for tool result checks
	if (Expr.StartsWith(TEXT("ToolResult.")))
	{
		if (Expr.Contains(TEXT("bSuccess")))
		{
			return ToolResult.bSuccess;
		}
		else if (Expr.Contains(TEXT("AffectedActorIds.Num()")))
		{
			// Parse count check
			if (Expr.Contains(TEXT("> 0")) || Expr.Contains(TEXT(">= 1")))
			{
				return ToolResult.AffectedActorIds.Num() > 0;
			}
		}
	}

	// Default: check if tool succeeded
	return ToolResult.bSuccess;
}

// ==================== RECOVERY DECISIONS ====================

ERecoveryAction FAgentEvaluator::DetermineRecoveryAction(const FAgentGoal& Goal, const FStepResult& FailedStep)
{
	// Recognize the failure pattern
	EFailurePattern Pattern = RecognizeFailurePattern(FailedStep);

	// Check retry count from step (need to access via different means in real impl)
	int32 RetryCount = 0;  // Would come from step state
	int32 MaxRetries = 2;

	// Get recovery action based on pattern
	ERecoveryAction Action = GetRecoveryForPattern(Pattern, RetryCount, MaxRetries);

	// Override based on goal state
	if (Goal.ExceededMaxAttempts())
	{
		return ERecoveryAction::EscalateToUser;
	}

	// If we have suggested fixes, prefer retry with fix
	if (Action == ERecoveryAction::Retry && FailedStep.SuggestedFixes.Num() > 0)
	{
		return ERecoveryAction::RetryWithFix;
	}

	return Action;
}

TArray<FSuggestedFix> FAgentEvaluator::GenerateSuggestedFixes(const FPlanStep& Step,
	const FStepResult& Result, const FAgentWorldModel& WorldModel)
{
	TArray<FSuggestedFix> Fixes;

	EFailurePattern Pattern = RecognizeFailurePattern(Result);

	switch (Pattern)
	{
	case EFailurePattern::ActorNotFound:
	{
		// Suggest refreshing the world model and trying with different ID
		FSuggestedFix Fix;
		Fix.Description = TEXT("Refresh scene and retry with updated actor reference");
		Fix.ParameterName = TEXT("actor_id");
		Fix.NewValue = TEXT("<refresh_required>");
		Fix.Confidence = 0.6f;
		Fixes.Add(Fix);
		break;
	}

	case EFailurePattern::InvalidArguments:
	{
		// Suggest checking argument format
		FSuggestedFix Fix;
		Fix.Description = TEXT("Validate and correct argument format");
		Fix.Confidence = 0.4f;
		Fixes.Add(Fix);
		break;
	}

	case EFailurePattern::PythonError:
	{
		// Suggest simplifying the Python code
		FSuggestedFix Fix;
		Fix.Description = TEXT("Simplify Python code and add error handling");
		Fix.ParameterName = TEXT("code");
		Fix.Confidence = 0.5f;
		Fixes.Add(Fix);
		break;
	}

	case EFailurePattern::Timeout:
	{
		// Suggest breaking into smaller steps
		FSuggestedFix Fix;
		Fix.Description = TEXT("Break operation into smaller steps");
		Fix.Confidence = 0.7f;
		Fixes.Add(Fix);
		break;
	}

	default:
		break;
	}

	return Fixes;
}

// ==================== CRITERION PARSING ====================

bool FAgentEvaluator::EvaluateSceneQueryCriterion(const FString& Query, const FAgentWorldModel& WorldModel)
{
	FSceneQueryFilter Filter = ParseSceneQueryCriterion(Query);

	// Build actor query from filter
	FActorQuery ActorQuery;
	ActorQuery.ClassFilter = Filter.ClassFilter;
	ActorQuery.LabelFilter = Filter.LabelFilter;
	ActorQuery.TagFilter = Filter.TagFilter;
	ActorQuery.MaxResults = 1000;  // High limit for counting

	// Query the world model
	TArray<FActorState> Actors = WorldModel.QueryActors(ActorQuery);
	int32 ActualCount = Actors.Num();

	// Check count condition if specified
	if (Filter.ExpectedCount >= 0)
	{
		return CheckCountCondition(ActualCount, Filter.CountOperator, Filter.ExpectedCount);
	}

	// No count specified, just check if any actors match
	return ActualCount > 0;
}

bool FAgentEvaluator::EvaluatePropertyCheckCriterion(const FString& Check, const FAgentWorldModel& WorldModel)
{
	// Parse expressions like:
	// "WorldModel.CountActors(ByLabel('Tree')) >= 10"
	// "WorldModel.GetActorCount() > 0"

	if (Check.Contains(TEXT("CountActors")))
	{
		// Extract the query type and parameter
		FString QueryPart;
		int32 ParenStart = Check.Find(TEXT("("));
		int32 ParenEnd = Check.Find(TEXT(")"), ESearchCase::IgnoreCase, ESearchDir::FromStart, ParenStart);

		if (ParenStart != INDEX_NONE && ParenEnd != INDEX_NONE)
		{
			QueryPart = Check.Mid(ParenStart + 1, ParenEnd - ParenStart - 1);
		}

		FActorQuery Query;

		if (QueryPart.Contains(TEXT("ByLabel")))
		{
			// Extract label from ByLabel('X')
			int32 QuoteStart = QueryPart.Find(TEXT("'"));
			int32 QuoteEnd = QueryPart.Find(TEXT("'"), ESearchCase::IgnoreCase, ESearchDir::FromStart, QuoteStart + 1);
			if (QuoteStart != INDEX_NONE && QuoteEnd != INDEX_NONE)
			{
				Query.LabelFilter = QueryPart.Mid(QuoteStart + 1, QuoteEnd - QuoteStart - 1);
			}
		}
		else if (QueryPart.Contains(TEXT("ByClass")))
		{
			int32 QuoteStart = QueryPart.Find(TEXT("'"));
			int32 QuoteEnd = QueryPart.Find(TEXT("'"), ESearchCase::IgnoreCase, ESearchDir::FromStart, QuoteStart + 1);
			if (QuoteStart != INDEX_NONE && QuoteEnd != INDEX_NONE)
			{
				Query.ClassFilter = QueryPart.Mid(QuoteStart + 1, QuoteEnd - QuoteStart - 1);
			}
		}
		else if (QueryPart.Contains(TEXT("ByTag")))
		{
			int32 QuoteStart = QueryPart.Find(TEXT("'"));
			int32 QuoteEnd = QueryPart.Find(TEXT("'"), ESearchCase::IgnoreCase, ESearchDir::FromStart, QuoteStart + 1);
			if (QuoteStart != INDEX_NONE && QuoteEnd != INDEX_NONE)
			{
				Query.TagFilter = QueryPart.Mid(QuoteStart + 1, QuoteEnd - QuoteStart - 1);
			}
		}

		int32 ActualCount = WorldModel.CountActors(Query);

		// Parse comparison operator and value
		// Look for patterns like ">= 10", "> 5", "== 3"
		FString Remainder = Check.Mid(ParenEnd + 1).TrimStartAndEnd();

		FString Operator;
		int32 ExpectedCount = 0;

		if (Remainder.StartsWith(TEXT(">=")))
		{
			Operator = TEXT(">=");
			ExpectedCount = FCString::Atoi(*Remainder.Mid(2).TrimStart());
		}
		else if (Remainder.StartsWith(TEXT("<=")))
		{
			Operator = TEXT("<=");
			ExpectedCount = FCString::Atoi(*Remainder.Mid(2).TrimStart());
		}
		else if (Remainder.StartsWith(TEXT("==")))
		{
			Operator = TEXT("==");
			ExpectedCount = FCString::Atoi(*Remainder.Mid(2).TrimStart());
		}
		else if (Remainder.StartsWith(TEXT(">")))
		{
			Operator = TEXT(">");
			ExpectedCount = FCString::Atoi(*Remainder.Mid(1).TrimStart());
		}
		else if (Remainder.StartsWith(TEXT("<")))
		{
			Operator = TEXT("<");
			ExpectedCount = FCString::Atoi(*Remainder.Mid(1).TrimStart());
		}

		return CheckCountCondition(ActualCount, Operator, ExpectedCount);
	}

	return false;
}

bool FAgentEvaluator::EvaluateAssetExistsCriterion(const FString& AssetPath)
{
	// Check if asset exists on disk
	// Convert game path to filesystem path
	FString FullPath = FPaths::ProjectContentDir();

	if (AssetPath.StartsWith(TEXT("/Game/")))
	{
		FullPath = FPaths::Combine(FullPath, AssetPath.Mid(6));  // Remove "/Game/"
	}
	else
	{
		FullPath = FPaths::Combine(FullPath, AssetPath);
	}

	// Add .uasset extension if not present
	if (!FullPath.EndsWith(TEXT(".uasset")))
	{
		FullPath += TEXT(".uasset");
	}

	return IFileManager::Get().FileExists(*FullPath);
}

// ==================== TOOL RESULT ANALYSIS ====================

bool FAgentEvaluator::AnalyzeToolResultSuccess(const FString& ToolName, const FToolResult& Result)
{
	if (!Result.bSuccess)
	{
		return false;
	}

	// Additional checks based on tool type
	if (ToolName == TEXT("python_execute"))
	{
		// Check for error indicators in output
		if (Result.RawOutput.Contains(TEXT("Error:")) ||
			Result.RawOutput.Contains(TEXT("Exception:")) ||
			Result.RawOutput.Contains(TEXT("Traceback")))
		{
			return false;
		}

		// Check status field
		if (Result.Status == TEXT("error") || Result.Status == TEXT("failed"))
		{
			return false;
		}
	}
	else if (ToolName == TEXT("scene_query"))
	{
		// Scene query is successful even if empty
		return true;
	}
	else if (ToolName == TEXT("set_actor_transform") ||
			 ToolName == TEXT("duplicate_actor") ||
			 ToolName == TEXT("get_actor"))
	{
		// These should have status "ok" or "success"
		return Result.Status == TEXT("ok") || Result.Status == TEXT("success") || Result.Status.IsEmpty();
	}

	return true;
}

TArray<FString> FAgentEvaluator::ExtractAffectedActors(const FString& ToolName, const FToolResult& Result)
{
	// Result.AffectedActorIds should already be populated by the executor
	// This method can do additional parsing if needed
	return Result.AffectedActorIds;
}

// ==================== PRIVATE HELPERS ====================

FAgentEvaluator::FSceneQueryFilter FAgentEvaluator::ParseSceneQueryCriterion(const FString& Query)
{
	FSceneQueryFilter Filter;

	// Parse comma-separated conditions
	TArray<FString> Parts;
	Query.ParseIntoArray(Parts, TEXT(","));

	for (FString Part : Parts)
	{
		Part = Part.TrimStartAndEnd();

		// Class filter: "class=StaticMeshActor" or "class=*Actor"
		if (Part.StartsWith(TEXT("class=")) || Part.StartsWith(TEXT("class =")))
		{
			int32 EqPos = Part.Find(TEXT("="));
			Filter.ClassFilter = Part.Mid(EqPos + 1).TrimStartAndEnd();
		}
		// Label filter: "label contains 'Tree'" or "label='TreeActor'"
		else if (Part.StartsWith(TEXT("label")))
		{
			if (Part.Contains(TEXT("contains")))
			{
				int32 QuoteStart = Part.Find(TEXT("'"));
				int32 QuoteEnd = Part.Find(TEXT("'"), ESearchCase::IgnoreCase, ESearchDir::FromStart, QuoteStart + 1);
				if (QuoteStart != INDEX_NONE && QuoteEnd != INDEX_NONE)
				{
					Filter.LabelFilter = TEXT("*") + Part.Mid(QuoteStart + 1, QuoteEnd - QuoteStart - 1) + TEXT("*");
				}
			}
			else if (Part.Contains(TEXT("=")))
			{
				int32 EqPos = Part.Find(TEXT("="));
				Filter.LabelFilter = Part.Mid(EqPos + 1).TrimStartAndEnd().TrimQuotes();
			}
		}
		// Tag filter: "tag=MyTag"
		else if (Part.StartsWith(TEXT("tag=")) || Part.StartsWith(TEXT("tag =")))
		{
			int32 EqPos = Part.Find(TEXT("="));
			Filter.TagFilter = Part.Mid(EqPos + 1).TrimStartAndEnd().TrimQuotes();
		}
		// Count condition: "count >= 10" or "count == 5"
		else if (Part.StartsWith(TEXT("count")))
		{
			if (Part.Contains(TEXT(">=")))
			{
				Filter.CountOperator = TEXT(">=");
				int32 OpPos = Part.Find(TEXT(">="));
				Filter.ExpectedCount = FCString::Atoi(*Part.Mid(OpPos + 2).TrimStart());
			}
			else if (Part.Contains(TEXT("<=")))
			{
				Filter.CountOperator = TEXT("<=");
				int32 OpPos = Part.Find(TEXT("<="));
				Filter.ExpectedCount = FCString::Atoi(*Part.Mid(OpPos + 2).TrimStart());
			}
			else if (Part.Contains(TEXT("==")))
			{
				Filter.CountOperator = TEXT("==");
				int32 OpPos = Part.Find(TEXT("=="));
				Filter.ExpectedCount = FCString::Atoi(*Part.Mid(OpPos + 2).TrimStart());
			}
			else if (Part.Contains(TEXT(">")))
			{
				Filter.CountOperator = TEXT(">");
				int32 OpPos = Part.Find(TEXT(">"));
				Filter.ExpectedCount = FCString::Atoi(*Part.Mid(OpPos + 1).TrimStart());
			}
			else if (Part.Contains(TEXT("<")))
			{
				Filter.CountOperator = TEXT("<");
				int32 OpPos = Part.Find(TEXT("<"));
				Filter.ExpectedCount = FCString::Atoi(*Part.Mid(OpPos + 1).TrimStart());
			}
		}
	}

	return Filter;
}

bool FAgentEvaluator::CheckCountCondition(int32 ActualCount, const FString& Operator, int32 ExpectedCount)
{
	if (Operator == TEXT(">="))
	{
		return ActualCount >= ExpectedCount;
	}
	else if (Operator == TEXT("<="))
	{
		return ActualCount <= ExpectedCount;
	}
	else if (Operator == TEXT("=="))
	{
		return ActualCount == ExpectedCount;
	}
	else if (Operator == TEXT(">"))
	{
		return ActualCount > ExpectedCount;
	}
	else if (Operator == TEXT("<"))
	{
		return ActualCount < ExpectedCount;
	}
	else if (Operator == TEXT("!="))
	{
		return ActualCount != ExpectedCount;
	}

	// Default: any count is valid
	return true;
}

FAgentEvaluator::EFailurePattern FAgentEvaluator::RecognizeFailurePattern(const FStepResult& Result)
{
	const FString& Error = Result.ToolResult.ErrorMessage;
	const FString& Output = Result.ToolResult.RawOutput;
	const FString& Summary = Result.Summary;

	// Check for common patterns
	if (Error.Contains(TEXT("not found")) || Error.Contains(TEXT("does not exist")) ||
		Error.Contains(TEXT("No actor")) || Error.Contains(TEXT("Invalid actor")))
	{
		return EFailurePattern::ActorNotFound;
	}

	if (Error.Contains(TEXT("Invalid argument")) || Error.Contains(TEXT("Missing required")) ||
		Error.Contains(TEXT("TypeError")) || Error.Contains(TEXT("ValueError")))
	{
		return EFailurePattern::InvalidArguments;
	}

	if (Result.PreconditionResult.FailedPreconditions.Num() > 0)
	{
		return EFailurePattern::PreconditionFailed;
	}

	if (Error.Contains(TEXT("timeout")) || Error.Contains(TEXT("Timeout")) ||
		Error.Contains(TEXT("timed out")))
	{
		return EFailurePattern::Timeout;
	}

	if (Error.Contains(TEXT("Traceback")) || Error.Contains(TEXT("Exception")) ||
		Error.Contains(TEXT("SyntaxError")) || Error.Contains(TEXT("NameError")))
	{
		return EFailurePattern::PythonError;
	}

	if (Error.Contains(TEXT("permission")) || Error.Contains(TEXT("Permission")) ||
		Error.Contains(TEXT("access denied")))
	{
		return EFailurePattern::PermissionDenied;
	}

	if (Error.Contains(TEXT("busy")) || Error.Contains(TEXT("locked")) ||
		Error.Contains(TEXT("in use")))
	{
		return EFailurePattern::ResourceBusy;
	}

	return EFailurePattern::Unknown;
}

ERecoveryAction FAgentEvaluator::GetRecoveryForPattern(EFailurePattern Pattern, int32 RetryCount, int32 MaxRetries)
{
	switch (Pattern)
	{
	case EFailurePattern::ActorNotFound:
		// Actor might have been deleted or ID changed - replan
		return ERecoveryAction::Replan;

	case EFailurePattern::InvalidArguments:
		// Bad arguments - need to replan with different approach
		if (RetryCount < MaxRetries)
		{
			return ERecoveryAction::RetryWithFix;
		}
		return ERecoveryAction::Replan;

	case EFailurePattern::PreconditionFailed:
		// Preconditions not met - replan to satisfy them
		return ERecoveryAction::Replan;

	case EFailurePattern::Timeout:
		// Might work with retry, but if it keeps timing out, escalate
		if (RetryCount < MaxRetries)
		{
			return ERecoveryAction::Retry;
		}
		return ERecoveryAction::EscalateToUser;

	case EFailurePattern::PythonError:
		// Python error might be fixable
		if (RetryCount < MaxRetries)
		{
			return ERecoveryAction::RetryWithFix;
		}
		return ERecoveryAction::Replan;

	case EFailurePattern::PermissionDenied:
		// Can't fix permissions - escalate
		return ERecoveryAction::EscalateToUser;

	case EFailurePattern::ResourceBusy:
		// Might free up with retry
		if (RetryCount < MaxRetries)
		{
			return ERecoveryAction::Retry;
		}
		return ERecoveryAction::EscalateToUser;

	case EFailurePattern::Unknown:
	default:
		// Unknown failure - try once more, then replan
		if (RetryCount < 1)
		{
			return ERecoveryAction::Retry;
		}
		return ERecoveryAction::Replan;
	}
}
