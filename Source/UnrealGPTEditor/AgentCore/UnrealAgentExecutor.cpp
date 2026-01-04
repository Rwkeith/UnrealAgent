// Copyright 2024-2026 UnrealGPT. All Rights Reserved.

#include "UnrealAgentExecutor.h"
#include "UnrealGPTToolDispatcher.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Dom/JsonObject.h"

FAgentExecutor::FAgentExecutor()
{
}

// ==================== STEP EXECUTION ====================

FStepResult FAgentExecutor::ExecuteStep(FPlanStep& Step, FAgentWorldModel& WorldModel,
	const FString& GoalId)
{
	FStepResult Result;
	Result.ExecutedAt = FDateTime::Now();

	bIsExecuting = true;
	bCancelRequested = false;

	// 1. CHECK PRECONDITIONS
	Result.PreconditionResult = CheckPreconditions(Step, WorldModel);

	if (!Result.PreconditionResult.Passed())
	{
		Result.Status = EStepResultStatus::Blocked;
		Result.Summary = FString::Printf(TEXT("Preconditions failed: %s"),
			*FString::Join(Result.PreconditionResult.FailedPreconditions, TEXT(", ")));

		Step.bExecuted = true;
		Step.Result = Result;
		LastResult = Result;
		bIsExecuting = false;
		return Result;
	}

	// Check for cancellation
	if (bCancelRequested)
	{
		Result.Status = EStepResultStatus::Skipped;
		Result.Summary = TEXT("Execution cancelled");
		bIsExecuting = false;
		return Result;
	}

	// 2. EXECUTE TOOL
	double StartTime = FPlatformTime::Seconds();

	Result.ToolResult = ExecuteTool(Step.ToolName, Step.ToolArguments);

	Result.ToolResult.ExecutionTime = static_cast<float>(FPlatformTime::Seconds() - StartTime);

	// Fire tool result delegate
	if (OnToolResult.IsBound())
	{
		OnToolResult.Execute(Step.ToolName, Result.ToolResult);
	}

	// Check for cancellation
	if (bCancelRequested)
	{
		Result.Status = EStepResultStatus::Skipped;
		Result.Summary = TEXT("Execution cancelled after tool call");
		bIsExecuting = false;
		return Result;
	}

	// 3. UPDATE WORLD MODEL
	UpdateWorldModel(Step, Result.ToolResult, WorldModel, GoalId);

	// 4. VERIFY OUTCOMES
	if (Evaluator && Step.ExpectedOutcomes.Num() > 0)
	{
		Result.OutcomeResult = Evaluator->VerifyOutcomes(Step, Result.ToolResult, WorldModel);
	}
	else
	{
		// No evaluator or no outcomes - just check tool success
		Result.OutcomeResult.bAllPassed = Result.ToolResult.bSuccess;
	}

	// 5. DETERMINE FINAL STATUS
	if (Result.ToolResult.bSuccess && Result.OutcomeResult.bAllPassed)
	{
		Result.Status = EStepResultStatus::Success;
		Result.Summary = FString::Printf(TEXT("Step completed: %s"), *Step.Description);
	}
	else if (Result.ToolResult.bSuccess)
	{
		// Tool succeeded but outcomes didn't verify
		Result.Status = EStepResultStatus::PartialSuccess;
		Result.Summary = FString::Printf(TEXT("Tool succeeded but outcomes not verified: %s"),
			*FString::Join(Result.OutcomeResult.FailedOutcomes, TEXT(", ")));
	}
	else
	{
		Result.Status = EStepResultStatus::Failed;
		Result.Summary = FString::Printf(TEXT("Step failed: %s"),
			*Result.ToolResult.ErrorMessage);
		Result.Issues.Add(Result.ToolResult.ErrorMessage);

		// Generate suggested fixes if we have an evaluator
		if (Evaluator)
		{
			Result.SuggestedFixes = Evaluator->GenerateSuggestedFixes(Step, Result, WorldModel);
		}
	}

	// Update step state
	Step.bExecuted = true;
	Step.Result = Result;
	LastResult = Result;
	bIsExecuting = false;

	// Fire completion delegate
	if (OnStepComplete.IsBound())
	{
		OnStepComplete.Execute(Step, Result);
	}

	return Result;
}

FPreconditionResult FAgentExecutor::CheckPreconditions(const FPlanStep& Step,
	const FAgentWorldModel& WorldModel)
{
	FPreconditionResult Result;
	Result.bAllPassed = true;

	for (const FStepPrecondition& Precondition : Step.Preconditions)
	{
		bool bPassed = EvaluatePrecondition(Precondition, WorldModel);

		if (!bPassed)
		{
			Result.FailedPreconditions.Add(Precondition.Description);

			if (Precondition.bBlocksExecution)
			{
				Result.bAllPassed = false;
			}

			// Suggest action to satisfy precondition
			Result.SuggestedActions.Add(FString::Printf(TEXT("Satisfy: %s"), *Precondition.Description));
		}
	}

	return Result;
}

FToolResult FAgentExecutor::ExecuteTool(const FString& ToolName, const TMap<FString, FString>& Args)
{
	FToolResult Result;

	// Convert args to JSON
	FString ArgsJson = ArgsToJson(Args);

	// Execute via internal method (bridges to existing dispatcher)
	FString ResultJson = ExecuteToolInternal(ToolName, ArgsJson);

	// Parse result
	Result = ParseToolResult(ToolName, ResultJson);

	return Result;
}

void FAgentExecutor::UpdateWorldModel(const FPlanStep& Step, const FToolResult& Result,
	FAgentWorldModel& WorldModel, const FString& GoalId)
{
	if (!Result.bSuccess)
	{
		return;  // Don't update world model on failure
	}

	const FString& ToolName = Step.ToolName;

	if (ToolName == TEXT("scene_query"))
	{
		UpdateWorldModelFromSceneQuery(Result, WorldModel);
	}
	else if (ToolName == TEXT("get_actor"))
	{
		UpdateWorldModelFromGetActor(Result, WorldModel);
	}
	else if (ToolName == TEXT("set_actor_transform"))
	{
		UpdateWorldModelFromSetTransform(Step.ToolArguments, Result, WorldModel, GoalId, Step.StepId);
	}
	else if (ToolName == TEXT("duplicate_actor"))
	{
		UpdateWorldModelFromDuplicate(Result, WorldModel, GoalId, Step.StepId);
	}
	else if (ToolName == TEXT("python_execute"))
	{
		UpdateWorldModelFromPythonExecute(Result, WorldModel, GoalId, Step.StepId);
	}
	// Other tools may not affect world model
}

// ==================== BATCH EXECUTION ====================

TArray<FStepResult> FAgentExecutor::ExecuteSteps(TArray<FPlanStep>& Steps,
	FAgentWorldModel& WorldModel, const FString& GoalId, bool bContinueOnError)
{
	TArray<FStepResult> Results;

	for (FPlanStep& Step : Steps)
	{
		if (bCancelRequested)
		{
			break;
		}

		FStepResult Result = ExecuteStep(Step, WorldModel, GoalId);
		Results.Add(Result);

		if (!Result.IsSuccess() && !bContinueOnError)
		{
			break;
		}
	}

	return Results;
}

// ==================== ASYNC EXECUTION ====================

void FAgentExecutor::ExecuteStepAsync(FPlanStep& Step, FAgentWorldModel& WorldModel,
	const FString& GoalId)
{
	CurrentStep = &Step;
	CurrentWorldModel = &WorldModel;
	CurrentGoalId = GoalId;

	// For now, execute synchronously on game thread
	// In production, this would use async tasks for non-blocking tools
	AsyncTask(ENamedThreads::GameThread, [this]()
	{
		if (CurrentStep && CurrentWorldModel)
		{
			ExecuteStep(*CurrentStep, *CurrentWorldModel, CurrentGoalId);
		}
		CurrentStep = nullptr;
		CurrentWorldModel = nullptr;
	});
}

void FAgentExecutor::Cancel()
{
	bCancelRequested = true;
}

// ==================== PRIVATE: TOOL EXECUTION BRIDGE ====================

FString FAgentExecutor::ArgsToJson(const TMap<FString, FString>& Args)
{
	TSharedRef<FJsonObject> JsonObject = MakeShared<FJsonObject>();

	for (const auto& Pair : Args)
	{
		// Try to detect JSON values (arrays, objects, numbers, booleans)
		const FString& Value = Pair.Value;

		if (Value.StartsWith(TEXT("[")) || Value.StartsWith(TEXT("{")))
		{
			// Parse as JSON
			TSharedPtr<FJsonValue> JsonValue;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Value);
			if (FJsonSerializer::Deserialize(Reader, JsonValue) && JsonValue.IsValid())
			{
				JsonObject->SetField(Pair.Key, JsonValue);
				continue;
			}
		}
		else if (Value == TEXT("true") || Value == TEXT("false"))
		{
			JsonObject->SetBoolField(Pair.Key, Value == TEXT("true"));
			continue;
		}
		else if (Value.IsNumeric())
		{
			JsonObject->SetNumberField(Pair.Key, FCString::Atod(*Value));
			continue;
		}

		// Default: treat as string
		JsonObject->SetStringField(Pair.Key, Value);
	}

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonObject, Writer);

	return OutputString;
}

FToolResult FAgentExecutor::ParseToolResult(const FString& ToolName, const FString& ResultJson)
{
	FToolResult Result;

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultJson);

	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		Result.bSuccess = false;
		Result.ErrorMessage = TEXT("Failed to parse tool result JSON");
		Result.RawOutput = ResultJson;
		return Result;
	}

	Result.RawOutput = ResultJson;

	// Parse status - use TryGetStringField to avoid warnings when field is missing
	if (JsonObject->TryGetStringField(TEXT("status"), Result.Status))
	{
		Result.bSuccess = (Result.Status == TEXT("ok") || Result.Status == TEXT("success"));
	}
	else
	{
		// No explicit status field - infer success based on tool-specific response structure
		// scene_query returns {summary: {...}, actors: [...]} without a status field
		if (ToolName == TEXT("scene_query"))
		{
			// scene_query is successful if it has a summary object
			Result.bSuccess = JsonObject->HasField(TEXT("summary")) || JsonObject->HasField(TEXT("actors"));
			Result.Status = Result.bSuccess ? TEXT("ok") : TEXT("error");
		}
		else
		{
			// Default: assume success if no error field is present
			Result.bSuccess = !JsonObject->HasField(TEXT("error"));
			Result.Status = Result.bSuccess ? TEXT("ok") : TEXT("error");
		}
	}

	// Parse error message
	if (JsonObject->HasField(TEXT("error")))
	{
		Result.ErrorMessage = JsonObject->GetStringField(TEXT("error"));
		Result.bSuccess = false;
	}
	else if (JsonObject->HasField(TEXT("message")))
	{
		// Some tools use "message" for errors
		FString Message = JsonObject->GetStringField(TEXT("message"));
		if (!Result.bSuccess)
		{
			Result.ErrorMessage = Message;
		}
	}

	// Extract affected actor IDs based on tool type
	if (ToolName == TEXT("duplicate_actor"))
	{
		if (JsonObject->HasField(TEXT("new_actor_id")))
		{
			Result.AffectedActorIds.Add(JsonObject->GetStringField(TEXT("new_actor_id")));
		}
	}
	else if (ToolName == TEXT("set_actor_transform") || ToolName == TEXT("get_actor"))
	{
		if (JsonObject->HasField(TEXT("actor_id")))
		{
			Result.AffectedActorIds.Add(JsonObject->GetStringField(TEXT("actor_id")));
		}
	}
	else if (ToolName == TEXT("python_execute"))
	{
		// Python might return created_actors array
		const TArray<TSharedPtr<FJsonValue>>* ActorsArray;
		if (JsonObject->TryGetArrayField(TEXT("created_actors"), ActorsArray))
		{
			for (const TSharedPtr<FJsonValue>& Value : *ActorsArray)
			{
				Result.AffectedActorIds.Add(Value->AsString());
			}
		}
	}

	return Result;
}

FString FAgentExecutor::ExecuteToolInternal(const FString& ToolName, const FString& ArgsJson)
{
	// Bridge to the existing UnrealGPTToolDispatcher
	// This is the integration point with your existing tool execution infrastructure

	bool bLastToolWasPythonExecute = false;
	bool bLastSceneQueryFoundResults = false;

	// Execute via the real dispatcher
	FString Result = UnrealGPTToolDispatcher::ExecuteToolCall(
		ToolName,
		ArgsJson,
		bLastToolWasPythonExecute,
		bLastSceneQueryFoundResults,
		nullptr  // No broadcast callback needed for agent-controlled execution
	);

	// If the result is empty or doesn't look like JSON, wrap it
	if (Result.IsEmpty())
	{
		TSharedRef<FJsonObject> ResultObject = MakeShared<FJsonObject>();
		ResultObject->SetStringField(TEXT("status"), TEXT("ok"));
		ResultObject->SetStringField(TEXT("message"), FString::Printf(TEXT("Tool %s completed"), *ToolName));

		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultObject, Writer);
		return OutputString;
	}

	// If result doesn't start with { or [, it might be plain text or an error
	if (!Result.StartsWith(TEXT("{")) && !Result.StartsWith(TEXT("[")))
	{
		// Check if it looks like an error
		bool bIsError = Result.Contains(TEXT("Error")) || Result.Contains(TEXT("error")) ||
						Result.Contains(TEXT("Unknown tool")) || Result.Contains(TEXT("Failed"));

		TSharedRef<FJsonObject> ResultObject = MakeShared<FJsonObject>();
		ResultObject->SetStringField(TEXT("status"), bIsError ? TEXT("error") : TEXT("ok"));
		ResultObject->SetStringField(TEXT("message"), Result);

		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ResultObject, Writer);
		return OutputString;
	}

	return Result;
}

// ==================== PRIVATE: PRECONDITION EVALUATION ====================

bool FAgentExecutor::EvaluatePrecondition(const FStepPrecondition& Precondition,
	const FAgentWorldModel& WorldModel)
{
	return EvaluateCheckExpression(Precondition.CheckExpression, WorldModel);
}

bool FAgentExecutor::EvaluateCheckExpression(const FString& Expression, const FAgentWorldModel& WorldModel)
{
	// Parse and evaluate expressions against the world model

	if (Expression.Contains(TEXT("FindActor")))
	{
		// "WorldModel.FindActor('ActorId') != null"
		int32 QuoteStart = Expression.Find(TEXT("'"));
		int32 QuoteEnd = Expression.Find(TEXT("'"), ESearchCase::IgnoreCase, ESearchDir::FromStart, QuoteStart + 1);

		if (QuoteStart != INDEX_NONE && QuoteEnd != INDEX_NONE)
		{
			FString ActorId = Expression.Mid(QuoteStart + 1, QuoteEnd - QuoteStart - 1);
			const FActorState* Actor = WorldModel.FindActor(ActorId);

			if (Expression.Contains(TEXT("!= null")))
			{
				return Actor != nullptr;
			}
			else if (Expression.Contains(TEXT("== null")))
			{
				return Actor == nullptr;
			}

			return Actor != nullptr;
		}
	}
	else if (Expression.Contains(TEXT("IsAreaClear")))
	{
		// "WorldModel.IsAreaClear(Bounds)" - would need bounds parsing
		// For now, return true (assume area is clear)
		return true;
	}
	else if (Expression.Contains(TEXT("CanModifyActor")))
	{
		// "WorldModel.CanModifyActor('ActorId')"
		int32 QuoteStart = Expression.Find(TEXT("'"));
		int32 QuoteEnd = Expression.Find(TEXT("'"), ESearchCase::IgnoreCase, ESearchDir::FromStart, QuoteStart + 1);

		if (QuoteStart != INDEX_NONE && QuoteEnd != INDEX_NONE)
		{
			FString ActorId = Expression.Mid(QuoteStart + 1, QuoteEnd - QuoteStart - 1);
			return WorldModel.CanModifyActor(ActorId);
		}
	}
	else if (Expression.Contains(TEXT("NeedsRefresh")))
	{
		// "WorldModel.NeedsRefresh()"
		return WorldModel.NeedsRefresh();
	}

	// Unknown expression - default to true (don't block)
	return true;
}

// ==================== PRIVATE: WORLD MODEL UPDATES ====================

void FAgentExecutor::UpdateWorldModelFromSceneQuery(const FToolResult& Result, FAgentWorldModel& WorldModel)
{
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Result.RawOutput);

	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* ActorsArray;
	if (!JsonObject->TryGetArrayField(TEXT("actors"), ActorsArray))
	{
		return;
	}

	for (const TSharedPtr<FJsonValue>& ActorValue : *ActorsArray)
	{
		const TSharedPtr<FJsonObject>* ActorObject;
		if (!ActorValue->TryGetObject(ActorObject))
		{
			continue;
		}

		FActorState Actor;
		Actor.ActorId = (*ActorObject)->GetStringField(TEXT("id"));
		Actor.Label = (*ActorObject)->GetStringField(TEXT("label"));
		Actor.ClassName = (*ActorObject)->GetStringField(TEXT("class"));

		// Parse location
		const TSharedPtr<FJsonObject>* LocationObj;
		if ((*ActorObject)->TryGetObjectField(TEXT("location"), LocationObj))
		{
			Actor.Location = FVector(
				(*LocationObj)->GetNumberField(TEXT("x")),
				(*LocationObj)->GetNumberField(TEXT("y")),
				(*LocationObj)->GetNumberField(TEXT("z"))
			);
		}

		Actor.MarkVerified();
		WorldModel.UpsertActor(Actor);
	}

	WorldModel.MarkRefreshed();
}

void FAgentExecutor::UpdateWorldModelFromGetActor(const FToolResult& Result, FAgentWorldModel& WorldModel)
{
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Result.RawOutput);

	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		return;
	}

	FActorState Actor;
	Actor.ActorId = JsonObject->GetStringField(TEXT("id"));
	Actor.Label = JsonObject->GetStringField(TEXT("label"));
	Actor.ClassName = JsonObject->GetStringField(TEXT("class"));

	// Parse full actor data...
	Actor.MarkVerified();
	WorldModel.UpsertActor(Actor);
}

void FAgentExecutor::UpdateWorldModelFromSetTransform(const TMap<FString, FString>& Args,
	const FToolResult& Result, FAgentWorldModel& WorldModel,
	const FString& GoalId, const FString& StepId)
{
	const FString* ActorIdPtr = Args.Find(TEXT("actor_id"));
	if (!ActorIdPtr)
	{
		return;
	}

	FActorState* Actor = WorldModel.FindActor(*ActorIdPtr);
	if (!Actor)
	{
		return;
	}

	// Update transform from args
	const FString* LocationStr = Args.Find(TEXT("location"));
	if (LocationStr)
	{
		TArray<FString> Parts;
		LocationStr->ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() == 3)
		{
			Actor->Location = FVector(
				FCString::Atof(*Parts[0]),
				FCString::Atof(*Parts[1]),
				FCString::Atof(*Parts[2])
			);
		}
	}

	const FString* RotationStr = Args.Find(TEXT("rotation"));
	if (RotationStr)
	{
		TArray<FString> Parts;
		RotationStr->ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() == 3)
		{
			Actor->Rotation = FRotator(
				FCString::Atof(*Parts[0]),
				FCString::Atof(*Parts[1]),
				FCString::Atof(*Parts[2])
			);
		}
	}

	const FString* ScaleStr = Args.Find(TEXT("scale"));
	if (ScaleStr)
	{
		TArray<FString> Parts;
		ScaleStr->ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() == 3)
		{
			Actor->Scale = FVector(
				FCString::Atof(*Parts[0]),
				FCString::Atof(*Parts[1]),
				FCString::Atof(*Parts[2])
			);
		}
	}

	Actor->MarkAssumed();  // Not verified until next query
	WorldModel.TrackModification(TEXT("Modified"), *ActorIdPtr, GoalId, StepId);
}

void FAgentExecutor::UpdateWorldModelFromDuplicate(const FToolResult& Result,
	FAgentWorldModel& WorldModel, const FString& GoalId, const FString& StepId)
{
	for (const FString& NewActorId : Result.AffectedActorIds)
	{
		// We don't have full actor data yet - just track the creation
		WorldModel.TrackModification(TEXT("Created"), NewActorId, GoalId, StepId);
	}
}

void FAgentExecutor::UpdateWorldModelFromPythonExecute(const FToolResult& Result,
	FAgentWorldModel& WorldModel, const FString& GoalId, const FString& StepId)
{
	// Python could have modified anything
	// Mark affected actors from result, and track modifications
	for (const FString& ActorId : Result.AffectedActorIds)
	{
		FActorState* Actor = WorldModel.FindActor(ActorId);
		if (Actor)
		{
			Actor->Confidence = EActorConfidence::Stale;  // Needs re-verification
		}
		WorldModel.TrackModification(TEXT("Modified"), ActorId, GoalId, StepId);
	}
}
